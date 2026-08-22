#pragma once
// Aero Engine — engine::DirectionalLight / engine::PointLight (task 1.3.3): two reflected light
// components. Two types, not one Light+discriminator (D1): reflect-gen cannot reflect an enum yet
// (its canonical kind is CXType_Enum, out of the subset), presence-of-component is the cleaner
// discriminator anyway, and this matches Godot's DirectionalLight3D / OmniLight3D split.
//
// Neither stores a direction or position (D6): a directional light aims down its entity's -Z world
// axis and a point light sits at its entity's world translation — both derived from the Transform
// in task 1.4.1. Colour is linear RGB in a Vec3 with a separate intensity multiplier (D7): Vec4 is
// out of the subset, and HDR wants intensity un-clamped anyway.
//
// REFLECTION: Vec3 + float(s), all in the subset — zero skips/warnings, pinned by
// reflect-gen.components_engine_light and the generated meta/JSON artifacts. Both `color` fields
// carry an AERO_COLOR (task 2.2.2) so the inspector renders a colour picker; `intensity`/`range`
// carry no range — honestly unbounded (HDR) or lacking a defensible bound (D19).

#include <aero/core/math.hpp>            // Vec3
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT

#include <type_traits>

namespace engine {

// An infinitely-distant light with parallel rays (the sun). Direction = the entity's -Z world axis
// (task 1.4.1). No position, no range — it is everywhere.
struct AERO_COMPONENT DirectionalLight {
    Vec3 color AERO_COLOR = Vec3::one();  // linear RGB; may exceed 1 (HDR); not clamped (plain data)
    float intensity = 1.0f;
    // Task 3.6.2 — shadow controls, APPENDED, never inserted: declaration order is JSON key order
    // AND inspector row order (the 3.1.5 MeshRenderer precedent). docs/09 section 2.3's missing-key
    // rule is silent, so every scene file authored before this task still loads with these defaults.
    //
    // Bias splits in two because a RasterizerState cannot be a runtime knob: it is baked into an
    // immutable pipeline at create(), so the shadow pipelines' slope-scaled bias (which defeats acne
    // on GRAZING surfaces) is fixed, and these two defeat the other two artefacts. shadowBias is a
    // constant offset in the depth comparison, for acne on FLAT surfaces facing the light;
    // shadowNormalBias is a world-unit offset along the GEOMETRIC normal applied before the
    // light-space transform, for peter-panning.
    //
    // shadowDistance is per-LIGHT rather than per-renderer because it is the sun's own reach, and
    // because a scene with two directional lights (legal, and WARNed about at the bridge) would
    // otherwise have to share one.
    bool castsShadows = true;
    float shadowBias AERO_RANGE(0.0f, 0.01f) = 0.0015f;
    float shadowNormalBias AERO_RANGE(0.0f, 0.5f) = 0.02f;
    float shadowDistance AERO_RANGE(1.0f, 1000.0f) = 50.0f;

    bool operator==(const DirectionalLight&) const = default;
};

static_assert(std::is_trivially_copyable_v<DirectionalLight>);
static_assert(std::is_standard_layout_v<DirectionalLight>);
static_assert(std::is_aggregate_v<DirectionalLight>);
static_assert(sizeof(DirectionalLight) == 8 * sizeof(float));  // 12 + 4 + 1 + 3 pad + 4 + 4 + 4

// A point light radiating from the entity's world position (task 1.4.1) out to `range`. The
// falloff curve is the renderer's business (1.4.1); this component carries only the cutoff radius.
struct AERO_COMPONENT PointLight {
    Vec3 color AERO_COLOR = Vec3::one();  // linear RGB (see above)
    float intensity = 1.0f;
    float range = 10.0f;  // world-unit cutoff radius; > 0 by convention (not validated)

    bool operator==(const PointLight&) const = default;
};

static_assert(std::is_trivially_copyable_v<PointLight>);
static_assert(std::is_standard_layout_v<PointLight>);
static_assert(std::is_aggregate_v<PointLight>);
static_assert(sizeof(PointLight) == 5 * sizeof(float));  // 12 + 4 + 4, no padding

}  // namespace engine
