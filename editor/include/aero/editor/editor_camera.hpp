#pragma once
// Aero Engine — engine::editor::EditorCamera (task 2.3.1): the editor's OWN camera, a tool eye, never
// a document one. This is what the Viewport renders through instead of the scene's `Camera`
// component — orbit, pan, dolly and fly the scene without ever touching the World.
//
// PUBLIC and ImGui-free, entt-free and render-free BY RULE — held by FILE PLACEMENT, not by a guard
// (R12: aero_editor_shell_test links doctest, which puts vcpkg's shared include root on the compile
// line, so a leaked `#include <imgui.h>` here would still compile; do not claim enforcement that does
// not exist). It is render-free specifically because `aero::scene_render` is PRIVATE on
// aero_editor_core (editor/CMakeLists.txt) — naming a `render::` type here would break the tier-0
// shell test's compile outright (D2). `viewport_panel.cpp`, which is src-private and does see
// `aero::render` transitively, assembles the one `render::CameraView` at its single call site.
//
// This file lands the pure gesture half first (task 2.3.1 step 2); the EditorCamera model itself
// arrives in step 4 of the same task.

#include <aero/core/math.hpp>
#include <aero/editor/scene_bounds.hpp>

#include <cstdint>

namespace engine::editor {

// Task E.1.3. The EDITOR's eye only -- engine::Camera stays perspective-only by its own D2.
enum class ProjectionMode : std::uint8_t { Perspective = 0, Orthographic };

// Which continuous navigation gesture owns the camera right now (D5). Explicit uint8_t:
// performance-enum-size, like every enum in this tree.
enum class CameraGesture : std::uint8_t { None = 0, Orbit, Pan, Dolly, Fly };

// Which physical button is holding that gesture open. `None` iff gesture == None.
enum class CameraButton : std::uint8_t { None = 0, Left, Right, Middle };

// LATCHED ACROSS FRAMES by the panel: rule 1 below needs the previous frame's value.
struct CameraGestureState {
    CameraGesture gesture = CameraGesture::None;
    CameraButton button = CameraButton::None;
    bool operator==(const CameraGestureState&) const noexcept = default;
};

// One frame of raw button/modifier state, as the panel reads it from ImGui. `...Pressed` means the
// button went !down -> down THIS frame (ImGui::IsMouseClicked); `...Down` means it is held
// (ImGui::IsMouseDown). `ctrlOrCmd` is io.KeyCtrl ALONE -- ImGui has already remapped Cmd on macOS
// (imgui.h:2683), so `io.KeyCtrl || io.KeySuper` would ALSO fire on physical Ctrl on macOS and on
// the Super key elsewhere (F22b).
struct CameraGestureInput {
    bool hovered = false;
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    bool leftPressed = false;
    bool rightPressed = false;
    bool middlePressed = false;
    bool alt = false;
    bool ctrlOrCmd = false;
};

// PURE (D5) -- no ImGui type reaches this function, which is what lets a tier-0 test drive the whole
// matrix with no window, no GPU and no ImGui context (the clickSelectionAction precedent).
// Three rules, IN ORDER:
//   1. CONTINUE -- current.gesture != None and the button named by current.button is still down
//      -> return `current` UNCHANGED. Modifier changes mid-gesture NEVER re-classify: letting go of
//      Alt halfway through an orbit keeps orbiting, which is what every 3D application does.
//      Deliberately does NOT consult `hovered`, so a drag that leaves the panel keeps going (E3).
//   2. END -- otherwise the gesture is over; if !in.hovered -> {None, None}.
//   3. START -- only on a FRESH PRESS while hovered, first match wins:
//        rightPressed && alt              -> {Dolly,  Right}
//        rightPressed                     -> {Fly,    Right}
//        middlePressed                    -> {Pan,    Middle}
//        leftPressed && alt && ctrlOrCmd  -> {Pan,    Left}     (the Mac-trackpad path)
//        leftPressed && alt               -> {Orbit,  Left}
//        otherwise                        -> {None,   None}
// The fresh-press requirement is what makes rule 3 independent of ImGui's hover subtleties (F23) and
// what guarantees a PLAIN LMB press can never retroactively become an orbit -- plain LMB is
// deliberately unbound and belongs to 2.3.2's click-picking (AC-13).
[[nodiscard]] CameraGestureState nextGesture(CameraGestureState current, const CameraGestureInput& in) noexcept;

// ---- the D8/C7 tuning constants -----------------------------------------------------------------
// Every value here is a TUNING value, judged by the human pass (editor/VALIDATION.md); each is named
// so retuning is a one-line change, and every tier-0 case asserts a RELATIONSHIP (a ratio, a sign, an
// invariance) rather than a magnitude, so retuning reddens nothing. Sensitivities are per logical
// POINT, not per pixel (D15) -- see viewport_panel.cpp's `.viewportHeightPoints = avail.y` comment for
// the one call site this matters at.
inline constexpr Vec3 DEFAULT_PIVOT = Vec3::zero();  // the seeded Cube's position
inline constexpr float DEFAULT_DISTANCE = 8.0F;      // frames a unit cube with headroom at 60deg
// The _RADIANS suffix on these two is LOAD-BEARING, not decoration: <wingdi.h> defines DEFAULT_PITCH
// as a font-pitch macro (0). A macro ignores namespaces, so `engine::editor::DEFAULT_PITCH` was
// substituted to `0` at every USE site on Windows while this header itself still compiled -- the
// Windows CI lane caught it as a wrong VALUE, not a build error. Do not drop the suffix. The yaw
// constant carries it too, to keep the pair symmetric and to match the yaw/pitchRadians members.
inline constexpr float DEFAULT_YAW_RADIANS = radians(30.0F);     // a right-front 3/4 view, not flat
inline constexpr float DEFAULT_PITCH_RADIANS = radians(-20.0F);  // looking slightly down
inline constexpr float DEFAULT_FOV_Y = radians(60.0F);           // matches engine::Camera's default
inline constexpr float DEFAULT_NEAR = 0.1F;                      // matches engine::Camera's default
inline constexpr float DEFAULT_FAR = 1000.0F;                    // >> Camera's 100: flying a 100-unit box takes ~20s
inline constexpr float DEFAULT_FLY_SPEED = 5.0F;                 // world units / second
inline constexpr float MIN_DISTANCE = 0.05F;
inline constexpr float MAX_DISTANCE = 10000.0F;
// EXACTLY HALF_PI since task E.1.3, and the retune is deliberate. The Top and Bottom views route
// through setPitch, so with any headroom here a "top" view is off vertical by that headroom -- at
// the old HALF_PI - 0.01F that is 0.572957 degrees, which turns a 10-unit vertical pillar into a
// 0.10-unit lateral streak, about 1.1% of a top view's framed height at distance 8. The headroom's
// stated purpose ("so the basis never degenerates") does not apply to THIS camera: the composition
// is yaw-outer / pitch-inner, so right() = qYaw * {1,0,0} is INDEPENDENT of pitch (see
// editor_camera.cpp's rotation() comment), viewMatrix() is inverse(compose(...)) with no lookAt and
// no up vector, and nothing in the file divides by cos(pitch). The one behaviour that changes is
// orbiting AT the pole, where a horizontal drag spins the image about the view axis -- which it
// already did within the old 0.57-degree cone.
inline constexpr float MAX_PITCH = HALF_PI;               // exactly +-90 degrees (task E.1.3)
inline constexpr float ORBIT_RADIANS_PER_POINT = 0.005F;  // ~0.29deg/pt; a 300-pt drag sweeps ~86deg
// D16, DERIVED: +yaw turns the view LEFT and +pitch looks UP, while ImGui's +y is screen-DOWN --
// hence both signs are -1. Named so a post-human-pass flip is one character with a reddening test.
inline constexpr float ORBIT_YAW_SIGN = -1.0F;
inline constexpr float ORBIT_PITCH_SIGN = -1.0F;
inline constexpr float DOLLY_STEP = 1.15F;               // one notch = +-15% distance
inline constexpr float DOLLY_NOTCHES_PER_POINT = 0.03F;  // a 100-pt drag ~= 3 notches
inline constexpr float FLY_SPEED_STEP = 1.15F;
inline constexpr float MIN_FLY_SPEED = 0.05F;
inline constexpr float MAX_FLY_SPEED = 500.0F;
inline constexpr float FLY_FAST_MULTIPLIER = 4.0F;  // Shift
inline constexpr float FOCUS_MARGIN = 1.3F;
inline constexpr float FOCUS_MIN_RADIUS = 0.35F;  // a point-sized entity (a light) still gets a sane distance
// C7: INV-5 requires 0 < nearPlane < farPlane and a non-degenerate fov after EVERY public call, and
// the setters are public -- these four give clampState() something to clamp to.
inline constexpr float MIN_FOV_Y = radians(1.0F);
inline constexpr float MAX_FOV_Y = radians(179.0F);
inline constexpr float MIN_NEAR_PLANE = 1.0e-3F;
inline constexpr float MIN_DEPTH_RANGE = 1.0e-3F;  // the floor on far - near

// Everything one frame of navigation needs, in LOGICAL POINTS (D15). The panel fills it from ImGui; a
// test fills it by hand. `gesture` selects which fields matter -- the rest are ignored.
struct CameraInput {
    Vec2 dragDelta{};                   // io.MouseDelta this frame, POINTS
    float wheelNotches = 0.0F;          // io.MouseWheel this frame
    float viewportHeightPoints = 1.0F;  // GetContentRegionAvail().y -- NEVER drawExtent().height
    CameraGesture gesture = CameraGesture::None;
    bool moveForward = false;
    bool moveBack = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;    // Fly only; WORLD +Y (E key)
    bool moveDown = false;  // Fly only; WORLD -Y (Q key)
    bool fast = false;      // Shift
};

// The editor's own eye. STATE IS FOUR NUMBERS (D17): {pivot, distance, yaw, pitch}, with
//   position() == pivot - forward() * distance      ALWAYS (INV-1)
// so there is no second stored representation of the eye to drift. Orbit changes yaw/pitch; pan
// changes pivot; dolly changes distance; fly changes yaw/pitch and then RESTORES the eye.
//
// RENDER-FREE ON PURPOSE (D2): this class never names a render:: type. viewport_panel.cpp -- which is
// src-private and does see render transitively -- assembles the render::CameraView at the one call
// site. Putting one here would break aero_editor_shell_test's compile outright.
class EditorCamera {
public:
    void update(const CameraInput& in, float deltaSeconds) noexcept;  // TOTAL (AC-18)
    void focusOn(const Aabb& bounds, float aspect) noexcept;          // INSTANT (D12)
    void reset() noexcept;                                            // the D8 default pose

