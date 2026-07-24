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
// reflect-gen.components_engine_light and the generated meta/JSON artifacts.

#include <aero/core/math.hpp>            // Vec3
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT

#include <type_traits>

namespace engine {

// An infinitely-distant light with parallel rays (the sun). Direction = the entity's -Z world axis
// (task 1.4.1). No position, no range — it is everywhere.
struct AERO_COMPONENT DirectionalLight {
    Vec3 color = Vec3::one();  // linear RGB; may exceed 1 (HDR); not clamped (plain data)
    float intensity = 1.0f;

    bool operator==(const DirectionalLight&) const = default;
};

static_assert(std::is_trivially_copyable_v<DirectionalLight>);
static_assert(std::is_standard_layout_v<DirectionalLight>);
static_assert(std::is_aggregate_v<DirectionalLight>);
static_assert(sizeof(DirectionalLight) == 4 * sizeof(float));  // 12 + 4, no padding

// A point light radiating from the entity's world position (task 1.4.1) out to `range`. The
// falloff curve is the renderer's business (1.4.1); this component carries only the cutoff radius.
struct AERO_COMPONENT PointLight {
    Vec3 color = Vec3::one();  // linear RGB (see above)
    float intensity = 1.0f;
    float range = 10.0f;  // world-unit cutoff radius; > 0 by convention (not validated)

    bool operator==(const PointLight&) const = default;
};

static_assert(std::is_trivially_copyable_v<PointLight>);
static_assert(std::is_standard_layout_v<PointLight>);
static_assert(std::is_aggregate_v<PointLight>);
static_assert(sizeof(PointLight) == 5 * sizeof(float));  // 12 + 4 + 4, no padding

}  // namespace engine
