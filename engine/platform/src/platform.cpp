#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/internal/native_window.hpp>
#include <aero/platform/window.hpp>

#include <SDL3/SDL.h>  // the ONLY place SDL is included in engine/ (boundary rule)

#include <atomic>
#include <cassert>
#include <optional>
#include <thread>
#include <utility>

namespace engine::platform {
namespace {

// One-per-process guard (D3). SDL_Quit is a GLOBAL teardown, so a second Context whose dtor runs
// first would pull SDL out from under the first. The ASSERT is debug-only; the counter itself is
// maintained in every config so it stays balanced — one relaxed atomic per construct/destruct.
std::atomic<int> liveContexts{0};

// These pin the engine-side Key contiguity that fromScancode's offset math relies on: reorder Key and
// the build fails HERE, rather than silently mis-mapping a run of keys.
static_assert(static_cast<int>(Key::Z) - static_cast<int>(Key::A) == 25);
static_assert(static_cast<int>(Key::Num9) - static_cast<int>(Key::Num1) == 8);
static_assert(static_cast<int>(Key::F12) - static_cast<int>(Key::F1) == 11);
static_assert(static_cast<int>(Key::Kp9) - static_cast<int>(Key::Kp1) == 8);

// SDL physical scancode -> engine Key (D1/D9). SDL's contiguous runs map by offset; the irregular keys
// are explicit. An unmapped scancode returns Key::Unknown, and translate() drops that event.
Key fromScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (sc - SDL_SCANCODE_A));
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {  // SDL orders 1..9 then 0
        return static_cast<Key>(static_cast<int>(Key::Num1) + (sc - SDL_SCANCODE_1));
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (sc - SDL_SCANCODE_F1));
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {  // SDL orders KP_1..KP_9 then KP_0
        return static_cast<Key>(static_cast<int>(Key::Kp1) + (sc - SDL_SCANCODE_KP_1));
    }
    switch (sc) {
        case SDL_SCANCODE_0:
            return Key::Num0;
        case SDL_SCANCODE_KP_0:
            return Key::Kp0;
        case SDL_SCANCODE_RETURN:
            return Key::Enter;
        case SDL_SCANCODE_ESCAPE:
            return Key::Escape;
        case SDL_SCANCODE_BACKSPACE:
            return Key::Backspace;
        case SDL_SCANCODE_TAB:
            return Key::Tab;
        case SDL_SCANCODE_SPACE:
            return Key::Space;
        case SDL_SCANCODE_DELETE:
            return Key::Delete;
        case SDL_SCANCODE_INSERT:
            return Key::Insert;
        case SDL_SCANCODE_HOME:
            return Key::Home;
        case SDL_SCANCODE_END:
            return Key::End;
        case SDL_SCANCODE_PAGEUP:
            return Key::PageUp;
        case SDL_SCANCODE_PAGEDOWN:
            return Key::PageDown;
        case SDL_SCANCODE_LEFT:
            return Key::Left;
        case SDL_SCANCODE_RIGHT:
            return Key::Right;
        case SDL_SCANCODE_UP:
            return Key::Up;
        case SDL_SCANCODE_DOWN:
            return Key::Down;
        case SDL_SCANCODE_LSHIFT:
            return Key::LeftShift;
        case SDL_SCANCODE_RSHIFT:
            return Key::RightShift;
        case SDL_SCANCODE_LCTRL:
            return Key::LeftCtrl;
        case SDL_SCANCODE_RCTRL:
            return Key::RightCtrl;
        case SDL_SCANCODE_LALT:
            return Key::LeftAlt;
        case SDL_SCANCODE_RALT:
            return Key::RightAlt;
        case SDL_SCANCODE_LGUI:
            return Key::LeftGui;
        case SDL_SCANCODE_RGUI:
            return Key::RightGui;
        case SDL_SCANCODE_CAPSLOCK:
            return Key::CapsLock;
        case SDL_SCANCODE_MINUS:
            return Key::Minus;
        case SDL_SCANCODE_EQUALS:
            return Key::Equals;
        case SDL_SCANCODE_LEFTBRACKET:
            return Key::LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET:
            return Key::RightBracket;
        case SDL_SCANCODE_BACKSLASH:
            return Key::Backslash;
        case SDL_SCANCODE_SEMICOLON:
            return Key::Semicolon;
        case SDL_SCANCODE_APOSTROPHE:
            return Key::Apostrophe;
        case SDL_SCANCODE_GRAVE:
            return Key::Grave;
        case SDL_SCANCODE_COMMA:
            return Key::Comma;
        case SDL_SCANCODE_PERIOD:
            return Key::Period;
        case SDL_SCANCODE_SLASH:
            return Key::Slash;
        case SDL_SCANCODE_KP_ENTER:
            return Key::KpEnter;
        case SDL_SCANCODE_KP_PLUS:
            return Key::KpPlus;
        case SDL_SCANCODE_KP_MINUS:
            return Key::KpMinus;
        case SDL_SCANCODE_KP_MULTIPLY:
            return Key::KpMultiply;
        case SDL_SCANCODE_KP_DIVIDE:
            return Key::KpDivide;
        case SDL_SCANCODE_KP_PERIOD:
            return Key::KpPeriod;
        case SDL_SCANCODE_PRINTSCREEN:
            return Key::PrintScreen;
        case SDL_SCANCODE_SCROLLLOCK:
            return Key::ScrollLock;
        case SDL_SCANCODE_PAUSE:
            return Key::Pause;
        default:
            return Key::Unknown;
    }
}