    [[nodiscard]] Vec3 position() const noexcept;  // pivot - forward()*distance (INV-1)
    [[nodiscard]] Quat rotation() const noexcept;  // yawQ * pitchQ (D16 -- no roll, ever)
    [[nodiscard]] Vec3 forward() const noexcept;   // rotation() * {0,0,-1}
    [[nodiscard]] Vec3 right() const noexcept;     // rotation() * {1,0,0}; right().y == 0 (INV-2)
    [[nodiscard]] Vec3 up() const noexcept;        // rotation() * {0,1,0}
    [[nodiscard]] Mat4 viewMatrix() const;         // inverse(compose({position, rotation, one}))
    [[nodiscard]] Mat4 projectionMatrix(float aspect) const;

    // ---- task E.1.3: the projection mode -------------------------------------------------------
    // The EDITOR's own lens, session state, persisted nowhere (E.4.x owns per-user preferences).
    // reset() restores Perspective, because a mode surviving the D8 default pose would be a state
    // the defaults do not describe.
    [[nodiscard]] ProjectionMode projectionMode() const noexcept;
    void setProjectionMode(ProjectionMode value) noexcept;

    // The orthographic half-height: distance * tan(fovY/2), which is the world height the
    // perspective frustum covers AT THE PIVOT'S DEPTH. Writing it this way is what makes the toggle
    // visually continuous -- everything on the plane through the pivot keeps its screen position,
    // and only depth-dependent parallax changes. THE ONE PLACE THAT FORMULA IS WRITTEN.
    [[nodiscard]] float orthoHalfHeight() const noexcept;

