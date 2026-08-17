#pragma once
// engine/render/src/mesh_pack.hpp — task 3.5.1. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the material_pack.hpp / skinning_pack.hpp precedent, reached
// from tests/ by a relative include.
//
// The pure cooked-section -> two-GPU-streams repack. It is a named function with a header for the
// same reason the two packers beside it are: a file-local repack is unfalsifiable, and a transposed
// offset or a skipped absent-attribute default draws a garbled mesh rather than failing anything.
//
// ATTRIBUTE-TABLE-DRIVEN, never assumed offsets. The cook emits its own canonical attribute order and
// its own stride; this reads BOTH out of the section's slice of the parsed table and reaches the bytes
// through cooked_mesh.hpp's bounds-checked get* primitives, so a cook that reorders its layout, or a
// hand-built file that permutes one, still repacks correctly. Nothing here knows that Position is at
// offset 0.

#include <aero/assets/cooked_mesh.hpp>
#include <aero/core/math.hpp>
#include <aero/render/mesh.hpp>  // MeshVertex

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace engine::render::detail {

// Stream 1's element, bound at slot 1 for skinned sections only. The joints stay the WIRE FORMAT's
// u32x4 verbatim: rhi::VertexFormat::Uint4 exists and UShort4 does not, and cooked_mesh.hpp already
// records that 8-bytes-per-vertex trade as v1's with its v2 fix named. Weights are the cooked Float4,
// copied bit for bit — the importers normalize, the cook does not, and neither does this.
struct SkinVertex {
    std::array<std::uint32_t, 4> joints{};
    Vec4 weights{};
};
static_assert(sizeof(SkinVertex) == 32);
static_assert(std::is_standard_layout_v<SkinVertex>);

struct PackedMeshSection {
    std::vector<MeshVertex> stream0;  // vertexCount entries, ALWAYS (defaults where an attribute is absent)
    std::vector<SkinVertex> stream1;  // vertexCount entries IFF the section carries Joints0 AND Weights0
    bool droppedAttributes = false;   // TexCoord1 / Color0 were present; the caller latches ONE WARN
};

// TOTAL for any section index parseCookedMesh returned Ok for. Both streams come back EMPTY — never
// partially filled — for an out-of-range index, an empty section, a section whose bulk bytes are
// shorter than vertexCount x vertexStride, an attribute slice that does not fit the parsed table, or
// an attribute that does not fit inside its own stride. A caller bug must not become a misread.
[[nodiscard]] PackedMeshSection packMeshSection(const assets::CookedMesh& mesh, std::uint32_t sectionIndex);

}  // namespace engine::render::detail
