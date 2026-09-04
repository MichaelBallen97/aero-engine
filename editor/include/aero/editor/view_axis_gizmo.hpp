#pragma once
// Aero Engine — engine::editor view-axis gizmo (task E.1.3): the viewport-corner orientation widget's
// LAYOUT, DEPTH ORDER, HIT TEST, the six canonical camera poses, and the snap animation.
//
// PUBLIC, ImGui-free, entt-free AND render-free BY RULE, held by FILE PLACEMENT like every header
// under editor/include (2.1.3 D9) -- see editor_camera.hpp's header comment for the full reasoning
// (R12: a leaked `#include <imgui.h>` here would still compile; do not claim enforcement that does
// not exist).
//
// COLOUR IS NOT HERE, DELIBERATELY. ViewAxisBall carries `positive` (a fact about the axis) and
// `depth` (a fact about the camera); viewport_panel.cpp maps those two facts to fill, alpha, ring and
// label. That is 2.3.2's OverlayRole line, and it is what keeps an ImU32 out of a public header.
//
// THE WIDGET IS THE SAME PICTURE IN BOTH PROJECTION MODES. It orthographically projects the camera's
// own basis onto a fixed ring, because it displays ORIENTATION, which is a property of the rotation
// and not of the lens.

#include <aero/core/math.hpp>
#include <aero/editor/axis_palette.hpp>
#include <aero/editor/editor_camera.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

// ---- vocabulary ---------------------------------------------------------------------------------

enum class ViewAxis : std::uint8_t { PosX = 0, NegX, PosY, NegY, PosZ, NegZ };
inline constexpr std::size_t VIEW_AXIS_COUNT = 6;

enum class ViewAxisHit : std::uint8_t { None = 0, Axis, Center };

// ---- tuning constants (D3/D10/D16) -- LOGICAL POINTS, judged by the manual validation pass -------
// Every value is a TUNING value; each is named so a retune is a one-line change, and every tier-0
// case asserts a RELATIONSHIP (an ordering, a sign, an inclusion) rather than a magnitude -- the
// 2.3.1 rule, so retuning reddens nothing.
inline constexpr float VIEW_AXIS_RING_RADIUS_POINTS = 30.0F;   // widget centre -> a ball's centre
inline constexpr float VIEW_AXIS_BALL_RADIUS_POINTS = 8.0F;    // hit radius AND the un-hovered draw radius
inline constexpr float VIEW_AXIS_CENTER_RADIUS_POINTS = 6.0F;  // < BALL, so a collapsed ball keeps an annulus (D10)
inline constexpr float VIEW_AXIS_MARGIN_POINTS = 10.0F;        // inset from the image's top-right corner
inline constexpr float VIEW_AXIS_HOVER_GROWTH = 1.25F;         // DRAW only -- never the hit radius (D10)
inline constexpr float VIEW_AXIS_BACK_ALPHA = 0.55F;           // the back hemisphere's alpha multiplier
inline constexpr float VIEW_AXIS_MIN_IMAGE_POINTS = 140.0F;    // below this, in EITHER axis, the widget hides (D16)

// The widget's half-extent: the ring plus one ball radius. The box is 2x this on each side.
inline constexpr float VIEW_AXIS_HALF_EXTENT_POINTS = VIEW_AXIS_RING_RADIUS_POINTS + VIEW_AXIS_BALL_RADIUS_POINTS;

// ---- layout -------------------------------------------------------------------------------------

struct ViewAxisBall {
    Vec2 offsetPoints{};  // from the widget centre, y DOWN (ImGui screen space)
    float depth = 0.0F;   // dot(axis, camera.forward()): LARGER == FARTHER, < 0 == in front
    ViewAxis axis = ViewAxis::PosX;
    bool positive = true;  // filled + always lettered when true; ring + hover-lettered when false
};

struct ViewAxisLayout {
    Vec2 centerPoints{};                                    // SCREEN points, not image-relative
    std::array<ViewAxisBall, VIEW_AXIS_COUNT> balls{};      // in ViewAxis order
    std::array<std::uint8_t, VIEW_AXIS_COUNT> drawOrder{};  // indices into `balls`, FAR -> NEAR
    bool visible = false;                                   // false => nothing draws and nothing is hit
};

struct ViewAxisPick {
    ViewAxisHit kind = ViewAxisHit::None;
    ViewAxis axis = ViewAxis::PosX;  // meaningful IFF kind == Axis
};

// TOTAL. `visible` is false -- and every other field defaulted -- for a non-finite or non-positive
// image rect, and for one smaller than VIEW_AXIS_MIN_IMAGE_POINTS in either axis (D16).
[[nodiscard]] ViewAxisLayout viewAxisLayout(const EditorCamera& camera, Vec2 imageOriginPoints,
                                            Vec2 imageSizePoints) noexcept;

