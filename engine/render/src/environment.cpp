// engine/render/src/environment.cpp — task E.2.1: the environment vocabulary behind environment.hpp.
// Pure arithmetic over engine math types, the culling.cpp / animation.cpp / tonemap.cpp shape:
// nothing here allocates, logs, recurses, touches a GPU or holds static mutable state, and there is
// no profiling include either (a handful of float operations, where a Tracy zone would cost more
// than the work it measured).
//
// TOTALITY, STATED EXACTLY. Both clamps and both resolvers are total in the strong sense: every
// input, including an out-of-range selector, yields a defined in-range answer. skyRadiance is total
// in the strong sense too -- a zero-length direction and a non-finite one both yield the horizon
// colour rather than a NaN pixel, which is what lets it serve as the ORACLE the GPU tier compares
// pixels against. ambientIrradiance propagates whatever its caller hands it, and is EXACTLY `mid`
// whenever halfDelta is zero.

#include <aero/render/environment.hpp>

#include <cmath>
#include <cstdint>

namespace engine::render {

BackgroundMode clampBackgroundMode(std::uint32_t raw) noexcept {
    return raw < BACKGROUND_MODE_COUNT ? static_cast<BackgroundMode>(raw) : BackgroundMode::Sky;
}

AmbientMode clampAmbientMode(std::uint32_t raw) noexcept {
    return raw < AMBIENT_MODE_COUNT ? static_cast<AmbientMode>(raw) : AmbientMode::Hemisphere;
}

SkyGradient resolveSkyGradient(const EnvironmentData& env) noexcept {
    if (env.backgroundMode == BackgroundMode::Solid) {
        // ZERO deltas, written as a value-initialised Vec3 rather than `sky - sky`: the shader then
        // evaluates horizon + 0 * w + 0 * w, which is EXACTLY horizon on every backend.
        return {.horizon = env.solidColor, .skyDelta = Vec3{}, .groundDelta = Vec3{}};
    }
    return {.horizon = env.horizonColor,
            .skyDelta = env.skyColor - env.horizonColor,
            .groundDelta = env.groundColor - env.horizonColor};
}

HemisphereAmbient resolveAmbient(const EnvironmentData& env) noexcept {
    if (env.ambientMode == AmbientMode::Flat) {
        return {.mid = env.ambientColor * env.ambientIntensity, .halfDelta = Vec3{}};
    }
    // (a + b) * 0.5F * I and (a - b) * 0.5F * I -- so a Flat resolution at intensity 1 and a
    // Hemisphere resolution with sky == ground produce `mid` by the same two operations.
    return {.mid = (env.skyColor + env.groundColor) * 0.5F * env.ambientIntensity,
            .halfDelta = (env.skyColor - env.groundColor) * 0.5F * env.ambientIntensity};
}

Vec3 skyRadiance(const SkyGradient& gradient, Vec3 direction) noexcept {
    // NORMALISE HERE, not at the call site, and handle the zero vector rather than dividing by it:
    // this function is the ORACLE the GPU tier compares against, so it must be total on exactly the
    // inputs a test can hand it. A zero-length direction yields y = 0 -> both weights 0 -> horizon.
    const float len =
        std::sqrt((direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z));
    const float y = len > 0.0F ? direction.y / len : 0.0F;
    // NOT std::clamp: std::clamp(NaN, lo, hi) returns NaN on libc++ (the 3.7.2 rule, hit again by
    // E.1.2's std::min(NaN, 48.0F)). This comparison chain sends a NaN to 0 on BOTH weights, so a
    // non-finite direction yields the horizon colour rather than a NaN pixel.
    const float t = (y > 0.0F) ? ((y < 1.0F) ? y : 1.0F) : 0.0F;     // saturate( y)
    const float b = (-y > 0.0F) ? ((-y < 1.0F) ? -y : 1.0F) : 0.0F;  // saturate(-y)
    const float wSky = 1.0F - std::pow(1.0F - t, SKY_CURVE_POWER);
    const float wGround = 1.0F - std::pow(1.0F - b, GROUND_CURVE_POWER);
    return gradient.horizon + (gradient.skyDelta * wSky) + (gradient.groundDelta * wGround);
}

Vec3 ambientIrradiance(const HemisphereAmbient& ambient, Vec3 unitNormal) noexcept {
    // ONE scaled delta, never a lerp: a Flat resolution (halfDelta == 0) is then EXACTLY `mid` on
    // every normal, which is what keeps DG16's and the OG battery's expectations true.
    return ambient.mid + (ambient.halfDelta * unitNormal.y);
}

}  // namespace engine::render