    [[nodiscard]] Vec3 pivot() const noexcept;
    [[nodiscard]] float distance() const noexcept;
    [[nodiscard]] float yaw() const noexcept;
    [[nodiscard]] float pitch() const noexcept;
    [[nodiscard]] float fovYRadians() const noexcept;
    [[nodiscard]] float nearPlane() const noexcept;
    [[nodiscard]] float farPlane() const noexcept;
    [[nodiscard]] float flySpeed() const noexcept;

    // Each CLAMPS through the same clampState() the gestures use, so INV-5 holds after every public
    // call, from a test as much as from a drag.
    void setPivot(Vec3 value) noexcept;
    void setDistance(float value) noexcept;
    void setYaw(float value) noexcept;
    void setPitch(float value) noexcept;
    void setFovYRadians(float value) noexcept;
    void setNearPlane(float value) noexcept;
    void setFarPlane(float value) noexcept;
    void setFlySpeed(float value) noexcept;

private:  // C3: members renamed so they never collide with the accessors above
    void applyLook(Vec2 drag) noexcept;
    void applyPan(Vec2 drag, float viewportHeightPoints) noexcept;
    void applyDolly(float notches) noexcept;
    void applyFly(Vec2 drag, const CameraInput& in, float wheelNotches, float deltaSeconds) noexcept;
    [[nodiscard]] static float dragNotches(Vec2 drag) noexcept;
    void clampState() noexcept;
    [[nodiscard]] bool stateIsFinite() const noexcept;

    Vec3 pivotPoint = DEFAULT_PIVOT;
    float orbitDistance = DEFAULT_DISTANCE;
    float yawRadians = DEFAULT_YAW_RADIANS;
    float pitchRadians = DEFAULT_PITCH_RADIANS;
    float fovYValue = DEFAULT_FOV_Y;
    float nearPlaneValue = DEFAULT_NEAR;
    float farPlaneValue = DEFAULT_FAR;
    float flySpeedValue = DEFAULT_FLY_SPEED;
    // C3 again: the MEMBER takes the distinct name on an accessor collision (the
    // tonemapParamsValue / gridEnabledValue precedent one layer up).
    ProjectionMode projectionModeValue = ProjectionMode::Perspective;
};

}  // namespace engine::editor
