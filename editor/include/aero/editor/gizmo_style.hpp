#pragma once
// Aero Engine — engine::editor gizmo style (task E.1.5): the PURE half of the transform-gizmo restyle.
// Every ImGuizmo type stops at editor/src/viewport_panel.cpp, exactly as gizmo.hpp's tool-state model
// does; nothing here names one, so a tier-0 test drives the whole model with no window, no GPU and no
// ImGui context.
//
// ImGui-FREE, entt-FREE, render-FREE and IMGUIZMO-FREE BY RULE, held by FILE PLACEMENT, not by a guard
// (R12 -- gizmo.hpp's own sentence, unchanged). It exposes sRGB BYTES, like axis_palette.hpp, and the
// one call site that needs an ImVec4 builds it there.
//
// WHAT THIS IS NOT: a theme. E.6.1's EditorTheme is specified to become the central owner of palette
// values; every constant below is named so that folding it in is a one-line move, and none of them is
// stated anywhere else in the tree. The three AXIS colours are not stated here either -- they are
// DERIVED from axis_palette.hpp, which is what makes the gizmo, the grid and the corner widget the same
// bytes (E.1.2 / E.1.3 handoff).

#include <aero/core/math.hpp>
#include <aero/editor/axis_palette.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

// sRGB display bytes, r g b a -- axis_palette.hpp's convention one channel wider, because ImGuizmo's
// planes, highlight and drag chrome carry alpha and the axes do not.
using Rgba8 = std::array<std::uint8_t, 4>;

// ONE ENTRY PER ImGuizmo::COLOR, IN ImGuizmo's ORDER, named for what each slot actually paints (read at
// the pinned port's source -- the header's one-word comments are not enough to tell that PLANE_X is a
// FILL whose 1-px outline is DIRECTION_X, or that INACTIVE replaces all seven draw colours at once).
// viewport_panel.cpp holds the ONLY table mapping this enum to ImGuizmo::COLOR, and carries the
// static_assert against ImGuizmo::COLOR::COUNT, so a port bump that adds a colour is a compile error
// there rather than a silently defaulted slot.
enum class GizmoColor : std::uint8_t {
    AxisX = 0,        // DIRECTION_X: the X shaft, its head, the X rotate ring, the X scale disc
    AxisY,            // DIRECTION_Y
    AxisZ,            // DIRECTION_Z
    PlaneX,           // PLANE_X: the FILL of the YZ plane quad; its outline is always DIRECTION_X
    PlaneY,           // PLANE_Y
    PlaneZ,           // PLANE_Z
    Highlight,        // SELECTION: the hovered handle, and the dragged one for the whole drag
    Inactive,         // INACTIVE: every handle at once while ImGuizmo::Enable(false) is in force
    TranslationLine,  // TRANSLATION_LINE: the drag-start-to-now line and its two end rings
    ScaleLine,        // SCALE_LINE: the ghost of the un-scaled axis during a scale drag
    RotationBorder,   // ROTATION_USING_BORDER: the outline of the swept sector during a rotate drag
    RotationFill,     // ROTATION_USING_FILL: that sector's fill
    HatchedAxis,      // HATCHED_AXIS_LINES: a flipped axis's stub -- UNREACHABLE while flip is off
    Text,             // TEXT: the drag readout ("X : 1.250")
    TextShadow,       // TEXT_SHADOW: its one-point offset shadow
};
inline constexpr std::size_t GIZMO_COLOR_COUNT = 15;
static_assert(static_cast<std::size_t>(GizmoColor::TextShadow) + 1U == GIZMO_COLOR_COUNT);

