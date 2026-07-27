// engine/render/src/render_target.cpp — task 2.2.3: the offscreen sibling of Renderer. nextTargetExtent
// is a pure, GPU-free sizing policy (D5/D6); RenderTarget mirrors Renderer's shape exactly (D2) except
// it acquires+submits its OWN command buffer instead of a swapchain image. See render_target.hpp for
// the full contract.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/render/render_target.hpp>
#include <aero/rhi/descriptors.hpp>  // rhi::{ColorAttachment, DepthStencilAttachment, RenderPassDesc}
#include <aero/rhi/device.hpp>       // rhi::Device (full)

#include <algorithm>
#include <cstdint>
#include <utility>

namespace engine::render {

namespace {

// Per-axis half of nextTargetExtent (C2): the round-up is done in std::uint64_t and saturated at
// `maxExtent` BEFORE narrowing back to std::uint32_t, so an extreme `maxExtent` cannot wrap the
// round-up (a 32-bit `((req + q - 1) / q) * q` wraps at req == maxExtent == UINT32_MAX, q == 64:
// req + 63 wraps to 62, 62 / 64 * 64 == 0 — silently smaller than the request, breaking INV-1).
[[nodiscard]] std::uint32_t axisNextExtent(std::uint32_t requested, std::uint32_t current, std::uint32_t quantum,
                                           std::uint32_t maxExtent) noexcept {
    const std::uint64_t q = std::max<std::uint64_t>(quantum, 1);
    const std::uint64_t maxE = std::max<std::uint64_t>(maxExtent, 1);
    const std::uint64_t req = std::clamp<std::uint64_t>(requested, std::uint64_t{1}, maxE);
    const std::uint64_t roundedUp = ((req + q - 1) / q) * q;  // 64-bit: cannot wrap for uint32 inputs
    const std::uint64_t want = std::min<std::uint64_t>(roundedUp, maxE);

    if (current == 0) {
        return static_cast<std::uint32_t>(want);  // first allocation
    }
    if (req > current) {
        return static_cast<std::uint32_t>(want);  // grow: the current allocation cannot hold it
    }
    if (want * 2 <= current) {
        return static_cast<std::uint32_t>(want);  // shrink, only once the need at least halves
    }
    return current;  // keep: no reallocation
}

// The same per-axis clamp nextTargetExtent applies to `requested`, exposed for drawRect (which must
// track the CLAMPED request even when the allocation itself is kept unchanged — E6).
[[nodiscard]] std::uint32_t clampAxis(std::uint32_t requested, std::uint32_t maxExtent) noexcept {
    const std::uint64_t maxE = std::max<std::uint64_t>(maxExtent, 1);
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(requested, std::uint64_t{1}, maxE));
}

// The real ceiling shared by every desktop 2D-texture backend this engine targets: Metal's hard max
// is 16384 (Apple Silicon and Intel alike), D3D12 feature level 11_0+ requires 16384, and Vulkan
// desktop drivers report maxImageDimension2D >= 16384 in practice. RENDER_TARGET_MAX_EXTENT's default
// (8192) stays comfortably under it (the header's own comment); this guard bites ONLY a caller who
// deliberately raises RenderTargetConfig::maxExtent past real hardware (R7 in the plan's risk table —
// exactly what a not-renderable-path test exercises). WITHOUT this pre-check, an over-ceiling
// createTexture() does not fail gracefully on Metal: the driver's own texture-descriptor validation
// calls std::abort() from inside SDL_CreateGPUTexture, before this RHI's "invalid handle" error model
// can ever run — verified empirically on this device and confirmed unconditional (independent of
// DeviceDesc::enableDebugLayer and the MTL_DEBUG_LAYER environment variable) against Apple's own
// developer forums. Checking here is what keeps create()/resize()'s documented "nullopt/false +
// ERROR, never a crash" contract actually true on every backend.
constexpr std::uint32_t HARDWARE_SAFE_TEXTURE_DIMENSION = 16384;

}  // namespace

rhi::Extent2D nextTargetExtent(rhi::Extent2D requested, rhi::Extent2D current, std::uint32_t quantum,
                               std::uint32_t maxExtent) noexcept {
    return {axisNextExtent(requested.width, current.width, quantum, maxExtent),
            axisNextExtent(requested.height, current.height, quantum, maxExtent)};
}

// --- RenderTarget -------------------------------------------------------------------------------

