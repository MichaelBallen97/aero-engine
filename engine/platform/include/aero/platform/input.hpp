#pragma once
// Aero Engine — input state (task 0.3.2). A polled snapshot of the keyboard and mouse, updated by
// folding the engine Event stream (Context::pollEvent feeds it; you read Context::input()). It holds
// NO SDL type and makes NO SDL call — it consumes only engine::platform::Event — so it is unit-tested
// by feeding it Events directly, with no Context and no display (input_test.cpp).
//
// FRAME MODEL: call newFrame() ONCE at the top of each frame, BEFORE draining pollEvent. It clears
// the per-frame edges (keyPressed/keyReleased, mouse button edges) and deltas (mouse motion, wheel)
// while keeping held state (keyDown, mouseButtonDown) and the last mouse position. A loop that reads
// only keyDown()/mousePosition() may skip newFrame(); edge queries and deltas require it.
//
//   ctx.newFrame();
//   for (Event ev; ctx.pollEvent(ev);) { /* optional discrete handling */ }
//   if (ctx.input().keyPressed(Key::Space)) { jump(); }   // edge, this frame
//   if (ctx.input().keyDown(Key::W))        { moveForward(); }  // held

#include <aero/platform/event.hpp>

#include <array>
#include <cstddef>

namespace engine::platform {

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
};

}  // namespace engine::platform
