// tests/render_skinning_test.cpp — task 3.5.1: the render-side skinning path (JP*, SN*).
//
// Tier 0 (no GPU, every lane, JP*): bindPose against the two frozen .aeroskel goldens, the matrix
// palette's composition order in both of the places it can be reversed (parent x child, global x
// inverse bind), the hierarchy-only ancestor's contribution, palette placement BY SLOT rather than by
// record order, the cooked-section repack, and the Mat4 -> three-rows packer.
//
// Tier 1 (a real Device, NO window — RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL,
// SN*): the mesh registry's lifecycle and the four-pipeline draw path.
//
// skinning_pack.hpp and mesh_pack.hpp are PRIVATE to engine/render (src/, never installed), so they
// are reached by a relative include — the render_material_test.cpp / material_pack.hpp precedent. The
// SYMBOLS come from aero_render, which aero_tests already links; no link-line and no
// include-directory change.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md). Enum comparisons use
// double parentheses, because engine::rhi::toString is found by ADL from doctest's stringifier.
//
// Every case-local table pins a LITERAL row count, never TABLE.size() against itself.

#include <aero/assets/cooked_skeleton.hpp>
#include <aero/render/render.hpp>

#include "../engine/render/src/skinning_pack.hpp"
#include "cooked_skeleton_golden.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

using engine::Mat4;
using engine::Quat;
using engine::Trs;
using engine::Vec3;
using engine::Vec4;
using engine::assets::COOKED_SKELETON_INVALID_INDEX;
using engine::assets::CookedSkeleton;
using engine::assets::CookedSkeletonJoint;
using engine::render::JointPose;

namespace {

// The two goldens, parsed. A REQUIRE rather than a CHECK: every case below reads fields off the
// result, so a failed parse must stop the case rather than produce a cascade of derived failures.
[[nodiscard]] CookedSkeleton parseGolden(std::span<const std::uint8_t> golden) {
    const engine::assets::CookedSkeletonParseResult result = engine::assets::parseCookedSkeleton(std::as_bytes(golden));
    REQUIRE(result.status == engine::assets::CookedSkeletonStatus::Ok);
    return result.skeleton;
}

[[nodiscard]] CookedSkeleton minimalGolden() {
    return parseGolden(std::span{aero_test::COOKED_SKELETON_GOLDEN_MINIMAL});
}

[[nodiscard]] CookedSkeleton closureGolden() {
    return parseGolden(std::span{aero_test::COOKED_SKELETON_GOLDEN_CLOSURE});
}

// bindPose into a right-sized vector — the shape every consumer uses, including the sample.
[[nodiscard]] std::vector<JointPose> bindPoseOf(const CookedSkeleton& skeleton) {
    std::vector<JointPose> pose(skeleton.joints.size());
    engine::render::bindPose(skeleton, pose);
    return pose;
}

[[nodiscard]] std::vector<Mat4> paletteOf(const CookedSkeleton& skeleton, std::span<const JointPose> pose) {
    std::vector<Mat4> palette(skeleton.paletteJointCount);
    engine::render::computeJointPalette(skeleton, pose, palette);
    return palette;
}

// A hand-built skeleton record, so the composition-order cases can choose transforms that make a
// reversed product land somewhere visibly different rather than on a coincidence.
[[nodiscard]] CookedSkeletonJoint makeJoint(std::uint32_t parent, std::uint32_t slot, const Trs& trs,
                                            const Mat4& inverseBind) {
    CookedSkeletonJoint joint;
    joint.parent = parent;
    joint.paletteSlot = slot;
    joint.sourceNodeLocalId = 0;
    joint.translation = trs.translation;
    joint.rotation = trs.rotation;
    joint.scale = trs.scale;
    joint.inverseBind = inverseBind;
    return joint;
}

}  // namespace

