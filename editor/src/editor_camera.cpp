// editor/src/editor_camera.cpp — task 2.3.1: nextGesture's three rules (step 2), and the
// orbit/pan/dolly/fly EditorCamera model (step 4).
#include <aero/editor/editor_camera.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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

namespace {

// F8b -- there is NO `isFinite` in the engine's public math surface, and this task does not add one:
// std::isfinite appears only inside private .cpp TUs elsewhere in the tree. A one-consumer utility
// does not justify widening ADR-005's public surface from a Phase-2 editor task.
[[nodiscard]] bool allFinite(float v) noexcept { return std::isfinite(v); }
[[nodiscard]] bool allFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }
[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

// D16/D17, exactly: yaw about WORLD Y (outer), pitch about LOCAL X (inner) -- so
// right() = qYaw * (qPitch * {1,0,0}) = qYaw * {1,0,0}, because {1,0,0} IS the pitch axis, and a
// rotation about world Y cannot give it a Y component. Roll is therefore IMPOSSIBLE, not merely
// unlikely (INV-2).
Quat EditorCamera::rotation() const noexcept {
    return fromAxisAngle(Vec3::unitY(), yawRadians) * fromAxisAngle(Vec3::unitX(), pitchRadians);
}
Vec3 EditorCamera::forward() const noexcept { return rotation() * Vec3{0.0F, 0.0F, -1.0F}; }
Vec3 EditorCamera::right() const noexcept { return rotation() * Vec3::unitX(); }
Vec3 EditorCamera::up() const noexcept { return rotation() * Vec3::unitY(); }
Vec3 EditorCamera::position() const noexcept { return pivotPoint - forward() * orbitDistance; }

Mat4 EditorCamera::viewMatrix() const { return inverse(compose({.translation = position(), .rotation = rotation()})); }

// task E.1.3: the ONE place `distance * tan(fovY/2)` is written. It is the world half-height the
// perspective frustum covers at the PIVOT'S depth -- which is what makes the mode toggle visually
// continuous for everything on the plane through the pivot (D11).
float EditorCamera::orthoHalfHeight() const noexcept { return orbitDistance * std::tan(0.5F * fovYValue); }

Mat4 EditorCamera::projectionMatrix(float aspect) const {
    const float safeAspect = (allFinite(aspect) && aspect > 0.0F) ? aspect : 1.0F;  // C4
    if (projectionModeValue == ProjectionMode::Orthographic) {
        // THE GUARD THE PERSPECTIVE ARM DOES NOT NEED. ortho() asserts right > left and top > bottom,
        // and BOTH extents carry fovY -- which clampState deliberately leaves NaN for stateIsFinite()
        // to sweep on the next update(). perspective()'s asserts read aspect (sanitized above), zNear
        // and zFar, NEVER fovY, so a poisoned fov merely produces a garbage matrix there and would
        // ABORT a Debug build here. 1.0 is an arbitrary finite fallback for a state the next update()
        // resets anyway.
        const float rawHalfHeight = orthoHalfHeight();
        const float halfHeight = (std::isfinite(rawHalfHeight) && rawHalfHeight > 0.0F) ? rawHalfHeight : 1.0F;
        const float halfWidth = halfHeight * safeAspect;
        return ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, nearPlaneValue, farPlaneValue);
    }
    return perspective(fovYValue, safeAspect, nearPlaneValue, farPlaneValue);
    // Do NOT flip Y for Vulkan -- math/transform.hpp's own warning: SDL converts behind the scenes and
    // a proj[1][1] *= -1 here would double-flip the image. It applies to BOTH arms.
}

Vec3 EditorCamera::pivot() const noexcept { return pivotPoint; }
float EditorCamera::distance() const noexcept { return orbitDistance; }
float EditorCamera::yaw() const noexcept { return yawRadians; }
float EditorCamera::pitch() const noexcept { return pitchRadians; }
float EditorCamera::fovYRadians() const noexcept { return fovYValue; }
float EditorCamera::nearPlane() const noexcept { return nearPlaneValue; }
float EditorCamera::farPlane() const noexcept { return farPlaneValue; }
float EditorCamera::flySpeed() const noexcept { return flySpeedValue; }
ProjectionMode EditorCamera::projectionMode() const noexcept { return projectionModeValue; }

// DELIBERATELY does NOT call clampState(), unlike every setter below it: the mode is not a clamped
// quantity and calling it here would imply otherwise. Nothing else on the camera changes either --
// pivot, distance, yaw, pitch, fov, near, far and flySpeed are all untouched (PM1 asserts it).
void EditorCamera::setProjectionMode(ProjectionMode value) noexcept { projectionModeValue = value; }

