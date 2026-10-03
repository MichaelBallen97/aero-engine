// editor/src/gizmo_style.cpp — task E.1.5: the pure screen-size resolver. ImGuizmo-free by
// construction -- nothing here names an ImGuizmo type; the apply site lives in viewport_panel.cpp.
#include <aero/editor/gizmo_style.hpp>

#include <algorithm>
#include <cmath>

namespace engine::editor {

namespace {

// task E.6.1: an unusable scale is 1 -- the refusal style of the extent guard below, so both functions
// stay total.
[[nodiscard]] float usableUiScale(float uiScale) noexcept {
    return (uiScale > 0.0F && std::isfinite(uiScale)) ? uiScale : 1.0F;
}

}  // namespace

GizmoStyle scaledGizmoStyle(const GizmoStyle& style, float uiScale) noexcept {
    const float s = usableUiScale(uiScale);
    GizmoStyle scaled = style;  // the colours, copied
    scaled.translationLineThicknessPoints *= s;
    scaled.translationArrowSizePoints *= s;
    scaled.rotationLineThicknessPoints *= s;
    scaled.rotationScreenRingThicknessPoints *= s;
    scaled.scaleLineThicknessPoints *= s;
    scaled.scaleDiscRadiusPoints *= s;
    scaled.hatchedAxisThicknessPoints *= s;
    scaled.centerDiscRadiusPoints *= s;
    return scaled;
}

GizmoScreenSize resolveGizmoScreenSize(Vec2 viewportPoints, float uiScale) noexcept {
    const float w = viewportPoints.x;
    const float h = viewportPoints.y;
    // NaN-safe NEGATED form: every direct comparison with NaN is false, so only `!(x > 0)` fails
    // CLOSED -- the idiom picking.cpp and viewport_panel.cpp:419 already use. isfinite catches +inf,
    // which passes the `> 0` test and would otherwise make `size` 0 and L +inf.
    if (!(w > 0.0F) || !(h > 0.0F) || !std::isfinite(w) || !std::isfinite(h)) {
        return gizmoLibraryDefaultScreenSize();
    }
    const float smaller = std::min(w, h);
    const float larger = std::max(w, h);
    // The knee: pixel-constant above it, proportional to the dock below it. Continuous at the knee by
    // construction -- both arms evaluate to the scaled length there.
    const float scaledLength = GIZMO_AXIS_LENGTH_POINTS * usableUiScale(uiScale);  // task E.6.1 (D10)
    const float axisLengthPoints = std::min(scaledLength, GIZMO_AXIS_MAX_VIEWPORT_FRACTION * smaller);
    // One point is 2 / max(w, h) of the library's unit in BOTH orientations (header comment).
    const float clipSpaceSize = (2.0F * axisLengthPoints) / larger;
    return GizmoScreenSize{.axisLengthPoints = axisLengthPoints,
                           .clipSpaceSize = clipSpaceSize,
                           .axisHideClipLength = GIZMO_AXIS_HIDE_FRACTION * clipSpaceSize,
                           .planeHideClipArea = GIZMO_PLANE_HIDE_AREA_FRACTION * clipSpaceSize * clipSpaceSize};
}

}  // namespace engine::editor