RenderTarget::RenderTarget(rhi::Device* deviceIn, const RenderTargetConfig& config,
                           rhi::TextureFormat depthFormat) noexcept
    : device(deviceIn), cfg(config), depthFormatValue(depthFormat) {}

RenderTarget::RenderTarget(RenderTarget&& other) noexcept
    : device(other.device),
      cfg(other.cfg),
      depthFormatValue(other.depthFormatValue),
      color(other.color),
      depth(other.depth),
      allocExtent(other.allocExtent),
      drawRect(other.drawRect) {
    other.device = nullptr;
    other.color = {};
    other.depth = {};
    other.allocExtent = {};
    other.drawRect = {};
}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept {
    if (this != &other) {
        destroyAll();
        device = other.device;
        cfg = other.cfg;
        depthFormatValue = other.depthFormatValue;
        color = other.color;
        depth = other.depth;
        allocExtent = other.allocExtent;
        drawRect = other.drawRect;
        other.device = nullptr;
        other.color = {};
        other.depth = {};
        other.allocExtent = {};
        other.drawRect = {};
    }
    return *this;
}

RenderTarget::~RenderTarget() { destroyAll(); }

void RenderTarget::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (color.valid()) {
        device->destroyTexture(color);
    }
    if (depth.valid()) {
        device->destroyTexture(depth);
    }
    device = nullptr;
    color = {};
    depth = {};
    allocExtent = {};
    drawRect = {};
}

std::optional<RenderTarget> RenderTarget::create(rhi::Device& device, rhi::Extent2D requested,
                                                 const RenderTargetConfig& config) {
    if (config.colorFormat == rhi::TextureFormat::Invalid || rhi::isDepthFormat(config.colorFormat)) {
        AERO_LOG_ERROR("render::RenderTarget::create — colorFormat must be a non-depth, non-Invalid format");
        return std::nullopt;
    }
    if (config.maxExtent == 0) {
        AERO_LOG_ERROR("render::RenderTarget::create — maxExtent must be > 0");
        return std::nullopt;
    }
    if (!device.supportsTextureFormat(config.colorFormat,
                                      rhi::TextureUsage::Sampler | rhi::TextureUsage::ColorTarget)) {
        AERO_LOG_ERROR(
            "render::RenderTarget::create — colorFormat does not support Sampler|ColorTarget on this device");
        return std::nullopt;
    }

    rhi::TextureFormat depthFormat = rhi::TextureFormat::Invalid;
    if (config.depth) {
        for (const rhi::TextureFormat f :
             {rhi::TextureFormat::D32Float, rhi::TextureFormat::D24Unorm, rhi::TextureFormat::D16Unorm}) {
            if (device.supportsTextureFormat(f, rhi::TextureUsage::DepthStencilTarget)) {
                depthFormat = f;
                break;
            }
        }
        if (depthFormat == rhi::TextureFormat::Invalid) {
            AERO_LOG_ERROR("render::RenderTarget::create — no supported depth format");
            return std::nullopt;
        }
    }

    RenderTarget target{&device, config, depthFormat};
    const rhi::Extent2D want = nextTargetExtent(requested, {0, 0}, config.quantum, config.maxExtent);
    if (!target.allocate(want)) {
        return std::nullopt;  // allocate() already destroyed anything it partially created
    }

    const rhi::Extent2D clamped{clampAxis(requested.width, config.maxExtent),
                                clampAxis(requested.height, config.maxExtent)};
    if (clamped.width != requested.width || clamped.height != requested.height) {
        const std::uint32_t maxExtent = config.maxExtent;
        AERO_LOG_WARN("render::RenderTarget::create — requested extent clamped to maxExtent ({}x{})", maxExtent,
                      maxExtent);
    }
    target.drawRect = clamped;
    return target;
}

