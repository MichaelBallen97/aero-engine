#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
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

// SDL → engine translation. Returns nullopt for events 0.3.1 does not model (they are drained and
// dropped by the caller). Reads the flattened SDL3 window-event fields: e.window.{windowID,data1,
// data2} (verified SDL_events.h:142-152; SDL_EVENT_WINDOW_RESIZED reports "data1xdata2").
std::optional<Event> translate(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
            return Event{EventType::Quit, {}, 0, 0};
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return Event{EventType::WindowClose, WindowId{e.window.windowID}, 0, 0};
        case SDL_EVENT_WINDOW_RESIZED:
            return Event{EventType::WindowResized, WindowId{e.window.windowID}, e.window.data1, e.window.data2};
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return Event{EventType::WindowPixelSizeChanged, WindowId{e.window.windowID}, e.window.data1,
                         e.window.data2};
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            return Event{EventType::WindowFocusGained, WindowId{e.window.windowID}, 0, 0};
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            return Event{EventType::WindowFocusLost, WindowId{e.window.windowID}, 0, 0};
        case SDL_EVENT_WINDOW_MINIMIZED:
            return Event{EventType::WindowMinimized, WindowId{e.window.windowID}, 0, 0};
        case SDL_EVENT_WINDOW_RESTORED:
            return Event{EventType::WindowRestored, WindowId{e.window.windowID}, 0, 0};
        default:
            return std::nullopt;  // input & everything else: drained, not surfaced (0.3.2 handles input)
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
            out = *ev;
            return true;  // a modeled event: hand it out
        }
        // else: unmodeled (input, etc.) — already dequeued, so keep draining. This is what makes the
        // caller's `while (pollEvent(e))` terminate when the SDL queue empties, dropping input for now.
    }
    return false;
}

}  // namespace engine::platform
