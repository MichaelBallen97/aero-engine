// editor/src/viewport_icons.cpp — task E.2.3: the icon predicate and the CPU-rasterised atlas.
//
// THE RASTERISER IS A SIGNED-DISTANCE COMPOSITOR AND NOTHING ELSE. Every glyph is a union of
// primitives -- discs, rings, strokes and boxes -- and a union is `alpha = max(alpha, coverage(d))`,
// which is ORDER-INDEPENDENT and therefore deterministic whatever order the primitives are drawn in
// (VI6 pins that). The only APPROXIMATING libm function anywhere below is std::sqrt, which IEEE-754
// requires to be correctly rounded, and the only other one is std::lround, whose result is exactly
// specified for every input; std::sin, std::cos and std::pow are absent and VI7 pins that as source
// text, because none is correctly rounded and one ulp would turn VI3/VI4's byte assertions into a
// three-lane tolerance argument.
//
// Sample point for texel (x, y) is its CENTRE, (x + 0.5F, y + 0.5F) in cell coordinates, so a
// 64-texel cell's centre is exactly (32.0F, 32.0F).

#include <aero/editor/viewport_icons.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::editor {
namespace {

// A glyph-space point, in texels. The alias exists so a corner table and a segment endpoint are
// spelled the same way, and so no signature carries four bare floats where two points are meant.
using Point = std::array<float, 2>;

// The cell's continuous centre. Named once so no glyph restates 32.0F.
constexpr float ICON_CENTER = static_cast<float>(VIEWPORT_ICON_CELL_TEXELS) * 0.5F;
// The one byte every RGB channel carries. A named constant rather than a bare 255U in three
// assignments, so the narrowing is stated once and reads as the rule VI3 asserts.
constexpr std::uint8_t ICON_RGB_BYTE = 255;

// ---- the primitives ------------------------------------------------------------------------------
// One texel of antialiasing, and no more: `d` is a signed distance in TEXELS, negative inside, and a
// texel whose centre is half a texel outside the edge is fully transparent. No pow, no exp -- a
// linear ramp is what makes the whole image reproducible bit for bit.
[[nodiscard]] float coverage(float signedDistanceTexels) noexcept {
    return std::clamp(0.5F - signedDistanceTexels, 0.0F, 1.0F);
}

[[nodiscard]] float sdCircle(float px, float py, float cx, float cy, float radius) noexcept {
    const float dx = px - cx;
    const float dy = py - cy;
    return std::sqrt((dx * dx) + (dy * dy)) - radius;
}

[[nodiscard]] float sdRing(float px, float py, float cx, float cy, float radius, float halfWidth) noexcept {
    return std::abs(sdCircle(px, py, cx, cy, radius)) - halfWidth;
}

// The two endpoints arrive as points rather than four floats, which is also the shape addPolyline
// and the glyph tables already hold them in.
[[nodiscard]] float sdSegment(float px, float py, Point a, Point b, float halfWidth) noexcept {
    const float pax = px - a[0];
    const float pay = py - a[1];
    const float bax = b[0] - a[0];
    const float bay = b[1] - a[1];
    const float lengthSq = (bax * bax) + (bay * bay);
    // A degenerate segment collapses to its first endpoint rather than dividing by zero. No caller
    // below builds one; the guard is what makes the helper total for a future one.
    const float h = lengthSq > 0.0F ? std::clamp(((pax * bax) + (pay * bay)) / lengthSq, 0.0F, 1.0F) : 0.0F;
    const float dx = pax - (bax * h);
    const float dy = pay - (bay * h);
    return std::sqrt((dx * dx) + (dy * dy)) - halfWidth;
}

[[nodiscard]] float sdBox(float px, float py, float cx, float cy, float halfW, float halfH) noexcept {
    const float qx = std::abs(px - cx) - halfW;
    const float qy = std::abs(py - cy) - halfH;
    const float ox = std::max(qx, 0.0F);
    const float oy = std::max(qy, 0.0F);
    return std::sqrt((ox * ox) + (oy * oy)) + std::min(std::max(qx, qy), 0.0F);
}

[[nodiscard]] float sdBoxOutline(float px, float py, float cx, float cy, float halfW, float halfH,
                                 float halfWidth) noexcept {
    return std::abs(sdBox(px, py, cx, cy, halfW, halfH)) - halfWidth;
}

// ---- shape composition --------------------------------------------------------------------------
// A union, and a union is ORDER-INDEPENDENT -- which is what makes the whole image reproducible
// whatever order a glyph lists its shapes in (VI6).
void addShape(float& alpha, float distance) noexcept { alpha = std::max(alpha, coverage(distance)); }

// Strokes the polyline through `points`, joining the last back to the first when `closed`. Fewer
// than two points is a no-op rather than an underflow of `size() - 1`.
void addPolyline(float& alpha, float px, float py, std::span<const Point> points, bool closed,
                 float halfWidth) noexcept {
    if (points.size() < 2U) {
        return;
    }
    const std::size_t strokes = closed ? points.size() : points.size() - 1U;
    for (std::size_t i = 0; i < strokes; ++i) {
        addShape(alpha, sdSegment(px, py, points[i], points[(i + 1U) % points.size()], halfWidth));
    }
}

// ---- the four glyphs -----------------------------------------------------------------------------
// EVERY magnitude below is a named constant, and every stroke is at least
// VIEWPORT_ICON_MIN_FEATURE_TEXELS wide (a half-width of 2 is a 4-texel feature), because the atlas
// is minified 2.91x on a 1x display. Every extent stays clear of the gutter by more than the
// half-width plus the half-texel antialiasing ramp, which is what makes VI4 a statement about the
// glyphs rather than about the clamp.

constexpr float SUN_DISC_RADIUS = 11.0F;
constexpr float SUN_RAY_INNER = 16.0F;
constexpr float SUN_RAY_OUTER = 26.0F;
constexpr float SUN_RAY_HALF_WIDTH = 2.5F;
// The eight 45-degree directions as EXACT literals: cos/sin of a multiple of 45 degrees is one of
// 0, +-1 and +-sqrt(2)/2, so the sun needs no trigonometry at all (VI7's whole point).
constexpr float DIAGONAL = 0.70710678F;
constexpr std::array<Point, 8> SUN_RAY_DIRECTIONS{{{1.0F, 0.0F},
                                                   {DIAGONAL, DIAGONAL},
                                                   {0.0F, 1.0F},
                                                   {-DIAGONAL, DIAGONAL},
                                                   {-1.0F, 0.0F},
                                                   {-DIAGONAL, -DIAGONAL},
                                                   {0.0F, -1.0F},
                                                   {DIAGONAL, -DIAGONAL}}};

[[nodiscard]] float directionalAlpha(float px, float py) noexcept {
    float alpha = coverage(sdCircle(px, py, ICON_CENTER, ICON_CENTER, SUN_DISC_RADIUS));
    for (const Point& dir : SUN_RAY_DIRECTIONS) {
        const Point inner{ICON_CENTER + (dir[0] * SUN_RAY_INNER), ICON_CENTER + (dir[1] * SUN_RAY_INNER)};
        const Point outer{ICON_CENTER + (dir[0] * SUN_RAY_OUTER), ICON_CENTER + (dir[1] * SUN_RAY_OUTER)};
        addShape(alpha, sdSegment(px, py, inner, outer, SUN_RAY_HALF_WIDTH));
    }
    return alpha;
}

constexpr float SPOT_DISC_CENTER_Y = 15.0F;
constexpr float SPOT_DISC_RADIUS = 7.0F;
constexpr float SPOT_STROKE_HALF_WIDTH = 2.5F;
// The cone below the lamp: a CLOSED trapezoid, corners in winding order.
constexpr std::array<Point, 4> SPOT_CONE{{{25.0F, 22.0F}, {39.0F, 22.0F}, {49.0F, 52.0F}, {15.0F, 52.0F}}};

[[nodiscard]] float spotAlpha(float px, float py) noexcept {
    float alpha = coverage(sdCircle(px, py, ICON_CENTER, SPOT_DISC_CENTER_Y, SPOT_DISC_RADIUS));
    addPolyline(alpha, px, py, SPOT_CONE, /*closed=*/true, SPOT_STROKE_HALF_WIDTH);
    return alpha;
}

constexpr float POINT_DISC_RADIUS = 9.0F;
constexpr float POINT_RING_NEAR_RADIUS = 17.0F;
constexpr float POINT_RING_FAR_RADIUS = 26.0F;
constexpr float POINT_RING_HALF_WIDTH = 2.0F;

[[nodiscard]] float pointAlpha(float px, float py) noexcept {
    float alpha = coverage(sdCircle(px, py, ICON_CENTER, ICON_CENTER, POINT_DISC_RADIUS));
    const float ringHalf = POINT_RING_HALF_WIDTH;
    addShape(alpha, sdRing(px, py, ICON_CENTER, ICON_CENTER, POINT_RING_NEAR_RADIUS, ringHalf));
    addShape(alpha, sdRing(px, py, ICON_CENTER, ICON_CENTER, POINT_RING_FAR_RADIUS, ringHalf));
    return alpha;
}

constexpr float CAMERA_BODY_CENTER_X = 27.0F;
constexpr float CAMERA_BODY_HALF_W = 13.0F;
constexpr float CAMERA_BODY_HALF_H = 10.0F;
constexpr float CAMERA_STROKE_HALF_WIDTH = 2.5F;
// The lens, hanging off the body's right edge: an OPEN polyline, never closed -- the fourth stroke
// would run back through the body outline it is attached to.
constexpr std::array<Point, 4> CAMERA_LENS{{{40.0F, 26.0F}, {50.0F, 20.0F}, {50.0F, 44.0F}, {40.0F, 38.0F}}};

[[nodiscard]] float cameraAlpha(float px, float py) noexcept {
    float alpha = coverage(sdBoxOutline(px, py, CAMERA_BODY_CENTER_X, ICON_CENTER, CAMERA_BODY_HALF_W,
                                        CAMERA_BODY_HALF_H, CAMERA_STROKE_HALF_WIDTH));
    addPolyline(alpha, px, py, CAMERA_LENS, /*closed=*/false, CAMERA_STROKE_HALF_WIDTH);
    return alpha;
}

// TOTAL over the enum, with NO `default:` -- a fifth kind is a -Wswitch error rather than a silently
// blank cell.
[[nodiscard]] float glyphAlpha(ViewportIconKind kind, float px, float py) noexcept {
    switch (kind) {
        case ViewportIconKind::DirectionalLight:
            return directionalAlpha(px, py);
        case ViewportIconKind::SpotLight:
            return spotAlpha(px, py);
        case ViewportIconKind::PointLight:
            return pointAlpha(px, py);
        case ViewportIconKind::Camera:
            return cameraAlpha(px, py);
    }
    return 0.0F;
}

}  // namespace