bool RenderTarget::allocate(rhi::Extent2D newAllocExtent) {
    AERO_PROFILE_ZONE;
    // Destroy the previous pair FIRST (F9 makes that safe even mid-frame: the backend defers the
    // actual GPU release until in-flight frames finish).
    if (color.valid()) {
        device->destroyTexture(color);
        color = {};
    }
    if (depth.valid()) {
        device->destroyTexture(depth);
        depth = {};
    }
    allocExtent = {};

    if (newAllocExtent.width > HARDWARE_SAFE_TEXTURE_DIMENSION ||
        newAllocExtent.height > HARDWARE_SAFE_TEXTURE_DIMENSION) {
        AERO_LOG_ERROR("render::RenderTarget::allocate — {}x{} exceeds the real hardware ceiling ({})",
                       newAllocExtent.width, newAllocExtent.height, HARDWARE_SAFE_TEXTURE_DIMENSION);
        return false;
    }

    color = device->createTexture({.format = cfg.colorFormat,
                                   .usage = rhi::TextureUsage::Sampler | rhi::TextureUsage::ColorTarget,
                                   .width = newAllocExtent.width,
                                   .height = newAllocExtent.height});
    if (!color.valid()) {
        AERO_LOG_ERROR("render::RenderTarget::allocate — color texture creation failed");
        return false;
    }
    device->setDebugName(color, "aero.rendertarget.color");

    if (depthFormatValue != rhi::TextureFormat::Invalid) {
        depth = device->createTexture({.format = depthFormatValue,
                                       .usage = rhi::TextureUsage::DepthStencilTarget,
                                       .width = newAllocExtent.width,
                                       .height = newAllocExtent.height});
        if (!depth.valid()) {
            AERO_LOG_ERROR("render::RenderTarget::allocate — depth texture creation failed");
            device->destroyTexture(color);
            color = {};
            return false;
        }
        device->setDebugName(depth, "aero.rendertarget.depth");
    }

    allocExtent = newAllocExtent;
    return true;
}

bool RenderTarget::resize(rhi::Extent2D requested) {
    const rhi::Extent2D clamped{clampAxis(requested.width, cfg.maxExtent), clampAxis(requested.height, cfg.maxExtent)};
    const rhi::Extent2D want = nextTargetExtent(requested, allocExtent, cfg.quantum, cfg.maxExtent);
    bool ok = true;
    if (want != allocExtent) {
        ok = allocate(want);
    }
    drawRect = clamped;
    return ok;
}

std::optional<Frame> RenderTarget::beginFrame(const rhi::Color& clearColor) {
    if (allocExtent.width == 0 || allocExtent.height == 0) {
        return std::nullopt;  // not renderable — resize()'s false already told the caller
    }
    AERO_PROFILE_ZONE;
    const rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
    if (!cmd.valid()) {
        AERO_LOG_ERROR("render::RenderTarget::beginFrame — acquireCommandBuffer failed");
        return std::nullopt;
    }

    const rhi::ColorAttachment colorAttachment{.texture = color, .clearColor = clearColor};  // Clear -> Store (default)
    rhi::RenderPassDesc desc{.colorAttachments = {&colorAttachment, 1}};
    if (depth.valid()) {
        desc.depthStencil = rhi::DepthStencilAttachment{.texture = depth};  // Clear -> DontCare, clearDepth 1.0
    }

    const rhi::RenderPassHandle pass = device->beginRenderPass(cmd, desc);
    if (!pass.valid()) {
        AERO_LOG_ERROR("render::RenderTarget::beginFrame — beginRenderPass failed");
        device->cancel(cmd);  // nothing was acquired from a swapchain: cancel is legal here
        return std::nullopt;
    }

    device->setViewport(pass, {.x = 0.0F,
                               .y = 0.0F,
                               .width = static_cast<float>(drawRect.width),
                               .height = static_cast<float>(drawRect.height)});
    device->setScissor(pass, {.x = 0, .y = 0, .width = drawRect.width, .height = drawRect.height});

    return Frame{device, cmd, pass, drawRect};
}

bool RenderTarget::endFrame(Frame frame) {
    // Renderer::endFrame's body verbatim, minus "presents": consumes `frame`, rejects an inert one.
    if (!frame.live) {
        AERO_LOG_ERROR("render::RenderTarget::endFrame — frame is inert (moved-from or already ended)");
        return false;
    }
    AERO_PROFILE_ZONE;
    frame.device->endRenderPass(frame.renderPass);    // operate through the frame's own device (C-note)
    const bool ok = frame.device->submit(frame.cmd);  // does NOT present — no swapchain was involved
    frame.live = false;                               // consumed — the by-value parameter's dtor is now a no-op
    return ok;
}

rhi::TextureHandle RenderTarget::colorTexture() const noexcept { return color; }
rhi::TextureFormat RenderTarget::colorFormat() const noexcept { return cfg.colorFormat; }
rhi::TextureFormat RenderTarget::depthFormat() const noexcept { return depthFormatValue; }
rhi::Extent2D RenderTarget::drawExtent() const noexcept { return drawRect; }
rhi::Extent2D RenderTarget::textureExtent() const noexcept { return allocExtent; }

}  // namespace engine::render
