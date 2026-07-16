// Aero Engine — input state fold (task 0.3.2). Pure engine logic: folds the Event stream into held +
// edge state. NO SDL here (the scancode/button/mod mapping lives in platform.cpp); this TU is what
// makes InputState testable with plain Events and no display.

#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>

#include <cstddef>

namespace engine::platform {

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

MousePosition InputState::mousePosition() const noexcept { return mousePos; }
MousePosition InputState::mouseDelta() const noexcept { return mouseMotion; }
MousePosition InputState::mouseWheel() const noexcept { return wheelAccum; }

}  // namespace engine::platform
