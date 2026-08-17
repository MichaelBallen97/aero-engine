// Aero Engine — the skeleton cook (task 3.5.1): canonicalization and the byte emit. See
// skeleton_cook.hpp for the contract and docs/09-file-formats.md section 12 for the normative
// format. NEVER THROWS. NEVER READS OR WRITES A FILE. NEVER LOGS. ZERO floating-point arithmetic:
// every float reaches the buffer through putF32's std::bit_cast and nothing else.
#include <aero/assets/skeleton_cook.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE (docs/09 section 12.2). The parser spells its own copy;
// neither reads the other's, so a transposed offset is a red golden rather than a shared mistake.
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_JOINT_COUNT = 36;
constexpr std::size_t H_PALETTE_JOINT_COUNT = 40;
constexpr std::size_t H_SOURCE_SKIN_INDEX = 44;
constexpr std::size_t H_TOTAL_BYTES = 48;

// Joint-record field offsets (docs/09 section 12.3).
constexpr std::size_t J_PARENT = 0;
constexpr std::size_t J_PALETTE_SLOT = 4;
constexpr std::size_t J_SOURCE_NODE_LOCAL_ID = 8;
constexpr std::size_t J_TRANSLATION = 16;
constexpr std::size_t J_ROTATION = 28;
constexpr std::size_t J_SCALE = 44;
constexpr std::size_t J_INVERSE_BIND = 56;

// How many localIds a cycle message names before it stops. A 1024-joint cycle produces a message,
// not a wall of text.
constexpr std::size_t MAX_NAMED_CYCLE_JOINTS = 8;

// One entry of the sorted localId -> input-position vector. Named so the comparator, the loops and
// the cycle message all spell the same type once.
using LocalIdEntry = std::pair<std::uint32_t, std::uint32_t>;

[[nodiscard]] SkeletonCookResult refuse(std::string message) {
    SkeletonCookResult out;
    out.status = SkeletonCookStatus::Invalid;
    out.message = std::move(message);
    return out;
}

void putVec3(std::span<std::byte> b, std::size_t offset, const Vec3& v) {
    putF32(b, offset + 0, v.x);
    putF32(b, offset + 4, v.y);
    putF32(b, offset + 8, v.z);
}

// x, y, z, w -- glTF's accessor order, which is also engine::Quat's member order.
void putQuat(std::span<std::byte> b, std::size_t offset, const Quat& q) {
    putF32(b, offset + 0, q.x);
    putF32(b, offset + 4, q.y);
    putF32(b, offset + 8, q.z);
    putF32(b, offset + 12, q.w);
}

// Column-major, cell for cell: columns[c] occupies the c-th group of four floats. No transpose --
// the file stores what engine::Mat4 stores, in the order it stores it.
void putMat4(std::span<std::byte> b, std::size_t offset, const Mat4& m) {
    for (std::size_t c = 0; c < 4; ++c) {
        const std::size_t o = offset + (c * 16);
        putF32(b, o + 0, m.columns[c].x);
        putF32(b, o + 4, m.columns[c].y);
        putF32(b, o + 8, m.columns[c].z);
        putF32(b, o + 12, m.columns[c].w);
    }
}

}  // namespace

