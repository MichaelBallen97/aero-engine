// tests/cooked_skeleton_test.cpp -- task 3.5.1: the .aeroskel container v1. A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 12's own tables -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly ONE field of something already valid. The two frozen
// goldens (SK1/SK2) are the one exception, and they are cook OUTPUT verified field by field against
// those same tables before being frozen.
#include <aero/assets/cooked_skeleton.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies the std::string_view
// operands of the magic and status-label CHECKs below through operator<<(std::ostream&,
// std::string_view), which MS STL defines inline in <string_view> against an INCOMPLETE
// std::basic_ostream -- only <ostream> completes it. libc++ and libstdc++ are self-sufficient, so
// omitting it builds clean on macOS and Linux and fails only on the Windows lane, with errors
// pointing inside the STL headers rather than at the CHECK. Written when the TU was created.
#include <ostream>
#include <vector>

using engine::assets::COOKED_SKELETON_FORMAT_VERSION;
using engine::assets::COOKED_SKELETON_HEADER_BYTES;
using engine::assets::COOKED_SKELETON_INVALID_INDEX;
using engine::assets::COOKED_SKELETON_JOINT_BYTES;
using engine::assets::COOKED_SKELETON_MAGIC;
using engine::assets::CookedSkeletonParseResult;
using engine::assets::CookedSkeletonStatus;
using engine::assets::cookedSkeletonStatusLabel;
using engine::assets::MAX_COOKED_SKELETON_JOINTS;
using engine::assets::MAX_COOKED_SKELETON_PALETTE;
using engine::assets::parseCookedSkeleton;

