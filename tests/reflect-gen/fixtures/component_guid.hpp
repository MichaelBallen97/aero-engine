#pragma once
// Task 3.1.5 fixture — the engine::Guid category, namespaced. Modelled on component_text.hpp (the
// std::string fixture task 2.2.2 added): NO annotation anywhere, so --emit-meta's output must include
// NO conditional `#include <aero/reflect/annotations.hpp>` and NO .custom<FieldUiMeta> line.
//
// THREE fields on purpose: a Guid alongside a primitive and a Vec3, so the case pins that the new
// category COEXISTS with the existing subset rather than replacing it.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/reflect/annotations.hpp>

#include <cstdint>

namespace engine::demo {
struct AERO_COMPONENT Referencing {
    engine::Guid asset;  // the task 3.1.5 category
    std::uint32_t subIndex = 0;
    engine::Vec3 tint = engine::Vec3::one();
};
}  // namespace engine::demo
