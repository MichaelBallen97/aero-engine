// tests/skeleton_cook_test.cpp -- task 3.5.1: cookSkeleton, the resolved-joints-to-container
// transform. A TU of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every case drives the PUBLIC cookSkeleton() and reads its
// output back through the PUBLIC parseCookedSkeleton(), so nothing here depends on an internal of
// either. The two golden INPUTS live here (minimalJoints/closureJoints); their frozen BYTES arrive
// with the golden header in the next commit, which is what turns this file's round trips into byte
// equalities.
#include <aero/assets/cooked_skeleton.hpp>
#include <aero/assets/skeleton_cook.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies std::string_view operands
// through operator<<(std::ostream&, std::string_view), which MS STL defines inline in <string_view>
// against an INCOMPLETE std::basic_ostream. libc++ and libstdc++ are self-sufficient, so omitting it
// builds clean on macOS and Linux and fails only on the Windows lane. Written when the TU was
// created, not after a lane said so.
#include <ostream>
#include <vector>

using engine::Guid;
using engine::Mat4;
using engine::Quat;
using engine::Vec3;
using engine::Vec4;
using engine::assets::COOKED_SKELETON_HEADER_BYTES;
using engine::assets::COOKED_SKELETON_INVALID_INDEX;
using engine::assets::COOKED_SKELETON_JOINT_BYTES;
using engine::assets::CookedSkeletonParseResult;
using engine::assets::CookedSkeletonStatus;
using engine::assets::cookSkeleton;
using engine::assets::MAX_COOKED_SKELETON_JOINTS;
using engine::assets::MAX_COOKED_SKELETON_PALETTE;
using engine::assets::parseCookedSkeleton;
using engine::assets::SKELETON_INVALID_INDEX;
using engine::assets::SkeletonCookInput;
using engine::assets::SkeletonCookJoint;
using engine::assets::SkeletonCookResult;
using engine::assets::SkeletonCookStatus;

namespace {

// A Mat4 that is identity except for its fourth column -- the shape every inverse bind matrix in
// these cases has, so a golden's bytes stay hand-checkable.
[[nodiscard]] Mat4 translationMatrix(float x, float y, float z) {
    Mat4 m = Mat4::identity();
    m.columns[3] = Vec4{x, y, z, 1.0F};
    return m;
}

// ---- the two golden INPUTS ---------------------------------------------------------------------
// MINIMAL: a root palette joint and its child palette joint, nil GUID, a distinct non-identity TRS
// on each and exactly one non-identity inverse bind matrix, so every field's presence is visible in
// the frozen bytes. Every float is exactly representable in binary32.
[[nodiscard]] std::array<SkeletonCookJoint, 2> minimalJoints() {
    SkeletonCookJoint root;
    root.localId = 3;
    root.parentLocalId = SKELETON_INVALID_INDEX;
    root.paletteSlot = 0;
    root.translation = Vec3{1.0F, 2.0F, 3.0F};
    root.rotation = Quat::identity();
    root.scale = Vec3::one();
    root.inverseBind = Mat4::identity();

    SkeletonCookJoint child;
    child.localId = 7;
    child.parentLocalId = 3;
    child.paletteSlot = 1;
    child.translation = Vec3{0.0F, 0.5F, 0.0F};
    child.rotation = Quat{0.0F, 0.0F, 1.0F, 0.0F};  // 180 degrees about Z, exactly representable
    child.scale = Vec3{2.0F, 2.0F, 2.0F};
    child.inverseBind = Mat4{std::array<Vec4, 4>{Vec4{0.5F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, 0.5F, 0.0F, 0.0F},
                                                 Vec4{0.0F, 0.0F, 0.5F, 0.0F}, Vec4{-0.5F, -1.25F, 0.0F, 1.0F}}};
    return {root, child};
}

// CLOSURE: a hierarchy-only ancestor over two SIBLING palette joints whose slots are swapped
// relative to the emitted record order (the smaller localId carries slot 1), a real GUID, and an
// ancestor whose input IBM is deliberately non-identity so the cook's identity override is visible.
[[nodiscard]] std::array<SkeletonCookJoint, 3> closureJoints() {
    SkeletonCookJoint root;
    root.localId = 10;
    root.parentLocalId = SKELETON_INVALID_INDEX;
    root.paletteSlot = SKELETON_INVALID_INDEX;
    root.translation = Vec3{0.0F, 1.0F, 0.0F};
    root.inverseBind = translationMatrix(9.0F, 9.0F, 9.0F);  // must NOT reach the file

    SkeletonCookJoint left;
    left.localId = 20;
    left.parentLocalId = 10;
    left.paletteSlot = 1;
    left.translation = Vec3{1.0F, 0.0F, 0.0F};
    left.inverseBind = translationMatrix(-1.0F, -1.0F, 0.0F);

    SkeletonCookJoint right;
    right.localId = 30;
    right.parentLocalId = 10;
    right.paletteSlot = 0;
    right.translation = Vec3{-1.0F, 0.0F, 0.0F};
    right.inverseBind = translationMatrix(1.0F, -1.0F, 0.0F);
    return {root, left, right};
}

[[nodiscard]] SkeletonCookResult cook(std::span<const SkeletonCookJoint> joints, Guid guid = Guid{},
                                      std::uint32_t skinIndex = 0) {
    SkeletonCookInput input;
    input.sourceGuid = guid;
    input.sourceSkinIndex = skinIndex;
    input.joints = joints;
    return cookSkeleton(input);
}

[[nodiscard]] Guid closureGuid() { return Guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}; }

