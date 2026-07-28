// editor/src/editor_camera.cpp — task 2.3.1: nextGesture's three rules, literally in order. This
// step lands only the pure gesture half (editor_camera.hpp's CameraGesture/CameraButton/
// CameraGestureState/CameraGestureInput/nextGesture); the EditorCamera model itself arrives in step 4.
#include <aero/editor/editor_camera.hpp>

namespace engine::editor {

namespace {

// Whether the button THIS gesture is latched to is still held down this frame.
[[nodiscard]] bool buttonStillDown(CameraButton button, const CameraGestureInput& in) noexcept {
    switch (button) {
        case CameraButton::Left:
            return in.leftDown;
        case CameraButton::Right:
            return in.rightDown;
        case CameraButton::Middle:
            return in.middleDown;
        case CameraButton::None:
            return false;
    }
    return false;  // unreachable; keeps every compiler happy about a missing return
}

}  // namespace

CameraGestureState nextGesture(CameraGestureState current, const CameraGestureInput& in) noexcept {
    // Rule 1 -- CONTINUE. Deliberately does NOT consult `in.hovered`: a drag that leaves the panel
    // keeps going (E3), and a modifier change mid-gesture never re-classifies (D5 rule 1).
    if (current.gesture != CameraGesture::None && buttonStillDown(current.button, in)) {
        return current;
    }

    // Rule 2 -- END. The gesture is over; only a fresh press while hovered can start a new one below.
    if (!in.hovered) {
        return CameraGestureState{};
    }

    // Rule 3 -- START, only on a FRESH PRESS while hovered. First match wins -- an if/else ladder,
    // not a switch, so bugprone-branch-clone has nothing to fold.
    if (in.rightPressed && in.alt) {
        return CameraGestureState{.gesture = CameraGesture::Dolly, .button = CameraButton::Right};
    }
    if (in.rightPressed) {
        return CameraGestureState{.gesture = CameraGesture::Fly, .button = CameraButton::Right};
    }
    if (in.middlePressed) {
        return CameraGestureState{.gesture = CameraGesture::Pan, .button = CameraButton::Middle};
    }
    if (in.leftPressed && in.alt && in.ctrlOrCmd) {
        return CameraGestureState{.gesture = CameraGesture::Pan, .button = CameraButton::Left};
    }
    if (in.leftPressed && in.alt) {
        return CameraGestureState{.gesture = CameraGesture::Orbit, .button = CameraButton::Left};
    }
    // A plain LMB press (or any other combination) starts NO gesture -- AC-13: plain LMB is reserved
    // for 2.3.2's click-picking.
    return CameraGestureState{};
}

}  // namespace engine::editor