namespace {

// The two record sizes and the two caps are part of the on-disk format, so they are pinned as BUILD
// failures rather than as a case: a change to any of them cannot be argued with at runtime.
static_assert(COOKED_SKELETON_HEADER_BYTES == 64);
static_assert(COOKED_SKELETON_JOINT_BYTES == 128);
static_assert(COOKED_SKELETON_JOINT_BYTES % 16 == 0, "128-byte records are what make the format padding-free");
static_assert(MAX_COOKED_SKELETON_JOINTS == 1024);
static_assert(MAX_COOKED_SKELETON_PALETTE == 256);
static_assert(COOKED_SKELETON_INVALID_INDEX == 0xFFFFFFFFU);

// This file's OWN copy of docs/09 section 12's offsets, spelled independently of the parser's: a
// transposed offset in cooked_skeleton.cpp must fail here rather than agree with itself.
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

constexpr std::size_t J_PARENT = 0;
constexpr std::size_t J_PALETTE_SLOT = 4;
constexpr std::size_t J_SOURCE_NODE_LOCAL_ID = 8;
constexpr std::size_t J_RESERVED0 = 12;
constexpr std::size_t J_TRANSLATION = 16;
constexpr std::size_t J_ROTATION = 28;
constexpr std::size_t J_SCALE = 44;
constexpr std::size_t J_INVERSE_BIND = 56;
constexpr std::size_t J_RESERVED2 = 120;

void put32(std::vector<std::byte>& b, std::size_t offset, std::uint32_t value) {
    engine::assets::putU32(std::span<std::byte>(b), offset, value);
}
void put64(std::vector<std::byte>& b, std::size_t offset, std::uint64_t value) {
    engine::assets::putU64(std::span<std::byte>(b), offset, value);
}
void putF(std::vector<std::byte>& b, std::size_t offset, float value) {
    engine::assets::putF32(std::span<std::byte>(b), offset, value);
}

// The offset of joint record `i`, spelled once.
[[nodiscard]] constexpr std::size_t rec(std::uint32_t i) {
    return COOKED_SKELETON_HEADER_BYTES + (std::size_t{i} * COOKED_SKELETON_JOINT_BYTES);
}

struct JointSpec {
    std::uint32_t parent = COOKED_SKELETON_INVALID_INDEX;
    std::uint32_t paletteSlot = COOKED_SKELETON_INVALID_INDEX;
    std::uint32_t localId = 0;
};

// A well-formed buffer for `joints`, with per-record TRS and IBM values derived from the record
// index so field crosstalk is visible: translation (i, i + 0.5, -i), a normalized-by-construction
// quaternion (0, 0, 0, 1) rotated only in the w slot's neighbours where the index makes it distinct,
// scale (1 + i, 1, 1), and an IBM that is identity with a translated fourth column (-i, 0, 0).
[[nodiscard]] std::vector<std::byte> build(std::span<const JointSpec> joints, std::uint32_t paletteJointCount,
                                           std::uint64_t guidHi = 0, std::uint64_t guidLo = 0,
                                           std::uint32_t sourceSkinIndex = 0) {
    const auto jointCount = static_cast<std::uint32_t>(joints.size());
    const std::size_t total = COOKED_SKELETON_HEADER_BYTES + (joints.size() * COOKED_SKELETON_JOINT_BYTES);
    std::vector<std::byte> b(total);
    for (std::size_t i = 0; i < COOKED_SKELETON_MAGIC.size(); ++i) {
        b[i] = static_cast<std::byte>(COOKED_SKELETON_MAGIC[i]);
    }
    put32(b, H_FORMAT_VERSION, COOKED_SKELETON_FORMAT_VERSION);
    put32(b, H_COOKER_VERSION, 1);
    put64(b, H_GUID_HI, guidHi);
    put64(b, H_GUID_LO, guidLo);
    put32(b, H_JOINT_COUNT, jointCount);
    put32(b, H_PALETTE_JOINT_COUNT, paletteJointCount);
    put32(b, H_SOURCE_SKIN_INDEX, sourceSkinIndex);
    put64(b, H_TOTAL_BYTES, total);
    for (std::uint32_t i = 0; i < jointCount; ++i) {
        const std::size_t o = rec(i);
        const auto f = static_cast<float>(i);
        put32(b, o + J_PARENT, joints[i].parent);
        put32(b, o + J_PALETTE_SLOT, joints[i].paletteSlot);
        put32(b, o + J_SOURCE_NODE_LOCAL_ID, joints[i].localId);
        putF(b, o + J_TRANSLATION + 0, f);
        putF(b, o + J_TRANSLATION + 4, f + 0.5F);
        putF(b, o + J_TRANSLATION + 8, -f);
        putF(b, o + J_ROTATION + 0, 0.0F);
        putF(b, o + J_ROTATION + 4, 0.0F);
        putF(b, o + J_ROTATION + 8, 0.0F);
        putF(b, o + J_ROTATION + 12, 1.0F);
        putF(b, o + J_SCALE + 0, 1.0F + f);
        putF(b, o + J_SCALE + 4, 1.0F);
        putF(b, o + J_SCALE + 8, 1.0F);
        // Column-major identity with the fourth column translated by (-i, 0, 0).
        putF(b, o + J_INVERSE_BIND + 0, 1.0F);
        putF(b, o + J_INVERSE_BIND + 20, 1.0F);
        putF(b, o + J_INVERSE_BIND + 40, 1.0F);
        putF(b, o + J_INVERSE_BIND + 48, -f);
        putF(b, o + J_INVERSE_BIND + 60, 1.0F);
    }
    return b;
}

// A two-record skeleton: a root palette joint and its child palette joint. The shape every refusal
// case below mutates exactly one field of.
[[nodiscard]] std::vector<std::byte> buildPair() {
    const std::array<JointSpec, 2> joints = {JointSpec{COOKED_SKELETON_INVALID_INDEX, 0, 10}, JointSpec{0, 1, 11}};
    return build(std::span<const JointSpec>(joints), 2);
}

[[nodiscard]] CookedSkeletonParseResult parse(const std::vector<std::byte>& b) {
    return parseCookedSkeleton(std::span<const std::byte>(b));
}

[[nodiscard]] std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

}  // namespace

