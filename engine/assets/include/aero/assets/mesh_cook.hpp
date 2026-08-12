#pragma once
// Aero Engine — the mesh cook (task 3.3.1): parallel arrays in, one .aeromesh container out.
//
// PURE, like cooked_mesh.hpp: no disk, no logging, no third party, no GPU, no per-OS macro.
//
// THE COOK CONVERTS NOTHING (D3). No axis flip, no winding reversal, no unit scaling, no handedness
// change, no normal renormalization, no tangent orthogonalization, no UV flip, and no setting for any
// of them. Every conversion this pipeline performs already happened inside the importer, per format,
// and by the time an ImportedModel exists the data is in the engine's own conventions. There is no
// MeshCookSettings type for exactly this reason: there is nothing to configure, and a settings struct
// with no fields is a shape that invites one. Adding it later is additive.
//
// IT ALSO RE-INDEXES NOTHING (D4). "Interleave + index" means precisely: turn the parallel arrays into
// one array-of-structs and re-emit the existing index list at the chosen width. No welding, no dedup,
// no vertex-cache optimization; SOURCE VERTEX ORDER IS PRESERVED. Welding would change vertex order
// (silently breaking any future morph-target or per-vertex correspondence), would need a hash map or a
// sort whose tie-breaking is a fresh determinism surface, and is a pure size optimization no consumer
// has asked for.
#include <aero/assets/cooked_mesh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {

// One source primitive as SPANS THE CALLER OWNS. EMPTY == ABSENT, the ImportedPrimitive rule verbatim
// -- with the same two exceptions: `positions` and `indices` must be non-empty or the primitive is
// dropped whole (AC-18).
//
// The spans may ALIAS each other and may be reused across primitives; the cook reads them and copies
// nothing it does not write.
struct MeshCookPrimitive {
    // THE POSITION in ImportedModel::meshes, and in that mesh's `primitives` -- see CookedSubmesh's
    // note. Together with the surviving attribute mask these two form the cook's ordering key.
    std::uint32_t sourceMeshIndex = 0;
    std::uint32_t sourcePrimitiveIndex = 0;
    std::uint32_t materialIndex = COOKED_INVALID_MATERIAL;  // preserved VERBATIM; never renumbered

    std::span<const Vec3> positions;
    std::span<const Vec3> normals;
    std::span<const Vec4> tangents;  // .w is the bitangent SIGN; preserved, never normalized
    std::span<const Vec2> uv0;
    std::span<const Vec2> uv1;
    std::span<const Vec4> colors;                          // linear RGBA
    std::span<const std::array<std::uint16_t, 4>> joints;  // widened u16 -> u32 on emit (D12)
    std::span<const Vec4> weights;
    std::span<const std::uint32_t> indices;
};

struct MeshCookInput {
    Guid sourceGuid;                                // may be nil; nil is legal and deterministic
    std::span<const MeshCookPrimitive> primitives;  // ORDER-INDEPENDENT -- see the contract below
};

// THE ORDER-INDEPENDENCE CONTRACT, stated exactly, because it is not unconditional:
//
//   The output is a pure function of (sourceGuid, the MULTISET of primitives) keyed by
//   (attributeMask, sourceMeshIndex, sourcePrimitiveIndex). Where that key REPEATS, the tied entries
//   keep the input's relative order (the sort is stable), so byte-identity across a reordering holds
//   exactly when the keys are distinct.
//
// Every meshCookPrimitives() result has distinct keys by construction -- one entry per
// (meshIndex, primitiveIndex). A caller that hands cookMesh two primitives with an identical key gets
// ONE WARNING naming the collision; nothing is dropped, because dropping would lose geometry over a
// caller's bookkeeping.

struct MeshCookStats {
    std::uint32_t sectionCount = 0;
    std::uint32_t submeshCount = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t droppedPrimitiveCount = 0;
    std::uint64_t byteSize = 0;
};

enum class MeshCookStatus : std::uint8_t { Ok = 0, Truncated };

struct MeshCookResult {
    MeshCookStatus status = MeshCookStatus::Ok;
    std::string message;                // "" IFF status == Ok. Two caps -> ONE status, TWO messages
                                        // joined with "; " (the ImportResult shape).
    std::vector<std::string> warnings;  // capped at MAX_COOK_WARNINGS
    std::size_t warningTotal = 0;       // UNCAPPED (the MAX_REPORTED_PER_CATEGORY shape, a fifth use)
    // A complete, parseable container; NEVER partial. parseCookedMesh(bytes) returns Ok for every
    // result whose bytes are non-empty -- INCLUDING the drop and cap arms, and including the
    // zero-primitive case, which is a valid 96-byte file rather than an absent one.
    //
    // EMPTY ONLY after an allocation failure, in which case status is Truncated and the message says
    // so. That arm is defence in depth against a ~928 MB worst case and no test in this tree reaches it.
    std::vector<std::byte> bytes;
    MeshCookStats stats;
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Deterministic (see the contract above).
[[nodiscard]] MeshCookResult cookMesh(const MeshCookInput& input);

}  // namespace engine::assets