// Setters are deliberately private-state mutators with a public face, not friends: a poisoned value
// routes through the SAME clampState() the gestures use, so INV-5 holds after every public call --
// including a directly-poisoned NaN, which the next update() then sweeps and resets (§6.1 case 10).
void EditorCamera::setPivot(Vec3 value) noexcept {
    pivotPoint = value;
    clampState();
}
void EditorCamera::setDistance(float value) noexcept {
    orbitDistance = value;
    clampState();
}
void EditorCamera::setYaw(float value) noexcept {
    yawRadians = value;
    clampState();
}
void EditorCamera::setPitch(float value) noexcept {
    pitchRadians = value;
    clampState();
}
void EditorCamera::setFovYRadians(float value) noexcept {
    fovYValue = value;
    clampState();
}
void EditorCamera::setNearPlane(float value) noexcept {
    nearPlaneValue = value;
    clampState();
}
void EditorCamera::setFarPlane(float value) noexcept {
    farPlaneValue = value;
    clampState();
}
void EditorCamera::setFlySpeed(float value) noexcept {
    flySpeedValue = value;
    clampState();
}

float EditorCamera::dragNotches(Vec2 drag) noexcept { return (drag.x - drag.y) * DOLLY_NOTCHES_PER_POINT; }

void EditorCamera::applyLook(Vec2 drag) noexcept {
    yawRadians += ORBIT_YAW_SIGN * drag.x * ORBIT_RADIANS_PER_POINT;
    pitchRadians += ORBIT_PITCH_SIGN * drag.y * ORBIT_RADIANS_PER_POINT;
}

void EditorCamera::applyPan(Vec2 drag, float viewportHeightPoints) noexcept {
    const float worldPerPoint = 2.0F * orbitDistance * std::tan(0.5F * fovYValue) / viewportHeightPoints;
    pivotPoint += (right() * -drag.x + up() * drag.y) * worldPerPoint;
}

void EditorCamera::applyDolly(float notches) noexcept { orbitDistance *= std::pow(DOLLY_STEP, -notches); }

void EditorCamera::applyFly(Vec2 drag, const CameraInput& in, float wheelNotches, float deltaSeconds) noexcept {
    const Vec3 eye = position();
    applyLook(drag);
    clampState();  // ORDERING IS LOAD-BEARING: the pitch clamp must run BETWEEN the look and the
                   // restore below, or a clamped pitch would silently MOVE the eye.
    pivotPoint = eye + forward() * orbitDistance;  // the eye is invariant; the pivot follows the gaze

    Vec3 move{};
    if (in.moveForward) {
        move += forward();
    }
    if (in.moveBack) {
        move -= forward();
    }
    if (in.moveRight) {
        move += right();
    }
    if (in.moveLeft) {
        move -= right();
    }
    if (in.moveUp) {
        move += Vec3::unitY();  // WORLD +Y, regardless of pitch (E key)
    }
    if (in.moveDown) {
        move -= Vec3::unitY();  // WORLD -Y (Q key)
    }
    const float speed = flySpeedValue * (in.fast ? FLY_FAST_MULTIPLIER : 1.0F);
    pivotPoint += normalizeOrZero(move) * (speed * deltaSeconds);  // normalized: forward+right is NOT
                                                                   // sqrt(2) faster

    flySpeedValue *= std::pow(FLY_SPEED_STEP, wheelNotches);  // the wheel NEVER reaches applyDolly here
}

void EditorCamera::clampState() noexcept {
    orbitDistance = std::clamp(orbitDistance, MIN_DISTANCE, MAX_DISTANCE);
    pitchRadians = std::clamp(pitchRadians, -MAX_PITCH, MAX_PITCH);
    if (std::isfinite(yawRadians)) {                      // remainder(inf, x) is NaN -- guard first
        yawRadians = std::remainder(yawRadians, TWO_PI);  // -> [-PI, PI]
        if (yawRadians <= -PI) {
            yawRadians = PI;  // -> (-PI, PI]  (AC-10, exactly)
        }
    }
    fovYValue = std::clamp(fovYValue, MIN_FOV_Y, MAX_FOV_Y);
    nearPlaneValue = std::max(nearPlaneValue, MIN_NEAR_PLANE);
    // BOTH floors, because MIN_DEPTH_RANGE alone stops holding INV-5's `near < far`: 1.0e-3F is below
    // half an ULP of a float at magnitudes >= 32768, so `near + MIN_DEPTH_RANGE == near` EXACTLY
    // there and far would silently land ON near. Nothing downstream survives that -- perspective()
    // asserts zFar > zNear in Debug and divides by zFar - zNear == 0 in Release -- and
    // stateIsFinite() cannot catch it, because BOTH members are perfectly finite. nextafter alone is
    // not the answer either: it gives 100.000008 where MIN_DEPTH_RANGE gives 100.001, needlessly
    // collapsing the usable depth range at ordinary magnitudes. Take the max of both floors.
    // The one input whose result is not finite is near == FLT_MAX (no float is greater), which lands
    // +inf in far -- and that the stateIsFinite() sweep below every caller DOES catch.
    farPlaneValue = std::max({farPlaneValue, nearPlaneValue + MIN_DEPTH_RANGE,
                              std::nextafter(nearPlaneValue, std::numeric_limits<float>::infinity())});
    flySpeedValue = std::clamp(flySpeedValue, MIN_FLY_SPEED, MAX_FLY_SPEED);
    // std::clamp does NOT sanitize NaN -- `v < lo` and `hi < v` are both false for NaN, so it returns
    // NaN unchanged. That is BY DESIGN here: stateIsFinite() immediately after every caller of this
    // function is what catches it. Do not "fix" clamp; do not drop the sweep.
}

