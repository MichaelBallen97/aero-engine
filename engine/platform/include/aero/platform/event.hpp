#pragma once
// Aero Engine — platform event & window-identity types (task 0.3.1).
// engine::platform's event vocabulary. SDL is the backend of the pump that PRODUCES these
// (engine/platform/src/platform.cpp) and never crosses this boundary (docs/04 boundary rule); the
// `lint` job's SDL step and tests/platform_boundary_probe.cpp enforce that mechanically.

#include <cstdint>

namespace engine::platform {

// Opaque, stable identity for a Window, carried on every window event so a multi-window consumer
// can tell them apart. 0 is the invalid/none id (SDL's own convention: a window id is never 0).
// Backed by a uint32 whose value happens to equal the backend's window id — an implementation
// detail callers must not rely on. This is NOT an SDL type.
struct WindowId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    constexpr bool operator==(const WindowId&) const noexcept = default;
};

// Every event the 0.3.1 pump surfaces. Enumerators are PascalCase, matching engine::LogLevel
// (docs/04 is silent on enumerator case; the codebase's one prior enum, LogLevel, sets the
// convention and clang-tidy leaves EnumConstantCase unset). Keyboard/mouse events arrive in 0.3.2,
// which extends this enum and Event's payload.
enum class EventType : std::uint8_t {
    None = 0,                // a default-constructed Event; pollEvent NEVER returns this
    Quit,                    // app-level quit request (last window closed, OS quit, Cmd/Alt+F4…)
    WindowClose,             // one window's close box / window-manager close request
    WindowResized,           // LOGICAL (point) size changed — width/height are points
    WindowPixelSizeChanged,  // FRAMEBUFFER (pixel) size changed — width/height are pixels; HiDPI
    WindowFocusGained,
    WindowFocusLost,
    WindowMinimized,
    WindowRestored,
};

// One event from the pump: a flat, trivially-copyable POD — no allocation, cheap to pass by value.
// `width`/`height` are meaningful ONLY for WindowResized (points) and WindowPixelSizeChanged
// (pixels); they are 0 for every other type. `window` is the originating window (invalid for Quit).
// 0.3.2 extends the payload (a union) for key codes / mouse coordinates.
struct Event {
    EventType type = EventType::None;
    WindowId window;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

}  // namespace engine::platform
