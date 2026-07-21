#pragma once
// Task 1.1.3 fixture — the single comprehensive runtime-codegen sample compiled + registered by
// aero_reflect_meta_test: Vec3 + Quat + primitives (all registered), a Vec4 (skipped — unsupported),
// a static member + a method (excluded). Distinct struct name (ReflectSample) so it never collides
// with the global `Transform` in component_basic/_unsupported (those stay text-only proofs).
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

struct AERO_COMPONENT ReflectSample {
    engine::Vec3 position;
    engine::Quat rotation;
    float mass;
    int hitPoints;
    bool active;
    engine::Vec4 velocity;  // unsupported -> skipped by codegen (still compiles)

    static constexpr int SCHEMA_VERSION = 1;  // static -> excluded from fields
    float lengthSquared() const;              // method -> excluded from fields
};