[[nodiscard]] std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// splitmix64, the guid.cpp shape: a fixed-seed, allocation-free, platform-identical stream, so the
// shuffles below are the SAME shuffles on every lane and in every run.
class Splitmix {
public:
    explicit constexpr Splitmix(std::uint64_t seed) noexcept : state(seed) {}
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

private:
    std::uint64_t state;
};

// A chain of `n` joints, root first, localIds spaced so they are neither positions nor contiguous.
[[nodiscard]] std::vector<SkeletonCookJoint> chain(std::uint32_t n) {
    std::vector<SkeletonCookJoint> joints;
    joints.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        SkeletonCookJoint j;
        j.localId = 100 + (i * 7);
        j.parentLocalId = i == 0 ? SKELETON_INVALID_INDEX : 100 + ((i - 1) * 7);
        j.paletteSlot = i;
        j.translation = Vec3{static_cast<float>(i), 0.0F, 0.0F};
        joints.push_back(j);
    }
    return joints;
}

}  // namespace

TEST_CASE("skeleton cook: any permutation of the same joints cooks identical bytes (KC3)") {
    const std::vector<SkeletonCookJoint> base = chain(6);
    const SkeletonCookResult reference = cook(base, closureGuid());
    REQUIRE((reference.status == SkeletonCookStatus::Ok));
    REQUIRE(reference.bytes.size() == COOKED_SKELETON_HEADER_BYTES + (6 * COOKED_SKELETON_JOINT_BYTES));

    // Eight deterministic permutations: the identity, the reverse, two rotations, and four
    // splitmix-seeded Fisher-Yates shuffles. The cook's output is a function of the RIG, never of
    // the caller's vector order.
    constexpr int PERMUTATIONS = 8;
    for (int p = 0; p < PERMUTATIONS; ++p) {
        std::vector<SkeletonCookJoint> shuffled = base;
        if (p == 1) {
            std::reverse(shuffled.begin(), shuffled.end());
        } else if (p == 2 || p == 3) {
            std::rotate(shuffled.begin(), shuffled.begin() + p, shuffled.end());
        } else if (p >= 4) {
            Splitmix rng(static_cast<std::uint64_t>(p) * 7919ULL);
            for (std::size_t i = shuffled.size(); i > 1; --i) {
                const auto j = static_cast<std::size_t>(rng.next() % i);
                std::swap(shuffled[i - 1], shuffled[j]);
            }
        }
        const SkeletonCookResult r = cook(shuffled, closureGuid());
        REQUIRE((r.status == SkeletonCookStatus::Ok));
        CHECK(r.bytes == reference.bytes);
    }
}

TEST_CASE("skeleton cook: an empty joint list is refused whole (KC4)") {
    const SkeletonCookResult r = cook(std::span<const SkeletonCookJoint>{});
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(!r.message.empty());
    CHECK(r.bytes.empty());
}

