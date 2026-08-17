#pragma once
// Aero Engine — the cooked skeleton container v1 (.aeroskel, task 3.5.1). The tree's SECOND
// first-party binary format, a sibling of .aeromesh rather than a region inside it: a skeleton is the
// DEFORMATION rig of one skin, and .aeromesh v1 does not move for it (no formatVersion bump, no
// golden churn).
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro.
//
// The normative specification of this format is docs/09-file-formats.md section 12. THIS HEADER IS
// NOT THE SPEC -- if the two ever disagree, docs/09 wins and one of them is a bug. The two
// COOKED_SKELETON_*_BYTES constants below are the ONLY sizes: `sizeof` is never taken of an on-disk
// record anywhere in this subsystem, because a struct's size is a compiler's opinion and a format's
// is not.
//
// INCLUDES, and the recorded reconciliation behind them: the task's own AC-2 asks for a
// core-and-standard-library-only include list AND for bytes formed exclusively through the eight
// existing put*/get* primitives. Both cannot hold -- those primitives live in cooked_mesh.hpp -- and
// the eight-places rule wins, exactly as it did one format ago (cooked_texture.hpp:24 carries the
// identical line). Nothing else is included, ever.
#include <aero/assets/cooked_mesh.hpp>  // the eight byte primitives + their endianness static_assert
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ---------------------------------------------------------------------------------
inline constexpr std::string_view COOKED_SKELETON_MAGIC = "AEROSKEL";  // 8 ASCII bytes, no NUL
inline constexpr std::uint32_t COOKED_SKELETON_FORMAT_VERSION = 1;
// "The same input now cooks to different bytes", and nothing else. It NEVER gates a parse -- see
// cooked_mesh.hpp's note on the distinction, which is the same one docs/09 section 9.11 states.
inline constexpr std::uint32_t COOKED_SKELETON_COOKER_VERSION = 1;

// ---- record sizes. The ONLY sizes. --------------------------------------------------------------
// 128 is a multiple of 16 and the header is 64, so this format has NO padding site anywhere:
// totalBytes == 64 + 128 * jointCount, always.
inline constexpr std::size_t COOKED_SKELETON_HEADER_BYTES = 64;
inline constexpr std::size_t COOKED_SKELETON_JOINT_BYTES = 128;

// The root marker for `parent` and the hierarchy-only marker for `paletteSlot`. One value, two
// meanings, both "this record names no entry in that table".
inline constexpr std::uint32_t COOKED_SKELETON_INVALID_INDEX = 0xFFFFFFFFU;

// ---- caps. Enforced by BOTH the writer and the parser. -----------------------------------------
// 1024 total records mirrors the importer's own MAX_JOINTS_PER_SKIN (closure ancestors are
// path-bounded, so they cannot push a legal rig past it in practice). 256 palette slots is the
// FORMAT's headroom and is deliberately wider than any renderer's: formats outlive renderers, and
// engine/render's own (narrower) bind-time limit lives in skinning.hpp, never here.
inline constexpr std::uint32_t MAX_COOKED_SKELETON_JOINTS = 1024;
inline constexpr std::uint32_t MAX_COOKED_SKELETON_PALETTE = 256;

// ---- the parsed records -------------------------------------------------------------------------
struct CookedSkeletonJoint {
    // Index into this table. COOKED_SKELETON_INVALID_INDEX = root; otherwise STRICTLY LESS than this
    // record's own index -- see CookedSkeleton's ordering note.
    std::uint32_t parent = COOKED_SKELETON_INVALID_INDEX;
    // < paletteJointCount, or COOKED_SKELETON_INVALID_INDEX for a hierarchy-only record (an ancestor
    // that carries a transform but is bound to no vertex).
    std::uint32_t paletteSlot = COOKED_SKELETON_INVALID_INDEX;
    // Provenance: the source node's localId, which is what lets a cooked animation clip (task 3.5.2)
    // and any diagnostic tie a record back to the node it came from.
    std::uint32_t sourceNodeLocalId = 0;
    Vec3 translation{};                // bind-LOCAL TRS, bit-copied. Locals rather than baked globals because a
    Quat rotation = Quat::identity();  // clip overwrites T, R and S channels member-wise.
    Vec3 scale = Vec3::one();
    Mat4 inverseBind = Mat4::identity();  // column-major, bit-copied; identity for hierarchy-only
};

// FULLY OWNED, unlike CookedMesh: there is no bulk region to retain a span into, and at <= 131 KB
// for a maximal file (64 + 128 * 1024) ownership is cheaper than a lifetime rule.
//
// ORDERING IS A FORMAT INVARIANT, not a convention: records are parents-before-children, so a
// consumer walks them in ONE forward pass with no recursion and no visited set. The parser enforces
// it as `parent < index`, which makes a cycle unrepresentable rather than merely detected.
struct CookedSkeleton {
    std::uint32_t formatVersion = 0;
    std::uint32_t cookerVersion = 0;
    Guid sourceGuid;
    std::uint32_t sourceSkinIndex = 0;  // the POSITION in the model's skin list, never a localId
    std::uint32_t paletteJointCount = 0;
    std::vector<CookedSkeletonJoint> joints;
};

enum class CookedSkeletonStatus : std::uint8_t {
    Ok = 0,
    TooSmall,            // shorter than the header
    BadMagic,            //
    UnsupportedVersion,  //
    ReservedNotZero,     // a reserved field is non-zero -- a REFUSAL, deliberately
    SizeMismatch,        // totalBytes != the buffer's own size, or != 64 + 128 * jointCount
    CapExceeded,         //
    BadHierarchy,        // zero/inconsistent counts, a forward parent reference, or a bad slot set
};
// A switch with NO `default:` (the cookedMeshStatusLabel precedent). NOT named toString: an engine
// toString(SomeEnum) is found by ADL inside doctest's stringifier and breaks every lane.
[[nodiscard]] std::string_view cookedSkeletonStatusLabel(CookedSkeletonStatus status) noexcept;

struct CookedSkeletonParseResult {
    CookedSkeletonStatus status = CookedSkeletonStatus::Ok;
    std::string message;      // "" IFF status == Ok
    CookedSkeleton skeleton;  // meaningful only when status == Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS.
//
// Written to the importer's hostile-input standard, because at Phase 5 this reads bytes out of a
// .pak that may have been shipped, patched, truncated by a failed download, or crafted. Every range
// check is a SUBTRACTION against the known-good size, never an addition that can wrap, and nothing
// is reserved before the header's counts have passed their caps.
//
// ONE DELIBERATE ASYMMETRY WITH .aeromesh: a .aeroskel is NEVER EMPTY. jointCount and
// paletteJointCount are both >= 1 at parse, because the skeleton cook is per-SKIN and a model with
// no skin produces no artifact at all, not an empty one. An empty skeleton is not a degenerate rig;
// it is the absence of one.
[[nodiscard]] CookedSkeletonParseResult parseCookedSkeleton(std::span<const std::byte> bytes);

}  // namespace engine::assets
