#pragma once
// engine::MeshRenderer (task 1.4.1): the reflected "this entity draws a primitive mesh" component.
// `primitive` is a uint32 SELECTOR, not an enum — reflect-gen cannot reflect an enum yet, and a uint32
// and a future `enum class : uint32` serialize identically (both JSON numbers), so this is
// forward-compatible. The primitive→geometry mapping and all GPU state live in render
// (engine::render::PrimitiveId / ForwardRenderer); this component is pure reflected data (no .cpp).
// Registered as the 5th built-in in engine/scene/src/transform.cpp. `primitive` carries an
// AERO_RANGE and `color` an AERO_COLOR (task 2.2.2) so the inspector renders a clamped selector
// slider and a colour picker respectively.
#include <aero/core/math.hpp>            // Vec3
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT

#include <cstdint>
#include <type_traits>

namespace engine {

struct AERO_COMPONENT MeshRenderer {
    // 0=Cube, 1=Sphere, 2=Plane (render::PrimitiveId); clamped in the bridge AND by the inspector's
    // AERO_RANGE(0, 2) (task 2.2.2, the documented selector range).
    std::uint32_t primitive AERO_RANGE(0, 2) = 0;
    Vec3 color AERO_COLOR = Vec3::one();  // linear-RGB base color; may exceed 1 (HDR), not clamped

    bool operator==(const MeshRenderer&) const = default;
};

static_assert(std::is_trivially_copyable_v<MeshRenderer>);
static_assert(std::is_standard_layout_v<MeshRenderer>);
static_assert(std::is_aggregate_v<MeshRenderer>);
static_assert(sizeof(MeshRenderer) == 4 * sizeof(float));  // 4 (uint32) + 12 (Vec3), no padding

}  // namespace engine
