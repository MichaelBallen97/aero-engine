// Aero Engine — Phase 0 clear pass (task 0.5.1). Opens a window + GPU device + engine::render::Renderer
// and clears the screen to a gently pulsing color every frame, vsync-paced, until closed. FIRST PIXELS.
// Built in CI (compile-proof, 3 OSes); NOT run there (needs a real GPU + visible window) — run it by
// hand to SEE the window, drag-resize it (stays stable), minimize/restore it (no busy-spin), Esc quits.
//
// DECLARATION ORDER IS LOAD-BEARING (C3): ctx (outer) -> window -> device -> renderer (inner), so RAII
// destroys the swapchain (renderer) before the device/window, then the window before the context.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

int main() {
    engine::platform::Context ctx;  // real driver (headless defaults to false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed — exiting");
        return 1;
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 0 Clear", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_LOG_CRITICAL("no GPU device — exiting");
        return 1;
    }
    std::optional<engine::render::Renderer> renderer = engine::render::Renderer::create(*device, *window);
    if (!renderer) {
        AERO_LOG_CRITICAL("renderer creation failed — exiting");
        return 1;
    }
    AERO_LOG_INFO("clear pass ready — swapchain format {}", engine::rhi::toString(renderer->colorFormat()));

    engine::FrameClock clock;
    bool running = true;
    while (running) {
        ctx.newFrame();
        engine::platform::Event ev;
        while (ctx.pollEvent(ev)) {
            switch (ev.type) {
                case engine::platform::EventType::Quit:
                case engine::platform::EventType::WindowClose:
                    running = false;
                    break;
                case engine::platform::EventType::WindowPixelSizeChanged:
                    AERO_LOG_INFO("framebuffer resized to {}x{}", ev.size.width, ev.size.height);
                    break;  // the RHI swapchain auto-tracks; nothing else to do
                default:
                    break;
            }
        }
        if (ctx.input().keyDown(engine::platform::Key::Escape)) {
            running = false;
        }

        clock.tick();
        const auto t = static_cast<float>(clock.totalSeconds());
        const engine::rhi::Color sky{0.5F + 0.5F * std::sin(t), 0.35F, 0.6F, 1.0F};  // gentle pulse

        if (std::optional<engine::render::Frame> frame = renderer->beginFrame(sky)) {
            // task 0.5.1: nothing to record — the pass already cleared. task 0.5.2 draws here.
            renderer->endFrame(std::move(*frame));  // present
        } else {
            // Not presentable (minimized): beginFrame returns immediately (no vsync wait), so idle a
            // beat to avoid pegging a core until the window is restored.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        AERO_PROFILE_FRAME_MARK;
    }
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    return 0;
}