TEST_CASE("skeleton cook: a duplicate localId is refused, and the message names it (KC5)") {
    std::vector<SkeletonCookJoint> joints = chain(3);
    joints[2].localId = joints[0].localId;  // 100 appears twice
    joints[2].parentLocalId = SKELETON_INVALID_INDEX;
    const SkeletonCookResult r = cook(joints);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message.find("100") != std::string::npos);
}

TEST_CASE("skeleton cook: an unresolvable parent is refused, never promoted to a root (KC6)") {
    std::vector<SkeletonCookJoint> joints = chain(3);
    joints[2].parentLocalId = 999;  // no such localId in the input
    const SkeletonCookResult r = cook(joints);
    REQUIRE((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message.find("999") != std::string::npos);
    // The seed this case exists for: a cook that quietly rooted the orphan would return Ok with
    // three records, which is a plausible rig bound to the wrong bones.
    CHECK(r.message.find("not in the input") != std::string::npos);
}

TEST_CASE("skeleton cook: a parent cycle is refused by Kahn exhaustion, self-cycle included (KC7)") {
    // A two-joint cycle. Neither is ever ready, so the first selection step finds nothing.
    std::vector<SkeletonCookJoint> pair = chain(2);
    pair[0].parentLocalId = pair[1].localId;
    const SkeletonCookResult twoCycle = cook(pair);
    CHECK((twoCycle.status == SkeletonCookStatus::Invalid));
    CHECK(twoCycle.bytes.empty());
    CHECK(twoCycle.message.find("cycle") != std::string::npos);

    // A self-cycle: a joint that is its own parent. Same detector, no special case.
    std::vector<SkeletonCookJoint> self = chain(2);
    self[1].parentLocalId = self[1].localId;
    const SkeletonCookResult selfCycle = cook(self);
    CHECK((selfCycle.status == SkeletonCookStatus::Invalid));
    CHECK(selfCycle.message.find("cycle") != std::string::npos);

    // A cycle BELOW a legal root still refuses: the roots emit, then nothing is ready.
    std::vector<SkeletonCookJoint> mixed = chain(4);
    mixed[2].parentLocalId = mixed[3].localId;
    const SkeletonCookResult below = cook(mixed);
    CHECK((below.status == SkeletonCookStatus::Invalid));
    CHECK(below.message.find("cycle") != std::string::npos);
}

TEST_CASE("skeleton cook: a palette slot claimed twice is refused (KC8)") {
    std::vector<SkeletonCookJoint> joints = chain(3);
    joints[2].paletteSlot = joints[1].paletteSlot;  // slot 1 twice, slot 2 unused
    const SkeletonCookResult r = cook(joints);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(!r.message.empty());
}

TEST_CASE("skeleton cook: a palette slot at or past the derived count is refused (KC9)") {
    std::vector<SkeletonCookJoint> joints = chain(3);
    joints[2].paletteSlot = 3;  // three slotted joints, so the range is [0, 3)
    const SkeletonCookResult r = cook(joints);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message.find("palette slot 3") != std::string::npos);
}

TEST_CASE("skeleton cook: a hole in the slot range is refused (KC10)") {
    // Slots {0, 1, 3} over three slotted joints: the count is 3, so 3 is out of range and slot 2 is
    // a hole. The two facts are the same fact -- the count is DERIVED by counting, so a hole always
    // shows up as an out-of-range slot, which is why the cook needs no separate unclaimed-slot arm.
    std::vector<SkeletonCookJoint> joints = chain(4);
    joints[2].paletteSlot = SKELETON_INVALID_INDEX;  // three slotted joints remain: 0, 1 and 3
    const SkeletonCookResult r = cook(joints);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    // And with the surviving slots made contiguous, the same rig cooks: the hole was the defect.
    joints[3].paletteSlot = 2;
    const SkeletonCookResult ok = cook(joints);
    CHECK((ok.status == SkeletonCookStatus::Ok));
}

