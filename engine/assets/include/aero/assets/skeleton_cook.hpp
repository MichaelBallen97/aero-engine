#pragma once
// Aero Engine — the skeleton cook (task 3.5.1): already-resolved plain data in, .aeroskel v1 bytes
// out. The PRODUCER half of the cooked skeleton container; cooked_skeleton.hpp is the FORMAT half,
// and a runtime that never cooks anything needs only that one.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// AND ZERO FLOATING-POINT ARITHMETIC -- every TRS component and every inverse-bind cell travels
// std::bit_cast bit for bit through putF32. Closure, ordering and validation are integer work. No
// hash container anywhere: grouping and ordering are sorted vectors, so no output can depend on an
// iteration order.
//
// THE COOK CONVERTS NOTHING. No axis flip, no unit scaling, no quaternion renormalization, no
// matrix decomposition, no rebasing of a rig onto a synthetic root. There is no SkeletonCookSettings
// type, for the same reason there is no MeshCookSettings and no TextureCookSettings: an empty
// settings struct is a shape that invites a field.
#include <aero/assets/cooked_skeleton.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {

// The caller's spelling of "no parent" and "no palette slot". Identical in value to
// COOKED_SKELETON_INVALID_INDEX -- the input's markers and the file's markers are the same markers,
// and this static_assert is what keeps them that way.
inline constexpr std::uint32_t SKELETON_INVALID_INDEX = 0xFFFFFFFFU;
static_assert(SKELETON_INVALID_INDEX == COOKED_SKELETON_INVALID_INDEX);

// One joint as the caller already resolved it: no model, no node tree, no editor type. `localId` is
// the caller's own identity for the joint (for the editor adapter, the source node's localId), and
// `parentLocalId` names another entry of the SAME input by that identity -- never a position.
struct SkeletonCookJoint {
    std::uint32_t localId = 0;
    std::uint32_t parentLocalId = SKELETON_INVALID_INDEX;  // INVALID = a root of this rig
    std::uint32_t paletteSlot = SKELETON_INVALID_INDEX;    // INVALID = hierarchy-only
    Vec3 translation{};                                    // bind-LOCAL TRS, bit-copied
    Quat rotation = Quat::identity();
    Vec3 scale = Vec3::one();
    Mat4 inverseBind = Mat4::identity();  // ignored for hierarchy-only records (see cookSkeleton)
};

struct SkeletonCookInput {
    Guid sourceGuid;                    // may be nil; nil is legal and deterministic
    std::uint32_t sourceSkinIndex = 0;  // the POSITION in the model's skin list, never a localId
    std::span<const SkeletonCookJoint> joints;
};

// BINARY, deliberately unlike MeshCookStatus's three values: a skeleton is never partially cooked.
// Dropping a joint the way the mesh cook drops a primitive would bind vertices to the wrong bones,
// which is a silently wrong picture rather than a smaller artifact.
enum class SkeletonCookStatus : std::uint8_t { Ok = 0, Invalid };

struct SkeletonCookResult {
    SkeletonCookStatus status = SkeletonCookStatus::Ok;
    std::string message;  // "" IFF Ok
    // v1's cook emits NO warning at all, and a test pins that. The vector exists for shape parity
    // with MeshCookResult and so the first thing that wants one does not have to change the result
    // type. Do NOT write a synthetic warning to make the field look used -- the model-level
    // advisories (multi-skin, out-of-range vertex joint index) belong to the editor adapter, which
    // is the only layer that can see a model at all.
    std::vector<std::string> warnings;
    std::vector<std::byte> bytes;  // EMPTY IFF Invalid
};

// Canonicalize and emit. Refusals, each with a message naming the defect: an empty joint list,
// either cap exceeded, a duplicate localId, a parentLocalId that names no entry of the input, a
// parent cycle, or a palette-slot set that is not a bijection onto [0, paletteCount).
//
// THE EMITTED ORDER IS NORMATIVE (docs/09 section 12.4) and is what makes the parser's one-line
// invariant hold by construction: parents-before-children, ties broken by ascending
// sourceNodeLocalId. It is produced by Kahn's algorithm over sorted vectors -- at each step the
// READY set (roots, plus every joint whose parent is already emitted) is chosen from in ascending
// localId order -- so the same joints in any input permutation cook to identical bytes. A step that
// finds no ready joint while joints remain IS the cycle detector; there is no separate traversal and
// no recursion anywhere.
//
// Hierarchy-only records (paletteSlot INVALID) are emitted with an IDENTITY inverse bind matrix
// whatever the input carried: they are bound to no vertex, so their IBM is never read against the
// palette, and writing identity keeps the bytes a function of the rig rather than of the caller's
// scratch.
[[nodiscard]] SkeletonCookResult cookSkeleton(const SkeletonCookInput& input);

}  // namespace engine::assets
