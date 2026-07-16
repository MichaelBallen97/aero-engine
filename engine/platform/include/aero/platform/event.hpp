#pragma once
// Aero Engine — platform event, input & window-identity types (tasks 0.3.1, 0.3.2).
// engine::platform's event and input vocabulary. SDL is the backend of the pump that PRODUCES these
// (engine/platform/src/platform.cpp) and never crosses this boundary (docs/04 boundary rule); the
// `lint` job's SDL step and tests/platform_boundary_probe.cpp enforce that mechanically. Every type
// here is an engine type — Key/MouseButton are engine enums whose mapping from the backend lives in
// the pump, not here.

#include <cstdint>

namespace engine::platform {

// Opaque, stable identity for a Window, carried on every window event so a multi-window consumer
// can tell them apart. 0 is the invalid/none id (a backend window id is never 0). Backed by a uint32
// whose value happens to equal the backend's window id — an implementation detail callers must not
// rely on. This is NOT an SDL type.
struct WindowId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    constexpr bool operator==(const WindowId&) const noexcept = default;
};

// A discrete 2D window size, in points or pixels depending on the getter. A local value type: the
// engine's float Vec2 (core/math) is the wrong tool for integer window dimensions. Moved here from
// window.hpp (task 0.3.2) so it can also serve as the resize event payload without duplication.
struct WindowSize {
    std::int32_t width = 0;
    std::int32_t height = 0;
    constexpr bool operator==(const WindowSize&) const noexcept = default;
};

// A 2D point / vector in FLOAT window-relative points — mouse position, per-frame motion delta, or
// per-frame wheel amount, depending on the getter (task 0.3.2). A local value type by the same
// reasoning as WindowSize: it keeps event.hpp free of the math umbrella and every event payload a
// trivially-copyable POD. Callers wanting engine math write Vec2{p.x, p.y}.
struct MousePosition {
    float x = 0.0f;
    float y = 0.0f;
    constexpr bool operator==(const MousePosition&) const noexcept = default;
};

// Physical keyboard keys — identified by POSITION, not by the character the current layout prints
// (task 0.3.2, D1): Key::W is always the physical W location, so WASD is layout-independent. This is
// the web KeyboardEvent.code model. Values are engine-owned and CONTIGUOUS from 0 — deliberately NOT
// equal to any backend scancode; the pump maps scancode -> Key. Count is the array-sizing sentinel,
// not a key. A curated set (letters, digits, F1-F12, navigation, arrows, modifiers, keypad,
// punctuation); exotic international/media keys are omitted for now and added additively when needed.
enum class Key : std::uint8_t {
    Unknown = 0,
    // Letters (contiguous — see fromScancode in platform.cpp)
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    // Number row (contiguous)
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    // Function row (contiguous)
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    // Editing / navigation
    Escape,
    Enter,
    Tab,
    Space,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,
    // Arrows
    Left,
    Right,
    Up,
    Down,
    // Modifiers
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    LeftGui,
    RightGui,
    CapsLock,
    // Punctuation (US-QWERTY physical positions)
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,
    // Keypad (Kp1..Kp9 contiguous)
    Kp0,
    Kp1,
    Kp2,
    Kp3,
    Kp4,
    Kp5,
    Kp6,
    Kp7,
    Kp8,
    Kp9,
    KpEnter,
    KpPlus,
    KpMinus,
    KpMultiply,
    KpDivide,
    KpPeriod,
    // Misc
    PrintScreen,
    ScrollLock,
    Pause,
    Count  // sentinel: number of keys, for array sizing — NOT a key
};

// Mouse buttons — engine-owned contiguous values, NOT the backend's numbering (which is
// Left=1/Middle=2/Right=3). The pump maps them explicitly. Count is the sizing sentinel.
enum class MouseButton : std::uint8_t { Left = 0, Right, Middle, X1, X2, Count };

// Held modifier flags, as engine-owned bits. The pump builds a KeyMods from the backend's mod mask.
enum class KeyMod : std::uint16_t {
    None = 0,
    LeftShift = 1U << 0U,
    RightShift = 1U << 1U,
    LeftCtrl = 1U << 2U,
    RightCtrl = 1U << 3U,
    LeftAlt = 1U << 4U,
    RightAlt = 1U << 5U,
    LeftGui = 1U << 6U,
    RightGui = 1U << 7U,
    CapsLock = 1U << 8U,
    NumLock = 1U << 9U,
};

