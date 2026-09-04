// engine/render/src/selection_outline.cpp — task E.1.4: the edge-detect composite. PostProcess's
// twin, built the same way for the same reasons -- one pipeline, one sampler, one fullscreen
// triangle recorded into a caller's already-open pass. Nothing here logs on the happy path; every
// failure path logs exactly one AERO_LOG_ERROR (create) or one LATCHED AERO_LOG_WARN (composite).

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/render/post_process.hpp>  // tonemapSourceUvMax -- the SAME function resolve() uses
#include <aero/render/renderer.hpp>      // Frame
#include <aero/render/selection_outline.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "selection_outline_pack.hpp"
#include "tonemap_pack.hpp"

#include <algorithm>  // std::clamp -- MSVC's STL supplies none of <algorithm> transitively
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace engine::render {
namespace {

// post_process.cpp's ScopedShader, verbatim, and it is not taste: create() has four exits between
// loading a shader and no longer needing it, and NOTHING IN THIS TREE CAN WITNESS A LEAKED SHADER on
// its own -- ~Device logs a WARN and releases it, so ASan sees no process leak and no assertion
// moves. With scope ownership, forgetting a destroy is UNSPELLABLE rather than merely untested.
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

[[nodiscard]] float sanitizeChannel(float v) noexcept {
    // FIRST, and this ordering is the whole point: std::clamp(NaN, 0, 1) returns NaN on libc++, so a
    // clamp-then-check would let a NaN through into the uniform ring (the 3.7.2 rule).
    if (!std::isfinite(v)) {
        return 0.0F;
    }
    return std::clamp(v, 0.0F, 1.0F);
}

[[nodiscard]] Vec4 sanitizeColor(Vec4 c) noexcept {
    return Vec4{sanitizeChannel(c.x), sanitizeChannel(c.y), sanitizeChannel(c.z), sanitizeChannel(c.w)};
}

}  // namespace

SelectionOutlineParams sanitizeSelectionOutlineParams(const SelectionOutlineParams& params) noexcept {
    SelectionOutlineParams sane;
    sane.primaryColorSrgb = sanitizeColor(params.primaryColorSrgb);
    sane.secondaryColorSrgb = sanitizeColor(params.secondaryColorSrgb);
    // 0 becomes MIN for free, because 0 < MIN: a radius of 0 takes no taps at all and would make the
    // outline unconditionally absent.
    sane.radiusPixels = std::clamp(params.radiusPixels, SELECTION_OUTLINE_MIN_RADIUS, SELECTION_OUTLINE_MAX_RADIUS);
    return sane;
}

SelectionOutline::SelectionOutline(rhi::Device* devicePtr, const SelectionOutlineConfig& config,
                                   rhi::GraphicsPipelineHandle pipelineHandle,
                                   rhi::SamplerHandle samplerHandle) noexcept
    : device(devicePtr), cfg(config), pipeline(pipelineHandle), sampler(samplerHandle) {}

std::optional<SelectionOutline> SelectionOutline::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                         const SelectionOutlineConfig& config) {
    AERO_PROFILE_ZONE;
    // 1. Cheap validation, no GPU object yet.
    if (config.outputColorFormat == rhi::TextureFormat::Invalid || rhi::isDepthFormat(config.outputColorFormat)) {
        AERO_LOG_ERROR("render::SelectionOutline::create - outputColorFormat must be a non-depth, non-Invalid format");
        return std::nullopt;
    }

    // 2. The two shaders, SCOPE-OWNED (see ScopedShader above): there is no destroy line on any exit
    //    from here, which is what makes "forgot to release the shader that DID load" unspellable.
    const ScopedShader vs{device, rhi::loadShader(device, shaderVfs, config.vertexShaderPath)};
    const ScopedShader fs{device, rhi::loadShader(device, shaderVfs, config.fragmentShaderPath)};
    if (!vs.get().valid() || !fs.get().valid()) {
        AERO_LOG_ERROR(
            "render::SelectionOutline::create - shader load failed (are res://fullscreen.vert / "
            "res://selection_outline.frag cooked?)");
        return std::nullopt;  // both shaders release themselves on the way out (RAII)
    }

    // 3. The pipeline. Every field that matters is written even where it equals its default, because
    //    a reader must be able to see the whole state without opening descriptors.hpp.
    const rhi::ColorTargetDesc colorTarget{
        .format = config.outputColorFormat,
        // THE ONE PLACE A MISTAKE MAKES THE WHOLE VIEWPORT TRANSPARENT.
        .blend = {.enableBlend = true,
                  .srcColorFactor = rhi::BlendFactor::SrcAlpha,
                  .dstColorFactor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .colorOp = rhi::BlendOp::Add,
                  // ALPHA IS NOT BLENDED. tonemap.frag writes a LITERAL 1.0 alpha because the editor's
                  // ImGui::Image alpha-blends this texture over the panel background (3.6.3 INV-6).
                  // SrcAlpha/OneMinusSrcAlpha on the ALPHA channel would write 0 wherever this shader
                  // outputs a transparent fragment -- which is MOST of the image -- and the viewport
                  // would go SEE-THROUGH. Zero/One leaves dstA untouched.
                  .srcAlphaFactor = rhi::BlendFactor::Zero,
                  .dstAlphaFactor = rhi::BlendFactor::One,
                  .alphaOp = rhi::BlendOp::Add,
                  .writeMask = rhi::ColorWriteMask::All},
    };
    const rhi::GraphicsPipelineDesc pipelineDesc{
        .vertexShader = vs.get(),
        .fragmentShader = fs.get(),
        .vertexBuffers = {},     // zero vertex buffers -- SV_VertexID does the work
        .vertexAttributes = {},  //   "
        .primitiveType = rhi::PrimitiveType::TriangleList,
        .rasterizer = {.fillMode = rhi::FillMode::Fill,
                       // DELIBERATELY NOT the engine's CullMode::Back convention, for
                       // PostProcess::create's recorded reason: a fullscreen triangle's winding is an
                       // artifact of fullscreen.vert.hlsl's vertex-id arithmetic, not a modelling
                       // decision, and Back here turns a one-character formula change into a fully
                       // black screen with no error anywhere.
                       .cullMode = rhi::CullMode::None,
                       .frontFace = rhi::FrontFace::CounterClockwise},
        .depthStencil = {.enableDepthTest = false, .enableDepthWrite = false},
        .colorTargets = std::span{&colorTarget, 1},
        .depthStencilFormat = config.outputDepthFormat,
    };
    const rhi::GraphicsPipelineHandle pipeline = device.createGraphicsPipeline(pipelineDesc);
    if (!pipeline.valid()) {
        AERO_LOG_ERROR("render::SelectionOutline::create - outline pipeline creation failed");
        return std::nullopt;
    }

    // 4. The sampler. NEAREST on both filters is not a preference -- it is what makes `mn < mx` exact.
    const rhi::SamplerDesc samplerDesc{
        // A sampled value is then EXACTLY a texel value, which is what makes the composite's
        // `mn < mx` an exact comparison rather than an epsilon one and what makes the band an exact
        // 2r integer. Linear would smear the three mask levels into each other and make both the
        // band width and the role classification fractional.
        .minFilter = rhi::Filter::Nearest,         .magFilter = rhi::Filter::Nearest,
        .mipmapMode = rhi::MipmapMode::Nearest,  // one level; it must never be filtered across one
        .addressU = rhi::AddressMode::ClampToEdge, .addressV = rhi::AddressMode::ClampToEdge,
        .addressW = rhi::AddressMode::ClampToEdge,
    };
    const rhi::SamplerHandle sampler = device.createSampler(samplerDesc);
    if (!sampler.valid()) {
        device.destroyGraphicsPipeline(pipeline);  // PP4: destroy anything already created
        AERO_LOG_ERROR("render::SelectionOutline::create - sampler creation failed");
        return std::nullopt;
    }

    return SelectionOutline{&device, config, pipeline, sampler};
}

