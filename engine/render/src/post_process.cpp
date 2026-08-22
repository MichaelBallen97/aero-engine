// engine/render/src/post_process.cpp — task 3.6.3: the owned HDR target and the one-triangle resolve.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/render/post_process.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "tonemap_pack.hpp"

#include <algorithm>  // std::min -- MSVC's STL supplies none of <algorithm> transitively
#include <array>
#include <span>
#include <utility>

namespace engine::render {
namespace {

// A SCOPE-OWNED shader handle, and the reason it exists is a sabotage finding rather than taste.
// create() has four exits between loading a shader and no longer needing it, and a destroy written
// at each of them is four chances to forget one. NOTHING IN THIS TREE CAN WITNESS A LEAKED SHADER
// on its own: ~Device logs a WARN and releases it, so ASan sees no process leak and no assertion
// moves. Measured -- deleting the two explicit destroys left the whole 42-case tier GREEN while
// ~Device reported "releasing 1 leaked shader(s)". So the closure is STRUCTURAL: with ownership
// here, forgetting a destroy is UNSPELLABLE rather than merely untested. PP4's log-capture arm is
// the witness that the release still happens.
//
// The valid() guard also keeps a failed load quiet: destroying an invalid handle is a documented
// no-op, but the backend logs an ERROR for it, and a failure path that reports its own cause should
// not also report a non-problem.
class ScopedShader {
public:
    ScopedShader(rhi::Device& owner, rhi::ShaderHandle shader) noexcept : device(&owner), handle(shader) {}
    ~ScopedShader() {
        if (device != nullptr && handle.valid()) {
            device->destroyShader(handle);
        }
    }
    ScopedShader(const ScopedShader&) = delete;
    ScopedShader& operator=(const ScopedShader&) = delete;
    ScopedShader(ScopedShader&&) = delete;
    ScopedShader& operator=(ScopedShader&&) = delete;

