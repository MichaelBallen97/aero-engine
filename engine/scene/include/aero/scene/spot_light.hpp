#pragma once
// engine::SpotLight (task E.2.2): a cone light -- the lamp that points at something. Pure reflected
// data, no .cpp. Registered as the TENTH built-in in engine/scene/src/transform.cpp.
//
// AIMED DOWN ITS ENTITY'S -Z WORLD AXIS, EXACTLY AS DirectionalLight IS (1.4.1's D6), and PLACED AT
// ITS ENTITY'S WORLD TRANSLATION, EXACTLY AS PointLight IS: one rule covers every directional thing
// in the engine, and nothing here stores a position or a direction. An entity carrying this and no
// Transform sits at the origin pointing down -Z (worldMatrix's identity contribution), which is
// PointLight's behaviour today and is not an error.
//
// THE TWO CONE ANGLES ARE HALF-ANGLES FROM THE AXIS, IN RADIANS, with the `Radians` suffix
// Camera::fovYRadians established (constants.hpp's D6: radians everywhere in the public API; the
// Inspector edits the radian value raw, camera.hpp's D17). The bound 1.5707964f is the float nearest
// pi/2 -- bit for bit HALF_PI -- so the Inspector clamps to a hemisphere at most. NOTHING VALIDATES
// inner <= outer: the render layer's punctual-falloff vocabulary folds inner >= outer into a
// hard-edged cone, and an outer cone of 0 lights nothing, which is what a cone with no interior
// means. (The layer is named rather than its header path, so that "no scene header mentions the
// render layer's files" stays greppable in both directions -- light.hpp's own posture.)
//
// intensity IS THE IRRADIANCE THE LIGHT DELIVERS ON ITS AXIS AT ONE WORLD UNIT -- task E.2.2's
// inverse-square falloff, shared with PointLight -- in the same units as DirectionalLight::intensity.
// range is the world-unit cutoff radius; <= 0 means the light contributes nothing (never NaN), and
// neither carries an AERO_RANGE: 1.3.3's D19, as for PointLight. Every default is a TUNING CONSTANT
// judged on the validation page; a change is a RECORDED AMENDMENT that moves on BOTH sides of the
// render boundary, which the bridge test's witness case enforces.
#include <aero/core/math.hpp>            // Vec3, radians
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT, AERO_COLOR, AERO_RANGE

#include <type_traits>

namespace engine {

struct AERO_COMPONENT SpotLight {
    Vec3 color AERO_COLOR = Vec3::one();  // linear RGB; may exceed 1 (HDR); not clamped (plain data)
    float intensity = 1.0f;               // irradiance on the axis at one world unit
    float range = 10.0f;                  // world-unit cutoff radius; the window reaches exactly 0 there
    // Half-angles from the axis, RADIANS. Full intensity inside inner, zero outside outer, a squared
    // linear blend in cosine space between (light_falloff.hpp). `= radians(...)`, never a
    // brace-initialiser after the annotation: camera.hpp's own form, the only one reflect-gen's
    // per-header cases have ever parsed after an AERO_RANGE.
    float innerConeRadians AERO_RANGE(0.0f, 1.5707964f) = radians(20.0f);
    float outerConeRadians AERO_RANGE(0.0f, 1.5707964f) = radians(30.0f);

    bool operator==(const SpotLight&) const = default;
};

static_assert(std::is_trivially_copyable_v<SpotLight>);
static_assert(std::is_standard_layout_v<SpotLight>);
static_assert(std::is_aggregate_v<SpotLight>);
static_assert(sizeof(SpotLight) == 7 * sizeof(float));  // 12 + 4 + 4 + 4 + 4, no padding
static_assert(alignof(SpotLight) == alignof(float));

}  // namespace engine