TEST_CASE("cooked skeleton: buffers shorter than a whole file are refused at every boundary (SK3)") {
    const std::vector<std::byte> whole = buildPair();
    REQUIRE(whole.size() == 320);
    // Below the header: TooSmall, before a single field is interpreted.
    const std::array<std::size_t, 2> tooSmall = {0, 63};
    for (const std::size_t n : tooSmall) {
        const std::vector<std::byte> truncated(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        const CookedSkeletonParseResult r = parse(truncated);
        CHECK((r.status == CookedSkeletonStatus::TooSmall));
    }
    // At and past the header: the header's own totalBytes no longer equals the buffer, which is a
    // SizeMismatch and NOT a TooSmall -- the file says how long it is and the buffer disagrees.
    const std::array<std::size_t, 3> sizeMismatch = {64, 191, 319};
    for (const std::size_t n : sizeMismatch) {
        const std::vector<std::byte> truncated(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        const CookedSkeletonParseResult r = parse(truncated);
        CHECK((r.status == CookedSkeletonStatus::SizeMismatch));
    }
    CHECK((parse(whole).status == CookedSkeletonStatus::Ok));
}

TEST_CASE("cooked skeleton: the magic is compared over all eight bytes (SK4)") {
    CHECK(COOKED_SKELETON_MAGIC == std::string_view{"AEROSKEL"});
    CHECK(COOKED_SKELETON_MAGIC.size() == 8);
    // Byte 7 ALONE is wrong -- the case a seven-byte comparison cannot see.
    std::vector<std::byte> b = buildPair();
    b[7] = static_cast<std::byte>('X');
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadMagic));
    CHECK(!r.message.empty());
    // And every other byte position too, so the loop is proven to cover the whole magic.
    for (std::size_t i = 0; i < 8; ++i) {
        std::vector<std::byte> mutated = buildPair();
        mutated[i] = static_cast<std::byte>(0x00);
        CHECK((parse(mutated).status == CookedSkeletonStatus::BadMagic));
    }
}

TEST_CASE("cooked skeleton: an unknown format version is refused, never read anyway (SK5)") {
    static_assert(COOKED_SKELETON_FORMAT_VERSION == 1);
    std::vector<std::byte> b = buildPair();
    put32(b, H_FORMAT_VERSION, 2);
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::UnsupportedVersion));
    CHECK(r.skeleton.joints.empty());
    put32(b, H_FORMAT_VERSION, 0);
    CHECK((parse(b).status == CookedSkeletonStatus::UnsupportedVersion));
    // cookerVersion is informational and NEVER gates a parse -- the one version field that may move.
    std::vector<std::byte> c = buildPair();
    put32(c, H_COOKER_VERSION, 99);
    const CookedSkeletonParseResult ok = parse(c);
    CHECK((ok.status == CookedSkeletonStatus::Ok));
    CHECK(ok.skeleton.cookerVersion == 99);
}

TEST_CASE("cooked skeleton: every reserved header field is a refusal when non-zero (SK6)") {
    const std::array<std::size_t, 3> reserved = {H_FLAGS, H_RESERVED0, H_RESERVED1};
    CHECK(reserved.size() == 3);  // a LITERAL row count: a deleted row must not shrink the guard
    for (const std::size_t offset : reserved) {
        std::vector<std::byte> b = buildPair();
        put32(b, offset, 1);
        const CookedSkeletonParseResult r = parse(b);
        CHECK((r.status == CookedSkeletonStatus::ReservedNotZero));
        CHECK(!r.message.empty());
    }
}

TEST_CASE("cooked skeleton: every reserved record field is a refusal naming its record (SK7)") {
    struct Arm {
        std::uint32_t record;
        std::size_t offset;
        bool wide;
    };
    const std::array<Arm, 4> arms = {Arm{0, J_RESERVED0, false}, Arm{0, J_RESERVED2, true}, Arm{1, J_RESERVED0, false},
                                     Arm{1, J_RESERVED2, true}};
    CHECK(arms.size() == 4);  // literal row count
    for (const Arm& arm : arms) {
        std::vector<std::byte> b = buildPair();
        if (arm.wide) {
            put64(b, rec(arm.record) + arm.offset, 1);
        } else {
            put32(b, rec(arm.record) + arm.offset, 1);
        }
        const CookedSkeletonParseResult r = parse(b);
        CHECK((r.status == CookedSkeletonStatus::ReservedNotZero));
        CHECK(r.message.find("record " + std::to_string(arm.record)) != std::string::npos);
    }
}