    [[nodiscard]] rhi::ShaderHandle get() const noexcept { return handle; }

private:
    rhi::Device* device;
    rhi::ShaderHandle handle;
};

}  // namespace

Vec2 tonemapSourceUvMax(rhi::Extent2D drawExtent, rhi::Extent2D textureExtent) noexcept {
    const auto axis = [](std::uint32_t draw, std::uint32_t texture) {
        if (texture == 0) {
            return 1.0F;  // a not-renderable target: 1.0 is the only answer that is not a division
        }  // by zero, and nothing is drawn from it anyway.
        return std::min(static_cast<float>(draw) / static_cast<float>(texture), 1.0F);
    };
    return Vec2{axis(drawExtent.width, textureExtent.width), axis(drawExtent.height, textureExtent.height)};
}

PostProcess::PostProcess(rhi::Device* devicePtr, const PostProcessConfig& config, RenderTarget&& sceneTarget,
                         rhi::GraphicsPipelineHandle pipelineHandle, rhi::SamplerHandle samplerHandle) noexcept
    : device(devicePtr), cfg(config), scene(std::move(sceneTarget)), pipeline(pipelineHandle), sampler(samplerHandle) {}

std::optional<PostProcess> PostProcess::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                               rhi::Extent2D requested, const PostProcessConfig& config) {
    // 1. Cheap validation, no GPU object yet.
    if (config.outputColorFormat == rhi::TextureFormat::Invalid || rhi::isDepthFormat(config.outputColorFormat)) {
        AERO_LOG_ERROR("render::PostProcess::create - outputColorFormat must be a non-depth, non-Invalid format");
        return std::nullopt;
    }

    // 2. The scene target. Its own create() validates sceneColorFormat (Invalid / a depth format /
    //    unsupported for Sampler|ColorTarget) and picks the depth format, each with its own ERROR --
    //    PP3 reaches RenderTarget's refusal THROUGH this config rather than duplicating it here.
    std::optional<RenderTarget> scene = RenderTarget::create(device, requested,
                                                             {.colorFormat = config.sceneColorFormat,
                                                              .depth = config.sceneDepth,
                                                              .quantum = config.quantum,
                                                              .maxExtent = config.maxExtent});
    if (!scene) {
        AERO_LOG_ERROR("render::PostProcess::create - scene render target creation failed");
        return std::nullopt;
    }

    // 3. The two shaders. loadShader returns an INVALID handle + its own ERROR on any failure. Both
    //    are SCOPE-OWNED (see ScopedShader above): there is no destroy line on any exit from here,
    //    which is what makes "forgot to release the shader that DID load" unspellable rather than
    //    merely untested. They stay alive until the end of this function, which is well past the
    //    pipeline creation that needs them.
    const ScopedShader vs{device, rhi::loadShader(device, shaderVfs, config.vertexShaderPath)};
    const ScopedShader fs{device, rhi::loadShader(device, shaderVfs, config.fragmentShaderPath)};
    if (!vs.get().valid() || !fs.get().valid()) {
        AERO_LOG_ERROR(
            "render::PostProcess::create - shader load failed (are res://fullscreen.vert / "
            "res://tonemap.frag cooked?)");
        return std::nullopt;  // `scene` and both shaders release themselves on the way out (RAII)
    }

    // 4. The pipeline. Every field that matters is written even where it equals its default, because
    //    a reader must be able to see the whole state without opening descriptors.hpp. The two
    //    exceptions are SampleCount::One (matches both targets by construction) and
    //    PrimitiveType::TriangleList (the default, and the only thing three vertices can be).
    const rhi::ColorTargetDesc colorTarget{
        .format = config.outputColorFormat,
        .blend = {.enableBlend = false, .writeMask = rhi::ColorWriteMask::All},  // D5: blending off
    };
    const rhi::GraphicsPipelineDesc pipelineDesc{
        .vertexShader = vs.get(),
        .fragmentShader = fs.get(),
        .vertexBuffers = {},     // zero vertex buffers -- SV_VertexID does the work
        .vertexAttributes = {},  //   "
        .primitiveType = rhi::PrimitiveType::TriangleList,
        .rasterizer =
            {
                .fillMode = rhi::FillMode::Fill,
                // DELIBERATELY NOT the engine's CullMode::Back convention: a fullscreen triangle's
                // winding is an artifact of the vertex-id arithmetic in fullscreen.vert.hlsl, not a
                // modelling decision, and a convention-following Back here turns a one-character
                // formula change into a fully black screen with no error anywhere.
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil = {.enableDepthTest = false, .enableDepthWrite = false},
        .colorTargets = std::span{&colorTarget, 1},
        // NOT always Invalid: the pipeline's depth format must match the pass it records into, and a
        // caller is free to resolve into a depth-carrying frame. PP12 covers both arms.
        .depthStencilFormat = config.outputDepthFormat,
    };
    const rhi::GraphicsPipelineHandle pipeline = device.createGraphicsPipeline(pipelineDesc);
    // Shaders may be destroyed the moment the pipeline exists; ScopedShader does it at the end of
    // this function, which is the same thing one statement later and cannot be forgotten.
    if (!pipeline.valid()) {
        AERO_LOG_ERROR("render::PostProcess::create - fullscreen pipeline creation failed");
        return std::nullopt;
    }

    // 5. The sampler.
    const rhi::SamplerDesc samplerDesc{
        // The blit is 1:1 BY CONSTRUCTION -- both targets are sized from the same requested extent,
        // so output pixel centre (i+0.5)/W maps to source UV (i+0.5)/allocW, which IS a texel centre.
        // Nearest is therefore provably exact and makes reading the unrendered margin past drawExtent
        // structurally unreachable rather than merely unlikely. Linear would be correct at 1:1 too
        // and would silently bleed the margin the moment the ratio changed.
        .minFilter = rhi::Filter::Nearest,
        .magFilter = rhi::Filter::Nearest,
        // The scene texture has one mip level (RenderTarget allocates at TextureDesc's default), so
        // this is inert -- spelled Nearest anyway so no line of this desc says something it does not
        // mean.
        .mipmapMode = rhi::MipmapMode::Nearest,
        // Belt-and-braces under the 1:1 invariant: it never fires, and Repeat would wrap the
        // unrendered margin into the picture if it ever did.
        .addressU = rhi::AddressMode::ClampToEdge,
        .addressV = rhi::AddressMode::ClampToEdge,
        .addressW = rhi::AddressMode::ClampToEdge,
    };
    const rhi::SamplerHandle sampler = device.createSampler(samplerDesc);
    if (!sampler.valid()) {
        device.destroyGraphicsPipeline(pipeline);
        AERO_LOG_ERROR("render::PostProcess::create - sampler creation failed");
        return std::nullopt;
    }

    return PostProcess{&device, config, std::move(*scene), pipeline, sampler};
}

PostProcess::PostProcess(PostProcess&& other) noexcept
    : device(other.device),
      cfg(other.cfg),
      scene(std::move(other.scene)),
      pipeline(other.pipeline),
      sampler(other.sampler),
      resolves(other.resolves),
      sceneEnded(other.sceneEnded),
      warnedExtentMismatch(other.warnedExtentMismatch),
      warnedResolveBeforeEndScene(other.warnedResolveBeforeEndScene),
      warnedNotRenderable(other.warnedNotRenderable) {
    other.reset();
}

PostProcess& PostProcess::operator=(PostProcess&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroyAll();
    device = other.device;
    cfg = other.cfg;
    scene = std::move(other.scene);
    pipeline = other.pipeline;
    sampler = other.sampler;
    resolves = other.resolves;
    sceneEnded = other.sceneEnded;
    warnedExtentMismatch = other.warnedExtentMismatch;
    warnedResolveBeforeEndScene = other.warnedResolveBeforeEndScene;
    warnedNotRenderable = other.warnedNotRenderable;
    other.reset();
    return *this;
}

PostProcess::~PostProcess() { destroyAll(); }

// ForwardRenderer's C1 shape. A DEFAULTED move would run this on a source whose handles the
// destination now also holds, which is a double free of the pipeline and the sampler.
void PostProcess::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (sampler.valid()) {
        device->destroySampler(sampler);
    }
    if (pipeline.valid()) {
        device->destroyGraphicsPipeline(pipeline);
    }
    scene.reset();  // RenderTarget's own dtor releases both textures
    reset();
}

