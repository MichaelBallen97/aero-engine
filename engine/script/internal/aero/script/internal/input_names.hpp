#pragma once
// Aero Engine -- the engine.input name tables (task 4.4.3; internal to engine/script).
// The script-visible spelling of five platform enums: Key, MouseButton, GamepadButton, GamepadAxis and GamepadStick.
// A script names a key as "W" or "Space", a pad button as "South", and the lookups here turn the name back into the
// enum. ONE RULE: each table spells its enum's enumerators in the enum's own order, `Unknown` and `Count` never, so
// the name at index i is the enumerator whose value is i (Key: i + 1, because Unknown has no name). The tables are
// written by hand; tests/script_input_test.cpp SI1 reads the enums as text and holds each table to them, and the
// declarations' string-literal unions are held to the same tables, so the three stay in step. Pure: no backend, no
// allocation, no state; every lookup is exact and case-sensitive and returns nothing for any other string.

#include <aero/platform/event.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace engine::script::internal {

// [i] names Key(i + 1).
inline constexpr std::array<std::string_view, 102> KEY_NAMES{
    "A",          "B",         "C",           "D",
    "E",          "F",         "G",           "H",
    "I",          "J",         "K",           "L",
    "M",          "N",         "O",           "P",
    "Q",          "R",         "S",           "T",
    "U",          "V",         "W",           "X",
    "Y",          "Z",         "Num0",        "Num1",
    "Num2",       "Num3",      "Num4",        "Num5",
    "Num6",       "Num7",      "Num8",        "Num9",
    "F1",         "F2",        "F3",          "F4",
    "F5",         "F6",        "F7",          "F8",
    "F9",         "F10",       "F11",         "F12",
    "Escape",     "Enter",     "Tab",         "Space",
    "Backspace",  "Delete",    "Insert",      "Home",
    "End",        "PageUp",    "PageDown",    "Left",
    "Right",      "Up",        "Down",        "LeftShift",
    "RightShift", "LeftCtrl",  "RightCtrl",   "LeftAlt",
    "RightAlt",   "LeftGui",   "RightGui",    "CapsLock",
    "Minus",      "Equals",    "LeftBracket", "RightBracket",
    "Backslash",  "Semicolon", "Apostrophe",  "Grave",
    "Comma",      "Period",    "Slash",       "Kp0",
    "Kp1",        "Kp2",       "Kp3",         "Kp4",
    "Kp5",        "Kp6",       "Kp7",         "Kp8",
    "Kp9",        "KpEnter",   "KpPlus",      "KpMinus",
    "KpMultiply", "KpDivide",  "KpPeriod",    "PrintScreen",
    "ScrollLock", "Pause",
};
inline constexpr std::array<std::string_view, 5> MOUSE_BUTTON_NAMES{"Left", "Right", "Middle", "X1", "X2"};
inline constexpr std::array<std::string_view, 26> GAMEPAD_BUTTON_NAMES{
    "South",     "East",       "West",         "North",         "Back",         "Guide",       "Start",
    "LeftStick", "RightStick", "LeftShoulder", "RightShoulder", "DpadUp",       "DpadDown",    "DpadLeft",
    "DpadRight", "Misc1",      "RightPaddle1", "LeftPaddle1",   "RightPaddle2", "LeftPaddle2", "Touchpad",
    "Misc2",     "Misc3",      "Misc4",        "Misc5",         "Misc6",
};
inline constexpr std::array<std::string_view, 6> GAMEPAD_AXIS_NAMES{"LeftX",  "LeftY",       "RightX",
                                                                    "RightY", "LeftTrigger", "RightTrigger"};
inline constexpr std::array<std::string_view, 2> GAMEPAD_STICK_NAMES{"Left", "Right"};

static_assert(KEY_NAMES.size() == static_cast<std::size_t>(platform::Key::Count) - 1U);
static_assert(MOUSE_BUTTON_NAMES.size() == static_cast<std::size_t>(platform::MouseButton::Count));
static_assert(GAMEPAD_BUTTON_NAMES.size() == static_cast<std::size_t>(platform::GamepadButton::Count));
static_assert(GAMEPAD_AXIS_NAMES.size() == static_cast<std::size_t>(platform::GamepadAxis::Count));
static_assert(GAMEPAD_STICK_NAMES.size() == static_cast<std::size_t>(platform::GamepadStick::Count));

namespace detail {

template <std::size_t N>
[[nodiscard]] constexpr bool noEmptyName(const std::array<std::string_view, N>& table) noexcept {
    for (const std::string_view name : table) {
        if (name.empty()) {
            return false;
        }
    }
    return true;
}

static_assert(noEmptyName(KEY_NAMES) && noEmptyName(MOUSE_BUTTON_NAMES) && noEmptyName(GAMEPAD_BUTTON_NAMES) &&
                  noEmptyName(GAMEPAD_AXIS_NAMES) && noEmptyName(GAMEPAD_STICK_NAMES),
              "a name table holds an empty entry");

// The index of `name` in `table`, or `N` when it is not there. An exact, case-sensitive scan.
template <std::size_t N>
[[nodiscard]] constexpr std::size_t indexOf(const std::array<std::string_view, N>& table,
                                            std::string_view name) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i] == name) {
            return i;
        }
    }
    return N;
}

}  // namespace detail

[[nodiscard]] constexpr std::optional<platform::Key> keyFromName(std::string_view name) noexcept {
    const std::size_t index = detail::indexOf(KEY_NAMES, name);
    if (index == KEY_NAMES.size()) {
        return std::nullopt;
    }
    return static_cast<platform::Key>(index + 1U);
}

[[nodiscard]] constexpr std::optional<platform::MouseButton> mouseButtonFromName(std::string_view name) noexcept {
    const std::size_t index = detail::indexOf(MOUSE_BUTTON_NAMES, name);
    if (index == MOUSE_BUTTON_NAMES.size()) {
        return std::nullopt;
    }
    return static_cast<platform::MouseButton>(index);
}

[[nodiscard]] constexpr std::optional<platform::GamepadButton> gamepadButtonFromName(std::string_view name) noexcept {
    const std::size_t index = detail::indexOf(GAMEPAD_BUTTON_NAMES, name);
    if (index == GAMEPAD_BUTTON_NAMES.size()) {
        return std::nullopt;
    }
    return static_cast<platform::GamepadButton>(index);
}

[[nodiscard]] constexpr std::optional<platform::GamepadAxis> gamepadAxisFromName(std::string_view name) noexcept {
    const std::size_t index = detail::indexOf(GAMEPAD_AXIS_NAMES, name);
    if (index == GAMEPAD_AXIS_NAMES.size()) {
        return std::nullopt;
    }
    return static_cast<platform::GamepadAxis>(index);
}

[[nodiscard]] constexpr std::optional<platform::GamepadStick> gamepadStickFromName(std::string_view name) noexcept {
    const std::size_t index = detail::indexOf(GAMEPAD_STICK_NAMES, name);
    if (index == GAMEPAD_STICK_NAMES.size()) {
        return std::nullopt;
    }
    return static_cast<platform::GamepadStick>(index);
}

}  // namespace engine::script::internal