// The widget's screen rect, a PURE function of the image rect -- so overlayOwnsPress can ask about
// THIS frame rather than the last drawn one (D9). DEGENERATE (max.x <= min.x) when the widget is not
// visible, which is what makes "an empty rect owns nothing" hold with no second predicate.
void viewAxisRect(Vec2 imageOriginPoints, Vec2 imageSizePoints, Vec2& outMin, Vec2& outMax) noexcept;

// Centre first, then balls NEAR -> FAR; the first within its radius wins (D10). `None` for a
// non-finite point, for an invisible layout, and for a point inside the rect but on no target.
[[nodiscard]] ViewAxisPick viewAxisPickAt(const ViewAxisLayout& layout, Vec2 mousePoints) noexcept;

// ---- the canonical poses ------------------------------------------------------------------------

struct ViewPose {
    float yaw = 0.0F;
    float pitch = 0.0F;
};

// The pose whose forward() is -axis, i.e. the eye ON that axis looking back at the pivot (D4).
// Returned UNCLAMPED and in the camera's own yaw representation ((-PI, PI], so Back is +PI, never
// -PI): the caller applies it through the setters, which clamp. At the POLES yaw is a free parameter
// and snaps to the nearest multiple of HALF_PI -- a top view whose world axes are not square on
// screen is not a canonical view.
[[nodiscard]] ViewPose viewAxisPose(ViewAxis axis, float currentYaw) noexcept;

// setYaw + setPitch, in that order, both clamped. Shared by the panel and the tests so "apply a pose"
// has ONE implementation.
void applyViewPose(EditorCamera& camera, ViewPose pose) noexcept;

[[nodiscard]] char viewAxisLabel(ViewAxis axis) noexcept;       // 'X' | 'Y' | 'Z'
[[nodiscard]] Axis viewAxisPaletteKey(ViewAxis axis) noexcept;  // for axisColorSrgbBytes
[[nodiscard]] bool viewAxisIsPositive(ViewAxis axis) noexcept;

// ---- the animation ------------------------------------------------------------------------------

inline constexpr float VIEW_SNAP_SECONDS = 0.25F;
inline constexpr float VIEW_SNAP_EPSILON_RADIANS = 1.0e-4F;  // below this: instant, no animation (D7)

// The signed shortest arc from `from` to `to`, in [-PI, PI]. EXACTLY +PI for a 180-degree flip:
// std::remainder rounds the quotient half-to-even and PI/(2PI) == 0.5 rounds to 0, so a Front<->Back
// snap always turns the SAME way and a test can assert the sign. Returns 0 for any non-finite input.
[[nodiscard]] float shortestAngleDelta(float from, float to) noexcept;

// A running view snap. Holds the START pose and two SIGNED DELTAS -- never the endpoints, because
// lerping endpoints takes the 340-degree way round from 170 to -170 (D6, seed S4).
class ViewSnapAnimation {
public:
    // Retargets from (fromYaw, fromPitch). Leaves active() FALSE when both deltas are under
    // VIEW_SNAP_EPSILON_RADIANS, or when any input is non-finite -- the caller then applies the
    // target directly.
    void start(float fromYaw, float fromPitch, ViewPose target) noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept;

    // Advances and returns the pose to apply THIS frame. `deltaSeconds` is clamped into
    // [0, VIEW_SNAP_SECONDS]; a NON-FINITE delta FINISHES the animation rather than being treated as
    // zero, because a wedged animation would rewrite yaw and pitch every frame forever (D7). The
    // final call clamps t to exactly 1, so the pose lands EXACTLY on the target and active() goes
    // false in the same call. Calling it while !active() returns the target pose and changes nothing.
    [[nodiscard]] ViewPose advance(float deltaSeconds) noexcept;

private:
    float startYawValue = 0.0F;
    float startPitchValue = 0.0F;
    float yawDelta = 0.0F;
    float pitchDelta = 0.0F;
    // THE LANDING POSE, held so the final call is EXACT. `start + delta` is not bit-equal to the
    // endpoint in float -- a + (b - a) != b in general -- and pitch's endpoint is a CLAMP BOUNDARY
    // (+-MAX_PITCH) that callers compare with ==, so an ulp of drift there is a red test and a
    // "top" view that is not quite square. THE TWO ARE COMPUTED DIFFERENTLY, ON PURPOSE:
    //   targetYawValue   = startYaw + yawDelta -- the MONOTONE endpoint of the path, which is
    //                      congruent to the requested yaw modulo a full turn and which setYaw wraps
    //                      anyway. Storing the requested yaw instead would make the final frame jump
    //                      from 190 degrees to -170, which is the very discontinuity the signed-delta
    //                      design exists to avoid.
    //   targetPitchValue = the requested pitch, VERBATIM. Pitch never wraps, so there is no monotone
    //                      endpoint to preserve and the exact value is strictly better.
    // Holding these does NOT reintroduce the endpoint-lerp bug: every INTERMEDIATE frame is still
    // startYaw + yawDelta * s, and only the final call reads them.
    float targetYawValue = 0.0F;
    float targetPitchValue = 0.0F;
    float elapsedSeconds = 0.0F;
    bool running = false;
};

}  // namespace engine::editor