void PostProcess::reset() noexcept {
    device = nullptr;
    scene.reset();
    pipeline = {};
    sampler = {};
    resolves = 0;
    sceneEnded = false;
    warnedExtentMismatch = false;
    warnedResolveBeforeEndScene = false;
    warnedNotRenderable = false;
}

bool PostProcess::resize(rhi::Extent2D requested) { return scene && scene->resize(requested); }

std::optional<Frame> PostProcess::beginScene(const rhi::Color& clearColor) {
    sceneEnded = false;  // cleared FIRST, unconditionally, before the forward pass can fail
    if (!scene) {
        return std::nullopt;
    }
    return scene->beginFrame(clearColor);
}

// The flag means "the scene pass has been CLOSED this cycle", and it has been closed whether or not
// the submit succeeded -- a failed submit is a lost frame, not an open pass. Setting it only on
// success would make a transient submit failure produce a spurious hasWarnedResolveBeforeEndScene()
// on the following resolve, which is the latch reporting the wrong thing.
bool PostProcess::endScene(Frame frame) {
    sceneEnded = true;
    return scene && scene->endFrame(std::move(frame));
}

void PostProcess::resolve(Frame& output, const TonemapParams& params) {
    AERO_PROFILE_ZONE_NAMED("render::PostProcess::resolve");
    if (device == nullptr || !scene || !pipeline.valid() || !sampler.valid() || !scene->colorTexture().valid()) {
        if (!warnedNotRenderable) {
            warnedNotRenderable = true;  // latched: a not-renderable pass stays not renderable
            AERO_LOG_WARN("render::PostProcess::resolve - pass is not renderable; nothing recorded");
        }
        return;  // resolveCount() UNMOVED -- PP10's asymmetry
    }
    // Read ONCE, into a named local: the extent feeds the mismatch comparison, the viewport and the
    // scissor, and the three must never disagree.
    const rhi::Extent2D outExtent = output.extent();
    const rhi::Extent2D sceneDraw = scene->drawExtent();

    if (!sceneEnded && !warnedResolveBeforeEndScene) {
        warnedResolveBeforeEndScene = true;
        AERO_LOG_WARN(
            "render::PostProcess::resolve - recorded before endScene(); the scene texture's "
            "contents are undefined this frame (INV-7)");
    }
    if (outExtent != sceneDraw && !warnedExtentMismatch) {
        warnedExtentMismatch = true;
        AERO_LOG_WARN(
            "render::PostProcess::resolve - output extent {}x{} != scene draw extent {}x{}; the "
            "picture will be stretched (INV-1)",
            outExtent.width, outExtent.height, sceneDraw.width, sceneDraw.height);
    }

    const TonemapParams sane = sanitizeTonemapParams(params);                  // INV-3, BEFORE either pack
    const Vec2 uvMax = tonemapSourceUvMax(sceneDraw, scene->textureExtent());  // INV-2

    const rhi::RenderPassHandle pass = output.pass();
    device->bindGraphicsPipeline(pass, pipeline);
    // Explicit, even though both current callers already have exactly this state: beginRenderPass
    // sets a default full-target viewport/scissor and RenderTarget::beginFrame narrows them to
    // drawExtent(). Setting them here makes the pass self-contained -- a resolve recorded into a
    // frame someone else has already narrowed still covers the whole output.
    device->setViewport(pass, {.x = 0.0F,
                               .y = 0.0F,
                               .width = static_cast<float>(outExtent.width),
                               .height = static_cast<float>(outExtent.height),
                               .minDepth = 0.0F,
                               .maxDepth = 1.0F});
    device->setScissor(pass, {.x = 0, .y = 0, .width = outExtent.width, .height = outExtent.height});

    const rhi::TextureSamplerBinding binding{scene->colorTexture(), sampler};
    device->bindFragmentSamplers(pass, 0, std::span{&binding, 1});

    const auto vertexBlock = detail::packTonemapVertex(uvMax);
    const auto fragmentBlock = detail::packTonemapFragment(sane);
    device->pushVertexUniforms(output.commandBuffer(), 0, vertexBlock);
    device->pushFragmentUniforms(output.commandBuffer(), 0, fragmentBlock);

    device->draw(pass, 3);  // ONE oversized triangle covering the whole viewport
    ++resolves;             // LAST: counts draws issued and nothing else
}

rhi::TextureFormat PostProcess::sceneColorFormat() const noexcept {
    return scene ? scene->colorFormat() : rhi::TextureFormat::Invalid;
}

rhi::TextureFormat PostProcess::sceneDepthFormat() const noexcept {
    return scene ? scene->depthFormat() : rhi::TextureFormat::Invalid;
}

rhi::Extent2D PostProcess::sceneDrawExtent() const noexcept { return scene ? scene->drawExtent() : rhi::Extent2D{}; }

rhi::Extent2D PostProcess::sceneTextureExtent() const noexcept {
    return scene ? scene->textureExtent() : rhi::Extent2D{};
}

std::size_t PostProcess::resolveCount() const noexcept { return resolves; }

bool PostProcess::hasWarnedExtentMismatch() const noexcept { return warnedExtentMismatch; }

bool PostProcess::hasWarnedResolveBeforeEndScene() const noexcept { return warnedResolveBeforeEndScene; }

}  // namespace engine::render
