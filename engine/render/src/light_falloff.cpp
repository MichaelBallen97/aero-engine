// engine/render/src/light_falloff.cpp -- task E.2.2: the three definitions light_falloff.hpp
// declares. A .cpp rather than inline definitions (the culling / shadow / tonemap / environment
// shape), so the public header stays declarations and their contracts.
#include <aero/render/light_falloff.hpp>

#include <cmath>

namespace engine::render {

namespace {

// saturate as a comparison chain: a NaN fails both comparisons and yields the literal 0.0F
// (environment.cpp's skyRadiance idiom). -0.0 also yields the literal 0.0F, because -0.0 > 0.0F is
// false -- which is what keeps "exactly 0" a claim about BITS below.
[[nodiscard]] float saturate(float x) noexcept { return (x > 0.0F) ? ((x < 1.0F) ? x : 1.0F) : 0.0F; }

// max with the floor SECOND and the comparison written so a NaN takes the floor.
[[nodiscard]] float atLeast(float x, float floor) noexcept { return x > floor ? x : floor; }

}  // namespace

float punctualDistanceAttenuation(float distanceSq, float range) noexcept {
    const float r = atLeast(range, PUNCTUAL_MIN_RANGE);
    const float f = distanceSq / (r * r);
    const float window = saturate(1.0F - (f * f));
    return (window * window) / atLeast(distanceSq, PUNCTUAL_MIN_DISTANCE_SQ);
}

SpotCone resolveSpotCone(float innerConeRadians, float outerConeRadians) noexcept {
    const float cosInner = std::cos(innerConeRadians);
    const float cosOuter = std::cos(outerConeRadians);
    // THE FINITENESS ARM COMES FIRST, and it is the one place a floor is not enough: a NaN
    // DIFFERENCE below takes SPOT_CONE_MIN_COS_DELTA and becomes a finite hard edge at `outer` with
    // attenuation 1.0 on the axis (measured). {0, 0} makes spotConeAttenuation exactly 0.0F for every
    // cosAngle, NaN included, on the CPU and -- because the block carries this pair -- on the GPU.
    // std::cos(+-inf) is NaN, so an infinite angle lands here too.
    if (!(std::isfinite(cosInner) && std::isfinite(cosOuter))) {
        return {.angleScale = 0.0F, .angleOffset = 0.0F};
    }
    const float scale = 1.0F / atLeast(cosInner - cosOuter, SPOT_CONE_MIN_COS_DELTA);
    return {.angleScale = scale, .angleOffset = -cosOuter * scale};
}

float spotConeAttenuation(float cosAngle, const SpotCone& cone) noexcept {
    const float t = saturate((cosAngle * cone.angleScale) + cone.angleOffset);
    return t * t;
}

}  // namespace engine::render