// A set of held modifiers carried on a key event. Predicates fold the left/right pairs so shortcut
// code asks shift()/ctrl()/alt()/gui() without caring which side; has() tests one specific modifier.
struct KeyMods {
    std::uint16_t bits = 0;
    [[nodiscard]] constexpr bool has(KeyMod mod) const noexcept {
        return (bits & static_cast<std::uint16_t>(mod)) != 0U;
    }
    [[nodiscard]] constexpr bool shift() const noexcept { return has(KeyMod::LeftShift) || has(KeyMod::RightShift); }
    [[nodiscard]] constexpr bool ctrl() const noexcept { return has(KeyMod::LeftCtrl) || has(KeyMod::RightCtrl); }
    [[nodiscard]] constexpr bool alt() const noexcept { return has(KeyMod::LeftAlt) || has(KeyMod::RightAlt); }
    [[nodiscard]] constexpr bool gui() const noexcept { return has(KeyMod::LeftGui) || has(KeyMod::RightGui); }
    constexpr bool operator==(const KeyMods&) const noexcept = default;
};

// Every event the pump surfaces. Window events (0.3.1) + input events (0.3.2). Enumerators are
// PascalCase, matching engine::LogLevel (docs/04 is silent on enumerator case; clang-tidy leaves
// EnumConstantCase unset). Input events are appended so the window values keep their 0.3.1 numbering.
enum class EventType : std::uint8_t {
    None = 0,                // a default-constructed Event; pollEvent NEVER returns this
    Quit,                    // app-level quit request (last window closed, OS quit, Cmd/Alt+F4…)
    WindowClose,             // one window's close box / window-manager close request
    WindowResized,           // LOGICAL (point) size changed — Event::size is points
    WindowPixelSizeChanged,  // FRAMEBUFFER (pixel) size changed — Event::size is pixels; HiDPI
    WindowFocusGained,
    WindowFocusLost,
    WindowMinimized,
    WindowRestored,
    // ---- input (task 0.3.2) ----
    KeyDown,          // a physical key went down — Event::key (repeat==true for auto-repeat)
    KeyUp,            // a physical key went up — Event::key
    MouseButtonDown,  // Event::mouseButton
    MouseButtonUp,    // Event::mouseButton
    MouseMoved,       // Event::mouseMotion (absolute position + relative delta)
    MouseWheel,       // Event::mouseWheel (normalized: FLIPPED already undone)
};

// ---- event payloads (task 0.3.2) --------------------------------------------------------------
// Each is a trivially-copyable POD. Which one is live is determined by Event::type (see the union).

struct KeyData {
    Key code = Key::Unknown;  // the physical key (named `code`, echoing web KeyboardEvent.code)
    KeyMods mods;             // modifiers held when the key event fired
    bool repeat = false;      // true if this is an OS auto-repeat, not a fresh press
};

struct MouseButtonData {
    MouseButton button = MouseButton::Left;
    float x = 0.0f;  // window-relative points at the time of the click
    float y = 0.0f;
    std::uint8_t clicks = 0;  // 1 single, 2 double, …
};

struct MouseMotionData {
    float x = 0.0f;  // absolute, window-relative points
    float y = 0.0f;
    float dx = 0.0f;  // relative motion since the last motion event
    float dy = 0.0f;
};

struct MouseWheelData {
    float x = 0.0f;  // horizontal scroll (normalized: positive right)
    float y = 0.0f;  // vertical scroll (normalized: positive away from the user)
};

// One event from the pump. `type` selects which union arm is meaningful; `window` is the originating
// window (invalid for Quit). The union's first arm (`size`) carries a default member initializer, so
// Event stays an AGGREGATE and TRIVIALLY COPYABLE (every arm is), and a default-constructed Event is
// {None, invalid window, zeroed size}. Because it is a union, build events field-by-field (the pump
// does: `Event ev; ev.type = …; ev.key = …;`) rather than by aggregate {,,,} construction.
struct Event {
    EventType type = EventType::None;
    WindowId window;
    union {
        WindowSize size{};            // WindowResized (points) | WindowPixelSizeChanged (pixels)
        KeyData key;                  // KeyDown | KeyUp
        MouseButtonData mouseButton;  // MouseButtonDown | MouseButtonUp
        MouseMotionData mouseMotion;  // MouseMoved
        MouseWheelData mouseWheel;    // MouseWheel
    };
};

}  // namespace engine::platform
