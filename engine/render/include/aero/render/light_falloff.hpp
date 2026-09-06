#pragma once
// Aero Engine — punctual-light falloff vocabulary (task E.2.2). PUBLIC and PURE: no rhi type, no
// allocation, no logging, nothing that can throw -- environment.hpp's posture. THIS FILE IS THE
// SOURCE shaders/scene.frag.hlsl TRANSCRIBES: punctualDistanceAttenuation and spotConeAttenuation
// below are the CPU mirrors of the two HLSL helpers of the same name, operation for operation, and
// LP1's comment-stripped source-text pin keeps the copies honest.
//
// THE CONE IS RESOLVED HERE, NEVER ON THE GPU. resolveSpotCone turns two half-angles into the
// {scale, offset} pair the shader evaluates as saturate(cosAngle * scale + offset)^2 -- one FMA and
// one square per light per fragment, no trigonometry, no angle and no branch in the shader: E.2.1's
// rule that the GPU receives what it adds and multiplies, never what it must interpret.
//
// EVERY FUNCTION IS TOTAL, AND ONE OF THEM NEEDS MORE THAN A FLOOR TO BE. The clamps are comparison
// chains, never std::max, which returns its FIRST argument when that argument is a NaN (the 3.7.2
// std::clamp rule, in its third instance): a NaN or negative range takes the floor and a NaN
// distance yields 0. A NaN ANGLE is the case a floor gets WRONG -- a NaN cosine DIFFERENCE takes
// the delta floor and becomes a perfectly finite hard-edged cone at `outer`, attenuation 1.0 on the
// axis (measured, not reasoned) -- so resolveSpotCone tests finiteness FIRST and yields {0, 0},
// which makes spotConeAttenuation exactly 0 everywhere: the light is OFF. Whether the GPU's max()
// and saturate() agree on a NaN is backend-defined and deliberately not claimed for the OTHER
// fields -- the inherited posture of every unsanitised light field.

#include <aero/core/math.hpp>

namespace engine::render {

// The three floors, each 1e-4 and each deliberate. PUNCTUAL_MIN_DISTANCE_SQ is Filament's "a
// punctual light occupies a volume of 1 cm" (surface_light_punctual.fs), the divisor's floor that
// keeps a surface touching the light finite. PUNCTUAL_MIN_RANGE is the floor scene.frag.hlsl has
// applied to `range` since 1.4.1, kept so range <= 0 still means "contributes nothing beyond 1e-4
// world units" rather than NaN -- below that distance the window is still a window and the divisor
// is floored (PF7 pins the number). SPOT_CONE_MIN_COS_DELTA is the smallest cos(inner) - cos(outer)
// the resolver admits; below it the edge is hard. Filament and Unity's URP use the same three values.
inline constexpr float PUNCTUAL_MIN_DISTANCE_SQ = 1e-4F;
inline constexpr float PUNCTUAL_MIN_RANGE = 1e-4F;
inline constexpr float SPOT_CONE_MIN_COS_DELTA = 1e-4F;

// window(d, r) / max(d^2, PUNCTUAL_MIN_DISTANCE_SQ), with r = max(range, PUNCTUAL_MIN_RANGE) and
// window = saturate(1 - (d^2 / r^2)^2)^2 -- the Karis/Frostbite window over the inverse-square law
// (Filament's getSquareFalloffAttenuation + getDistanceAttenuation; URP's DistanceAttenuation).
// window is EXACTLY 1 at d = 0, EXACTLY 0 for every d >= r (its derivative vanishes there too, so
// the cutoff is a fade, not a ring), and non-increasing between. intensity * this is the irradiance
// at distance d, so intensity is the irradiance at ONE world unit up to the window.
[[nodiscard]] float punctualDistanceAttenuation(float distanceSq, float range) noexcept;

// What the shader receives per spot light.
struct SpotCone {
    float angleScale;   // 1 / max(cos(inner) - cos(outer), SPOT_CONE_MIN_COS_DELTA); 0 when OFF
    float angleOffset;  // -cos(outer) * angleScale; 0 when OFF
    [[nodiscard]] bool operator==(const SpotCone&) const = default;
};

// Half-angles from the axis, RADIANS (SpotLight's own unit). inner >= outer clamps the cosine
// difference to SPOT_CONE_MIN_COS_DELTA: a hard edge at `outer`, never an error and never a NaN.
// A NaN or infinite angle yields {0, 0} -- the light is off (see the header comment).
[[nodiscard]] SpotCone resolveSpotCone(float innerConeRadians, float outerConeRadians) noexcept;

// saturate(cosAngle * angleScale + angleOffset)^2. cosAngle is dot(spotDirection, lightToSurface):
// 1 on the axis. EXACTLY 1 inside the inner cone (a value above 1 saturates), EXACTLY 0 outside the
// outer cone by more than an ulp of cosine (a value below 0 saturates), and in between the square
// of a linear blend in cosine space -- URP's AngleAttenuation, Filament's getAngleAttenuation.
[[nodiscard]] float spotConeAttenuation(float cosAngle, const SpotCone& cone) noexcept;

}  // namespace engine::render
