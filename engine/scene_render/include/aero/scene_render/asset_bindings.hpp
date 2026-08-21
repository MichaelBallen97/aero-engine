#pragma once
// Aero Engine -- engine::scene_render (task 3.1.5): the ONE place a component's asset GUID becomes a
// renderer handle. It lives here for the same reason buildRenderView does: this is the only code in the
// tree that sees BOTH engine::scene's vocabulary (Guid, through aero::core) and engine::render's
// (MeshHandle, MaterialHandle). render/ is deliberately scene-and-identity-free, so a Guid map inside
// ForwardRenderer would be the wrong layer; the editor filling this table is the right one.
//
// STORAGE IS SORTED VECTORS, keyed by Guid::operator< (guid.hpp's documented total order). NOT a hash
// container: std::unordered_map's move constructor is not noexcept on MSVC's STL, which is the 3.1.2 R9
// / C2607 rule, and a table with a few dozen entries binary-searches faster than it hashes anyway.
// Nothing here hashes a Guid, so guid.hpp's std::hash LINKAGE NOTE constrains this header not at all --
// that is a design property of the sorted-vector choice, not a coincidence.
#include <aero/core/guid.hpp>
#include <aero/render/render.hpp>  // render::MeshHandle, render::MaterialHandle

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>  // std::pair -- the sorted vectors' element type
#include <vector>

namespace engine::scene_render {

struct MeshBindingSubmesh {
    std::uint32_t submesh = 0;          // MeshInstance::submesh -- the POSITION in the cooked submesh table
    std::uint32_t sourceMeshIndex = 0;  // the D2 join key, copied verbatim from CookedSubmesh
    render::MaterialHandle material{};  // resolved from CookedSubmesh::materialIndex; INVALID is legal
                                        // and resolves to ForwardRenderer::defaultMaterial() at draw
};

struct MeshBinding {
    render::MeshHandle mesh{};
    std::vector<MeshBindingSubmesh> submeshes;  // in cooked submesh order; NOT filtered by anything
};

class AssetBindingTable {
public:
    void setMesh(Guid guid, MeshBinding binding);  // replaces an existing entry wholesale
    void removeMesh(Guid guid) noexcept;
    [[nodiscard]] const MeshBinding* findMesh(Guid guid) const noexcept;  // nullptr for nil or absent

    void setMaterial(Guid guid, render::MaterialHandle handle);
    void removeMaterial(Guid guid) noexcept;
    [[nodiscard]] render::MaterialHandle findMaterial(Guid guid) const noexcept;  // invalid for nil/absent

    void clear() noexcept;
    [[nodiscard]] std::size_t meshCount() const noexcept;
    [[nodiscard]] std::size_t materialCount() const noexcept;

private:
    std::vector<std::pair<Guid, MeshBinding>> meshes;                // SORTED by Guid
    std::vector<std::pair<Guid, render::MaterialHandle>> materials;  // SORTED by Guid
};

// The point of the sorted-vector decision, written down so a future storage change cannot make it
// quietly false: AssetBindingTable becomes a member of SceneRenderer, whose moves are `noexcept =
// default`. Under P1286R2 a throwing member does NOT delete that defaulted move -- it terminates at
// run time. These two turn that terminate into a compile error, exactly as entity_ops.hpp's
// equivalent asserts on CommandStack already do.
static_assert(std::is_nothrow_move_constructible_v<AssetBindingTable>);
static_assert(std::is_nothrow_move_assignable_v<AssetBindingTable>);

}  // namespace engine::scene_render
