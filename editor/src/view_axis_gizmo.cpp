// editor/src/view_axis_gizmo.cpp — task E.1.3: the corner widget's layout, depth order and hit test.
// ImGui-free by construction -- nothing here names an ImGui type, which is what lets a tier-0 test
// drive the whole widget with no window, no GPU and no ImGui context.
#include <aero/editor/view_axis_gizmo.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::editor {

namespace {

// F14 again: there is still no isFinite in the public math surface, and this task does not add one --
// the picking.cpp:35-36 / scene_bounds.cpp:22 / editor_camera.cpp:70 "copied, not shared" precedent
// 2.2.4 recorded, not a new one.
[[nodiscard]] bool allFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

// THE ONE VISIBILITY PREDICATE (D16). All four consequences -- nothing draws, nothing is hit, the
// rect is degenerate, and the press claim is empty -- fall out of THIS function rather than out of
// four comparisons that could drift apart. Written with negated comparisons so a NaN extent takes the
// invisible branch: `+inf > 0.0F` is TRUE, so the positive form alone lets infinity through.
[[nodiscard]] bool viewAxisVisible(Vec2 origin, Vec2 size) noexcept {
    if (!allFinite(origin) || !allFinite(size)) {
        return false;
    }
    return !(size.x < VIEW_AXIS_MIN_IMAGE_POINTS) && !(size.y < VIEW_AXIS_MIN_IMAGE_POINTS);
}

// The widget centre, in SCREEN points. Top-right of the image, inset by the margin plus the
// half-extent so the whole box clears the corner.
[[nodiscard]] Vec2 viewAxisCenter(Vec2 origin, Vec2 size) noexcept {
    constexpr float HALF = VIEW_AXIS_HALF_EXTENT_POINTS;
    return Vec2{origin.x + size.x - VIEW_AXIS_MARGIN_POINTS - HALF, origin.y + VIEW_AXIS_MARGIN_POINTS + HALF};
}

// The six signed world axes, in ViewAxis order.
[[nodiscard]] Vec3 viewAxisDirection(ViewAxis axis) noexcept {
    switch (axis) {  // NO default: -- a seventh enumerator must be a -Wswitch error, never silent
        case ViewAxis::PosX:
            return Vec3{1.0F, 0.0F, 0.0F};
        case ViewAxis::NegX:
            return Vec3{-1.0F, 0.0F, 0.0F};
        case ViewAxis::PosY:
            return Vec3{0.0F, 1.0F, 0.0F};
        case ViewAxis::NegY:
            return Vec3{0.0F, -1.0F, 0.0F};
        case ViewAxis::PosZ:
            return Vec3{0.0F, 0.0F, 1.0F};
        case ViewAxis::NegZ:
            return Vec3{0.0F, 0.0F, -1.0F};
    }
    return Vec3{1.0F, 0.0F, 0.0F};
}

}  // namespace

ViewAxisLayout viewAxisLayout(const EditorCamera& camera, Vec2 imageOriginPoints, Vec2 imageSizePoints) noexcept {
    ViewAxisLayout layout{};
    if (!viewAxisVisible(imageOriginPoints, imageSizePoints)) {
        return layout;  // D16: default-constructed, visible == false -- one predicate, four consequences
    }
    layout.centerPoints = viewAxisCenter(imageOriginPoints, imageSizePoints);

    // `right`/`up`/`forward` are ORTHONORMAL by INV-2, so the transpose IS the inverse and the axis
    // in the camera's own basis is three dots rather than a matrix inverse.
    const Vec3 right = camera.right();
    const Vec3 up = camera.up();
    const Vec3 forward = camera.forward();

    for (std::size_t i = 0; i < VIEW_AXIS_COUNT; ++i) {
        const auto axis = static_cast<ViewAxis>(i);
        const Vec3 a = viewAxisDirection(axis);
        const float vx = dot(a, right);
        const float vy = dot(a, up);
        const float vz = dot(a, forward);
        // THE `-` ON THE Y TERM IS THE ONE SIGN THAT IS EASY TO GET WRONG, and it is the same flip
        // viewportNdc performs in the other direction (picking.cpp:56-57: "y is FLIPPED: ImGui's +y
        // is screen-DOWN, NDC's is UP"). VA2 is its discriminator: at the shipped default pose the
        // +Y ball must land ABOVE the widget centre in screen space.
        layout.balls[i] =
            ViewAxisBall{.offsetPoints = Vec2{vx * VIEW_AXIS_RING_RADIUS_POINTS, -vy * VIEW_AXIS_RING_RADIUS_POINTS},
                         .depth = vz,  // LARGER == FARTHER; < 0 == in front of the eye
                         .axis = axis,
                         .positive = viewAxisIsPositive(axis)};
        layout.drawOrder[i] = static_cast<std::uint8_t>(i);
    }

    // TOTALITY BEFORE THE SORT, and it is not belt-and-braces: a poisoned camera (a direct
    // setYaw(NaN) between two update()s) makes every `depth` NaN, and std::sort with a comparator
    // that is not a strict weak ordering is UNDEFINED BEHAVIOUR that can read out of bounds -- not
    // merely a wrong order. VA5's poisoned-camera arm covers it.
    for (const ViewAxisBall& ball : layout.balls) {
        if (!std::isfinite(ball.depth) || !allFinite(ball.offsetPoints)) {
            return ViewAxisLayout{};
        }
    }

    // FAR -> NEAR: descending depth, ties broken by ViewAxis order. Total and deterministic, so a
    // degenerate pose (two axes at identical depth) has a defined order and the hit test's near ->
    // far walk is the exact reverse of the draw's. Six uint8_t, no allocation.
    std::sort(layout.drawOrder.begin(), layout.drawOrder.end(), [&layout](std::uint8_t lhs, std::uint8_t rhs) noexcept {
        const float a = layout.balls[lhs].depth;
        const float b = layout.balls[rhs].depth;
        if (a != b) {
            return a > b;
        }
        return lhs < rhs;
    });

    layout.visible = true;
    return layout;
}

