// Aero Engine — render Renderer implementation (task 0.5.1). SDL-free: only the rhi::Device API +
// engine types (no <aero/platform/...> needed — create() forwards the Window reference straight into
// device.createSwapchain, which takes the same forward-declared type). See renderer.hpp for contracts.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/descriptors.hpp>  // rhi::{ColorAttachment, RenderPassDesc}
#include <aero/rhi/device.hpp>       // rhi::Device (full), rhi::SwapchainTexture

namespace engine::render {

// --- Frame ------------------------------------------------------------------------------------

Frame::Frame(rhi::Device* device, rhi::CommandBufferHandle cmd, rhi::RenderPassHandle pass,
             rhi::Extent2D extent) noexcept
    : device(device), cmd(cmd), renderPass(pass), pixelExtent(extent), live(true) {}

Frame::Frame(Frame&& other) noexcept
    : device(other.device),
      cmd(other.cmd),
      renderPass(other.renderPass),
      pixelExtent(other.pixelExtent),
      live(other.live) {
    other.device = nullptr;
    other.cmd = {};
    other.renderPass = {};
    other.live = false;
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        disposeIfLive();  // never leak the frame we are overwriting
        device = other.device;
        cmd = other.cmd;
        renderPass = other.renderPass;
        pixelExtent = other.pixelExtent;
        live = other.live;
        other.device = nullptr;
        other.cmd = {};
        other.renderPass = {};
        other.live = false;
    }
    return *this;
}

Frame::~Frame() { disposeIfLive(); }

void Frame::disposeIfLive() noexcept {
    if (live && device != nullptr && cmd.valid()) {
        AERO_LOG_WARN("render::Frame dropped without endFrame — disposing the frame");
        device->endRenderPass(renderPass);  // logged no-op if already ended; submit would force-end anyway
        device->submit(cmd);                // dispose the swapchain image so it never starves (E5)
    }
    live = false;
}

rhi::Extent2D Frame::extent() const noexcept { return pixelExtent; }
rhi::RenderPassHandle Frame::pass() const noexcept { return renderPass; }
rhi::CommandBufferHandle Frame::commandBuffer() const noexcept { return cmd; }

// --- Renderer ---------------------------------------------------------------------------------

Renderer::Renderer(rhi::Device* device, rhi::SwapchainHandle swapchain, rhi::TextureFormat depthFormat) noexcept
    : device(device), swapchain(swapchain), depthFormatValue(depthFormat) {}

Renderer::Renderer(Renderer&& other) noexcept
    : device(other.device),
      swapchain(other.swapchain),
      depthFormatValue(other.depthFormatValue),
      depthTexture(other.depthTexture),
      depthExtent(other.depthExtent) {
    other.device = nullptr;
    other.swapchain = {};
    other.depthTexture = {};
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        if (device != nullptr) {
            if (depthTexture.valid()) {
                device->destroyTexture(depthTexture);  // never orphan our own depth texture
            }
            if (swapchain.valid()) {
                device->destroySwapchain(swapchain);  // never orphan our own swapchain (0.3.1 lesson)
            }
        }
        device = other.device;
        swapchain = other.swapchain;
        depthFormatValue = other.depthFormatValue;
        depthTexture = other.depthTexture;
        depthExtent = other.depthExtent;
        other.device = nullptr;
        other.swapchain = {};
        other.depthTexture = {};
    }
    return *this;
}

Renderer::~Renderer() {
    if (device != nullptr) {
        if (depthTexture.valid()) {
            device->destroyTexture(depthTexture);
        }
        if (swapchain.valid()) {
            device->destroySwapchain(swapchain);
        }
    }
}

std::optional<Renderer> Renderer::create(rhi::Device& device, const platform::Window& window,
                                         const RendererConfig& config) {
    const rhi::SwapchainHandle swapchain = device.createSwapchain(window, {.presentMode = config.presentMode});
    if (!swapchain.valid()) {
        AERO_LOG_ERROR("render::Renderer::create — swapchain creation failed");
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
            AERO_LOG_ERROR("render::Renderer::create — no supported depth format");
            device.destroySwapchain(swapchain);  // don't leak the swapchain we just made
            return std::nullopt;
        }
    }

    return Renderer{&device, swapchain, depthFormat};
}

std::optional<Frame> Renderer::beginFrame(const rhi::Color& clearColor) {
    AERO_PROFILE_ZONE;
    const rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
    if (!cmd.valid()) {
        AERO_LOG_ERROR("render::beginFrame — acquireCommandBuffer failed");
        return std::nullopt;
    }
    const std::optional<rhi::SwapchainTexture> acquired = device->acquireSwapchainTexture(cmd, swapchain);
    if (!acquired.has_value()) {
        device->cancel(cmd);  // no acquire happened -> cancel is legal; NOT an error (minimized)
        return std::nullopt;
    }

    const rhi::ColorAttachment color{.texture = acquired->texture, .clearColor = clearColor};
    rhi::RenderPassDesc desc{.colorAttachments = {&color, 1}};
    if (depthFormatValue != rhi::TextureFormat::Invalid) {
        if (!depthTexture.valid() || depthExtent != acquired->extent) {
            if (depthTexture.valid()) {
                device->destroyTexture(depthTexture);  // resize: safe (the backend defers the free)
            }
            depthTexture = device->createTexture({.format = depthFormatValue,
                                                  .usage = rhi::TextureUsage::DepthStencilTarget,
                                                  .width = acquired->extent.width,
                                                  .height = acquired->extent.height});
            depthExtent = acquired->extent;
            if (!depthTexture.valid()) {
                AERO_LOG_ERROR("render::beginFrame — depth texture creation failed");
                device->submit(cmd);  // image already acquired: submit to dispose
                return std::nullopt;
            }
        }
        desc.depthStencil = rhi::DepthStencilAttachment{.texture = depthTexture};  // Clear -> DontCare, clearDepth 1.0
    }

    const rhi::RenderPassHandle pass = device->beginRenderPass(cmd, desc);
    if (!pass.valid()) {
        AERO_LOG_ERROR("render::beginFrame — beginRenderPass failed");
        device->submit(cmd);  // image already acquired: submit to dispose (cancel is illegal now)
        return std::nullopt;
    }
    return Frame{device, cmd, pass, acquired->extent};
}

bool Renderer::endFrame(Frame frame) {
    if (!frame.live) {
        AERO_LOG_ERROR("render::endFrame — frame is inert (moved-from or already ended)");
        return false;
    }
    AERO_PROFILE_ZONE;
    frame.device->endRenderPass(frame.renderPass);    // operate through the frame's own device (C-note)
    const bool ok = frame.device->submit(frame.cmd);  // presents the cleared image
    frame.live = false;                               // consumed — the by-value parameter's dtor is now a no-op
    return ok;
}

rhi::TextureFormat Renderer::colorFormat() const noexcept {
    return device != nullptr ? device->swapchainFormat(swapchain) : rhi::TextureFormat::Invalid;
}

rhi::TextureFormat Renderer::depthFormat() const noexcept { return depthFormatValue; }

}  // namespace engine::render
