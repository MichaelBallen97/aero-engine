// Aero Engine — the cooked skeleton container v1: the label and the hostile-input parser (task
// 3.5.1). See cooked_skeleton.hpp for the contract and docs/09-file-formats.md section 12 for the
// normative format. NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Reserves nothing before the two
// header counts have been validated against their frozen caps.
#include <aero/assets/cooked_skeleton.hpp>
#include <aero/core/profiler.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE. docs/09 section 12.2 is the normative table; these mirror
// it and nothing else in this TU spells a header offset as a literal.
constexpr std::size_t H_MAGIC = 0;
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_FLAGS = 32;
constexpr std::size_t H_JOINT_COUNT = 36;
constexpr std::size_t H_PALETTE_JOINT_COUNT = 40;
constexpr std::size_t H_SOURCE_SKIN_INDEX = 44;
constexpr std::size_t H_TOTAL_BYTES = 48;
constexpr std::size_t H_RESERVED0 = 56;
constexpr std::size_t H_RESERVED1 = 60;
static_assert(H_RESERVED1 + 4 == COOKED_SKELETON_HEADER_BYTES);

// Joint-record field offsets (docs/09 section 12.3).
constexpr std::size_t J_PARENT = 0;
constexpr std::size_t J_PALETTE_SLOT = 4;
constexpr std::size_t J_SOURCE_NODE_LOCAL_ID = 8;
constexpr std::size_t J_RESERVED0 = 12;
constexpr std::size_t J_TRANSLATION = 16;
constexpr std::size_t J_ROTATION = 28;
constexpr std::size_t J_SCALE = 44;
constexpr std::size_t J_INVERSE_BIND = 56;
constexpr std::size_t J_RESERVED2 = 120;
static_assert(J_RESERVED2 + 8 == COOKED_SKELETON_JOINT_BYTES);

[[nodiscard]] CookedSkeletonParseResult refuse(CookedSkeletonStatus status, std::string message) {
    CookedSkeletonParseResult out;
    out.status = status;
    out.message = std::move(message);
    return out;
}

[[nodiscard]] Vec3 readVec3(std::span<const std::byte> b, std::size_t offset) {
    return Vec3{getF32(b, offset), getF32(b, offset + 4), getF32(b, offset + 8)};
}

// x, y, z, w -- glTF's accessor order, which is also engine::Quat's member order.
[[nodiscard]] Quat readQuat(std::span<const std::byte> b, std::size_t offset) {
    return Quat{getF32(b, offset), getF32(b, offset + 4), getF32(b, offset + 8), getF32(b, offset + 12)};
}

// Column-major, cell for cell: columns[c] occupies the c-th group of four floats, exactly as the
// cook wrote them. No transpose anywhere -- a matrix that travels bit for bit must also travel cell
// for cell.
[[nodiscard]] Mat4 readMat4(std::span<const std::byte> b, std::size_t offset) {
    Mat4 m;
    for (std::size_t c = 0; c < 4; ++c) {
        const std::size_t o = offset + (c * 16);
        m.columns[c] = Vec4{getF32(b, o), getF32(b, o + 4), getF32(b, o + 8), getF32(b, o + 12)};
    }
    return m;
}

}  // namespace