TEST_CASE("skeleton cook: the joint cap is enforced by the writer (KC11)") {
    const std::vector<SkeletonCookJoint> atCap = chain(MAX_COOKED_SKELETON_JOINTS);
    // At the cap the palette cap bites first (1024 slotted joints), so the joint cap is proven with
    // a rig whose palette stays legal: only the first 256 joints carry a slot.
    std::vector<SkeletonCookJoint> overCap = chain(MAX_COOKED_SKELETON_JOINTS + 1);
    for (std::size_t i = MAX_COOKED_SKELETON_PALETTE; i < overCap.size(); ++i) {
        overCap[i].paletteSlot = SKELETON_INVALID_INDEX;
    }
    const SkeletonCookResult r = cook(overCap);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message.find(std::to_string(MAX_COOKED_SKELETON_JOINTS + 1)) != std::string::npos);

    // Exactly at the cap, with a legal palette, cooks -- and parses.
    std::vector<SkeletonCookJoint> legal = atCap;
    for (std::size_t i = MAX_COOKED_SKELETON_PALETTE; i < legal.size(); ++i) {
        legal[i].paletteSlot = SKELETON_INVALID_INDEX;
    }
    const SkeletonCookResult ok = cook(legal);
    REQUIRE((ok.status == SkeletonCookStatus::Ok));
    CHECK((parseCookedSkeleton(std::span<const std::byte>(ok.bytes)).status == CookedSkeletonStatus::Ok));
}

