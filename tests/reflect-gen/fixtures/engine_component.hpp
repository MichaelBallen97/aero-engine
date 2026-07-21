#pragma once
// Task 1.1.1 fixture: proves engine headers + engine types parse through libclang, and the exact
// annotated shape 1.1.2 will detect. Needs the per-OS sysroot flags (F4) + -I engine/core/include.
#include <aero/core/math.hpp>

struct [[engine::component]] Demo {
    engine::Vec3 position;
    engine::Quat rotation;
    float mass;
};