TEST_CASE("cooked skeleton: totalBytes is COMPARED against both the buffer and the arithmetic (SK8)") {
    // Arm 1 -- the stored size disagrees with the buffer's own size.
    std::vector<std::byte> b = buildPair();
    put64(b, H_TOTAL_BYTES, 321);
    CHECK((parse(b).status == CookedSkeletonStatus::SizeMismatch));
    put64(b, H_TOTAL_BYTES, 319);
    CHECK((parse(b).status == CookedSkeletonStatus::SizeMismatch));
    // Arm 2 -- the stored size matches the buffer but NOT 64 + 128 * jointCount. A parser that
    // derived the size from jointCount instead of comparing would accept this file and then read
    // one record where two were written.
    std::vector<std::byte> c = buildPair();
    put32(c, H_JOINT_COUNT, 1);
    put32(c, H_PALETTE_JOINT_COUNT, 1);
    REQUIRE(c.size() == 320);
    const CookedSkeletonParseResult r = parse(c);
    CHECK((r.status == CookedSkeletonStatus::SizeMismatch));
    CHECK(!r.message.empty());
}

TEST_CASE("cooked skeleton: both caps are enforced by the parser (SK9)") {
    std::vector<std::byte> b = buildPair();
    put32(b, H_JOINT_COUNT, MAX_COOKED_SKELETON_JOINTS + 1);
    CHECK((parse(b).status == CookedSkeletonStatus::CapExceeded));
    // The palette cap needs paletteJointCount <= jointCount to reach it at all, so the joint count
    // is raised with it -- the cap is proven, not the count comparison.
    std::vector<std::byte> c = buildPair();
    put32(c, H_JOINT_COUNT, 300);
    put32(c, H_PALETTE_JOINT_COUNT, MAX_COOKED_SKELETON_PALETTE + 1);
    CHECK((parse(c).status == CookedSkeletonStatus::CapExceeded));
    // Exactly at each cap is NOT a CapExceeded -- it fails later, on the size arithmetic.
    std::vector<std::byte> d = buildPair();
    put32(d, H_JOINT_COUNT, MAX_COOKED_SKELETON_JOINTS);
    CHECK((parse(d).status == CookedSkeletonStatus::SizeMismatch));
}

TEST_CASE("cooked skeleton: a zero joint count is refused -- a .aeroskel is never empty (SK10)") {
    std::vector<std::byte> b = buildPair();
    put32(b, H_JOINT_COUNT, 0);
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(!r.message.empty());
    // The header-only buffer, which is the smallest thing that is not TooSmall, is refused for the
    // same reason: its counts are zero.
    std::vector<std::byte> headerOnly(COOKED_SKELETON_HEADER_BYTES);
    for (std::size_t i = 0; i < COOKED_SKELETON_MAGIC.size(); ++i) {
        headerOnly[i] = static_cast<std::byte>(COOKED_SKELETON_MAGIC[i]);
    }
    put32(headerOnly, H_FORMAT_VERSION, 1);
    put64(headerOnly, H_TOTAL_BYTES, COOKED_SKELETON_HEADER_BYTES);
    CHECK((parse(headerOnly).status == CookedSkeletonStatus::BadHierarchy));
}

TEST_CASE("cooked skeleton: a zero palette joint count is refused (SK11)") {
    std::vector<std::byte> b = buildPair();
    put32(b, H_PALETTE_JOINT_COUNT, 0);
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(!r.message.empty());
}

TEST_CASE("cooked skeleton: more palette slots than joint records is refused (SK12)") {
    std::vector<std::byte> b = buildPair();
    put32(b, H_PALETTE_JOINT_COUNT, 3);
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(!r.message.empty());
}

