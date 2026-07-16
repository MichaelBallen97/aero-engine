#pragma once
// Aero Engine — platform context (task 0.3.1): the SDL lifetime and the process-global event pump.
// Create EXACTLY ONE, on the MAIN thread (SDL_INIT_VIDEO must init there — SDL_init.h:81). RAII: the
// constructor initializes SDL's video/event subsystem, the destructor shuts it down. It is the
// factory for windows and the source of events. SDL never crosses this boundary (docs/04) — enforced
// by the lint grep and tests/platform_boundary_probe.cpp.
//
// Context holds no SDL type (SDL init and the event queue are process-global), so it needs no pimpl:
// its state is a bool and a std::thread::id.

#include <aero/platform/event.hpp>
#include <aero/platform/window.hpp>

#include <optional>
#include <thread>

namespace engine::platform {

struct ContextConfig {
    // Run with no display: selects SDL's "dummy" video driver (SDL_HINT_VIDEO_DRIVER=dummy) before
    // init, so windows can be created and the queue pumped on a headless machine. This is what the
    // unit tests and CI use. false uses the platform's real default driver (Cocoa/Win32/X11/Wayland).
    bool headless = false;
};

class Context {
public:
    explicit Context(const ContextConfig& config = {});
    ~Context();

    // Non-copyable AND non-movable: it owns global SDL init (SDL_Quit tears SDL down for the whole
    // process), so exactly one, pinned to its construction thread.
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    // False if SDL failed to initialize (rare — e.g. no usable video driver). A false Context is
    // inert: createWindow returns nullopt and pollEvent returns false. No exception is thrown.
    [[nodiscard]] bool valid() const noexcept { return initialized; }

    // Create a window. Returns nullopt (and logs the SDL reason at ERROR) on failure. Must be called
    // on the thread that constructed this Context (the main thread); asserted in debug.
    [[nodiscard]] std::optional<Window> createWindow(const WindowConfig& config = {});

    // Pull the next translated event, draining and DISCARDING any backend event 0.3.1 does not model
    // (input lands in 0.3.2). Returns false when the queue is empty. Must be called on the Context's
    // thread; asserted in debug. Drive it as:
    //     platform::Event ev;
    //     while (ctx.pollEvent(ev)) { /* handle ev */ }
    bool pollEvent(Event& out);

private:
    bool initialized = false;
    std::thread::id ownerThread;  // the main thread; pollEvent/createWindow must run here
};

}  // namespace engine::platform