TEST_CASE("render skinning: bindPose reproduces the cooked bind-local TRS record for record (JP1)") {
    // The seed this exists for returns identity poses, which draws the rig collapsed at the origin —
    // loud on screen and invisible to every other tier-0 case here, because computeJointPalette would
    // still produce a self-consistent (wrong) palette from them.
    const CookedSkeleton minimal = minimalGolden();
    REQUIRE(minimal.joints.size() == 2);
    const std::vector<JointPose> minimalPose = bindPoseOf(minimal);
    REQUIRE(minimalPose.size() == 2);

    // Golden A's two records, restated as literals: the root's (1,2,3) translation and the child's
    // 180-degree-about-Z rotation with a uniform scale of 2.
    CHECK(minimalPose[0].translation == Vec3{1.0F, 2.0F, 3.0F});
    CHECK((minimalPose[0].rotation == Quat::identity()));
    CHECK(minimalPose[0].scale == Vec3::one());
    CHECK(minimalPose[1].translation == Vec3{0.0F, 0.5F, 0.0F});
    CHECK((minimalPose[1].rotation == Quat{0.0F, 0.0F, 1.0F, 0.0F}));
    CHECK(minimalPose[1].scale == Vec3{2.0F, 2.0F, 2.0F});

    // ...and the same, record for record, against the skeleton's own fields, so a THIRD record added
    // to a golden cannot silently escape the pinning above.
    for (std::size_t i = 0; i < minimal.joints.size(); ++i) {
        INFO("minimal record: ", i);
        CHECK(minimalPose[i].translation == minimal.joints[i].translation);
        CHECK((minimalPose[i].rotation == minimal.joints[i].rotation));
        CHECK(minimalPose[i].scale == minimal.joints[i].scale);
    }

    const CookedSkeleton closure = closureGolden();
    REQUIRE(closure.joints.size() == 3);
    const std::vector<JointPose> closurePose = bindPoseOf(closure);
    REQUIRE(closurePose.size() == 3);
    // The hierarchy-only ancestor is a real record with a real transform — bindPose does not skip it.
    CHECK(closurePose[0].translation == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(closurePose[1].translation == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(closurePose[2].translation == Vec3{-1.0F, 0.0F, 0.0F});
    for (std::size_t i = 0; i < closure.joints.size(); ++i) {
        INFO("closure record: ", i);
        CHECK(closurePose[i].translation == closure.joints[i].translation);
        CHECK((closurePose[i].rotation == closure.joints[i].rotation));
        CHECK(closurePose[i].scale == closure.joints[i].scale);
    }
}

TEST_CASE("render skinning: the minimal golden's bind palette equals globalBind x inverseBind (JP2)") {
    const CookedSkeleton skeleton = minimalGolden();
    const std::vector<JointPose> pose = bindPoseOf(skeleton);
    const std::vector<Mat4> palette = paletteOf(skeleton, pose);
    REQUIRE(palette.size() == 2);

    // Hand-derived, every value exactly representable in binary32 (which is why Golden A's inputs
    // were chosen the way they were). Slot 0 is the root: T(1,2,3) composed with an identity inverse
    // bind is the translation itself.
    CHECK(engine::approxEquals(palette[0], engine::translation(Vec3{1.0F, 2.0F, 3.0F})));

    // Slot 1 is the child. global[1] = T(1,2,3) * (T(0,0.5,0) * R180z * S(2,2,2)), whose upper 3x3 is
    // diag(-2,-2,2) and whose translation column is (1, 2.5, 3). Its inverse bind is a half-scale
    // with translation (-0.5,-1.25,0), so the product's columns are (-1,0,0), (0,-1,0), (0,0,1) and
    // the translation column is (2,5,3) — derived by hand from section 12's own record table, never
    // read back out of the function under test.
    const Mat4 expectedChild{std::array<Vec4, 4>{Vec4{-1.0F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, -1.0F, 0.0F, 0.0F},
                                                 Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{2.0F, 5.0F, 3.0F, 1.0F}}};
    CHECK(engine::approxEquals(palette[1], expectedChild));

    // The two slots are DISTINCT matrices, so nothing above could be satisfied by a degenerate answer
    // (an all-identity palette passes neither, but a palette that wrote one matrix twice would need
    // this line to be caught).
    CHECK(palette[0] != palette[1]);
}

TEST_CASE("render skinning: the closure golden pins the ancestor's reach and placement BY SLOT (JP3)") {
    const CookedSkeleton skeleton = closureGolden();
    REQUIRE(skeleton.joints.size() == 3);
    REQUIRE(skeleton.paletteJointCount == 2);
    // Record order and slot order DIVERGE by construction (the golden's own note): record 1 carries
    // slot 1 and record 2 carries slot 0. That divergence is what makes this case able to see a
    // palette written in record order at all.
    CHECK(skeleton.joints[0].paletteSlot == COOKED_SKELETON_INVALID_INDEX);
    CHECK(skeleton.joints[1].paletteSlot == 1);
    CHECK(skeleton.joints[2].paletteSlot == 0);

    // Arm 1 — the BIND pose. Both inverse binds are exactly the inverses of their joints' global bind
    // transforms, so both palette entries are identity: that is what an inverse bind matrix MEANS,
    // and it is the arm that reddens if the hierarchy-only ancestor's (0,1,0) is skipped (each entry
    // then carries a stray -1 in Y).
    const std::vector<JointPose> bind = bindPoseOf(skeleton);
    const std::vector<Mat4> bindPalette = paletteOf(skeleton, bind);
    REQUIRE(bindPalette.size() == 2);
    CHECK(engine::approxEquals(bindPalette[0], Mat4::identity()));
    CHECK(engine::approxEquals(bindPalette[1], Mat4::identity()));

    // Arm 2 — POSED, with three distinct translations, so the two palette entries are distinct and
    // the ancestor's contribution is visible in both. Hand-derived:
    //   global[1] = T(0,5,0) * T(2,0,0) = T(2,5,0);  out[1] = T(2,5,0) * T(-1,-1,0) = T(1,4,0)
    //   global[2] = T(0,5,0) * T(-3,0,0) = T(-3,5,0); out[0] = T(-3,5,0) * T(1,-1,0) = T(-2,4,0)
    // A palette written in record order swaps these two; a pass that skipped the ancestor loses the
    // +4 in Y from both.
    std::vector<JointPose> posed = bind;
    posed[0].translation = Vec3{0.0F, 5.0F, 0.0F};
    posed[1].translation = Vec3{2.0F, 0.0F, 0.0F};
    posed[2].translation = Vec3{-3.0F, 0.0F, 0.0F};
    const std::vector<Mat4> posedPalette = paletteOf(skeleton, posed);
    REQUIRE(posedPalette.size() == 2);
    CHECK(engine::approxEquals(posedPalette[0], engine::translation(Vec3{-2.0F, 4.0F, 0.0F})));
    CHECK(engine::approxEquals(posedPalette[1], engine::translation(Vec3{1.0F, 4.0F, 0.0F})));
    CHECK(posedPalette[0] != posedPalette[1]);
}

TEST_CASE("render skinning: the parent-child product composes parent x child, not the reverse (JP4)") {
    // A 90-degree child rotation with both joints translated: parent * child and child * parent are
    // then two DIFFERENT matrices whose translation columns are (4,0,0) and (1,3,0). A reversed
    // composition still draws a moving, lit mesh — it merely rotates limbs about the wrong pivot.
    CookedSkeleton skeleton;
    skeleton.paletteJointCount = 2;
    skeleton.joints.push_back(
        makeJoint(COOKED_SKELETON_INVALID_INDEX, 0, Trs{.translation = Vec3{3.0F, 0.0F, 0.0F}}, Mat4::identity()));
    skeleton.joints.push_back(
        makeJoint(0, 1,
                  Trs{.translation = Vec3{1.0F, 0.0F, 0.0F},
                      .rotation = engine::fromAxisAngle(Vec3{0.0F, 0.0F, 1.0F}, engine::radians(90.0F))},
                  Mat4::identity()));

    const std::vector<JointPose> pose = bindPoseOf(skeleton);
    const std::vector<Mat4> palette = paletteOf(skeleton, pose);
    REQUIRE(palette.size() == 2);

    const Mat4 parentGlobal = engine::translation(Vec3{3.0F, 0.0F, 0.0F});
    const Mat4 childLocal =
        engine::compose(Trs{.translation = Vec3{1.0F, 0.0F, 0.0F},
                            .rotation = engine::fromAxisAngle(Vec3{0.0F, 0.0F, 1.0F}, engine::radians(90.0F))});
    CHECK(engine::approxEquals(palette[0], parentGlobal));
    CHECK(engine::approxEquals(palette[1], parentGlobal * childLocal));
    // The reversed product is a DIFFERENT matrix, stated here so the assertion above is known to be
    // discriminating rather than accidentally satisfied by both orders.
    CHECK_FALSE(engine::approxEquals(parentGlobal * childLocal, childLocal * parentGlobal));
    // ...and the translation columns, hand-derived: 3 + 1 along +X versus the parent's offset rotated
    // into +Y by the child's own 90 degrees.
    CHECK(engine::approxEquals(palette[1].columns[3], Vec4{4.0F, 0.0F, 0.0F, 1.0F}));
    CHECK(engine::approxEquals((childLocal * parentGlobal).columns[3], Vec4{1.0F, 3.0F, 0.0F, 1.0F}));
}

TEST_CASE("render skinning: the palette multiplies global x inverseBind, not the reverse (JP5)") {
    // A translation and a uniform scale do NOT commute, which is what makes this case able to see the
    // reversal at all: two translations would agree in both orders and prove nothing.
    const Mat4 inverseBind = engine::scaling(Vec3{2.0F, 2.0F, 2.0F});
    CookedSkeleton skeleton;
    skeleton.paletteJointCount = 1;
    skeleton.joints.push_back(
        makeJoint(COOKED_SKELETON_INVALID_INDEX, 0, Trs{.translation = Vec3{2.0F, 0.0F, 0.0F}}, inverseBind));

    const std::vector<JointPose> pose = bindPoseOf(skeleton);
    const std::vector<Mat4> palette = paletteOf(skeleton, pose);
    REQUIRE(palette.size() == 1);

    const Mat4 global = engine::translation(Vec3{2.0F, 0.0F, 0.0F});
    CHECK(engine::approxEquals(palette[0], global * inverseBind));
    CHECK_FALSE(engine::approxEquals(global * inverseBind, inverseBind * global));
    // Hand-derived translation columns: scaling first leaves the joint's own (2,0,0); scaling the
    // GLOBAL doubles it to (4,0,0).
    CHECK(engine::approxEquals(palette[0].columns[3], Vec4{2.0F, 0.0F, 0.0F, 1.0F}));
    CHECK(engine::approxEquals((inverseBind * global).columns[3], Vec4{4.0F, 0.0F, 0.0F, 1.0F}));
}

TEST_CASE("render skinning: a two-root skeleton roots both chains at identity (JP6)") {
    // Two roots is legal (the closure of two disjoint joint sets), and neither may inherit the
    // other's transform. A pass that treated record 0 as everyone's parent puts root 1 at (1,1,0).
    CookedSkeleton skeleton;
    skeleton.paletteJointCount = 2;
    skeleton.joints.push_back(
        makeJoint(COOKED_SKELETON_INVALID_INDEX, 0, Trs{.translation = Vec3{1.0F, 0.0F, 0.0F}}, Mat4::identity()));
    skeleton.joints.push_back(
        makeJoint(COOKED_SKELETON_INVALID_INDEX, 1, Trs{.translation = Vec3{0.0F, 1.0F, 0.0F}}, Mat4::identity()));

    const std::vector<JointPose> pose = bindPoseOf(skeleton);
    const std::vector<Mat4> palette = paletteOf(skeleton, pose);
    REQUIRE(palette.size() == 2);
    CHECK(engine::approxEquals(palette[0], engine::translation(Vec3{1.0F, 0.0F, 0.0F})));
    CHECK(engine::approxEquals(palette[1], engine::translation(Vec3{0.0F, 1.0F, 0.0F})));
    CHECK(palette[0] != palette[1]);
}

TEST_CASE("render skinning: packJointPaletteRows extracts ROWS of the column-major matrix (JP7)") {
    // THE falsifiability case for the packer. The two matrices carry 24 mutually distinct cell values
    // (1..12 and 13..24), so the expectation below is a set of LITERALS and never the mapping applied
    // to itself — comparing rows against `transpose(m).columns[k]` would agree with a transposed
    // packer, which is precisely the trap the 3.4.2 ME24 finding recorded.
    //
    // Under a transpose, row 0 would read (1,2,3,0); under a dropped column it would read (1,4,7,0);
    // under a duplicated row 0 the third row would read (1,4,7,10). None of those satisfies the
    // table.
    const Mat4 first{std::array<Vec4, 4>{Vec4{1.0F, 2.0F, 3.0F, 0.0F}, Vec4{4.0F, 5.0F, 6.0F, 0.0F},
                                         Vec4{7.0F, 8.0F, 9.0F, 0.0F}, Vec4{10.0F, 11.0F, 12.0F, 1.0F}}};
    const Mat4 second{std::array<Vec4, 4>{Vec4{13.0F, 14.0F, 15.0F, 0.0F}, Vec4{16.0F, 17.0F, 18.0F, 0.0F},
                                          Vec4{19.0F, 20.0F, 21.0F, 0.0F}, Vec4{22.0F, 23.0F, 24.0F, 1.0F}}};
    const std::array<Mat4, 2> palette{first, second};

    // Eight entries: six written, two left as the caller's zeroed tail. The renderer pushes the full
    // block every skinned draw, so "the packer never writes past 3 x count" is a real property.
    std::array<Vec4, 8> rows{};
    engine::render::detail::packJointPaletteRows(palette, rows);

    constexpr std::array EXPECTED_ROWS{
        Vec4{1.0F, 4.0F, 7.0F, 10.0F},    Vec4{2.0F, 5.0F, 8.0F, 11.0F},    Vec4{3.0F, 6.0F, 9.0F, 12.0F},
        Vec4{13.0F, 16.0F, 19.0F, 22.0F}, Vec4{14.0F, 17.0F, 20.0F, 23.0F}, Vec4{15.0F, 18.0F, 21.0F, 24.0F},
    };
    CHECK(EXPECTED_ROWS.size() == 6);
    for (std::size_t i = 0; i < EXPECTED_ROWS.size(); ++i) {
        INFO("row: ", i);
        CHECK(rows[i] == EXPECTED_ROWS[i]);
    }
    // The affine bottom row is DROPPED, never emitted as a fourth entry — the stride is three, which
    // the second matrix's rows landing at 3..5 already proves, and the untouched tail confirms.
    CHECK(rows[6] == Vec4{});
    CHECK(rows[7] == Vec4{});

    // An empty palette writes nothing at all (the renderer's static path never calls this, but the
    // sample's first frame can hand it a palette before the pose exists).
    std::array<Vec4, 3> untouched{};
    engine::render::detail::packJointPaletteRows({}, untouched);
    CHECK(untouched[0] == Vec4{});
    CHECK(untouched[1] == Vec4{});
    CHECK(untouched[2] == Vec4{});
}

TEST_CASE("render skinning: the 85-joint cap is the measured 4096-byte push-uniform ceiling (JP8)") {
    // The static_assert in skinning.hpp is the build-time half; this is the runtime restatement, with
    // the arithmetic spelled out so a future edit to the constant has to face the derivation rather
    // than just the number. 48 bytes per joint (three 16-byte rows), 4096 bytes per push-uniform slot
    // on the tightest backend.
    CHECK(engine::render::MAX_SKINNING_JOINTS == 85);
    CHECK(3U * engine::render::MAX_SKINNING_JOINTS == 255U);               // float4 rows
    CHECK(3U * engine::render::MAX_SKINNING_JOINTS * 16U == 4080U);        // bytes pushed
    CHECK(3U * engine::render::MAX_SKINNING_JOINTS * 16U <= 4096U);        // the SDL ceiling
    CHECK(3U * (engine::render::MAX_SKINNING_JOINTS + 1U) * 16U > 4096U);  // and it is TIGHT
    // The format's cap is deliberately wider than the renderer's: formats outlive renderers, and a
    // .aeroskel carrying 256 palette slots is legal even though this renderer refuses to draw it.
    CHECK(engine::assets::MAX_COOKED_SKELETON_PALETTE == 256);
    CHECK(engine::render::MAX_SKINNING_JOINTS < engine::assets::MAX_COOKED_SKELETON_PALETTE);
}
