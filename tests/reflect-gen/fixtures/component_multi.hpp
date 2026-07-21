#pragma once
// Task 1.1.2 fixture — two components: a global struct and a namespaced class, proving multiple
// detection, the class kind, and fully-qualified naming (engine::demo::Light).
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

struct AERO_COMPONENT Player {
    engine::Vec3 position;
    int score;
};

namespace engine::demo {
class AERO_COMPONENT Light {
public:
    engine::Vec3 color;
    double intensity;
};
}  // namespace engine::demo
