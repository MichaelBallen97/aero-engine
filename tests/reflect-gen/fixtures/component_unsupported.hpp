#pragma once
// Task 1.1.2 fixture — a component with an out-of-subset field (Vec4). Detection stays exit 0, the
// field is tagged [unsupported], and a warning names it on stderr.
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

struct AERO_COMPONENT Transform {
    engine::Vec3 position;
    engine::Vec4 velocity;
    float mass;
};
