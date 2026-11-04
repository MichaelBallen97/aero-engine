// Aero Engine — input state fold (tasks 0.3.2, 4.4.3). Pure engine logic: folds the Event stream into held +
// edge state. NO SDL here (the scancode/button/mod mapping lives in platform.cpp); this TU is what
// makes InputState testable with plain Events and no display.

#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::platform {

namespace {

// A stick's -32768 must read -1 and its 32767 +1, and 0 must read 0: one divisor cannot give both ends
// (-32768/32767 is -1.00003, 32767/32768 is 0.99997). A trigger's range is 0..32767 (SDL_gamepad.h:1256).
float normalizedAxis(GamepadAxis axis, std::int16_t value) noexcept {
    if (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger) {
        return value <= 0 ? 0.0F : static_cast<float>(value) / 32767.0F;
    }
    return value < 0 ? static_cast<float>(value) / 32768.0F : static_cast<float>(value) / 32767.0F;
}

}  // namespace

StickPosition radialDeadzone(StickPosition raw) noexcept {
    const float length = std::hypot(raw.x, raw.y);             // hypot(x, +-0) == |x| exactly (C Annex F.10.4.3)
    if (!std::isfinite(length) || length <= STICK_DEADZONE) {  // finiteness first; a NaN lands here
        return StickPosition{};
    }
    const float span = 1.0F - STICK_DEADZONE;
    const float scaled = std::min((length - STICK_DEADZONE) / span, 1.0F);  // corners clamp to length 1
    const float k = scaled / length;
    return StickPosition{raw.x * k, raw.y * k};
}

void InputState::process(const Event& event) noexcept {
    switch (event.type) {
        case EventType::KeyDown: {
            const auto i = static_cast<std::size_t>(event.key.code);
            if (i >= keysDown.size()) {
                break;  // Unknown / out-of-range — ignore (defensive; translate() should not emit it)
            }
            if (!event.key.repeat) {
                keysPressed[i] = true;  // auto-repeat is NOT a fresh press (D5)
            }
            keysDown[i] = true;
            break;
        }
        case EventType::KeyUp: {
            const auto i = static_cast<std::size_t>(event.key.code);
            if (i >= keysDown.size()) {
                break;
            }
            keysDown[i] = false;
            keysReleased[i] = true;
            break;
        }
        case EventType::MouseButtonDown: {
            const auto i = static_cast<std::size_t>(event.mouseButton.button);
            if (i < buttonsDown.size()) {
                buttonsDown[i] = true;
                buttonsPressed[i] = true;
            }
            mousePos = MousePosition{event.mouseButton.x, event.mouseButton.y};
            break;
        }
        case EventType::MouseButtonUp: {
            const auto i = static_cast<std::size_t>(event.mouseButton.button);
            if (i < buttonsDown.size()) {
                buttonsDown[i] = false;
                buttonsReleased[i] = true;
            }
            mousePos = MousePosition{event.mouseButton.x, event.mouseButton.y};
            break;
        }
        case EventType::MouseMoved: {
            mousePos = MousePosition{event.mouseMotion.x, event.mouseMotion.y};
            mouseMotion.x += event.mouseMotion.dx;
            mouseMotion.y += event.mouseMotion.dy;
            break;
        }
        case EventType::MouseWheel: {
            wheelAccum.x += event.mouseWheel.x;
            wheelAccum.y += event.mouseWheel.y;
            break;
        }
        case EventType::WindowFocusLost: {
            // No key-up/button-up arrives after focus loss (alt-tab while holding a key), so clear held
            // state to avoid stuck keys (D13). This-frame edges and mousePos are left as-is.
            keysDown.fill(false);
            buttonsDown.fill(false);
            break;
        }
        case EventType::GamepadConnected: {
            const std::size_t slot = event.gamepad.slot;
            if (slot < gamepads.size()) {
                gamepads[slot] = GamepadSlot{};  // a new pad shows nothing of the one that left
                gamepads[slot].connected = true;
            }
            break;
        }
        case EventType::GamepadDisconnected: {
            const std::size_t slot = event.gamepad.slot;
            if (slot < gamepads.size() && gamepads[slot].connected) {
                GamepadSlot& pad = gamepads[slot];
                for (std::size_t i = 0; i < pad.down.size(); ++i) {
                    if (pad.down[i]) {
                        pad.released[i] = true;  // edges stay until newFrame()
                    }
                }
                pad.down.fill(false);
                pad.axes.fill(0.0F);
                pad.connected = false;
            }
            break;
        }
        case EventType::GamepadButtonDown: {
            const std::size_t slot = event.gamepadButton.slot;
            const auto i = static_cast<std::size_t>(event.gamepadButton.button);
            if (slot < gamepads.size() && gamepads[slot].connected && i < gamepads[slot].down.size() &&
                !gamepads[slot].down[i]) {
                gamepads[slot].down[i] = true;
                gamepads[slot].pressed[i] = true;
            }
            break;
        }
        case EventType::GamepadButtonUp: {
            const std::size_t slot = event.gamepadButton.slot;
            const auto i = static_cast<std::size_t>(event.gamepadButton.button);
            // A release for a press the state never saw (dropped while unfocused) changes nothing.
            if (slot < gamepads.size() && gamepads[slot].connected && i < gamepads[slot].down.size() &&
                gamepads[slot].down[i]) {
                gamepads[slot].down[i] = false;
                gamepads[slot].released[i] = true;
            }
            break;
        }
        case EventType::GamepadAxisMoved: {
            const std::size_t slot = event.gamepadAxis.slot;
            const auto i = static_cast<std::size_t>(event.gamepadAxis.axis);
            if (slot < gamepads.size() && gamepads[slot].connected && i < gamepads[slot].axes.size()) {
                gamepads[slot].axes[i] = normalizedAxis(event.gamepadAxis.axis, event.gamepadAxis.value);
            }
            break;
        }
        default:
            break;  // Quit, WindowClose, resize, focus-gained, minimize/restore — not input state
    }
}