std::optional<ViewportIconKind> viewportIconFor(const World& world, Entity entity) {
    if (!world.alive(entity)) {
        return std::nullopt;  // a dead OR null handle -- alive() answers both, silently
    }
    // has<T> is silent for an unregistered type (picking.hpp's F15 note), which is what makes VI10's
    // no-log claim reachable. The order IS the priority order, and it IS the atlas's cell order.
    if (world.has<DirectionalLight>(entity)) {
        return ViewportIconKind::DirectionalLight;
    }
    if (world.has<SpotLight>(entity)) {
        return ViewportIconKind::SpotLight;
    }
    if (world.has<PointLight>(entity)) {
        return ViewportIconKind::PointLight;
    }
    if (world.has<Camera>(entity)) {
        return ViewportIconKind::Camera;
    }
    return std::nullopt;
}

bool buildViewportIconAtlas(std::span<std::uint8_t> out) noexcept {
    if (out.size() != VIEWPORT_ICON_ATLAS_BYTES) {
        return false;  // WRITES NOTHING -- VI2 asserts a canary byte is unchanged after each refusal
    }
    // RGB IS WRITTEN 255 FIRST, UNCONDITIONALLY, OVER THE WHOLE SPAN, so VI3 is a statement about the
    // image rather than about the glyphs. Alpha starts fully transparent and the glyph unions into it.
    for (std::size_t i = 0; i < out.size(); i += 4U) {
        out[i] = ICON_RGB_BYTE;
        out[i + 1U] = ICON_RGB_BYTE;
        out[i + 2U] = ICON_RGB_BYTE;
        out[i + 3U] = 0U;
    }
    for (std::size_t k = 0; k < VIEWPORT_ICON_COUNT; ++k) {
        const auto kind = static_cast<ViewportIconKind>(k);
        // The CELL comes from viewportIconCell, never from `k`: one table decides where a glyph goes,
        // and the UV rect the sampler reads is derived from that same table.
        const std::uint32_t cellX = viewportIconCell(kind) * VIEWPORT_ICON_CELL_TEXELS;
        for (std::uint32_t y = 0; y < VIEWPORT_ICON_CELL_TEXELS; ++y) {
            const std::size_t rowBase = static_cast<std::size_t>(y) * VIEWPORT_ICON_ATLAS_WIDTH;
            for (std::uint32_t x = 0; x < VIEWPORT_ICON_CELL_TEXELS; ++x) {
                const float sx = static_cast<float>(x) + 0.5F;
                const float sy = static_cast<float>(y) + 0.5F;
                const std::size_t texel = rowBase + cellX + x;
                const float alpha = glyphAlpha(kind, sx, sy);
                out[(texel * 4U) + 3U] = static_cast<std::uint8_t>(std::lround(alpha * 255.0F));
            }
        }
    }
    return true;
}

}  // namespace engine::editor
