// engine/render/src/sky_pass.cpp — task E.2.1: the one pipeline, the two blocks and the three-vertex
// draw that put a gradient behind everything else.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/render/sky_pass.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "sky_pack.hpp"

#include <span>
#include <utility>

namespace engine::render {
namespace {

// post_process.cpp's ScopedShader, verbatim in intent and for the same measured reason: create() has
// several exits between loading a shader and no longer needing it, and NOTHING IN THIS TREE CAN
// WITNESS A LEAKED SHADER on its own -- ~Device logs a WARN and releases it, so ASan sees no process
// leak and no assertion moves. With ownership here, forgetting a destroy is UNSPELLABLE rather than
// merely untested; SB5's log-capture arm is the witness that the release still happens.
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

SkyPass::SkyPass(rhi::Device* devicePtr, rhi::GraphicsPipelineHandle pipelineHandle) noexcept
    : device(devicePtr), pipeline(pipelineHandle) {}

std::optional<SkyPass> SkyPass::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                       const SkyPassConfig& config) {
    AERO_PROFILE_ZONE;
    // 1. Cheap validation, no GPU object yet.
    if (config.colorFormat == rhi::TextureFormat::Invalid || rhi::isDepthFormat(config.colorFormat)) {
        AERO_LOG_ERROR("render::SkyPass::create - colorFormat must be a non-depth, non-Invalid format");
        return std::nullopt;
    }

    // 2. The two shaders, both SCOPE-OWNED: there is no destroy line on any exit from here, which is
    //    what makes "forgot to release the shader that DID load" unspellable.
    const ScopedShader vs{device, rhi::loadShader(device, shaderVfs, config.vertexShaderPath)};
    const ScopedShader fs{device, rhi::loadShader(device, shaderVfs, config.fragmentShaderPath)};
    if (!vs.get().valid() || !fs.get().valid()) {
        AERO_LOG_ERROR("render::SkyPass::create - shader load failed (are res://sky.vert / res://sky.frag cooked?)");
        return std::nullopt;  // both shaders release themselves on the way out (RAII)
    }

    // 3. The pipeline. Every field that matters is written even where it equals its default, because
    //    a reader must be able to see the whole state without opening descriptors.hpp.
    const rhi::ColorTargetDesc colorTarget{
        .format = config.colorFormat,
        .blend = {.enableBlend = false, .writeMask = rhi::ColorWriteMask::All},
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
                // winding is an artifact of the vertex-id arithmetic in sky.vert.hlsl, not a
                // modelling decision, and a convention-following Back turns a one-character formula
                // change into a fully black screen with no error anywhere (fullscreen.vert.hlsl's
                // own reason, verbatim).
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        // Spelled although it is the default: it must match the target the pass records into, and
        // every target in this tree is single-sampled.
        .sampleCount = rhi::SampleCount::One,
        // Depth is neither tested nor written: the sky is UNDER everything, and leaving depth at the
        // cleared 1.0 is what lets E.1.1's Tested debug lines still draw over it.
        .depthStencil = {.enableDepthTest = false, .enableDepthWrite = false},
        .colorTargets = std::span{&colorTarget, 1},
        // NOT always Invalid: the pipeline's depth format must MATCH the pass it records into, and
        // SceneRenderer's pass carries depth.
        .depthStencilFormat = config.depthFormat,
    };
    const rhi::GraphicsPipelineHandle pipeline = device.createGraphicsPipeline(pipelineDesc);
    if (!pipeline.valid()) {
        AERO_LOG_ERROR("render::SkyPass::create - sky pipeline creation failed");
        return std::nullopt;
    }
    return SkyPass{&device, pipeline};
}

SkyPass::SkyPass(SkyPass&& other) noexcept
    : device(other.device),
      pipeline(other.pipeline),
      draws(other.draws),
      warnedDegenerateCamera(other.warnedDegenerateCamera),
      warnedNotRenderable(other.warnedNotRenderable) {
    other.reset();
}

SkyPass& SkyPass::operator=(SkyPass&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroyAll();
    device = other.device;
    pipeline = other.pipeline;
    draws = other.draws;
    warnedDegenerateCamera = other.warnedDegenerateCamera;
    warnedNotRenderable = other.warnedNotRenderable;
    other.reset();
    return *this;
}

SkyPass::~SkyPass() { destroyAll(); }

// PostProcess's C1 shape. A DEFAULTED move would run this on a source whose pipeline the destination
// now also holds, which is a double free.
void SkyPass::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (pipeline.valid()) {
        device->destroyGraphicsPipeline(pipeline);
    }
    reset();
}

void SkyPass::reset() noexcept {
    device = nullptr;
    pipeline = {};
    draws = 0;
    warnedDegenerateCamera = false;
    warnedNotRenderable = false;
}

void SkyPass::draw(Frame& frame, const RenderView& view) {
    AERO_PROFILE_ZONE_NAMED("render::SkyPass::draw");
    if (device == nullptr || !pipeline.valid()) {
        if (!warnedNotRenderable) {
            warnedNotRenderable = true;  // latched: a not-renderable pass stays not renderable
            AERO_LOG_WARN("render::SkyPass::draw - pass is not renderable; nothing recorded");
        }
        return;  // drawCount() UNMOVED
    }
    if (!view.hasCamera) {
        // SILENTLY, the forward pass's own 0-camera rule: nothing to unproject through, and a WARN
        // here would fire on the ordinary "no camera yet" frame every SceneRenderer consumer has.
        return;
    }
    const std::optional<detail::GpuSkyCamera> cameraBlock = detail::packSkyCamera(view.camera);
    if (!cameraBlock.has_value()) {
        if (!warnedDegenerateCamera) {
            warnedDegenerateCamera = true;
            AERO_LOG_WARN(
                "render::SkyPass::draw - inverse(proj * view) is not finite (degenerate camera); "
                "no sky recorded");
        }
        return;  // drawCount() UNMOVED
    }

    const rhi::RenderPassHandle pass = frame.pass();
    device->bindGraphicsPipeline(pass, pipeline);
    // NO setViewport AND NO setScissor -- see sky_pass.hpp. The frame's own state is what the forward
    // pass will draw under, so sky and geometry cover the same rect by construction.
    const detail::GpuSkyParams gradientBlock = detail::packSkyParams(resolveSkyGradient(view.environment));
    device->pushVertexUniforms(frame.commandBuffer(), 0, std::as_bytes(std::span{&*cameraBlock, 1}));
    device->pushFragmentUniforms(frame.commandBuffer(), 0, std::as_bytes(std::span{&gradientBlock, 1}));

    device->draw(pass, 3);  // ONE oversized triangle covering the whole viewport
    ++draws;                // LAST: counts draws issued and nothing else
}

std::size_t SkyPass::drawCount() const noexcept { return draws; }

bool SkyPass::hasWarnedDegenerateCamera() const noexcept { return warnedDegenerateCamera; }

}  // namespace engine::render
