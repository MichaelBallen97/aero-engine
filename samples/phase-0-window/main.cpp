// Aero Engine — Phase 0 window smoke (task 0.3.1). Opens a real window and pumps events until it is
// closed. NOT run in CI (headless); build-proofed there, run by a human to SEE a window. Note the
// boundary at work: this app never touches SDL — timing is core's FrameClock, the idle sleep is the
// standard library. (No renderer yet; the window shows undefined contents until 0.5.)

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/platform/platform.hpp>

#include <chrono>
#include <optional>
#include <thread>

int main() {
    engine::platform::Context ctx;  // real driver (headless defaults to false)
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed — exiting");
        return 1;
    }
    std::optional<engine::platform::Window> maybeWindow =
        ctx.createWindow({.title = "Aero — Phase 0 Window", .width = 1280, .height = 720});
    if (!maybeWindow) {
        return 1;
    }
    const engine::platform::Window& window = *maybeWindow;  // guard above proves this is engaged
    AERO_LOG_INFO("window {}x{} (framebuffer {}x{})", window.size().width, window.size().height,
                  window.pixelSize().width, window.pixelSize().height);

    engine::FrameClock clock;
    bool running = true;
    while (running) {
        engine::platform::Event ev;
        while (ctx.pollEvent(ev)) {
            switch (ev.type) {
                case engine::platform::EventType::Quit:
                case engine::platform::EventType::WindowClose:
                    running = false;
                    break;
                case engine::platform::EventType::WindowPixelSizeChanged:
                    AERO_LOG_INFO("framebuffer resized to {}x{}", ev.width, ev.height);
                    break;
                default:
                    break;
            }
        }
        clock.tick();
        AERO_PROFILE_FRAME_MARK;
        // No vsync/renderer yet: a tiny sleep keeps the smoke from pegging a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    return 0;
}
