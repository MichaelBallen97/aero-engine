#pragma once
// Task 2.2.2 fixture — the std::string category, namespaced. Carries NO annotation, so --emit-meta's
// output must include NO conditional `#include <aero/reflect/annotations.hpp>` (the AC-4 negative
// case, sibling of annotations_meta's positive one).
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

#include <cstdint>
#include <string>

namespace engine::demo {
struct AERO_COMPONENT Labelled {
    std::string label;
    std::string notes;
    float weight;
    std::uint16_t slot;
    engine::Vec3 tint;
};
}  // namespace engine::demo