// SDL mouse button -> engine MouseButton. Explicit: SDL's MIDDLE==2 and RIGHT==3, so an arithmetic
// (button-1) map would swap them. An unknown button returns MouseButton::Count (event dropped).
MouseButton fromSdlButton(std::uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return MouseButton::Left;
        case SDL_BUTTON_RIGHT:
            return MouseButton::Right;
        case SDL_BUTTON_MIDDLE:
            return MouseButton::Middle;
        case SDL_BUTTON_X1:
            return MouseButton::X1;
        case SDL_BUTTON_X2:
            return MouseButton::X2;
        default:
            return MouseButton::Count;
    }
}

// SDL modifier bitmask -> engine KeyMods. Accumulate in `unsigned` and narrow once (a per-bit `|=`
// on a uint16 would trip bugprone-narrowing-conversions under --warnings-as-errors).
KeyMods fromSdlMod(SDL_Keymod mod) {
    unsigned bits = 0;
    auto set = [&](SDL_Keymod sdlBit, KeyMod engineBit) {
        if ((mod & sdlBit) != 0) {
            bits |= static_cast<unsigned>(engineBit);
        }
    };
    set(SDL_KMOD_LSHIFT, KeyMod::LeftShift);
    set(SDL_KMOD_RSHIFT, KeyMod::RightShift);
    set(SDL_KMOD_LCTRL, KeyMod::LeftCtrl);
    set(SDL_KMOD_RCTRL, KeyMod::RightCtrl);
    set(SDL_KMOD_LALT, KeyMod::LeftAlt);
    set(SDL_KMOD_RALT, KeyMod::RightAlt);
    set(SDL_KMOD_LGUI, KeyMod::LeftGui);
    set(SDL_KMOD_RGUI, KeyMod::RightGui);
    set(SDL_KMOD_CAPS, KeyMod::CapsLock);
    set(SDL_KMOD_NUM, KeyMod::NumLock);
    return KeyMods{static_cast<std::uint16_t>(bits)};
}

