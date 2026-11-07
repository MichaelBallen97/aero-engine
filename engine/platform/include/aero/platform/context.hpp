#pragma once
// Aero Engine — platform context (task 0.3.1): the SDL lifetime and the process-global event pump.
// Create EXACTLY ONE, on the MAIN thread (SDL_INIT_VIDEO must init there — SDL_init.h:81). RAII: the
// constructor initializes SDL's video/event subsystem, the destructor shuts it down. It is the
// factory for windows and the source of events. SDL never crosses this boundary (docs/04) — enforced
// by the lint grep and tests/platform_boundary_probe.cpp.
//
// Context holds no SDL type (SDL init and the event queue are process-global), so it needs no pimpl on the
// default path: its state is a bool, a thread id and, when the gamepad subsystem runs, a GamepadTable defined in
// src/platform.cpp behind a unique_ptr.

#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>
#include <aero/platform/window.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace engine::platform::internal {
struct RawEventAccessor;
}  // namespace engine::platform::internal

namespace engine::platform {

// Which joystick drivers the Context starts (task 4.4.3). Gamepads are opt-in: the subsystem costs about 15-20 ms
// at start-up and leaves framework threads behind, so a Context that does not read a pad pays nothing.
enum class GamepadDrivers : std::uint8_t {
    Off = 0,      // no gamepad subsystem: nothing connects, and the Context pays nothing for it (the default)
    System,       // SDL's default drivers: every real pad connects
    VirtualOnly,  // for headless tests and CI: every real device is ignored and the slow drivers are skipped, so
                  // only a virtual pad made through the backend connects. A shipped game never uses it.
};

struct ContextConfig {
    // Run with no display: selects SDL's "dummy" video driver (SDL_HINT_VIDEO_DRIVER=dummy) before
    // init, so windows can be created and the queue pumped on a headless machine. This is what the
    // unit tests and CI use. false uses the platform's real default driver (Cocoa/Win32/X11/Wayland).
    bool headless = false;
    GamepadDrivers gamepads = GamepadDrivers::Off;
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

    // Pull the next translated event, draining and DISCARDING any backend event still unmodeled (e.g.
    // text input, sensors; gamepad events are modeled when `gamepads` is not Off). Also folds keyboard,
    // mouse and gamepad events into input() as it translates them (tasks 0.3.2 and 4.4.3): a pad's connect,
    // disconnect, buttons and axes surface as the five gamepad EventTypes. Returns false when the queue is
    // empty. Must be called on the Context's thread; asserted
    // in debug. Drive it as:
    //     platform::Event ev;
    //     while (ctx.pollEvent(ev)) { /* handle ev */ }
    bool pollEvent(Event& out);

    // Advance input to a new frame: clears this-frame key/button edges, every gamepad slot's button
    // edges and the per-frame mouse motion/wheel deltas; held state (keyDown, held pad buttons, axes)
    // and the last mouse position persist. Call ONCE at the top of each frame, before the pollEvent
    // drain loop (see input.hpp). Inert Context: harmless.
    void newFrame() noexcept { inputState.newFrame(); }

    // The current input snapshot, updated by pollEvent as it translates events. Read-only to callers.
    [[nodiscard]] const InputState& input() const noexcept { return inputState; }

private:
    friend struct internal::RawEventAccessor;
    struct GamepadTable;                         // src/platform.cpp: the slots' backend handles and the waiting ids
    std::unique_ptr<GamepadTable> gamepadTable;  // null unless the gamepad subsystem started
    bool initialized = false;
    std::thread::id ownerThread;  // the main thread; pollEvent/createWindow must run here
    InputState inputState;        // folded by pollEvent (src/platform.cpp); frame-reset by newFrame()
    // Optional observer of the raw backend event stream (task 2.1.1). Void-based so no SDL type
    // reaches this public header; `sdlEvent` is a const SDL_Event*. Fired for EVERY event in
    // pollEvent, before translate/discard. Cleared to {nullptr,nullptr} by default.
    void (*rawEventSink)(void* user, const void* sdlEvent) = nullptr;
    void* rawEventSinkUser = nullptr;
};

// The folder a game reads its shipped data from: UTF-8, ending in a separator, empty when the OS cannot say. It is the
// executable's folder, and Contents/Resources inside a macOS bundle. Needs no Context, and the copy outlives SDL.
[[nodiscard]] std::string basePath();

}  // namespace engine::platform
