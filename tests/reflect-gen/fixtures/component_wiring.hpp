#pragma once
// Task 1.1.4 fixture — the SECOND header in aero_reflect_meta_test's aero_reflect_generate() call, so
// the machine-generated aggregator is proven over more than one per-header register function (D12).
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

#include <cstdint>

struct AERO_COMPONENT ReflectWiring {
    engine::Vec3 target;
    float speed;
    std::uint32_t gear;
    bool engaged;
};