// SDL → engine translation. Returns nullopt for events not modeled (they are drained and dropped by
// the caller). Reads the flattened SDL3 window-event fields: e.window.{windowID,data1,data2}
// (verified SDL_events.h:142-152; SDL_EVENT_WINDOW_RESIZED reports "data1xdata2"). Event is a tagged
// union (task 0.3.2), so every case builds the event field-by-field rather than via aggregate {,,,}.
std::optional<Event> translate(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT: {
            Event ev;
            ev.type = EventType::Quit;
            return ev;
        }
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            Event ev;
            ev.type = EventType::WindowClose;
            ev.window = WindowId{e.window.windowID};
            return ev;
        }
        case SDL_EVENT_WINDOW_RESIZED: {
            Event ev;
            ev.type = EventType::WindowResized;
            ev.window = WindowId{e.window.windowID};
            ev.size = WindowSize{e.window.data1, e.window.data2};  // logical/points
            return ev;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            Event ev;
            ev.type = EventType::WindowPixelSizeChanged;
            ev.window = WindowId{e.window.windowID};
            ev.size = WindowSize{e.window.data1, e.window.data2};  // framebuffer/pixels
            return ev;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            Event ev;
            ev.type = EventType::WindowFocusGained;
            ev.window = WindowId{e.window.windowID};
            return ev;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            Event ev;
            ev.type = EventType::WindowFocusLost;
            ev.window = WindowId{e.window.windowID};
            return ev;
        }
        case SDL_EVENT_WINDOW_MINIMIZED: {
            Event ev;
            ev.type = EventType::WindowMinimized;
            ev.window = WindowId{e.window.windowID};
            return ev;
        }
        case SDL_EVENT_WINDOW_RESTORED: {
            Event ev;
            ev.type = EventType::WindowRestored;
            ev.window = WindowId{e.window.windowID};
            return ev;
        }
        // ---- input (task 0.3.2) ----
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const Key code = fromScancode(e.key.scancode);
            if (code == Key::Unknown) {
                return std::nullopt;  // physical key outside the curated set — dropped (D9)
            }
            Event ev;
            ev.type = (e.type == SDL_EVENT_KEY_DOWN) ? EventType::KeyDown : EventType::KeyUp;
            ev.window = WindowId{e.key.windowID};
            ev.key = KeyData{code, fromSdlMod(e.key.mod), e.key.repeat};
            return ev;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            Event ev;
            ev.type = EventType::MouseMoved;
            ev.window = WindowId{e.motion.windowID};
            ev.mouseMotion = MouseMotionData{e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel};
            return ev;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const MouseButton button = fromSdlButton(e.button.button);
            if (button == MouseButton::Count) {
                return std::nullopt;  // button outside Left/Right/Middle/X1/X2 — dropped
            }
            Event ev;
            ev.type = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? EventType::MouseButtonDown : EventType::MouseButtonUp;
            ev.window = WindowId{e.button.windowID};
            ev.mouseButton = MouseButtonData{button, e.button.x, e.button.y, e.button.clicks};
            return ev;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            float x = e.wheel.x;
            float y = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {  // natural scrolling: undo the flip (D14)
                x = -x;
                y = -y;
            }
            Event ev;
            ev.type = EventType::MouseWheel;
            ev.window = WindowId{e.wheel.windowID};
            ev.mouseWheel = MouseWheelData{x, y};
            return ev;
        }
        default:
            return std::nullopt;  // still-unmodeled events (display, text, gamepad…) are drained, dropped
    }
}