void InputState::newFrame() noexcept {
    keysPressed.fill(false);
    keysReleased.fill(false);
    buttonsPressed.fill(false);
    buttonsReleased.fill(false);
    mouseMotion = MousePosition{};
    wheelAccum = MousePosition{};
    for (GamepadSlot& pad : gamepads) {
        pad.pressed.fill(false);
        pad.released.fill(false);
    }
    // keysDown, buttonsDown, mousePos persist across frames.
}

bool InputState::keyDown(Key key) const noexcept {
    const auto i = static_cast<std::size_t>(key);
    return i < keysDown.size() && keysDown[i];
}
bool InputState::keyPressed(Key key) const noexcept {
    const auto i = static_cast<std::size_t>(key);
    return i < keysPressed.size() && keysPressed[i];
}
bool InputState::keyReleased(Key key) const noexcept {
    const auto i = static_cast<std::size_t>(key);
    return i < keysReleased.size() && keysReleased[i];
}

bool InputState::mouseButtonDown(MouseButton button) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return i < buttonsDown.size() && buttonsDown[i];
}
bool InputState::mouseButtonPressed(MouseButton button) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return i < buttonsPressed.size() && buttonsPressed[i];
}
bool InputState::mouseButtonReleased(MouseButton button) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return i < buttonsReleased.size() && buttonsReleased[i];
}

bool InputState::gamepadConnected(std::size_t slot) const noexcept {
    return slot < gamepads.size() && gamepads[slot].connected;
}
bool InputState::gamepadButtonDown(GamepadButton button, std::size_t slot) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return slot < gamepads.size() && i < gamepads[slot].down.size() && gamepads[slot].down[i];
}
bool InputState::gamepadButtonPressed(GamepadButton button, std::size_t slot) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return slot < gamepads.size() && i < gamepads[slot].pressed.size() && gamepads[slot].pressed[i];
}
bool InputState::gamepadButtonReleased(GamepadButton button, std::size_t slot) const noexcept {
    const auto i = static_cast<std::size_t>(button);
    return slot < gamepads.size() && i < gamepads[slot].released.size() && gamepads[slot].released[i];
}
float InputState::gamepadAxis(GamepadAxis axis, std::size_t slot) const noexcept {
    const auto i = static_cast<std::size_t>(axis);
    return (slot < gamepads.size() && i < gamepads[slot].axes.size()) ? gamepads[slot].axes[i] : 0.0F;
}
StickPosition InputState::gamepadStick(GamepadStick stick, std::size_t slot) const noexcept {
    switch (stick) {
        case GamepadStick::Left:
            return radialDeadzone(
                StickPosition{gamepadAxis(GamepadAxis::LeftX, slot), gamepadAxis(GamepadAxis::LeftY, slot)});
        case GamepadStick::Right:
            return radialDeadzone(
                StickPosition{gamepadAxis(GamepadAxis::RightX, slot), gamepadAxis(GamepadAxis::RightY, slot)});
        case GamepadStick::Count:
            break;
    }
    return StickPosition{};
}

MousePosition InputState::mousePosition() const noexcept { return mousePos; }
MousePosition InputState::mouseDelta() const noexcept { return mouseMotion; }
MousePosition InputState::mouseWheel() const noexcept { return wheelAccum; }

}  // namespace engine::platform
