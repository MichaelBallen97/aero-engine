#pragma once
// Aero Engine — input state (tasks 0.3.2, 4.4.3). A polled snapshot of the keyboard, the mouse and four gamepad slots,
// updated by folding the engine Event stream (Context::pollEvent feeds it; you read Context::input()). It holds NO SDL
// type and makes NO SDL call — it consumes only engine::platform::Event — so it is unit-tested by feeding it Events
// directly, with no Context and no display (input_test.cpp).
//
// FRAME MODEL: call newFrame() ONCE at the top of each frame, BEFORE draining pollEvent. It clears
// the per-frame edges (keyPressed/keyReleased, mouse button edges) and deltas (mouse motion, wheel)
// while keeping held state (keyDown, mouseButtonDown) and the last mouse position. newFrame() also
// clears every gamepad slot's pressed and released edges; a held pad button and a stick position stay.
// A loop that reads only keyDown()/mousePosition() may skip newFrame(); edge queries and deltas
// require it.
//
// A focus loss (WindowFocusLost) clears held keys and mouse buttons only. A held pad button and the
// axes stay as they were, because SDL keeps a held pad button held and always delivers its release.
//
//   ctx.newFrame();
//   for (Event ev; ctx.pollEvent(ev);) { /* optional discrete handling */ }
//   if (ctx.input().keyPressed(Key::Space)) { jump(); }   // edge, this frame
//   if (ctx.input().keyDown(Key::W))        { moveForward(); }  // held

#include <aero/platform/event.hpp>

#include <array>
#include <cstddef>

namespace engine::platform {

// A stick's position, x right and y down, each in [-1, 1] before the deadzone (up reads -1, as the backend
// documents it).
struct StickPosition {
    float x = 0.0F;
    float y = 0.0F;
    constexpr bool operator==(const StickPosition&) const noexcept = default;
};

// The round deadzone: 8000 of the backend's 32767, "centered within ~8000 of zero".
inline constexpr float STICK_DEADZONE = 8000.0F / 32767.0F;

// Zero at or inside the deadzone (and for any non-finite input), else the position rescaled so the deadzone's
// edge reads 0 and full tilt reads length 1, the direction kept.
[[nodiscard]] StickPosition radialDeadzone(StickPosition raw) noexcept;

class InputState {
public:
    // ---- keyboard ----
    [[nodiscard]] bool keyDown(Key key) const noexcept;      // held right now
    [[nodiscard]] bool keyPressed(Key key) const noexcept;   // went down THIS frame (ignores repeat)
    [[nodiscard]] bool keyReleased(Key key) const noexcept;  // went up THIS frame

    // ---- mouse buttons ----
    [[nodiscard]] bool mouseButtonDown(MouseButton button) const noexcept;
    [[nodiscard]] bool mouseButtonPressed(MouseButton button) const noexcept;
    [[nodiscard]] bool mouseButtonReleased(MouseButton button) const noexcept;

    // ---- mouse position / deltas ----
    [[nodiscard]] MousePosition mousePosition() const noexcept;  // window-relative points, persistent
    [[nodiscard]] MousePosition mouseDelta() const noexcept;     // summed motion THIS frame
    [[nodiscard]] MousePosition mouseWheel() const noexcept;     // summed wheel THIS frame (y = vertical)

    // ---- gamepads (task 4.4.3): four engine slots; an out-of-range slot, button or axis reads false / 0 ----
    [[nodiscard]] bool gamepadConnected(std::size_t slot = 0) const noexcept;
    [[nodiscard]] bool gamepadButtonDown(GamepadButton button, std::size_t slot = 0) const noexcept;
    [[nodiscard]] bool gamepadButtonPressed(GamepadButton button, std::size_t slot = 0) const noexcept;
    [[nodiscard]] bool gamepadButtonReleased(GamepadButton button, std::size_t slot = 0) const noexcept;
    // A stick axis in [-1, 1] (exactly -1 at -32768 and +1 at 32767), a trigger in [0, 1]; no deadzone.
    [[nodiscard]] float gamepadAxis(GamepadAxis axis, std::size_t slot = 0) const noexcept;
    // The stick's two axes through radialDeadzone.
    [[nodiscard]] StickPosition gamepadStick(GamepadStick stick, std::size_t slot = 0) const noexcept;

    // ---- driving (the pump calls these; also the unit-test seam) ----
    void process(const Event& event) noexcept;  // fold one engine event into the state
    void newFrame() noexcept;                   // clear this-frame edges & deltas; keep held state

private:
    // Index is the enum value (contiguous from 0); arrays are sized to the enum Count sentinel.
    std::array<bool, static_cast<std::size_t>(Key::Count)> keysDown{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keysPressed{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keysReleased{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsDown{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsPressed{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> buttonsReleased{};
    MousePosition mousePos{};     // last known absolute position (persists across frames)
    MousePosition mouseMotion{};  // accumulated motion delta this frame
    MousePosition wheelAccum{};   // accumulated wheel this frame

    struct GamepadSlot {
        std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> down{};
        std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> pressed{};
        std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> released{};
        std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};
        bool connected = false;
    };
    std::array<GamepadSlot, MAX_GAMEPADS> gamepads{};
};

}  // namespace engine::platform
