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
    : device_(device), cmd_(cmd), pass_(pass), extent_(extent), live_(true) {}

Frame::Frame(Frame&& other) noexcept
    : device_(other.device_), cmd_(other.cmd_), pass_(other.pass_), extent_(other.extent_), live_(other.live_) {
    other.device_ = nullptr;
    other.cmd_ = {};
    other.pass_ = {};
    other.live_ = false;
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        disposeIfLive();  // never leak the frame we are overwriting
        device_ = other.device_;
        cmd_ = other.cmd_;
        pass_ = other.pass_;
        extent_ = other.extent_;
        live_ = other.live_;
        other.device_ = nullptr;
        other.cmd_ = {};
        other.pass_ = {};
        other.live_ = false;
    }
    return *this;
}

Frame::~Frame() { disposeIfLive(); }

void Frame::disposeIfLive() noexcept {
    if (live_ && device_ != nullptr && cmd_.valid()) {
        AERO_LOG_WARN("render::Frame dropped without endFrame — disposing the frame");
        device_->endRenderPass(pass_);  // logged no-op if already ended; submit would force-end anyway
        device_->submit(cmd_);          // dispose the swapchain image so it never starves (E5)
    }
    live_ = false;
}

rhi::Extent2D Frame::extent() const noexcept { return extent_; }
rhi::RenderPassHandle Frame::pass() const noexcept { return pass_; }
rhi::CommandBufferHandle Frame::commandBuffer() const noexcept { return cmd_; }

// --- Renderer ---------------------------------------------------------------------------------

Renderer::Renderer(rhi::Device* device, rhi::SwapchainHandle swapchain) noexcept
    : device_(device), swapchain_(swapchain) {}

Renderer::Renderer(Renderer&& other) noexcept : device_(other.device_), swapchain_(other.swapchain_) {
    other.device_ = nullptr;
    other.swapchain_ = {};
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        if (device_ != nullptr && swapchain_.valid()) {
            device_->destroySwapchain(swapchain_);  // never orphan our own swapchain (0.3.1 lesson)
        }
        device_ = other.device_;
        swapchain_ = other.swapchain_;
        other.device_ = nullptr;
        other.swapchain_ = {};
    }
    return *this;
}

Renderer::~Renderer() {
    if (device_ != nullptr && swapchain_.valid()) {
        device_->destroySwapchain(swapchain_);
    }
}

std::optional<Renderer> Renderer::create(rhi::Device& device, const platform::Window& window,
                                         const RendererConfig& config) {
    const rhi::SwapchainHandle swapchain = device.createSwapchain(window, {.presentMode = config.presentMode});
    if (!swapchain.valid()) {
        AERO_LOG_ERROR("render::Renderer::create — swapchain creation failed");
        return std::nullopt;
    }
    return Renderer{&device, swapchain};
}

std::optional<Frame> Renderer::beginFrame(const rhi::Color& clearColor) {
    AERO_PROFILE_ZONE;
    const rhi::CommandBufferHandle cmd = device_->acquireCommandBuffer();
    if (!cmd.valid()) {
        AERO_LOG_ERROR("render::beginFrame — acquireCommandBuffer failed");
        return std::nullopt;
    }
    const std::optional<rhi::SwapchainTexture> acquired = device_->acquireSwapchainTexture(cmd, swapchain_);
    if (!acquired.has_value()) {
        device_->cancel(cmd);  // no acquire happened -> cancel is legal; NOT an error (minimized)
        return std::nullopt;
    }
    const rhi::ColorAttachment color{.texture = acquired->texture, .clearColor = clearColor};
    const rhi::RenderPassHandle pass = device_->beginRenderPass(cmd, {.colorAttachments = {&color, 1}});
    if (!pass.valid()) {
        AERO_LOG_ERROR("render::beginFrame — beginRenderPass failed");
        device_->submit(cmd);  // image already acquired: submit to dispose (cancel is illegal now)
        return std::nullopt;
    }
    return Frame{device_, cmd, pass, acquired->extent};
}

bool Renderer::endFrame(Frame frame) {
    if (!frame.live_) {
        AERO_LOG_ERROR("render::endFrame — frame is inert (moved-from or already ended)");
        return false;
    }
    AERO_PROFILE_ZONE;
    frame.device_->endRenderPass(frame.pass_);          // operate through the frame's own device_ (C-note)
    const bool ok = frame.device_->submit(frame.cmd_);  // presents the cleared image
    frame.live_ = false;                                // consumed — the by-value parameter's dtor is now a no-op
    return ok;
}

rhi::TextureFormat Renderer::colorFormat() const noexcept {
    return device_ != nullptr ? device_->swapchainFormat(swapchain_) : rhi::TextureFormat::Invalid;
}

}  // namespace engine::render