// Mirrors ImGuizmo::Style FIELD FOR FIELD, in our vocabulary and in POINTS -- ImGui's coordinate unit,
// the same unit SetRect and io.MousePos use (2.3.3 D18), and the unit every hit tolerance in the
// library is a literal in. The two "circle size" fields are RADII.
struct GizmoStyle {
    float translationLineThicknessPoints = 0.0F;
    float translationArrowSizePoints = 0.0F;         // the head is 2x this long and 2x this wide
    float rotationLineThicknessPoints = 0.0F;        // the three axis rings
    float rotationScreenRingThicknessPoints = 0.0F;  // ImGuizmo's RotationOuterLineThickness: the white ring
    float scaleLineThicknessPoints = 0.0F;
    float scaleDiscRadiusPoints = 0.0F;             // ImGuizmo's ScaleLineCircleSize
    float hatchedAxisThicknessPoints = 0.0F;        // <= 0 disables the hatching outright (ImGuizmo.cpp:1389)
    float centerDiscRadiusPoints = 0.0F;            // ImGuizmo's CenterCircleSize
    std::array<Rgba8, GIZMO_COLOR_COUNT> colors{};  // indexed by GizmoColor
    bool operator==(const GizmoStyle&) const noexcept = default;
};

// ---- tuning values ------------------------------------------------------------------------------
// Judged on the manual validation pass (rows 1-6 and 10 of the page). Each is named so a retune is a
// one-line change; every tier-0 case asserts a RELATIONSHIP, never a magnitude (gizmo.hpp's rule).

inline constexpr float GIZMO_TRANSLATION_LINE_THICKNESS_POINTS = 4.0F;
inline constexpr float GIZMO_TRANSLATION_ARROW_SIZE_POINTS = 10.0F;  // a 20 x 20 point cone silhouette
inline constexpr float GIZMO_ROTATION_LINE_THICKNESS_POINTS = 3.0F;
inline constexpr float GIZMO_ROTATION_SCREEN_RING_THICKNESS_POINTS = 2.0F;  // thinner than the axis rings
inline constexpr float GIZMO_SCALE_LINE_THICKNESS_POINTS = 4.0F;
inline constexpr float GIZMO_SCALE_DISC_RADIUS_POINTS = 7.0F;
inline constexpr float GIZMO_HATCHED_AXIS_THICKNESS_POINTS = 0.0F;  // dead by construction: flip is off
inline constexpr float GIZMO_CENTER_DISC_RADIUS_POINTS = 7.0F;
// NOT a tuning value: the library's hit test for the centre disc is a hard-coded +/-10-point square
// (ImGuizmo.cpp:1132-1133), so a disc drawn larger than this lies about where it can be grabbed. GS3
// asserts GIZMO_CENTER_DISC_RADIUS_POINTS <= this. Re-read those two lines at every port bump.
inline constexpr float GIZMO_CENTER_HIT_HALF_EXTENT_POINTS = 10.0F;

inline constexpr std::uint8_t GIZMO_PLANE_FILL_ALPHA = 115;  // the library's 38 % read as absent
// OPAQUE, and deliberately NOT E.1.4's amber (255,176,64): the gizmo's origin sits inside the selected
// object, so a hot handle in the outline's colour would read as part of the outline. GS2 asserts the
// gap against SELECTION_OUTLINE_PRIMARY_DEFAULT and against all three axis colours.
inline constexpr Rgba8 GIZMO_HIGHLIGHT_SRGB{255U, 232U, 64U, 255U};
inline constexpr std::uint8_t GIZMO_ROTATION_FILL_ALPHA = 96;
inline constexpr Rgba8 GIZMO_INACTIVE_SRGB{153U, 153U, 153U, 153U};  // achromatic: r == g == b
inline constexpr Rgba8 GIZMO_TRANSLATION_LINE_SRGB{220U, 220U, 220U, 200U};
inline constexpr Rgba8 GIZMO_SCALE_LINE_SRGB{150U, 150U, 150U, 255U};  // the library's 0.25 grey vanished
inline constexpr Rgba8 GIZMO_HATCHED_AXIS_SRGB{0U, 0U, 0U, 128U};      // dead by construction, like above
inline constexpr Rgba8 GIZMO_TEXT_SRGB{255U, 255U, 255U, 255U};
inline constexpr Rgba8 GIZMO_TEXT_SHADOW_SRGB{0U, 0U, 0U, 255U};

