#pragma once
// Aero Engine — render Renderer (task 0.5.1). The smallest real renderer: it owns one window's
// swapchain and drives the per-frame begin -> clear -> present cycle over the RHI. It DRAWS NOTHING
// in v0; the open render pass is exposed on Frame so task 0.5.2 (textured cube) records draws into it
// with no change to this API. No third-party type appears here (rule #3) — every member is an engine
// type, so there is no pimpl (unlike platform/rhi, which bury SDL/miniaudio).
//
// LIFETIME CONTRACTS:
//   * The rhi::Device and the platform::Window passed to create() MUST outlive the Renderer.
//   * ~Renderer destroys the swapchain; declare the Renderer AFTER (inner to) the Device and Window
//     so RAII tears the swapchain down first (see samples/phase-0-clear).
//   * A Frame must NOT outlive its Renderer, and each beginFrame() must be matched by exactly one
//     endFrame() (or the Frame is dropped and disposed with a WARN).
//
// ERROR MODEL (docs/04, mirrors rhi): nothing throws. create() returns nullopt (+ ERROR) on failure;
// beginFrame() returns nullopt on a non-presentable window (minimized) WITHOUT logging an error
// (that is normal — skip the frame); endFrame() returns false (+ log) on backend submit failure.

#include <aero/rhi/format.hpp>   // rhi::TextureFormat (colorFormat)
#include <aero/rhi/handles.hpp>  // rhi::{RenderPassHandle, CommandBufferHandle, SwapchainHandle}
#include <aero/rhi/types.hpp>    // rhi::{Color, Extent2D, PresentMode}

#include <optional>

namespace engine::rhi {
class Device;  // forward-declared: render's one heavy rhi type; renderer.cpp includes device.hpp
}  // namespace engine::rhi

namespace engine::platform {
class Window;  // forward-declared: named only as a create() by-reference parameter (like device.hpp)
}  // namespace engine::platform

namespace engine::render {

// How to create a Renderer. presentMode is baked into the swapchain at create(): a mode the platform
// cannot honor makes create() FAIL (nullopt), never a silent downgrade (rhi swapchain contract).
struct RendererConfig {
    rhi::PresentMode presentMode = rhi::PresentMode::Vsync;  // 0.5.x gate mode
};

// One in-flight frame: a command buffer with a swapchain image acquired onto it and an OPEN render
// pass that has already cleared the color target. Move-only. Passed to Renderer::endFrame to present;
// if instead it is dropped, ~Frame force-ends the pass and submits (disposing the swapchain image so
// it never starves) and logs one WARN — a bug in the caller, not a feature.
class Frame {
public:
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    // This frame's REAL swapchain pixel size — authoritative during a resize (not Window::pixelSize()).
    [[nodiscard]] rhi::Extent2D extent() const noexcept;
    // The open render pass. In v0 nothing is recorded into it (it only cleared). Task 0.5.2 binds a
    // pipeline and records drawIndexed() here, between beginFrame() and endFrame().
    [[nodiscard]] rhi::RenderPassHandle pass() const noexcept;
    // The frame's command buffer — for pushVertex/FragmentUniforms (task 0.5.2's MVP). Invalidated by
    // endFrame().
    [[nodiscard]] rhi::CommandBufferHandle commandBuffer() const noexcept;

private:
    friend class Renderer;  // the only maker of Frames; reads these members in endFrame()
    Frame(rhi::Device* device, rhi::CommandBufferHandle cmd, rhi::RenderPassHandle pass, rhi::Extent2D extent) noexcept;
    void disposeIfLive() noexcept;  // dtor + move-assign share this (C1)

    // Members are plain camelBack, no trailing underscore (docs/04 / .clang-tidy MemberCase); where a
    // name would collide with an accessor, the member takes a distinct name (pass()->renderPass,
    // extent()->pixelExtent), matching FrameClock (frameCount()->frames).
    rhi::Device* device = nullptr;   // non-owning; the Device outlives the Frame (contract)
    rhi::CommandBufferHandle cmd{};  // invalid on a moved-from / already-ended frame
    rhi::RenderPassHandle renderPass{};
    rhi::Extent2D pixelExtent{};
    bool live = false;  // true between construction and endFrame(); gates disposeIfLive()
};

class Renderer {
public:
    // Claim `window` for presentation and build the renderer. nullopt (+ ERROR) if the swapchain
    // cannot be created (e.g. an unsupported presentMode, or the window is already claimed). The
    // Device and Window must outlive the returned Renderer.
    [[nodiscard]] static std::optional<Renderer> create(rhi::Device& device, const platform::Window& window,
                                                        const RendererConfig& config = {});

    ~Renderer();  // destroys the swapchain (no-op if moved-from)
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Begin a frame: acquire a command buffer + swapchain image and open a render pass that CLEARS
    // the color target to `clearColor` (LoadOp::Clear -> StoreOp::Store). Returns nullopt when the
    // window cannot present right now (minimized) — skip this frame; this is NOT an error. On a real
    // backend failure the ERROR is logged and nullopt returned.
    [[nodiscard]] std::optional<Frame> beginFrame(const rhi::Color& clearColor);

    // Close `frame`'s render pass and submit its command buffer — this PRESENTS the cleared image.
    // Consumes `frame` (a moved-from/already-ended frame is rejected: false + ERROR). Returns false
    // (+ log) on backend submit failure; the frame's handles are invalidated regardless.
    bool endFrame(Frame frame);

    // The swapchain's color format — what a task-0.5.2 pipeline's ColorTargetDesc.format must equal.
    // One of the four SDR 8-bit formats. Invalid on a moved-from Renderer.
    [[nodiscard]] rhi::TextureFormat colorFormat() const noexcept;

private:
    Renderer(rhi::Device* device, rhi::SwapchainHandle swapchain) noexcept;

    rhi::Device* device = nullptr;     // non-owning; outlives the Renderer (contract)
    rhi::SwapchainHandle swapchain{};  // owned: ~Renderer destroys it. No per-frame state lives here.
};

}  // namespace engine::render