TEST_CASE("skeleton cook: the palette cap is enforced by the writer (KC12)") {
    const std::vector<SkeletonCookJoint> overCap = chain(MAX_COOKED_SKELETON_PALETTE + 1);
    const SkeletonCookResult r = cook(overCap);
    CHECK((r.status == SkeletonCookStatus::Invalid));
    CHECK(r.bytes.empty());
    CHECK(r.message.find(std::to_string(MAX_COOKED_SKELETON_PALETTE + 1)) != std::string::npos);

    const std::vector<SkeletonCookJoint> atCap = chain(MAX_COOKED_SKELETON_PALETTE);
    const SkeletonCookResult ok = cook(atCap);
    REQUIRE((ok.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(ok.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    CHECK(parsed.skeleton.paletteJointCount == MAX_COOKED_SKELETON_PALETTE);
}

TEST_CASE("skeleton cook: ties break by ascending localId, not by input order (KC13)") {
    // Two roots, two interleaved sibling chains. The localIds are chosen so that the ready set is
    // never in input order: input order is [50, 10, 60, 20], and the emitted order must be the
    // ascending-localId topological one.
    //
    //   10 (root) -> 20        50 (root) -> 60
    std::array<SkeletonCookJoint, 4> joints{};
    joints[0].localId = 50;
    joints[0].parentLocalId = SKELETON_INVALID_INDEX;
    joints[0].paletteSlot = 0;
    joints[1].localId = 10;
    joints[1].parentLocalId = SKELETON_INVALID_INDEX;
    joints[1].paletteSlot = 1;
    joints[2].localId = 60;
    joints[2].parentLocalId = 50;
    joints[2].paletteSlot = 2;
    joints[3].localId = 20;
    joints[3].parentLocalId = 10;
    joints[3].paletteSlot = 3;

    const SkeletonCookResult r = cook(joints);
    REQUIRE((r.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(r.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    REQUIRE(parsed.skeleton.joints.size() == 4);
    // Ready set {10, 50} -> 10 first; then {20, 50} -> 20; then {50}; then {60}.
    const std::array<std::uint32_t, 4> expectedOrder = {10, 20, 50, 60};
    CHECK(expectedOrder.size() == 4);  // literal row count
    for (std::size_t i = 0; i < expectedOrder.size(); ++i) {
        CHECK(parsed.skeleton.joints[i].sourceNodeLocalId == expectedOrder[i]);
    }
    const std::array<std::uint32_t, 4> expectedParents = {COOKED_SKELETON_INVALID_INDEX, 0,
                                                          COOKED_SKELETON_INVALID_INDEX, 2};
    for (std::size_t i = 0; i < expectedParents.size(); ++i) {
        CHECK(parsed.skeleton.joints[i].parent == expectedParents[i]);
    }
}

TEST_CASE("skeleton cook: parents are remapped to emission indices, scrambled input included (KC14)") {
    // A four-deep chain supplied in a scrambled order. Every record's parent index is pinned to a
    // literal, which is what an off-by-one remap after the sort cannot survive.
    std::vector<SkeletonCookJoint> joints = chain(4);
    const std::vector<SkeletonCookJoint> scrambled = {joints[2], joints[0], joints[3], joints[1]};
    const SkeletonCookResult r = cook(scrambled);
    REQUIRE((r.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(r.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    REQUIRE(parsed.skeleton.joints.size() == 4);
    const std::array<std::uint32_t, 4> expectedParents = {COOKED_SKELETON_INVALID_INDEX, 0, 1, 2};
    CHECK(expectedParents.size() == 4);  // literal row count
    for (std::size_t i = 0; i < expectedParents.size(); ++i) {
        CHECK(parsed.skeleton.joints[i].parent == expectedParents[i]);
        CHECK(parsed.skeleton.joints[i].sourceNodeLocalId == 100 + (static_cast<std::uint32_t>(i) * 7));
        CHECK(parsed.skeleton.joints[i].paletteSlot == static_cast<std::uint32_t>(i));
    }
}

TEST_CASE("skeleton cook: hierarchy-only records carry an identity IBM whatever the input said (KC15)") {
    const std::array<SkeletonCookJoint, 3> joints = closureJoints();
    REQUIRE(joints[0].paletteSlot == SKELETON_INVALID_INDEX);
    REQUIRE(joints[0].inverseBind.columns[3].x == doctest::Approx(9.0F));  // the input's, deliberately absurd
    const SkeletonCookResult r = cook(joints, closureGuid());
    REQUIRE((r.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(r.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    REQUIRE(parsed.skeleton.joints.size() == 3);
    const engine::assets::CookedSkeletonJoint& ancestor = parsed.skeleton.joints[0];
    REQUIRE(ancestor.paletteSlot == COOKED_SKELETON_INVALID_INDEX);
    CHECK(approxEquals(ancestor.inverseBind, Mat4::identity()));
    // The ancestor's own TRS is preserved -- only the IBM is overridden.
    CHECK(ancestor.translation.y == doctest::Approx(1.0F));
    // And a slotted record's IBM is NOT overridden.
    CHECK(parsed.skeleton.joints[1].inverseBind.columns[3].x == doctest::Approx(-1.0F));
}

TEST_CASE("skeleton cook: no cook path emits a warning today (KC16)") {
    // v1's cook has no advisory of its own: the model-level ones (multi-skin, out-of-range vertex
    // joint index) belong to the editor adapter, which is the only layer that can see a model. This
    // case is what makes a future warning a visible decision rather than a drift.
    const std::array<SkeletonCookJoint, 2> minimal = minimalJoints();
    const std::array<SkeletonCookJoint, 3> closure = closureJoints();
    const SkeletonCookResult a = cook(minimal);
    const SkeletonCookResult b = cook(closure, closureGuid());
    REQUIRE((a.status == SkeletonCookStatus::Ok));
    REQUIRE((b.status == SkeletonCookStatus::Ok));
    CHECK(a.warnings.empty());
    CHECK(b.warnings.empty());
    // Refusals carry a message, never a warning.
    const SkeletonCookResult refused = cook(std::span<const SkeletonCookJoint>{});
    CHECK(refused.warnings.empty());
    CHECK(!refused.message.empty());
    CHECK(a.message.empty());
    CHECK(b.message.empty());
}

TEST_CASE("skeleton cook: the output parses back Ok with every field equal (KC17)") {
    // The two goldens first, field for field against the canonical order the cook chose.
    const std::array<SkeletonCookJoint, 2> minimal = minimalJoints();
    const SkeletonCookResult minimalCooked = cook(minimal);
    REQUIRE((minimalCooked.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult m = parseCookedSkeleton(std::span<const std::byte>(minimalCooked.bytes));
    REQUIRE((m.status == CookedSkeletonStatus::Ok));
    REQUIRE(m.skeleton.joints.size() == 2);
    CHECK(m.skeleton.formatVersion == 1);
    CHECK(m.skeleton.paletteJointCount == 2);
    CHECK(!m.skeleton.sourceGuid.valid());
    CHECK(m.skeleton.joints[0].parent == COOKED_SKELETON_INVALID_INDEX);
    CHECK(m.skeleton.joints[0].sourceNodeLocalId == 3);
    CHECK(m.skeleton.joints[1].parent == 0);
    CHECK(m.skeleton.joints[1].sourceNodeLocalId == 7);
    CHECK(m.skeleton.joints[1].rotation.z == doctest::Approx(1.0F));
    CHECK(m.skeleton.joints[1].scale.x == doctest::Approx(2.0F));
    CHECK(m.skeleton.joints[1].inverseBind.columns[3].y == doctest::Approx(-1.25F));

    const std::array<SkeletonCookJoint, 3> closure = closureJoints();
    const SkeletonCookResult closureCooked = cook(closure, closureGuid(), 2);
    REQUIRE((closureCooked.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult c = parseCookedSkeleton(std::span<const std::byte>(closureCooked.bytes));
    REQUIRE((c.status == CookedSkeletonStatus::Ok));
    REQUIRE(c.skeleton.joints.size() == 3);
    CHECK(c.skeleton.sourceGuid == closureGuid());
    CHECK(c.skeleton.sourceSkinIndex == 2);
    CHECK(c.skeleton.paletteJointCount == 2);
    CHECK(c.skeleton.joints[0].paletteSlot == COOKED_SKELETON_INVALID_INDEX);
    CHECK(c.skeleton.joints[1].paletteSlot == 1);
    CHECK(c.skeleton.joints[2].paletteSlot == 0);

    // Then a property-style pass: sixteen joints in eight deterministic orders, each round trip
    // reproducing the same canonical table.
    const std::vector<SkeletonCookJoint> base = chain(16);
    const SkeletonCookResult reference = cook(base);
    REQUIRE((reference.status == SkeletonCookStatus::Ok));
    constexpr int ROUNDS = 8;
    for (int p = 0; p < ROUNDS; ++p) {
        std::vector<SkeletonCookJoint> shuffled = base;
        Splitmix rng(static_cast<std::uint64_t>(p) + 1ULL);
        for (std::size_t i = shuffled.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(rng.next() % i);
            std::swap(shuffled[i - 1], shuffled[j]);
        }
        const SkeletonCookResult r = cook(shuffled);
        REQUIRE((r.status == SkeletonCookStatus::Ok));
        const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(r.bytes));
        REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
        REQUIRE(parsed.skeleton.joints.size() == 16);
        for (std::uint32_t i = 0; i < 16; ++i) {
            CHECK(parsed.skeleton.joints[i].sourceNodeLocalId == 100 + (i * 7));
            CHECK(parsed.skeleton.joints[i].paletteSlot == i);
            CHECK(parsed.skeleton.joints[i].parent == (i == 0 ? COOKED_SKELETON_INVALID_INDEX : i - 1));
        }
    }
}

TEST_CASE("skeleton cook: TRS and IBM cells reach the file bit for bit (KC18)") {
    // The cook performs no floating-point arithmetic at all, so these patterns must survive
    // unchanged. Negative zero and a signalling NaN are what any "helpful" normalization destroys,
    // and neither compares equal under ==.
    constexpr std::uint32_t NEG_ZERO = 0x80000000U;
    constexpr std::uint32_t SIGNALLING = 0x7FA00000U;
    std::array<SkeletonCookJoint, 2> joints = minimalJoints();
    joints[0].translation.x = std::bit_cast<float>(NEG_ZERO);
    joints[1].inverseBind.columns[2].y = std::bit_cast<float>(SIGNALLING);
    joints[1].rotation.x = std::bit_cast<float>(NEG_ZERO);

    const SkeletonCookResult r = cook(joints);
    REQUIRE((r.status == SkeletonCookStatus::Ok));
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(r.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    REQUIRE(parsed.skeleton.joints.size() == 2);
    CHECK(bits(parsed.skeleton.joints[0].translation.x) == NEG_ZERO);
    CHECK(bits(parsed.skeleton.joints[1].inverseBind.columns[2].y) == SIGNALLING);
    CHECK(bits(parsed.skeleton.joints[1].rotation.x) == NEG_ZERO);
    // The record size cannot absorb a composed matrix: 128 bytes hold TRS AND a 4x4, so a cook that
    // baked TRS into a matrix would move every subsequent byte -- which the goldens pin exactly.
    CHECK(r.bytes.size() == COOKED_SKELETON_HEADER_BYTES + (2 * COOKED_SKELETON_JOINT_BYTES));
}