namespace detail {
[[nodiscard]] constexpr Rgba8 opaqueRgba(std::array<std::uint8_t, 3> rgb) noexcept {
    return Rgba8{rgb[0], rgb[1], rgb[2], 255U};
}
[[nodiscard]] constexpr Rgba8 withAlpha(Rgba8 color, std::uint8_t alpha) noexcept {
    return Rgba8{color[0], color[1], color[2], alpha};
}
}  // namespace detail

// THE style. constexpr so the derivation from the palette is a compile-time fact (AX3's static_assert
// precedent, GS12) and so the apply site can hold it as a namespace-scope constant.
[[nodiscard]] constexpr GizmoStyle defaultGizmoStyle() noexcept {
    GizmoStyle style{};
    style.translationLineThicknessPoints = GIZMO_TRANSLATION_LINE_THICKNESS_POINTS;
    style.translationArrowSizePoints = GIZMO_TRANSLATION_ARROW_SIZE_POINTS;
    style.rotationLineThicknessPoints = GIZMO_ROTATION_LINE_THICKNESS_POINTS;
    style.rotationScreenRingThicknessPoints = GIZMO_ROTATION_SCREEN_RING_THICKNESS_POINTS;
    style.scaleLineThicknessPoints = GIZMO_SCALE_LINE_THICKNESS_POINTS;
    style.scaleDiscRadiusPoints = GIZMO_SCALE_DISC_RADIUS_POINTS;
    style.hatchedAxisThicknessPoints = GIZMO_HATCHED_AXIS_THICKNESS_POINTS;
    style.centerDiscRadiusPoints = GIZMO_CENTER_DISC_RADIUS_POINTS;
    auto at = [&style](GizmoColor slot) -> Rgba8& { return style.colors[static_cast<std::size_t>(slot)]; };
    at(GizmoColor::AxisX) = detail::opaqueRgba(axisColorSrgbBytes(Axis::X));
    at(GizmoColor::AxisY) = detail::opaqueRgba(axisColorSrgbBytes(Axis::Y));
    at(GizmoColor::AxisZ) = detail::opaqueRgba(axisColorSrgbBytes(Axis::Z));
    at(GizmoColor::PlaneX) = detail::withAlpha(at(GizmoColor::AxisX), GIZMO_PLANE_FILL_ALPHA);
    at(GizmoColor::PlaneY) = detail::withAlpha(at(GizmoColor::AxisY), GIZMO_PLANE_FILL_ALPHA);
    at(GizmoColor::PlaneZ) = detail::withAlpha(at(GizmoColor::AxisZ), GIZMO_PLANE_FILL_ALPHA);
    at(GizmoColor::Highlight) = GIZMO_HIGHLIGHT_SRGB;
    at(GizmoColor::Inactive) = GIZMO_INACTIVE_SRGB;
    at(GizmoColor::TranslationLine) = GIZMO_TRANSLATION_LINE_SRGB;
    at(GizmoColor::ScaleLine) = GIZMO_SCALE_LINE_SRGB;
    at(GizmoColor::RotationBorder) = GIZMO_HIGHLIGHT_SRGB;
    at(GizmoColor::RotationFill) = detail::withAlpha(GIZMO_HIGHLIGHT_SRGB, GIZMO_ROTATION_FILL_ALPHA);
    at(GizmoColor::HatchedAxis) = GIZMO_HATCHED_AXIS_SRGB;
    at(GizmoColor::Text) = GIZMO_TEXT_SRGB;
    at(GizmoColor::TextShadow) = GIZMO_TEXT_SHADOW_SRGB;
    return style;
}

// ---- screen size --------------------------------------------------------------------------------
// WHAT THE LIBRARY CALLS A SIZE. ImGuizmo::SetGizmoSizeClipSpace takes the length a unit axis should
// have in a WIDTH-NORMALISED clip space: GetSegmentLengthClipSpace (ImGuizmo.cpp:863-887) divides y by
// the display ratio in a landscape rect and multiplies x by it in a portrait one (:880-884), so one
// screen point is 2 / max(w, h) of its unit in BOTH orientations. Its default, 0.1, is therefore 5 % of
// the LARGER viewport dimension -- a thumbnail in a narrow dock, a monster when maximised.