SelectionOutline::SelectionOutline(SelectionOutline&& other) noexcept
    : device(other.device),
      cfg(other.cfg),
      pipeline(other.pipeline),
      sampler(other.sampler),
      composites(other.composites),
      warnedInvalidMask(other.warnedInvalidMask),
      warnedNotRenderable(other.warnedNotRenderable) {
    other.reset();
}

SelectionOutline& SelectionOutline::operator=(SelectionOutline&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroyAll();
    device = other.device;
    cfg = other.cfg;
    pipeline = other.pipeline;
    sampler = other.sampler;
    composites = other.composites;
    warnedInvalidMask = other.warnedInvalidMask;
    warnedNotRenderable = other.warnedNotRenderable;
    other.reset();
    return *this;
}

SelectionOutline::~SelectionOutline() { destroyAll(); }

// PostProcess::destroyAll's shape. A DEFAULTED move would run this on a source whose handles the
// destination now also holds, which is a double free of the pipeline and the sampler.
void SelectionOutline::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (sampler.valid()) {
        device->destroySampler(sampler);
    }
    if (pipeline.valid()) {
        device->destroyGraphicsPipeline(pipeline);
    }
    reset();
}

void SelectionOutline::reset() noexcept {
    device = nullptr;
    pipeline = {};
    sampler = {};
    composites = 0;
    warnedInvalidMask = false;
    warnedNotRenderable = false;
}