void viewAxisRect(Vec2 imageOriginPoints, Vec2 imageSizePoints, Vec2& outMin, Vec2& outMax) noexcept {
    if (!viewAxisVisible(imageOriginPoints, imageSizePoints)) {
        // DEGENERATE, which is what makes "an empty rect owns nothing" hold in overlayOwnsPress with
        // no second predicate -- the same shape overlayRowTopLeft/BottomRight already relies on.
        outMin = Vec2::zero();
        outMax = Vec2::zero();
        return;
    }
    const Vec2 center = viewAxisCenter(imageOriginPoints, imageSizePoints);
    constexpr float HALF = VIEW_AXIS_HALF_EXTENT_POINTS;
    outMin = Vec2{center.x - HALF, center.y - HALF};
    outMax = Vec2{center.x + HALF, center.y + HALF};
}

ViewAxisPick viewAxisPickAt(const ViewAxisLayout& layout, Vec2 mousePoints) noexcept {
    if (!layout.visible || !allFinite(mousePoints)) {
        return ViewAxisPick{};
    }
    // SQUARED distances against squared radii throughout: no sqrt, and no length(Vec2) call, which
    // is not constexpr. Every comparison is the NEGATED `<=` A10 idiom (picking.cpp:275-277) so a
    // NaN distance takes the REJECT branch rather than the accept one.
    const Vec2 fromCenter = mousePoints - layout.centerPoints;
    constexpr float CENTER_R2 = VIEW_AXIS_CENTER_RADIUS_POINTS * VIEW_AXIS_CENTER_RADIUS_POINTS;
    if (lengthSquared(fromCenter) <= CENTER_R2) {
        // D10: THE CENTRE FIRST. CENTER_RADIUS < BALL_RADIUS, so a ball collapsed onto the centre
        // (a canonical view, where the axis you are looking down projects to zero offset) still
        // leaves a 2-point annulus that reaches the ball rather than the badge.
        return ViewAxisPick{.kind = ViewAxisHit::Center};
    }
    constexpr float BALL_R2 = VIEW_AXIS_BALL_RADIUS_POINTS * VIEW_AXIS_BALL_RADIUS_POINTS;
    // NEAR -> FAR, the exact reverse of drawOrder, so the ball drawn on top is the ball you hit.
    for (std::size_t i = VIEW_AXIS_COUNT; i > 0; --i) {
        const ViewAxisBall& ball = layout.balls[layout.drawOrder[i - 1U]];
        const Vec2 delta = mousePoints - (layout.centerPoints + ball.offsetPoints);
        if (lengthSquared(delta) <= BALL_R2) {
            return ViewAxisPick{.kind = ViewAxisHit::Axis, .axis = ball.axis};
        }
    }
    return ViewAxisPick{};  // inside the rect but on no target: chrome, and not a click on anything
}

char viewAxisLabel(ViewAxis axis) noexcept {
    switch (axis) {  // NO default: -- total, so a seventh enumerator is a -Wswitch error
        case ViewAxis::PosX:
        case ViewAxis::NegX:
            return 'X';
        case ViewAxis::PosY:
        case ViewAxis::NegY:
            return 'Y';
        case ViewAxis::PosZ:
        case ViewAxis::NegZ:
            return 'Z';
    }
    return 'X';
}

Axis viewAxisPaletteKey(ViewAxis axis) noexcept {
    switch (axis) {
        case ViewAxis::PosX:
        case ViewAxis::NegX:
            return Axis::X;
        case ViewAxis::PosY:
        case ViewAxis::NegY:
            return Axis::Y;
        case ViewAxis::PosZ:
        case ViewAxis::NegZ:
            return Axis::Z;
    }
    return Axis::X;
}

bool viewAxisIsPositive(ViewAxis axis) noexcept {
    switch (axis) {
        case ViewAxis::PosX:
        case ViewAxis::PosY:
        case ViewAxis::PosZ:
            return true;
        case ViewAxis::NegX:
        case ViewAxis::NegY:
        case ViewAxis::NegZ:
            return false;
    }
    return true;
}

}  // namespace engine::editor
