#pragma once
// engine::MeshRenderer (task 1.4.1): the reflected "this entity draws a primitive mesh" component.
// `primitive` is a uint32 SELECTOR, not an enum — reflect-gen cannot reflect an enum yet, and a uint32
// and a future `enum class : uint32` serialize identically (both JSON numbers), so this is
// forward-compatible. The primitive→geometry mapping and all GPU state live in render
// (engine::render::PrimitiveId / ForwardRenderer); this component is pure reflected data (no .cpp).
// Registered as the 5th built-in in engine/scene/src/transform.cpp. `primitive` carries an
// AERO_RANGE and `color` an AERO_COLOR (task 2.2.2) so the inspector renders a clamped selector
// slider and a colour picker respectively.
#include <aero/core/guid.hpp>            // Guid (task 3.1.5)
#include <aero/core/math.hpp>            // Vec3
#include <aero/reflect/annotations.hpp>  // AERO_COMPONENT

#include <cstdint>
#include <type_traits>

namespace engine {

struct AERO_COMPONENT MeshRenderer {
    // 0=Cube, 1=Sphere, 2=Plane (render::PrimitiveId); clamped in the bridge AND by the inspector's
    // AERO_RANGE(0, 2) (task 2.2.2, the documented selector range). CONSULTED ONLY WHEN `mesh` IS NIL.
    std::uint32_t primitive AERO_RANGE(0, 2) = 0;
    Vec3 color AERO_COLOR = Vec3::one();  // linear-RGB base color; may exceed 1 (HDR), not clamped

    // ---- task 3.1.5: the asset reference. APPENDED, never inserted -- declaration order IS the JSON
    // payload order AND the inspector row order, so old fields must keep reading first.
    //
    // NIL `mesh` => draw `primitive`, byte for byte as before 3.1.5. A VALID `mesh` names the cooked
    // model an asset GUID identifies, and `meshIndex` selects WHICH mesh of it: the POSITION in
    // ImportedModel::meshes, the same number ImportedNode::meshIndex holds and CookedSubmesh::
    // sourceMeshIndex records. Position, never localId -- an FBX localId is a raw ufbx typed_id and is
    // not an index into anything.
    //
    // STATED FRAGILITY: the reference survives re-imports of the same bytes and any edit that preserves
    // mesh ORDER; a re-export that REORDERS meshes silently retargets it. That is what every index-based
    // sub-asset id accepts, and the degradation is honest -- a meshIndex matching no cooked submesh
    // draws NOTHING and is counted (render::RenderView::unresolvedMeshes), never the wrong-but-plausible
    // thing. A content-hash encoding stays reachable for whoever needs cross-reorder stability (4.4.4).
    //
    // `material` NIL => each submesh draws the material its own source assigned (resolved by the
    // scene_render binding table); VALID => that one material overrides EVERY submesh of this entity.
    // Per-submesh overrides need a reflectable array, which the subset does not have.
    Guid mesh{};
    std::uint32_t meshIndex = 0;
    Guid material{};

    bool operator==(const MeshRenderer&) const = default;
};

static_assert(std::is_trivially_copyable_v<MeshRenderer>);
static_assert(std::is_standard_layout_v<MeshRenderer>);
static_assert(std::is_aggregate_v<MeshRenderer>);
// 4 (uint32 primitive) + 12 (Vec3 color) + 16 (Guid mesh) + 4 (uint32 meshIndex)
// + 4 PADDING (Guid is 8-aligned; 36 rounds up to 40) + 16 (Guid material) = 56, alignof 8.
//
// The four padding bytes are STATED rather than removed. Reordering to
// {primitive, meshIndex, color, mesh, material} would pack to 52 -- and would put `meshIndex` BEFORE
// `color` in declaration order, which is JSON payload order and inspector row order. Four bytes per
// MeshRenderer is not worth reordering the on-disk key order of every scene in the tree.
static_assert(sizeof(MeshRenderer) == 56);
static_assert(alignof(MeshRenderer) == 8);

}  // namespace engine