inline constexpr float GIZMO_AXIS_LENGTH_POINTS = 90.0F;          // a screen-parallel axis, tip to origin
inline constexpr float GIZMO_AXIS_MAX_VIEWPORT_FRACTION = 0.15F;  // the knee: below a 600-pt smaller
                                                                  // dimension the gizmo scales with the dock
// The two hide thresholds, as FRACTIONS of the resolved size so the foreshortening at which an axis or
// a plane disappears is the same in every dock. 0.2 and 0.25 are the library's own default ratios
// (0.02 / 0.1 and 0.0025 / 0.01, ImGuizmo.cpp:748-749, :782): at the one viewport where the resolved
// size is exactly 0.1, the behaviour is the library's.
inline constexpr float GIZMO_AXIS_HIDE_FRACTION = 0.2F;         // of the size
inline constexpr float GIZMO_PLANE_HIDE_AREA_FRACTION = 0.25F;  // of the size SQUARED (an area)
// The library's defaults, mirrored ONLY for the fail-closed fallback below and for GS8's reference point.
inline constexpr float GIZMO_LIBRARY_DEFAULT_CLIP_SIZE = 0.1F;                // mGizmoSizeClipSpace, :782
inline constexpr float GIZMO_LIBRARY_DEFAULT_AXIS_HIDE_CLIP_LENGTH = 0.02F;   // mPlaneLimit (sic), :749
inline constexpr float GIZMO_LIBRARY_DEFAULT_PLANE_HIDE_CLIP_AREA = 0.0025F;  // mAxisLimit (sic), :748

// NAMED BY MEANING, because the library's setters are named by the opposite: SetPlaneLimit feeds the
// value ComputeTripodAxisAndVisibility compares against the AXIS's projected length (:1230) and
// SetAxisLimit the one it compares against the PLANE's projected area (:1229). viewport_panel.cpp is
// the ONE place that crossing is spelled, with the citation beside it, and I125 pins it.
struct GizmoScreenSize {
    float axisLengthPoints = 0.0F;    // what the eye sees for a screen-parallel axis
    float clipSpaceSize = 0.0F;       // -> ImGuizmo::SetGizmoSizeClipSpace
    float axisHideClipLength = 0.0F;  // -> ImGuizmo::SetPlaneLimit   (sic)
    float planeHideClipArea = 0.0F;   // -> ImGuizmo::SetAxisLimit    (sic)
    bool operator==(const GizmoScreenSize&) const noexcept = default;
};

// The library's own numbers, as one value: what resolveGizmoScreenSize returns for a viewport it
// refuses, and what a caller that never resolves would have had.
[[nodiscard]] constexpr GizmoScreenSize gizmoLibraryDefaultScreenSize() noexcept {
    return GizmoScreenSize{.axisLengthPoints = 0.0F,
                           .clipSpaceSize = GIZMO_LIBRARY_DEFAULT_CLIP_SIZE,
                           .axisHideClipLength = GIZMO_LIBRARY_DEFAULT_AXIS_HIDE_CLIP_LENGTH,
                           .planeHideClipArea = GIZMO_LIBRARY_DEFAULT_PLANE_HIDE_CLIP_AREA};
}

// PURE and TOTAL. `viewportPoints` is the image rect ImGuizmo::SetRect is given, in POINTS.
//   L    = min(GIZMO_AXIS_LENGTH_POINTS, GIZMO_AXIS_MAX_VIEWPORT_FRACTION * min(w, h))
//   size = 2 * L / max(w, h)
// A non-finite or non-positive extent FAILS CLOSED to gizmoLibraryDefaultScreenSize() -- unreachable
// from the panel (onDraw returns at step 1 for any such rect) and asserted anyway (GS9).
[[nodiscard]] GizmoScreenSize resolveGizmoScreenSize(Vec2 viewportPoints) noexcept;

}  // namespace engine::editor