SDL_WindowFlags toFlags(const WindowConfig& cfg) {
    SDL_WindowFlags flags = 0;
    if (cfg.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (cfg.highDpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (cfg.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

}  // namespace

// ---- Window::Impl + methods -----------------------------------------------------------------

struct Window::Impl {
    SDL_Window* window = nullptr;

    // Cleanup lives HERE, not in ~Window: the resource is owned by Impl, so Impl must be the one to
    // release it. Putting cleanup in ~Window instead left the defaulted move-assignment operator
    // broken — unique_ptr<Impl>'s move-assign destroys the LHS Impl via its (trivial) deleter
    // without ever calling ~Window on it, orphaning the overwritten SDL_Window. With cleanup here,
    // that same unique_ptr destruction runs ~Impl -> SDL_DestroyWindow correctly.
    ~Impl() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
};

Window::Window(std::unique_ptr<Impl> impl) noexcept : impl(std::move(impl)) {}
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;
// A moved-from Window holds a null impl (unique_ptr's moved-from state), so ~Impl never runs for it
// — no double-free.
Window::~Window() = default;

WindowId Window::id() const noexcept { return WindowId{SDL_GetWindowID(impl->window)}; }

WindowSize Window::size() const {
    WindowSize s;
    SDL_GetWindowSize(impl->window, &s.width, &s.height);  // logical/points
    return s;
}

WindowSize Window::pixelSize() const {
    WindowSize s;
    SDL_GetWindowSizeInPixels(impl->window, &s.width, &s.height);  // framebuffer/pixels (HiDPI)
    return s;
}

void Window::setTitle(std::string_view title) {
    // std::string_view is not guaranteed NUL-terminated; SDL wants a C string, so materialize one.
    SDL_SetWindowTitle(impl->window, std::string(title).c_str());
}
void Window::setSize(std::int32_t width, std::int32_t height) { SDL_SetWindowSize(impl->window, width, height); }
void Window::show() { SDL_ShowWindow(impl->window); }
void Window::hide() { SDL_HideWindow(impl->window); }

// The native-window seam (task 0.4.2): hands engine::rhi's backend TU the SDL_Window* it needs for
// SDL_ClaimWindowForGPUDevice, without widening window.hpp's public surface. window.impl->window is
// never null on a live (not moved-from) Window — the caller contract every other Window member shares.
SDL_Window* internal::NativeWindowAccessor::get(const Window& window) noexcept { return window.impl->window; }

// ---- Context --------------------------------------------------------------------------------

Context::Context(const ContextConfig& config) {
    // NOT assert(fetch_add()==0): a side effect inside assert() vanishes under NDEBUG. Run the
    // counter always, fire the assert (debug-only) on the runtime result — the ~JobGraph idiom.
    if (liveContexts.fetch_add(1, std::memory_order_relaxed) != 0) {
        assert(false && "exactly one platform::Context per process");
    }
    if (config.headless) {
        // Must be set BEFORE SDL_Init to take effect. "dummy" creates windows and pumps events with
        // no display server — the headless path for tests/CI (SDL_hints.h:3743).
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    }
    // SDL3's SDL_Init returns bool (true == success) — NOT SDL2's 0-on-success (SDL_init.h:238).
    if (!SDL_Init(SDL_INIT_VIDEO)) {  // implies SDL_INIT_EVENTS (SDL_init.h:81)
        AERO_LOG_CRITICAL("platform: SDL_Init(VIDEO) failed: {}", SDL_GetError());
        return;  // initialized stays false → inert Context (D10)
    }
    initialized = true;
    ownerThread = std::this_thread::get_id();
    AERO_LOG_INFO("platform initialized (video driver: {})", SDL_GetCurrentVideoDriver());
}

Context::~Context() {
    if (initialized) {
        SDL_Quit();
    }
    liveContexts.fetch_sub(1, std::memory_order_relaxed);
}

std::optional<Window> Context::createWindow(const WindowConfig& config) {
    AERO_PROFILE_ZONE_NAMED("platform::createWindow");
    // The inert-Context guard MUST run before the thread assert: an inert Context (SDL_Init failed)
    // never set ownerThread, so it stays default-constructed — asserting against it first would
    // abort even a correct-thread call. AC-5/E2/D10 require nullopt/false, never abort, on an inert
    // Context.
    if (!initialized) {
        return std::nullopt;
    }
    assert(std::this_thread::get_id() == ownerThread && "createWindow() off the main thread");
    // SDL3 SDL_CreateWindow(title, w, h, flags) — no x/y (SDL_video.h:1184). NULL on failure.
    SDL_Window* w = SDL_CreateWindow(config.title.c_str(), config.width, config.height, toFlags(config));
    if (w == nullptr) {
        AERO_LOG_ERROR("platform: SDL_CreateWindow('{}', {}x{}) failed: {}", config.title, config.width, config.height,
                       SDL_GetError());
        return std::nullopt;
    }
    auto impl = std::make_unique<Window::Impl>();
    impl->window = w;
    return Window{std::move(impl)};
}

bool Context::pollEvent(Event& out) {
    // Same guard-before-assert ordering as createWindow, and for the same reason: an inert Context
    // never set ownerThread, so the assert must not run before this check (AC-5/E2/D10).
    if (!initialized) {
        return false;
    }
    assert(std::this_thread::get_id() == ownerThread && "pollEvent() off the main thread");
    SDL_Event e;
    while (SDL_PollEvent(&e)) {  // SDL_PollEvent implicitly pumps (SDL_events.h:1088)
        if (std::optional<Event> ev = translate(e)) {
            inputState.process(*ev);  // task 0.3.2: fold key/mouse/focus into the polled state
            out = *ev;
            return true;  // a modeled event: hand it out
        }
        // else: still-unmodeled — already dequeued, keep draining.
    }
    return false;
}

}  // namespace engine::platform