TEST_CASE("cooked skeleton: a parent at or after its own record is refused at every boundary (SK13)") {
    // THE ordering invariant. Three forward references, each the mutation of one u32.
    struct Arm {
        std::uint32_t record;
        std::uint32_t parent;
    };
    const std::array<Arm, 3> arms = {Arm{0, 0}, Arm{1, 1}, Arm{0, 1}};
    CHECK(arms.size() == 3);  // literal row count
    for (const Arm& arm : arms) {
        std::vector<std::byte> b = buildPair();
        put32(b, rec(arm.record) + J_PARENT, arm.parent);
        const CookedSkeletonParseResult r = parse(b);
        CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
        CHECK(r.message.find("record " + std::to_string(arm.record)) != std::string::npos);
    }
    // The legal shapes stay legal: a root marker anywhere, and a parent strictly before.
    std::vector<std::byte> ok = buildPair();
    put32(ok, rec(1) + J_PARENT, COOKED_SKELETON_INVALID_INDEX);
    CHECK((parse(ok).status == CookedSkeletonStatus::Ok));
}

TEST_CASE("cooked skeleton: a palette slot at or past the count is refused (SK14)") {
    std::vector<std::byte> b = buildPair();
    put32(b, rec(1) + J_PALETTE_SLOT, 2);  // paletteJointCount is 2, so 2 is one past the end
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(r.message.find("record 1") != std::string::npos);
    std::vector<std::byte> c = buildPair();
    put32(c, rec(1) + J_PALETTE_SLOT, 4000);
    CHECK((parse(c).status == CookedSkeletonStatus::BadHierarchy));
}

TEST_CASE("cooked skeleton: a palette slot claimed twice is refused (SK15)") {
    std::vector<std::byte> b = buildPair();
    put32(b, rec(1) + J_PALETTE_SLOT, 0);  // record 0 already claims slot 0
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(!r.message.empty());
}

TEST_CASE("cooked skeleton: a palette slot claimed by nobody is refused (SK16)") {
    // Record 1 becomes hierarchy-only while the header still declares two slots, so slot 1 is a
    // hole. The bijection is only proven when BOTH halves hold, and this is the half a duplicate
    // check alone cannot see.
    std::vector<std::byte> b = buildPair();
    put32(b, rec(1) + J_PALETTE_SLOT, COOKED_SKELETON_INVALID_INDEX);
    const CookedSkeletonParseResult r = parse(b);
    CHECK((r.status == CookedSkeletonStatus::BadHierarchy));
    CHECK(!r.message.empty());
    // With the count lowered to match, the same records are a legal one-slot palette.
    put32(b, H_PALETTE_JOINT_COUNT, 1);
    CHECK((parse(b).status == CookedSkeletonStatus::Ok));
}

TEST_CASE("cooked skeleton: TRS and IBM cells are read back bit for bit (SK17)") {
    // Bit patterns, not values: negative zero and a signalling-NaN pattern are what a "helpful"
    // normalization would destroy, and neither compares equal under ==.
    constexpr std::uint32_t NEG_ZERO = 0x80000000U;
    constexpr std::uint32_t SIGNALLING = 0x7FA00000U;
    constexpr std::uint32_t ODD_VALUE = 0x3E4CCCCDU;  // 0.2f
    std::vector<std::byte> b = buildPair();
    put32(b, rec(0) + J_TRANSLATION + 0, NEG_ZERO);
    put32(b, rec(0) + J_ROTATION + 4, SIGNALLING);
    put32(b, rec(0) + J_SCALE + 8, ODD_VALUE);
    put32(b, rec(1) + J_INVERSE_BIND + 24, SIGNALLING);  // columns[1].z
    put32(b, rec(1) + J_INVERSE_BIND + 52, NEG_ZERO);    // columns[3].y
    const CookedSkeletonParseResult r = parse(b);
    REQUIRE((r.status == CookedSkeletonStatus::Ok));
    REQUIRE(r.skeleton.joints.size() == 2);
    CHECK(bits(r.skeleton.joints[0].translation.x) == NEG_ZERO);
    CHECK(bits(r.skeleton.joints[0].rotation.y) == SIGNALLING);
    CHECK(bits(r.skeleton.joints[0].scale.z) == ODD_VALUE);
    CHECK(bits(r.skeleton.joints[1].inverseBind.columns[1].z) == SIGNALLING);
    CHECK(bits(r.skeleton.joints[1].inverseBind.columns[3].y) == NEG_ZERO);
    // And the untouched cells still carry the builder's own values, so the mutations above did not
    // simply overwrite everything.
    CHECK(r.skeleton.joints[1].translation.x == doctest::Approx(1.0F));
    CHECK(r.skeleton.joints[1].inverseBind.columns[3].x == doctest::Approx(-1.0F));
}

