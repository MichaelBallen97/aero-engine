#pragma once
// Task 1.1.2 fixture — the all-supported happy path (Vec3 + Quat + primitives, no warnings), a sibling
// NON-component to prove non-detection, and a static member + method to prove they are NOT collected
// as fields (E11/AC-9).
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

struct AERO_COMPONENT Transform {
    engine::Vec3 position;
    engine::Quat rotation;
    float mass;
    int hitPoints;
    bool active;

    static constexpr int SCHEMA_VERSION = 1;  // static data member — must NOT appear as a field
    float lengthSquared() const;              // method — must NOT appear as a field
};

struct NotAComponent {
    float x;
};
