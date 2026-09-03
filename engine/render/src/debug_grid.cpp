// engine/render/src/debug_grid.cpp — task E.1.2: the ground grid, as pure arithmetic.
//
// NO LOGGING. This runs every frame; a WARN at 60 Hz is a flood, and the batch already counts every
// refusal it makes. NO ALLOCATION, NO STATIC STATE. See debug_grid.hpp for the continuity argument,
// the libm rule, the lattice rule and the bound.

#include <aero/render/debug_grid.hpp>

#include <algorithm>  // std::clamp, std::min, std::max -- MSVC's STL supplies none of it transitively
#include <array>
#include <cmath>  // std::isfinite, std::sqrt, std::floor, std::ceil, std::fabs
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::render {
namespace {

[[nodiscard]] bool finiteVec(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The FADE, evaluated at one vertex. Pure arithmetic: a smoothstep over the normalised distance from
// the disc centre, starting at FADE_INNER * radius and reaching exactly 1 at the rim.
[[nodiscard]] float radialFade(float distanceFromCentre, float radius) noexcept {
    const float inner = DEBUG_GRID_FADE_INNER * radius;
    const float band = radius - inner;
    if (!(band > 0.0F)) {
        return 0.0F;  // a degenerate disc contributes nothing; the ! form is NaN-safe
    }
    const float u = std::clamp((distanceFromCentre - inner) / band, 0.0F, 1.0F);
    return 1.0F - (u * u * (3.0F - (2.0F * u)));  // smoothstep, and 1 - it, in one expression
}

// ONE grid line, subdivided so the alpha really varies ALONG it. A line whose two endpoints sit on
// the rim of a circle has the SAME radius at both ends, so an unsubdivided line fades UNIFORMLY and
// the effect is lost entirely -- which is why this exists and why line(a, b, colour) cannot be used
// (it applies one colour to both vertices and cannot express a fade at all).
//
// SEGMENTS+1 vertices are evaluated into a stack ring, then expanded into SEGMENTS pairs in ONE
// stack array and pushed with ONE lines(span) call. Nothing is allocated.
//
// `alongX == true`  -> the line runs along world X at a constant z (family A)
// `alongX == false` -> the line runs along world Z at a constant x (family B)
[[nodiscard]] std::uint32_t emitFadedLine(DebugDrawBatch& batch, bool alongX, float constantCoord, float spanFrom,
                                          float spanTo, Vec3 centre, Vec3 rgb, float alpha, float radius) {
    std::array<DebugLineVertex, DEBUG_GRID_FADE_SEGMENTS + 1U> ring{};
    for (std::uint32_t i = 0; i <= DEBUG_GRID_FADE_SEGMENTS; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(DEBUG_GRID_FADE_SEGMENTS);
        const float span = spanFrom + ((spanTo - spanFrom) * t);
        const Vec3 position = alongX ? Vec3{span, DEBUG_GRID_PLANE_HEIGHT, constantCoord}
                                     : Vec3{constantCoord, DEBUG_GRID_PLANE_HEIGHT, span};
        const float dx = position.x - centre.x;
        const float dz = position.z - centre.z;
        const float distance = std::sqrt((dx * dx) + (dz * dz));  // sqrt is IEEE-exact; length() is 3D
        ring[i] =
            DebugLineVertex{.position = position,
                            .rgba = packDebugColor(Vec4{rgb.x, rgb.y, rgb.z, alpha * radialFade(distance, radius)})};
    }
    std::array<DebugLineVertex, std::size_t{2} * DEBUG_GRID_FADE_SEGMENTS> pairs{};
    for (std::size_t i = 0; i < DEBUG_GRID_FADE_SEGMENTS; ++i) {
        pairs[2U * i] = ring[i];
        pairs[(2U * i) + 1U] = ring[i + 1U];
    }
    return batch.lines(std::span<const DebugLineVertex>{pairs}, DebugDepth::Tested);
}

// One family of one cadence. Returns the number of LINES THE BATCH ACCEPTED.
//
// `centreAlong` is the disc centre's coordinate on the axis the lines are CONSTANT in;
// `centreSpan`  is its coordinate on the axis they SPAN.
[[nodiscard]] std::uint32_t emitFamily(DebugDrawBatch& batch, bool alongX, float centreAlong, float centreSpan,
                                       Vec3 centre, float spacing, float radius, Vec3 rgb, float alpha, bool skipZero) {
    const float kMinF = std::ceil((centreAlong - radius) / spacing);
    const float kMaxF = std::floor((centreAlong + radius) / spacing);
    // THE THREE THINGS THIS GUARD IS FOR, and none of them is hypothetical.
    //  (1) NON-FINITE. focus is only required to be FINITE, and 1e38F / 1e-6F is +inf. `inf <= inf`
    //      is TRUE, `inf - inf` is NaN, and std::min(NaN, 48.0F) returns NaN because min is defined
    //      in terms of `<` -- so a cast to uint32_t below would be UB that UBSan traps.
    //  (2) AN EMPTY RANGE, written `!(kMinF <= kMaxF)` rather than `kMaxF < kMinF` so a NaN that
    //      slipped past (1) still takes this exit.
    //  (3) THE COUNT. kMin and kMax come from two INDEPENDENT divisions, each rounding; at
    //      |centreAlong / spacing| ~ 1e7 half an ulp is 0.6, enough to move ceil at one end and
    //      floor at the other and produce 50 or 51 lines where 49 is the bound
    //      DEBUG_GRID_MAX_LINES is derived from. The clamp makes the bound STRUCTURAL.
    if (!std::isfinite(kMinF) || !std::isfinite(kMaxF) || !(kMinF <= kMaxF)) {
        return 0;
    }
    const float span = std::min(kMaxF - kMinF, static_cast<float>(2U * DEBUG_GRID_RADIUS_CELLS));
    const auto steps = static_cast<std::uint32_t>(span);  // in [0, 48], finite by the guard above
    std::uint32_t accepted = 0;
    float previousK = 0.0F;
    for (std::uint32_t i = 0; i <= steps; ++i) {
        const float k = kMinF + static_cast<float>(i);
        // At |k| >= 2^24 the float spacing exceeds 1 and `kMinF + i` stops changing -- MEASURED at
        // centreAlong = 1e6 with spacing 0.01, which is an ordinary "fly far out, then orbit
        // closely" pose. A float-INDEXED loop would never terminate here; this one would emit up to
        // 49 coincident lines, so it stops at the first repeat instead. BREAK, not continue: k can
        // never change again.
        if (i != 0U && k == previousK) {
            break;
        }
        previousK = k;
        if (skipZero && k == 0.0F) {
            continue;  // the axis takes this line (D8)
        }
        const float constantCoord = k * spacing;  // THE LATTICE: an exact multiple of the spacing
        const float offset = constantCoord - centreAlong;
        if (std::fabs(offset) >= radius) {
            continue;  // a degenerate chord: no line, rather than a zero-length one
        }
        const float halfChord = std::sqrt((radius * radius) - (offset * offset));
        accepted += emitFadedLine(batch, alongX, constantCoord, centreSpan - halfChord, centreSpan + halfChord, centre,
                                  rgb, alpha, radius);
    }
    return accepted;
}

}  // namespace

float debugGridPow10(int exponent) noexcept {
    // ITERATED MULTIPLICATION AND DIVISION FROM 1.0F, at most sixteen steps. IEEE-754 requires both
    // operations to be correctly rounded, so the sequence is BIT-IDENTICAL on every lane -- which is
    // the whole reason std::pow and std::log10 are absent from this file (GR22 pins that).
    //
    // EACH CALL STARTS FRESH FROM 1.0F. Never a running accumulator carried across the three
    // cadences: 1.0F divided by ten six times and multiplied back six times is 0.99999994, NOT 1.0
    // (measured), so a line the emitter calls "x == 1" would not be at 1 and GR12 would redden.
    // And `10.0F * pow10(n)` is not `pow10(n + 1)` at n == -2 -- the ONLY n in this range where the
    // two differ (pow10(-1) is 0.100000001, 10.0F * pow10(-2) is 0.099999994) -- which is why the
    // three spacings are three fresh calls rather than one call and two multiplies. GR6 asserts both
    // halves of that so neither is a claim anyone has to take on trust.
    const int clamped = std::clamp(exponent, DEBUG_GRID_MIN_LEVEL, DEBUG_GRID_MAX_LEVEL);
    float result = 1.0F;
    if (clamped >= 0) {
        for (int i = 0; i < clamped; ++i) {
            result *= 10.0F;
        }
    } else {
        for (int i = 0; i < -clamped; ++i) {
            result /= 10.0F;
        }
    }
    return result;
}

DebugGridCadence debugGridCadence(const DebugGridParams& params) noexcept {
    DebugGridCadence out{};
    // THE TOTALITY GATE. A bad camera is not an error to report -- it is a frame with no grid in it.
    // EditorCamera::clampState() guarantees far > near > 0 but NOT finiteness (its own comment says
    // so: a directly-poisoned NaN survives until the next update() sweeps it), so farPlane() really
    // can arrive non-finite and this arm really is load-bearing.
    if (!finiteVec(params.eye) || !finiteVec(params.focus) || !std::isfinite(params.farPlane) ||
        params.farPlane <= 0.0F) {
        return out;  // .valid stays false and every other field stays zero
    }

    // THE VIEW SCALE (D5). max of the two arms, because each alone is degenerate:
    //   * orbit distance alone is wrong while FLYING -- fly mode translates the pivot WITH the eye
    //     and leaves distance() untouched, so 500 m up keeps the cadence it had on the ground;
    //   * height alone is degenerate at eye.y ~ 0 -- a camera on the horizon would get the minimum
    //     cadence for a subject a hundred metres away.
    // max is HOMOGENEOUS, which is what makes GR6's self-similarity exact rather than approximate.
    const float distance = length(params.eye - params.focus);
    const float height = std::fabs(params.eye.y - DEBUG_GRID_PLANE_HEIGHT);
    out.viewScale = std::clamp(std::max(distance, height), DEBUG_GRID_MIN_VIEW_SCALE, DEBUG_GRID_MAX_VIEW_SCALE);

    const float reference = out.viewScale / DEBUG_GRID_TARGET_DIVISIONS;

    // THE DECADE, by iteration rather than by log10 (see debugGridPow10). At most sixteen steps.
    out.level = DEBUG_GRID_MIN_LEVEL;
    while (out.level < DEBUG_GRID_MAX_LEVEL && debugGridPow10(out.level + 1) <= reference) {
        ++out.level;
    }
    for (std::uint32_t c = 0; c < DEBUG_GRID_CADENCES; ++c) {
        out.spacing[c] = debugGridPow10(out.level + static_cast<int>(c));
    }

    // THE FRACTION, LINEAR rather than logarithmic (D4). It is continuous, monotone, exactly 0 at
    // reference == spacing[0] and exactly 1 at reference == BASE * spacing[0] -- which is all the
    // continuity argument needs. It paces the crossfade slightly differently from a log fraction;
    // that is a LOOK decision, judged on the validation page, not a correctness one. It CLAMPS at
    // both clamped levels: the crossfade stops there, the cadence does not.
    out.fraction = std::clamp((reference / out.spacing[0] - 1.0F) / (DEBUG_GRID_BASE - 1.0F), 0.0F, 1.0F);

    // THE WEIGHTS -- the continuity rule, and the ONE place it is written.
    const float a = params.style.minorAlpha;
    const float b = params.style.majorAlpha;
    const float f = out.fraction;
    out.weight[0] = a * (1.0F - f);
    out.weight[1] = (b * (1.0F - f)) + (a * f);
    out.weight[2] = b * f;

    // THE RADII -- a function of the SPACING and the FAR PLANE, and of NOTHING ELSE. Never of the
    // level, never of viewScale: that is what carries continuity to the extent as well as to the
    // alpha (INV-3). The far-plane term does not weaken it, because farPlane is the SAME on both
    // sides of a decade boundary, so a given world spacing keeps the same radius across the change --
    // which is the property INV-3 actually needs. GR6 scales farPlane with the pose for that reason.
    for (std::uint32_t c = 0; c < DEBUG_GRID_CADENCES; ++c) {
        out.radius[c] = std::min(static_cast<float>(DEBUG_GRID_RADIUS_CELLS) * out.spacing[c],
                                 DEBUG_GRID_FAR_FRACTION * params.farPlane);
    }
    out.valid = true;
    return out;
}

std::uint32_t emitDebugGrid(DebugDrawBatch& batch, const DebugGridParams& params) {
    const DebugGridCadence cadence = debugGridCadence(params);
    if (!cadence.valid) {
        return 0;
    }
    // THE DISC CENTRE: the focus, projected onto the plane. NOT SNAPPED -- the lattice is
    // world-absolute (D6), so the line at x = 0 really is the world's x = 0.
    const Vec3 centre{params.focus.x, DEBUG_GRID_PLANE_HEIGHT, params.focus.z};
    std::uint32_t accepted = 0;

    // THE ORDER IS PART OF THE CONTRACT (GR24): cadence-major, family A then family B, k ascending,
    // axes LAST. Nothing depends on it for correctness -- the Tested bucket is one draw and depth
    // write is off on all four pipelines -- but a stable order is what makes GR6's vertex arm and
    // GR21's memcmp meaningful, and what makes a diff of two frames readable.
    for (std::uint32_t c = 0; c < DEBUG_GRID_CADENCES; ++c) {
        // family A: lines PARALLEL TO X, at constant z = k * spacing, spanning x
        accepted += emitFamily(batch, true, centre.z, centre.x, centre, cadence.spacing[c], cadence.radius[c],
                               params.style.lineColor, cadence.weight[c], params.drawAxes);
        // family B: lines PARALLEL TO Z, at constant x = k * spacing, spanning z
        accepted += emitFamily(batch, false, centre.x, centre.z, centre, cadence.spacing[c], cadence.radius[c],
                               params.style.lineColor, cadence.weight[c], params.drawAxes);
    }

    if (!params.drawAxes) {
        return accepted;  // the k == 0 lines came back above; no axis vertex is emitted at all
    }
    // THE AXES, LAST, AT THE COARSEST RADIUS so they span the whole grid, with the same radial fade.
    // Clipped to the disc like everything else, which means FLYING FAR ENOUGH FROM THE ORIGIN LOSES
    // THEM -- correct rather than tolerated: they mark the origin, and the origin is not there.
    const float axisRadius = cadence.radius[DEBUG_GRID_CADENCES - 1U];
    if (std::fabs(centre.z) < axisRadius) {
        const float half = std::sqrt((axisRadius * axisRadius) - (centre.z * centre.z));
        const Vec3 rgb{params.style.axisXColor.x, params.style.axisXColor.y, params.style.axisXColor.z};
        accepted += emitFadedLine(batch, true, 0.0F, centre.x - half, centre.x + half, centre, rgb,
                                  params.style.axisXColor.w, axisRadius);
    }
    if (std::fabs(centre.x) < axisRadius) {
        const float half = std::sqrt((axisRadius * axisRadius) - (centre.x * centre.x));
        const Vec3 rgb{params.style.axisZColor.x, params.style.axisZColor.y, params.style.axisZColor.z};
        accepted += emitFadedLine(batch, false, 0.0F, centre.z - half, centre.z + half, centre, rgb,
                                  params.style.axisZColor.w, axisRadius);
    }
    return accepted;
}

}  // namespace engine::render
