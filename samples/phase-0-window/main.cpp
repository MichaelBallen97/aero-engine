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

    // task 0.3.3 — open a silent audio device (real backend), completing Epic 0.3's DoD: this one app
    // now does window + events + input + audio. Non-fatal: a machine with no audio still gets a window.
    std::optional<engine::platform::AudioDevice> audio = engine::platform::AudioDevice::open();
    if (audio) {
        AERO_LOG_INFO("audio device opened: {} Hz, {} ch (silent)", audio->sampleRate(), audio->channels());
    } else {
        AERO_LOG_WARN("no audio device available — continuing without sound");
    }

    engine::FrameClock clock;
    bool running = true;
    while (running) {
        ctx.newFrame();  // task 0.3.2: reset per-frame input edges/deltas before draining
        engine::platform::Event ev;
        while (ctx.pollEvent(ev)) {
            switch (ev.type) {
                case engine::platform::EventType::Quit:
                case engine::platform::EventType::WindowClose:
                    running = false;
                    break;
                case engine::platform::EventType::WindowPixelSizeChanged:
                    AERO_LOG_INFO("framebuffer resized to {}x{}", ev.size.width, ev.size.height);
                    break;
                case engine::platform::EventType::KeyDown:
                    AERO_LOG_INFO("key down: code {} (repeat={})", static_cast<int>(ev.key.code), ev.key.repeat);
                    break;
                case engine::platform::EventType::MouseButtonDown:
                    AERO_LOG_INFO("click button {} at {:.0f},{:.0f}", static_cast<int>(ev.mouseButton.button),
                                  ev.mouseButton.x, ev.mouseButton.y);
                    break;
                case engine::platform::EventType::MouseWheel:
                    AERO_LOG_INFO("wheel {:.1f},{:.1f}", ev.mouseWheel.x, ev.mouseWheel.y);
                    break;
                default:
                    break;
            }
        }
        // Polled-state style: Escape quits. (Held/edge state is available via ctx.input() too.)
        if (ctx.input().keyDown(engine::platform::Key::Escape)) {
            running = false;
        }
        clock.tick();
        AERO_PROFILE_FRAME_MARK;
        // No vsync/renderer yet: a tiny sleep keeps the smoke from pegging a core.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    return 0;
}