std::string_view cookedSkeletonStatusLabel(CookedSkeletonStatus status) noexcept {
    switch (status) {
        case CookedSkeletonStatus::Ok:
            return "Ok";
        case CookedSkeletonStatus::TooSmall:
            return "Too small";
        case CookedSkeletonStatus::BadMagic:
            return "Bad magic";
        case CookedSkeletonStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedSkeletonStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedSkeletonStatus::SizeMismatch:
            return "Size mismatch";
        case CookedSkeletonStatus::CapExceeded:
            return "Cap exceeded";
        case CookedSkeletonStatus::BadHierarchy:
            return "Bad hierarchy";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

CookedSkeletonParseResult parseCookedSkeleton(std::span<const std::byte> bytes) {
    AERO_PROFILE_ZONE_NAMED("assets::parseCookedSkeleton");

    // 1. shorter than the header.
    if (bytes.size() < COOKED_SKELETON_HEADER_BYTES) {
        return refuse(CookedSkeletonStatus::TooSmall,
                      std::format("the buffer is {} bytes, shorter than the {}-byte header", bytes.size(),
                                  COOKED_SKELETON_HEADER_BYTES));
    }

    // 2. magic. Compared BYTE BY BYTE over all eight bytes -- never a memcmp of a reinterpret_cast'd
    //    pointer, and never a prefix: the last byte is as load-bearing as the first.
    for (std::size_t i = 0; i < COOKED_SKELETON_MAGIC.size(); ++i) {
        if (bytes[H_MAGIC + i] != static_cast<std::byte>(COOKED_SKELETON_MAGIC[i])) {
            return refuse(CookedSkeletonStatus::BadMagic, "the buffer does not begin with AEROSKEL");
        }
    }

    // 3. version.
    const std::uint32_t formatVersion = getU32(bytes, H_FORMAT_VERSION);
    if (formatVersion != COOKED_SKELETON_FORMAT_VERSION) {
        return refuse(CookedSkeletonStatus::UnsupportedVersion,
                      std::format("cooked skeleton format version {} (this build reads version {})", formatVersion,
                                  COOKED_SKELETON_FORMAT_VERSION));
    }

    // 4. the header's reserved space. A REFUSAL, deliberately: occupying one of these is a
    //    formatVersion bump, so a non-zero value here is a file this build cannot claim to read.
    if (getU32(bytes, H_FLAGS) != 0) {
        return refuse(CookedSkeletonStatus::ReservedNotZero, "the header's reserved flags field is not zero");
    }
    if (getU32(bytes, H_RESERVED0) != 0 || getU32(bytes, H_RESERVED1) != 0) {
        return refuse(CookedSkeletonStatus::ReservedNotZero, "a trailing reserved header field is not zero");
    }

    // 5. the two counts against each other. Cheap, cap-independent, and it is what makes "a
    //    .aeroskel is never empty" a parse requirement rather than a comment.
    const std::uint32_t jointCount = getU32(bytes, H_JOINT_COUNT);
    const std::uint32_t paletteJointCount = getU32(bytes, H_PALETTE_JOINT_COUNT);
    if (jointCount == 0) {
        return refuse(CookedSkeletonStatus::BadHierarchy, "the header declares zero joints");
    }
    if (paletteJointCount == 0) {
        return refuse(CookedSkeletonStatus::BadHierarchy, "the header declares zero palette joints");
    }
    if (paletteJointCount > jointCount) {
        return refuse(
            CookedSkeletonStatus::BadHierarchy,
            std::format("the header declares {} palette joints over {} joint records", paletteJointCount, jointCount));
    }

    // 6. the caps. NOTHING IS RESERVED UNTIL THIS BLOCK HAS PASSED -- that is why it is here.
    if (jointCount > MAX_COOKED_SKELETON_JOINTS) {
        return refuse(
            CookedSkeletonStatus::CapExceeded,
            std::format("the header declares {} joints, over the cap of {}", jointCount, MAX_COOKED_SKELETON_JOINTS));
    }
    if (paletteJointCount > MAX_COOKED_SKELETON_PALETTE) {
        return refuse(CookedSkeletonStatus::CapExceeded,
                      std::format("the header declares {} palette joints, over the cap of {}", paletteJointCount,
                                  MAX_COOKED_SKELETON_PALETTE));
    }

    // 7. totalBytes. The parser COMPARES the stored value against both the buffer's own size and the
    //    format's arithmetic; it never DERIVES the size from jointCount and carries on, which would
    //    make the stored field decorative.
    const std::uint64_t totalBytes = getU64(bytes, H_TOTAL_BYTES);
    if (totalBytes != bytes.size()) {
        return refuse(
            CookedSkeletonStatus::SizeMismatch,
            std::format("the header declares {} total bytes but the buffer holds {}", totalBytes, bytes.size()));
    }
    const std::uint64_t expectedBytes =
        COOKED_SKELETON_HEADER_BYTES + (std::uint64_t{jointCount} * COOKED_SKELETON_JOINT_BYTES);
    if (totalBytes != expectedBytes) {
        return refuse(CookedSkeletonStatus::SizeMismatch,
                      std::format("the header declares {} total bytes but {} joint records need {}", totalBytes,
                                  jointCount, expectedBytes));
    }

    CookedSkeleton skeleton;  // ONLY NOW is anything reserved.
    skeleton.joints.reserve(jointCount);

    // 8. the records, plus the slot bijection. `seen` is a linear vector over paletteJointCount --
    //    no hash container anywhere in this subsystem, and none is needed: the slot space is dense
    //    by definition.
    std::vector<bool> seen(paletteJointCount, false);
    for (std::uint32_t i = 0; i < jointCount; ++i) {
        const std::size_t o = COOKED_SKELETON_HEADER_BYTES + (std::size_t{i} * COOKED_SKELETON_JOINT_BYTES);
        if (getU32(bytes, o + J_RESERVED0) != 0 || getU64(bytes, o + J_RESERVED2) != 0) {
            return refuse(CookedSkeletonStatus::ReservedNotZero,
                          std::format("joint record {} has a non-zero reserved field", i));
        }
        CookedSkeletonJoint joint;
        joint.parent = getU32(bytes, o + J_PARENT);
        joint.paletteSlot = getU32(bytes, o + J_PALETTE_SLOT);
        joint.sourceNodeLocalId = getU32(bytes, o + J_SOURCE_NODE_LOCAL_ID);

        // THE ORDERING INVARIANT, in one line and with no graph work at all: parents-before-children
        // is a byte-layout property here, so a cycle is unrepresentable rather than detected.
        if (joint.parent != COOKED_SKELETON_INVALID_INDEX && joint.parent >= i) {
            return refuse(
                CookedSkeletonStatus::BadHierarchy,
                std::format("joint record {} names parent {}, which is not strictly before it", i, joint.parent));
        }
        if (joint.paletteSlot != COOKED_SKELETON_INVALID_INDEX) {
            if (joint.paletteSlot >= paletteJointCount) {
                return refuse(CookedSkeletonStatus::BadHierarchy,
                              std::format("joint record {} names palette slot {} of {}", i, joint.paletteSlot,
                                          paletteJointCount));
            }
            if (seen[joint.paletteSlot]) {
                return refuse(
                    CookedSkeletonStatus::BadHierarchy,
                    std::format("joint record {} claims palette slot {}, already claimed", i, joint.paletteSlot));
            }
            seen[joint.paletteSlot] = true;
        }

        joint.translation = readVec3(bytes, o + J_TRANSLATION);
        joint.rotation = readQuat(bytes, o + J_ROTATION);
        joint.scale = readVec3(bytes, o + J_SCALE);
        joint.inverseBind = readMat4(bytes, o + J_INVERSE_BIND);
        skeleton.joints.push_back(joint);
    }

    // 9. every slot claimed. The bijection is only proven once BOTH halves hold: no slot twice
    //    (above) and no slot missing (here). A palette with a hole binds vertices to an identity
    //    matrix nobody wrote.
    for (std::uint32_t slot = 0; slot < paletteJointCount; ++slot) {
        if (!seen[slot]) {
            return refuse(CookedSkeletonStatus::BadHierarchy,
                          std::format("palette slot {} of {} is claimed by no joint record", slot, paletteJointCount));
        }
    }

    // 10. done.
    skeleton.formatVersion = formatVersion;
    skeleton.cookerVersion = getU32(bytes, H_COOKER_VERSION);
    skeleton.sourceGuid = Guid{getU64(bytes, H_GUID_HI), getU64(bytes, H_GUID_LO)};
    skeleton.sourceSkinIndex = getU32(bytes, H_SOURCE_SKIN_INDEX);
    skeleton.paletteJointCount = paletteJointCount;
    return CookedSkeletonParseResult{CookedSkeletonStatus::Ok, std::string{}, std::move(skeleton)};
}

}  // namespace engine::assets
