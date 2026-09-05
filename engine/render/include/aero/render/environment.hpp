#pragma once
// Aero Engine — render environment vocabulary (task E.2.1). PUBLIC and PURE: no rhi type, no
// allocation, no logging, nothing that can throw -- the tonemap.hpp posture. THIS FILE IS THE SOURCE
// shaders/sky.frag.hlsl TRANSCRIBES (the two curve constants) and the source scene.frag.hlsl's
// hemispheric term transcribes; HE16's comment-stripped source-text pin keeps the copies honest.
//
// THE MODES EXIST HERE AND NOWHERE ELSE. resolveSkyGradient and resolveAmbient turn a mode into
// three colours the GPU adds together; the shaders carry no branch, no selector and no mode. And
// THE GPU RECEIVES DIFFERENCES: a zero delta makes `x + 0 * w` exact on every backend, which is what
// makes Solid reproduce a CLEAR and Flat reproduce a CONSTANT bit for bit. A lerp intrinsic would
// not: DXC maps HLSL `lerp` to SPIR-V's FMix (DirectXShaderCompiler docs/SPIR-V.rst), and FMix is
// SPECIFIED as x * (1 - a) + y * a (GLSL.std.450), whose two roundings need not sum back to x when
// x == y. DO NOT "SIMPLIFY" THE RESOLVERS INTO ENDPOINTS.
//
// The one edge the delta rule does not cover, stated rather than left to be rediscovered: for a
// channel that is NEGATIVE ZERO, -0.0 + 0.0 * w is +0.0 -- numerically equal, bit-different. No
// default here is -0.0 and no test seeds one.

#include <aero/core/math.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::render {

// The values are EXPLICIT because engine::Environment stores them as uint32 selectors (docs/09 §2.3
// payload); the default is 0 on BOTH so the bridge's out-of-range clamp lands on the default, which
// is the clampPrimitive rule. `: std::uint8_t` because docs/04 requires an explicit underlying type.
enum class BackgroundMode : std::uint8_t { Sky = 0, Solid = 1, Count = 2 };
enum class AmbientMode : std::uint8_t { Hemisphere = 0, Flat = 1, Count = 2 };
inline constexpr std::size_t BACKGROUND_MODE_COUNT = 2;
inline constexpr std::size_t AMBIENT_MODE_COUNT = 2;
static_assert(static_cast<std::size_t>(BackgroundMode::Count) == BACKGROUND_MODE_COUNT);
static_assert(static_cast<std::size_t>(AmbientMode::Count) == AMBIENT_MODE_COUNT);

// The RESOLVED shape the bridge fills field for field from engine::Environment -- the
// DirectionalLightData posture. THE DEFAULTS MIRROR THE COMPONENT'S and neither header can include
// the other (render never learns about scene), so scene_render_test's witness case asserts the
// agreement rather than assuming it. Change one, change the other, or that case reddens.
struct EnvironmentData {
    BackgroundMode backgroundMode = BackgroundMode::Sky;
    Vec3 skyColor{0.16F, 0.26F, 0.48F};
    Vec3 horizonColor{0.52F, 0.58F, 0.68F};
    Vec3 groundColor{0.10F, 0.09F, 0.085F};
    Vec3 solidColor{0.06F, 0.06F, 0.07F};
    AmbientMode ambientMode = AmbientMode::Hemisphere;
    Vec3 ambientColor{0.03F, 0.03F, 0.03F};
    float ambientIntensity = 0.5F;
    [[nodiscard]] bool operator==(const EnvironmentData&) const = default;
};

// TOTAL: any value >= Count yields the DEFAULT enumerator (0), which is clampPrimitive's rule
// verbatim. There is no error channel and nothing warns: a hand-edited scene file carrying a 7 is a
// legal uint32 and the Inspector clamps it on the next edit.
[[nodiscard]] BackgroundMode clampBackgroundMode(std::uint32_t raw) noexcept;
[[nodiscard]] AmbientMode clampAmbientMode(std::uint32_t raw) noexcept;

// What sky.frag.hlsl receives: colour(dir) = horizon + skyDelta * wSky + groundDelta * wGround.
struct SkyGradient {
    Vec3 horizon;
    Vec3 skyDelta;     // skyColor - horizonColor; EXACTLY ZERO in Solid mode
    Vec3 groundDelta;  // groundColor - horizonColor; EXACTLY ZERO in Solid mode
    [[nodiscard]] bool operator==(const SkyGradient&) const = default;
};

// What scene.frag.hlsl receives: ambient(N) = mid + halfDelta * N.y.
struct HemisphereAmbient {
    Vec3 mid;        // 0.5 * (sky + ground) * intensity;  ambientColor * intensity in Flat mode
    Vec3 halfDelta;  // 0.5 * (sky - ground) * intensity;  EXACTLY ZERO in Flat mode
    [[nodiscard]] bool operator==(const HemisphereAmbient&) const = default;
};

// Sky   -> {horizonColor, skyColor - horizonColor, groundColor - horizonColor}
// Solid -> {solidColor, 0, 0}   (skyColor / horizonColor / groundColor are NOT read)
[[nodiscard]] SkyGradient resolveSkyGradient(const EnvironmentData& env) noexcept;

// Hemisphere -> {0.5 * (sky + ground) * I, 0.5 * (sky - ground) * I}   (horizonColor is NOT read)
// Flat       -> {ambientColor * I, 0}                                  (sky / ground are NOT read)
[[nodiscard]] HemisphereAmbient resolveAmbient(const EnvironmentData& env) noexcept;

// The gradient's shape, and the two numbers sky.frag.hlsl transcribes as bare literals.
//   wSky    = 1 - pow(1 - saturate( dir.y), SKY_CURVE_POWER)
//   wGround = 1 - pow(1 - saturate(-dir.y), GROUND_CURVE_POWER)
// Both are EXACTLY 0 at the horizon, so the two halves meet at horizonColor and the gradient is
// continuous there. Ground is steeper so the picture below the horizon reaches groundColor within a
// few degrees, which is what keeps E.1.2's grid legible over it (validation row 3 judges that).
// TUNING CONSTANTS (D8): no automated case claims they are RIGHT, only that the copies agree.
inline constexpr float SKY_CURVE_POWER = 4.0F;
inline constexpr float GROUND_CURVE_POWER = 8.0F;

// The reference the GPU tier compares pixels against. `direction` need NOT be unit length; a
// zero-length input yields `horizon` (t = b = 0) -- TOTAL, never NaN. The zenith is
// horizon + skyDelta, which is within two ulps of skyColor and is deliberately NOT bit-equal to it
// (E.1.3's `a + (b - a)` is not `b`): the case that pins it says so and states its tolerance.
[[nodiscard]] Vec3 skyRadiance(const SkyGradient& gradient, Vec3 direction) noexcept;

// mid + halfDelta * unitNormal.y. EXACTLY `mid` for a Flat resolution on every normal.
[[nodiscard]] Vec3 ambientIrradiance(const HemisphereAmbient& ambient, Vec3 unitNormal) noexcept;

}  // namespace engine::render
