// engine/render/src/light_gizmo.cpp — task E.2.3: the three light gizmos as world-space lines.
//
// Every emitter has the same four-step shape: a TOTALITY gate that returns 0 without touching the
// batch, debugCircleBasis for the plane, the circle(s), and the rays that terminate on emitted circle
// VERTICES. The return is always read OFF the batch -- lineCount() after minus before -- so it is
// what the batch ACCEPTED rather than what the emitter meant to push (E.1.2's GR8 lesson at the
// source: read the value under test off the thing under test).

#include <aero/render/light_gizmo.hpp>

#include <algorithm>  // std::clamp, std::max
#include <cmath>      // std::isfinite, std::cos, std::sin
#include <cstdint>
#include <utility>  // std::pair

namespace engine::render {
namespace {

// debug_draw.cpp's own `finite`, restated because that one is in an anonymous namespace in another
// translation unit and is not reachable. Three isfinite calls, nothing else.
[[nodiscard]] bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// wireCircle's own clamp, applied ONCE per emitter and then handed to wireCircle, so the circle and
// the rays can never clamp differently. Passing style.segments raw and clamping only locally is the
// seed GZ7 exists to catch at segments = 10000.
[[nodiscard]] std::uint32_t resolveSegments(const LightGizmoStyle& style) noexcept {
    return std::clamp(style.segments, MIN_CIRCLE_SEGMENTS, MAX_CIRCLE_SEGMENTS);
}

// The k-th of `rays` points on a circle of radius `radius` centred at `center`, built from EXACTLY
// the expression wireCircle evaluates for its own vertex i = k * (n / rays). Same basis, same angle,
// same operator order -- so for k > 0 this is BIT-EQUAL to a vertex the circle already pushed. For
// k == 0 it differs from wireCircle's FIRST vertex only by adding basis.v * 0.0F, which is exact
// unless a component of center + u*radius is exactly -0.0F, where the sum becomes +0.0F: equal,
// bit-different, and unobservable after projection. The CLOSING vertex is not vertex 0 either --
// at i == n the angle is TWO_PI exactly and std::sin(TWO_PI_f) is about -1.75e-7.
[[nodiscard]] Vec3 circleVertex(const DebugCircleBasis& basis, Vec3 center, float radius, std::uint32_t n,
                                std::uint32_t rays, std::uint32_t k) noexcept {
    const std::uint32_t stride = std::max(1U, n / rays);
    const std::uint32_t i = (k * stride) % n;
    const float t = TWO_PI * static_cast<float>(i) / static_cast<float>(n);
    return center + (basis.u * (std::cos(t) * radius)) + (basis.v * (std::sin(t) * radius));
}

}  // namespace

std::uint32_t emitDirectionalLightGizmo(DebugDrawBatch& batch, const DirectionalGizmoParams& params) {
    const std::uint32_t before = batch.lineCount();
    if (!finite(params.origin) || !finite(params.direction)) {
        return 0U;  // TOTAL: nothing emitted, nothing rejected
    }
    const DebugCircleBasis basis = debugCircleBasis(params.direction);
    if (!basis.valid) {
        return 0U;  // a zero direction is not a bad push either
    }
    const Vec3 axis = normalizeOrZero(params.direction);
    const std::uint32_t n = resolveSegments(params.style);
    // params.direction, NOT axis: wireCircle recomputes debugCircleBasis from what it is handed, and
    // handing it the SAME argument is what makes its basis the same BITS as `basis` above. A
    // normalized copy would agree to an ulp and no further, which is exactly the tolerance argument
    // GZ13's bit-equality exists to avoid.
    batch.wireCircle(params.origin, params.direction, LIGHT_GIZMO_DIRECTIONAL_RADIUS, params.style.color, n,
                     params.style.depth);
    for (std::uint32_t k = 0; k < LIGHT_GIZMO_DIRECTIONAL_RAYS; ++k) {
        const Vec3 start =
            circleVertex(basis, params.origin, LIGHT_GIZMO_DIRECTIONAL_RADIUS, n, LIGHT_GIZMO_DIRECTIONAL_RAYS, k);
        batch.line(start, start + (axis * LIGHT_GIZMO_DIRECTIONAL_LENGTH), params.style.color, params.style.depth);
    }
    return batch.lineCount() - before;
}

std::uint32_t emitPointLightGizmo(DebugDrawBatch& batch, const PointGizmoParams& params) {
    const std::uint32_t before = batch.lineCount();
    if (!finite(params.center) || !std::isfinite(params.range) || params.range <= 0.0F) {
        return 0U;
    }
    // No debugCircleBasis here: nothing lands on a vertex, so there is no basis to share. wireSphere
    // is three wireCircle calls on the world axes.
    batch.wireSphere(params.center, params.range, params.style.color, resolveSegments(params.style),
                     params.style.depth);
    return batch.lineCount() - before;
}

std::uint32_t emitSpotLightGizmo(DebugDrawBatch& batch, const SpotGizmoParams& params) {
    const std::uint32_t before = batch.lineCount();
    if (!finite(params.apex) || !finite(params.direction) || !std::isfinite(params.range) ||
        !std::isfinite(params.innerConeRadians) || !std::isfinite(params.outerConeRadians) || params.range <= 0.0F) {
        return 0U;  // TOTAL: nothing emitted, nothing rejected
    }
    const DebugCircleBasis basis = debugCircleBasis(params.direction);
    if (!basis.valid) {
        return 0U;
    }
    const Vec3 axis = normalizeOrZero(params.direction);
    // The cone half-angle is clamped to [0, HALF_PI] -- the SAME bound spot_light.hpp's AERO_RANGE
    // carries, restated here because this emitter is reachable from a sample and a test that never
    // went through the Inspector. Beyond HALF_PI the "cone" folds inside out.
    const float outer = std::clamp(params.outerConeRadians, 0.0F, HALF_PI);
    const float inner = std::clamp(params.innerConeRadians, 0.0F, HALF_PI);
    const std::uint32_t n = resolveSegments(params.style);

    const auto emitCap = [&](float angle, float alphaScale) {
        // THE RIM IS ON THE RANGE SPHERE: axial range*cos(angle), radius range*sin(angle).
        const Vec3 capCenter = params.apex + (axis * (params.range * std::cos(angle)));
        const float capRadius = params.range * std::sin(angle);
        Vec4 color = params.style.color;
        color.w *= alphaScale;
        if (capRadius > 0.0F) {
            batch.wireCircle(capCenter, params.direction, capRadius, color, n, params.style.depth);
        }
        return std::pair{capCenter, capRadius};
    };
    const auto [outerCenter, outerRadius] = emitCap(outer, 1.0F);
    // MIRRORS the falloff vocabulary's fold of inner >= outer into a hard edge at `outer`. It cannot
    // CARRY that decision: the resolved cone pair carries no bit saying its delta was floored.
    if (inner < outer) {
        (void)emitCap(inner, params.style.innerAlphaScale);
    }
    if (outerRadius <= 0.0F) {
        // A cone with no aperture is a ray. One line, length `range`, and NO rejection.
        batch.line(params.apex, params.apex + (axis * params.range), params.style.color, params.style.depth);
        return batch.lineCount() - before;
    }
    for (std::uint32_t k = 0; k < LIGHT_GIZMO_SPOT_RIM_RAYS; ++k) {
        const Vec3 rim = circleVertex(basis, outerCenter, outerRadius, n, LIGHT_GIZMO_SPOT_RIM_RAYS, k);
        batch.line(params.apex, rim, params.style.color, params.style.depth);
    }
    return batch.lineCount() - before;
}

}  // namespace engine::render