SkeletonCookResult cookSkeleton(const SkeletonCookInput& input) {
    AERO_PROFILE_ZONE_NAMED("assets::cookSkeleton");

    // 1. the two whole-input refusals. An empty rig is not a degenerate skeleton, it is the absence
    //    of one, and the format cannot represent it (docs/09 section 12.0).
    const std::size_t jointCount = input.joints.size();
    if (jointCount == 0) {
        return refuse("the joint list is empty: a skeleton with no joints has no representation");
    }
    if (jointCount > MAX_COOKED_SKELETON_JOINTS) {
        return refuse(
            std::format("the joint list holds {} joints, over the cap of {}", jointCount, MAX_COOKED_SKELETON_JOINTS));
    }

    // 2. localId -> input position, as a SORTED VECTOR. Sorting by (localId, position) makes
    //    duplicates adjacent, so one linear pass finds them.
    std::vector<LocalIdEntry> byLocalId;
    byLocalId.reserve(jointCount);
    for (std::size_t i = 0; i < jointCount; ++i) {
        byLocalId.emplace_back(input.joints[i].localId, static_cast<std::uint32_t>(i));
    }
    std::sort(byLocalId.begin(), byLocalId.end());
    for (std::size_t k = 1; k < byLocalId.size(); ++k) {
        if (byLocalId[k].first == byLocalId[k - 1].first) {
            return refuse(std::format("two joints share localId {}", byLocalId[k].first));
        }
    }

    // 3. parents, resolved through that one sorted vector by binary search. An unresolvable parent
    //    is a REFUSAL and is never quietly promoted to a root: a joint whose parent is missing is a
    //    rig whose bind poses are wrong, and silently rooting it produces a plausible wrong picture.
    const auto byKey = [](const LocalIdEntry& entry, std::uint32_t value) { return entry.first < value; };
    std::vector<std::uint32_t> parentPos(jointCount, SKELETON_INVALID_INDEX);
    for (std::size_t i = 0; i < jointCount; ++i) {
        const std::uint32_t parentId = input.joints[i].parentLocalId;
        if (parentId == SKELETON_INVALID_INDEX) {
            continue;
        }
        const auto it = std::lower_bound(byLocalId.begin(), byLocalId.end(), parentId, byKey);
        if (it == byLocalId.end() || it->first != parentId) {
            return refuse(std::format("joint with localId {} names parent localId {}, which is not in the input",
                                      input.joints[i].localId, parentId));
        }
        parentPos[i] = it->second;
    }

    // 4. the palette slots: a bijection onto [0, paletteCount), where paletteCount is DERIVED by
    //    counting rather than declared.
    std::uint32_t paletteCount = 0;
    for (const SkeletonCookJoint& joint : input.joints) {
        if (joint.paletteSlot != SKELETON_INVALID_INDEX) {
            ++paletteCount;
        }
    }
    if (paletteCount == 0) {
        return refuse("no joint carries a palette slot: this rig binds no vertex");
    }
    if (paletteCount > MAX_COOKED_SKELETON_PALETTE) {
        return refuse(std::format("the rig carries {} palette slots, over the cap of {}", paletteCount,
                                  MAX_COOKED_SKELETON_PALETTE));
    }
    std::vector<bool> seen(paletteCount, false);
    for (std::size_t i = 0; i < jointCount; ++i) {
        const std::uint32_t slot = input.joints[i].paletteSlot;
        if (slot == SKELETON_INVALID_INDEX) {
            continue;
        }
        if (slot >= paletteCount) {
            return refuse(std::format("joint with localId {} names palette slot {} of {}", input.joints[i].localId,
                                      slot, paletteCount));
        }
        if (seen[slot]) {
            return refuse(std::format("palette slot {} is claimed by more than one joint", slot));
        }
        seen[slot] = true;
    }
    for (std::uint32_t slot = 0; slot < paletteCount; ++slot) {
        // UNREACHABLE, and defence in depth rather than a check: paletteCount slots that are all
        // distinct and all below paletteCount cover the range by pigeonhole, so the two arms above
        // already prove the bijection here. The PARSER's version of this check is the reachable one,
        // because a hostile file DECLARES its paletteJointCount instead of counting. Do not write a
        // synthetic case for this arm -- it would only look like proof.
        if (!seen[slot]) {
            return refuse(std::format("palette slot {} of {} is claimed by no joint", slot, paletteCount));
        }
    }

    // 5. the normative order. Kahn over the sorted vector: at each step the first READY joint in
    //    ascending-localId order is emitted, so ties break by localId and the result is independent
    //    of the caller's input order. A step that finds nothing ready while joints remain IS the
    //    cycle detector -- there is no visited set, no colouring and no recursion.
    std::vector<bool> emitted(jointCount, false);
    std::vector<std::uint32_t> emissionIndex(jointCount, SKELETON_INVALID_INDEX);
    std::vector<std::uint32_t> order;
    order.reserve(jointCount);
    while (order.size() < jointCount) {
        std::uint32_t chosen = SKELETON_INVALID_INDEX;
        for (const LocalIdEntry& entry : byLocalId) {
            const std::uint32_t pos = entry.second;
            if (emitted[pos]) {
                continue;
            }
            const std::uint32_t parent = parentPos[pos];
            if (parent == SKELETON_INVALID_INDEX || emitted[parent]) {
                chosen = pos;
                break;
            }
        }
        if (chosen == SKELETON_INVALID_INDEX) {
            std::string names;
            std::size_t named = 0;
            for (const LocalIdEntry& entry : byLocalId) {
                if (emitted[entry.second] || named == MAX_NAMED_CYCLE_JOINTS) {
                    continue;
                }
                if (!names.empty()) {
                    names += ", ";
                }
                names += std::to_string(entry.first);
                ++named;
            }
            return refuse(std::format("the parent links form a cycle among localIds {}", names));
        }
        emissionIndex[chosen] = static_cast<std::uint32_t>(order.size());
        emitted[chosen] = true;
        order.push_back(chosen);
    }

    // 6. emit. The buffer is value-initialized, so every reserved field is already 0 and no explicit
    //    store is needed for one anywhere; 128-byte records over a 64-byte header leave no padding
    //    site at all.
    const std::size_t totalBytes = COOKED_SKELETON_HEADER_BYTES + (jointCount * COOKED_SKELETON_JOINT_BYTES);
    std::vector<std::byte> out(totalBytes);
    const std::span<std::byte> bytes(out);
    for (std::size_t i = 0; i < COOKED_SKELETON_MAGIC.size(); ++i) {
        bytes[i] = static_cast<std::byte>(COOKED_SKELETON_MAGIC[i]);
    }
    putU32(bytes, H_FORMAT_VERSION, COOKED_SKELETON_FORMAT_VERSION);
    putU32(bytes, H_COOKER_VERSION, COOKED_SKELETON_COOKER_VERSION);
    putU64(bytes, H_GUID_HI, input.sourceGuid.hi);
    putU64(bytes, H_GUID_LO, input.sourceGuid.lo);
    putU32(bytes, H_JOINT_COUNT, static_cast<std::uint32_t>(jointCount));
    putU32(bytes, H_PALETTE_JOINT_COUNT, paletteCount);
    putU32(bytes, H_SOURCE_SKIN_INDEX, input.sourceSkinIndex);
    putU64(bytes, H_TOTAL_BYTES, totalBytes);

    for (std::size_t e = 0; e < order.size(); ++e) {
        const std::uint32_t pos = order[e];
        const SkeletonCookJoint& joint = input.joints[pos];
        const std::size_t o = COOKED_SKELETON_HEADER_BYTES + (e * COOKED_SKELETON_JOINT_BYTES);
        // The parent is REMAPPED from an input position to an EMISSION index, which is what makes
        // "every non-root parent is strictly less than its own index" true by construction.
        const std::uint32_t parent = parentPos[pos];
        putU32(bytes, o + J_PARENT,
               parent == SKELETON_INVALID_INDEX ? COOKED_SKELETON_INVALID_INDEX : emissionIndex[parent]);
        putU32(bytes, o + J_PALETTE_SLOT, joint.paletteSlot);
        putU32(bytes, o + J_SOURCE_NODE_LOCAL_ID, joint.localId);
        putVec3(bytes, o + J_TRANSLATION, joint.translation);
        putQuat(bytes, o + J_ROTATION, joint.rotation);
        putVec3(bytes, o + J_SCALE, joint.scale);
        const bool hierarchyOnly = joint.paletteSlot == SKELETON_INVALID_INDEX;
        putMat4(bytes, o + J_INVERSE_BIND, hierarchyOnly ? Mat4::identity() : joint.inverseBind);
    }

    SkeletonCookResult result;
    result.status = SkeletonCookStatus::Ok;
    result.bytes = std::move(out);
    return result;
}

}  // namespace engine::assets