bool EditorCamera::stateIsFinite() const noexcept {
    return allFinite(pivotPoint) && allFinite(orbitDistance) && allFinite(yawRadians) && allFinite(pitchRadians) &&
           allFinite(fovYValue) && allFinite(nearPlaneValue) && allFinite(farPlaneValue) && allFinite(flySpeedValue);
}

void EditorCamera::reset() noexcept {
    pivotPoint = DEFAULT_PIVOT;
    orbitDistance = DEFAULT_DISTANCE;
    yawRadians = DEFAULT_YAW_RADIANS;
    pitchRadians = DEFAULT_PITCH_RADIANS;
    fovYValue = DEFAULT_FOV_Y;
    nearPlaneValue = DEFAULT_NEAR;
    farPlaneValue = DEFAULT_FAR;
    flySpeedValue = DEFAULT_FLY_SPEED;
    projectionModeValue = ProjectionMode::Perspective;  // task E.1.3: reset() IS the D8 default pose,
                                                        // and a mode surviving it would be a state the
                                                        // defaults do not describe
}

void EditorCamera::update(const CameraInput& in, float deltaSeconds) noexcept {
    // Guard 1 (AC-18, C4): reject non-finite INPUTS per field, before they can touch state. isfinite
    // AND the sign -- the negated `>` alone lets +inf through (+inf > 0.0F is true).
    const Vec2 drag = allFinite(in.dragDelta) ? in.dragDelta : Vec2::zero();
    const float wheel = allFinite(in.wheelNotches) ? in.wheelNotches : 0.0F;
    const float height =
        (allFinite(in.viewportHeightPoints) && in.viewportHeightPoints > 0.0F) ? in.viewportHeightPoints : 1.0F;
    const float dt = (allFinite(deltaSeconds) && deltaSeconds > 0.0F) ? deltaSeconds : 0.0F;

    switch (in.gesture) {
        case CameraGesture::Orbit:
            applyLook(drag);
            break;
        case CameraGesture::Pan:
            applyPan(drag, height);
            break;
        case CameraGesture::Dolly:
            applyDolly(dragNotches(drag));
            break;
        case CameraGesture::Fly:
            applyFly(drag, in, wheel, dt);
            break;
        case CameraGesture::None:
            break;
    }
    // The wheel ALWAYS dollies -- hovering, orbiting or panning -- EXCEPT while flying, where it is
    // the speed control applyFly has already consumed (AC-8). Spelled ONCE, here.
    if (in.gesture != CameraGesture::Fly) {
        applyDolly(wheel);
    }

    clampState();  // INV-5
    if (!stateIsFinite()) {
        reset();  // last-resort recovery: a poisoned camera must not persist across frames (E11) --
                  // the only alternative a user would have is restarting the editor
    }
}

void EditorCamera::focusOn(const Aabb& bounds, float aspect) noexcept {
    if (!bounds.valid()) {
        return;  // the CALLER decides the fallback (viewport_panel.cpp's focusSelection)
    }
    const float safeAspect = (allFinite(aspect) && aspect > 0.0F) ? aspect : 1.0F;  // C4
    const float radius = std::max(bounds.radius() * FOCUS_MARGIN, FOCUS_MIN_RADIUS);
    const float halfY = 0.5F * fovYValue;
    const float halfX = std::atan(std::tan(halfY) * safeAspect);
    const float halfFov = std::min(halfY, halfX);  // fit the TIGHTER axis (AC-16, E21)
    pivotPoint = bounds.center();
    orbitDistance = radius / std::sin(halfFov);
    clampState();  // yaw/pitch DELIBERATELY untouched
    if (!stateIsFinite()) {
        reset();  // total, independently of the Aabb
    }
}

}  // namespace engine::editor