void SelectionOutline::composite(Frame& output, const SelectionMaskView& mask, const SelectionOutlineParams& params) {
    AERO_PROFILE_ZONE_NAMED("render::SelectionOutline::composite");
    // 1. A moved-from or otherwise unusable composite. LATCHED: it stays unusable, so an unlatched
    //    WARN here would fire every frame forever. compositeCount() UNMOVED (PP10's asymmetry).
    if (device == nullptr || !pipeline.valid() || !sampler.valid()) {
        if (!warnedNotRenderable) {
            warnedNotRenderable = true;
            AERO_LOG_WARN("render::SelectionOutline::composite - not renderable; nothing recorded");
        }
        return;
    }
    // 2. THE EMPTY-SELECTION PATH, and it is SILENT. A default SelectionMaskView is what an empty
    //    selection and every mask-pass failure return; "nothing is selected" is not a diagnostic.
    if (!mask.valid) {
        return;
    }
    // 3. A view marked valid whose texture does not resolve is a BUG IN THE PRODUCER, not a state.
    if (!mask.texture.valid() || mask.textureExtent.width == 0 || mask.textureExtent.height == 0) {
        if (!warnedInvalidMask) {
            warnedInvalidMask = true;
            AERO_LOG_WARN(
                "render::SelectionOutline::composite - a mask marked valid carries a dead texture or a "
                "degenerate extent; nothing recorded. This warning latches once per object");
        }
        return;
    }

    // 4. BEFORE either pack. tonemapSourceUvMax is the SAME function PostProcess::resolve uses, so
    //    the outline's source sub-rect cannot disagree with the colour image's.
    const SelectionOutlineParams sane = sanitizeSelectionOutlineParams(params);
    const Vec2 uvMax = tonemapSourceUvMax(mask.drawExtent, mask.textureExtent);
    // EXACT texel multiples, which is what makes the band an exact 2r integer.
    const Vec2 texelStep{static_cast<float>(sane.radiusPixels) / static_cast<float>(mask.textureExtent.width),
                         static_cast<float>(sane.radiusPixels) / static_cast<float>(mask.textureExtent.height)};

    // 5. Explicit viewport and scissor, PostProcess::resolve's own reasoning: it makes the pass
    //    self-contained, so a composite recorded into a frame someone else has already narrowed still
    //    covers the whole output.
    const rhi::Extent2D outExtent = output.extent();
    const rhi::RenderPassHandle pass = output.pass();
    device->bindGraphicsPipeline(pass, pipeline);
    device->setViewport(pass, {.x = 0.0F,
                               .y = 0.0F,
                               .width = static_cast<float>(outExtent.width),
                               .height = static_cast<float>(outExtent.height),
                               .minDepth = 0.0F,
                               .maxDepth = 1.0F});
    device->setScissor(pass, {.x = 0, .y = 0, .width = outExtent.width, .height = outExtent.height});

    // 6. The mask, through this object's own Nearest/ClampToEdge sampler.
    const rhi::TextureSamplerBinding binding{mask.texture, sampler};
    device->bindFragmentSamplers(pass, 0, std::span{&binding, 1});

    // 7. The vertex block is detail::packTonemapVertex, REUSED, never respelled -- fullscreen.vert's
    //    FullscreenParams has exactly one packer in this tree (SO6 pins it).
    const auto vertexBlock = detail::packTonemapVertex(uvMax);
    const auto fragmentBlock = detail::packSelectionOutlineFragment(sane, texelStep, uvMax);
    device->pushVertexUniforms(output.commandBuffer(), 0, vertexBlock);
    device->pushFragmentUniforms(output.commandBuffer(), 0, fragmentBlock);

    device->draw(pass, 3);  // ONE oversized triangle covering the whole viewport
    ++composites;           // LAST: counts draws issued and nothing else
}

std::size_t SelectionOutline::compositeCount() const noexcept { return composites; }

bool SelectionOutline::hasWarnedInvalidMask() const noexcept { return warnedInvalidMask; }

bool SelectionOutline::hasWarnedNotRenderable() const noexcept { return warnedNotRenderable; }

}  // namespace engine::render