TEST_CASE("cooked skeleton: cookedSkeletonStatusLabel is total, non-empty and distinct (SK18)") {
    const std::array<CookedSkeletonStatus, 8> all = {CookedSkeletonStatus::Ok,
                                                     CookedSkeletonStatus::TooSmall,
                                                     CookedSkeletonStatus::BadMagic,
                                                     CookedSkeletonStatus::UnsupportedVersion,
                                                     CookedSkeletonStatus::ReservedNotZero,
                                                     CookedSkeletonStatus::SizeMismatch,
                                                     CookedSkeletonStatus::CapExceeded,
                                                     CookedSkeletonStatus::BadHierarchy};
    CHECK(all.size() == 8);  // literal row count -- an added enumerator must be added here too
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string_view label = cookedSkeletonStatusLabel(all[i]);
        CHECK(!label.empty());
        CHECK(label != std::string_view{"Unknown"});
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            CHECK(label != cookedSkeletonStatusLabel(all[j]));
        }
    }
}

TEST_CASE("cooked skeleton: the message is empty IFF the parse succeeded (SK19)") {
    const CookedSkeletonParseResult ok = parse(buildPair());
    REQUIRE((ok.status == CookedSkeletonStatus::Ok));
    CHECK(ok.message.empty());

    struct Arm {
        std::size_t offset;
        std::uint32_t value;
    };
    const std::array<Arm, 5> arms = {Arm{H_FORMAT_VERSION, 7}, Arm{H_FLAGS, 1}, Arm{H_JOINT_COUNT, 0},
                                     Arm{H_PALETTE_JOINT_COUNT, 9}, Arm{H_JOINT_COUNT, 5000}};
    CHECK(arms.size() == 5);  // literal row count
    for (const Arm& arm : arms) {
        std::vector<std::byte> b = buildPair();
        put32(b, arm.offset, arm.value);
        const CookedSkeletonParseResult r = parse(b);
        REQUIRE((r.status != CookedSkeletonStatus::Ok));
        CHECK(!r.message.empty());
    }
    // Record-scoped refusals name the record index, which is what makes a message actionable on a
    // 1024-record file.
    std::vector<std::byte> forward = buildPair();
    put32(forward, rec(1) + J_PARENT, 1);
    CHECK(parse(forward).message.find("record 1") != std::string::npos);
}

TEST_CASE("cooked skeleton: the parse result is FULLY OWNED -- no retained span (SK20)") {
    // The source buffer is scoped and destroyed before a single field is read. Under ASan a
    // retained span would be a use-after-free here rather than a wrong value, which is exactly the
    // assertion: this container, unlike .aeromesh, copies everything.
    CookedSkeletonParseResult r;
    {
        const std::vector<std::byte> b = buildPair();
        r = parse(b);
        REQUIRE((r.status == CookedSkeletonStatus::Ok));
    }
    REQUIRE(r.skeleton.joints.size() == 2);
    CHECK(r.skeleton.formatVersion == 1);
    CHECK(r.skeleton.cookerVersion == 1);
    CHECK(r.skeleton.paletteJointCount == 2);
    CHECK(r.skeleton.joints[0].parent == COOKED_SKELETON_INVALID_INDEX);
    CHECK(r.skeleton.joints[0].paletteSlot == 0);
    CHECK(r.skeleton.joints[0].sourceNodeLocalId == 10);
    CHECK(r.skeleton.joints[1].parent == 0);
    CHECK(r.skeleton.joints[1].paletteSlot == 1);
    CHECK(r.skeleton.joints[1].sourceNodeLocalId == 11);
    CHECK(r.skeleton.joints[1].translation.y == doctest::Approx(1.5F));
    CHECK(r.skeleton.joints[1].scale.x == doctest::Approx(2.0F));
    CHECK(r.skeleton.joints[1].rotation.w == doctest::Approx(1.0F));
    CHECK(r.skeleton.joints[1].inverseBind.columns[3].x == doctest::Approx(-1.0F));
}
