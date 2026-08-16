// tests/editor/skeleton_cook_source_test.cpp -- task 3.5.1: the ImportedModel -> skeleton cook
// adapter. A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the mesh_cook_source_test.cpp precedent): skeleton_cook_source.hpp depends only on
// aero/editor/model_import.hpp and aero/assets/skeleton_cook.hpp, and aero::assets is a PUBLIC,
// UNGATED dependency of aero_editor_core -- so every case here must be PRESENT and PASSING in all
// three build configurations. No GPU, no window, no ImGui context, no sleeps.
//
// Most cases drive a HAND-BUILT ImportedModel rather than a document, deliberately: the traps this
// adapter exists to discharge (a localId that is not a position, a non-joint node between two joints,
// a parent cycle, a Structure-depth skin) are either impossible or invisible in glTF, where localIds
// and positions always coincide. KS1 is the real-import closure that keeps the hand-built models
// honest.
#include <aero/assets/cooked_skeleton.hpp>
#include <aero/assets/skeleton_cook.hpp>
#include <aero/editor/model_import.hpp>
#include <aero/editor/skeleton_cook_source.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies std::string_view operands
// through operator<<(std::ostream&, std::string_view), which MS STL defines inline in <string_view>
// against an INCOMPLETE std::basic_ostream. Written when the TU was created.
#include <ostream>
#include <vector>

using engine::Guid;
using engine::Mat4;
using engine::Vec3;
using engine::Vec4;
using engine::assets::COOKED_SKELETON_INVALID_INDEX;
using engine::assets::CookedSkeletonParseResult;
using engine::assets::CookedSkeletonStatus;
using engine::assets::parseCookedSkeleton;
using engine::assets::SKELETON_INVALID_INDEX;
using engine::assets::SkeletonCookJoint;
using engine::assets::SkeletonCookResult;
using engine::assets::SkeletonCookStatus;
using engine::editor::cookImportedSkeleton;
using engine::editor::ImportDepth;
using engine::editor::ImportedMesh;
using engine::editor::ImportedModel;
using engine::editor::ImportedNode;
using engine::editor::ImportedPrimitive;
using engine::editor::ImportedSkin;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::INVALID_SUBASSET;
using engine::editor::MAX_IMPORT_WARNINGS;
using engine::editor::MAX_NODE_DEPTH;
using engine::editor::skeletonCookJoints;
using engine::editor::SkeletonSourceResult;
using engine::editor::VertexAttribute;

namespace {

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

[[nodiscard]] std::string readFixture(const std::string& path) {
    const scene_golden::FileBytes bytes = scene_golden::readBytes(path);
    REQUIRE_MESSAGE(bytes.ok, bytes.error);
    return bytes.text;
}

// A node with a localId that is deliberately NOT its position, plus a translation derived from that
// localId so a mis-resolution shows up as a wrong number rather than as a crash.
[[nodiscard]] ImportedNode node(std::uint32_t localId, std::uint32_t parent) {
    ImportedNode n;
    n.name = "node" + std::to_string(localId);
    n.localId = localId;
    n.parent = parent;
    n.translation = Vec3{static_cast<float>(localId), 0.0F, 0.0F};
    return n;
}

[[nodiscard]] Mat4 translationMatrix(float x, float y, float z) {
    Mat4 m = Mat4::identity();
    m.columns[3] = Vec4{x, y, z, 1.0F};
    return m;
}

// A skin over the given joint localIds, with one distinct non-identity IBM per joint.
[[nodiscard]] ImportedSkin skin(const std::vector<std::uint32_t>& joints) {
    ImportedSkin s;
    s.name = "skin";
    s.joints = joints;
    for (std::size_t k = 0; k < joints.size(); ++k) {
        const auto f = static_cast<float>(k + 1);
        s.inverseBindMatrices.push_back(translationMatrix(-f, -2.0F * f, 0.0F));
    }
    return s;
}

// The FBX shape: four joints in a chain whose localIds are raw ids, non-dense and non-positional.
// nodes[localId] is out of bounds for every one of them, which is what makes this model the case
// that reddens if anyone indexes the vector by id.
[[nodiscard]] ImportedModel fbxShapedModel() {
    ImportedModel model;
    model.nodes.push_back(node(100, INVALID_SUBASSET));
    model.nodes.push_back(node(205, 100));
    model.nodes.push_back(node(300, 205));
    model.nodes.push_back(node(407, 300));
    model.roots.push_back(100);
    model.skins.push_back(skin({100, 205, 300, 407}));
    return model;
}

[[nodiscard]] const SkeletonCookJoint* findJoint(const SkeletonSourceResult& result, std::uint32_t localId) {
    for (const SkeletonCookJoint& joint : result.joints) {
        if (joint.localId == localId) {
            return &joint;
        }
    }
    return nullptr;
}

[[nodiscard]] bool sameBits(float a, float b) noexcept {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

[[nodiscard]] bool sameMatrixBits(const Mat4& a, const Mat4& b) noexcept {
    for (std::size_t c = 0; c < 4; ++c) {
        if (!sameBits(a.columns[c].x, b.columns[c].x) || !sameBits(a.columns[c].y, b.columns[c].y) ||
            !sameBits(a.columns[c].z, b.columns[c].z) || !sameBits(a.columns[c].w, b.columns[c].w)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("skeleton_cook_source: a real imported skin closes over its own nodes (KS1)") {
    // tests/fixtures/assets/skinned.gltf: four joint nodes, all scene roots, one skin listing them in
    // source order with four distinct non-identity inverse bind matrices. The closure adds nothing
    // here BECAUSE every joint is a root -- which is itself the assertion.
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/skinned.gltf");
    const ImportResult imported =
        importModel("skinned.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE((imported.status == ImportStatus::Ok));
    REQUIRE(imported.model.skins.size() == 1);
    REQUIRE(imported.model.skins[0].joints.size() == 4);

    const SkeletonSourceResult result = skeletonCookJoints(imported.model, 0);
    REQUIRE(result.ok);
    CHECK(result.error.empty());
    CHECK(result.warnings.empty());
    REQUIRE(result.joints.size() == 4);
    for (std::size_t k = 0; k < 4; ++k) {
        const std::uint32_t jointLocalId = imported.model.skins[0].joints[k];
        const SkeletonCookJoint& joint = result.joints[k];
        CHECK(joint.localId == jointLocalId);
        CHECK(joint.paletteSlot == static_cast<std::uint32_t>(k));  // slot == SOURCE POSITION
        const SkeletonCookJoint* found = findJoint(result, jointLocalId);
        REQUIRE(found != nullptr);
        // Field for field against the import's own node and its own IBM, bit for bit.
        for (const ImportedNode& source : imported.model.nodes) {
            if (source.localId != jointLocalId) {
                continue;
            }
            CHECK(joint.parentLocalId == source.parent);
            CHECK(sameBits(joint.translation.x, source.translation.x));
            CHECK(sameBits(joint.rotation.w, source.rotation.w));
            CHECK(sameBits(joint.scale.y, source.scale.y));
        }
        CHECK(sameMatrixBits(joint.inverseBind, imported.model.skins[0].inverseBindMatrices[k]));
    }
    // The fixture's fourth IBM carries (31, 32, 33) in its translation column -- a value nothing else
    // in this suite produces, so a shifted IBM lookup is visible rather than plausible.
    CHECK(result.joints[3].inverseBind.columns[3].x == doctest::Approx(31.0F));
}

TEST_CASE("skeleton_cook_source: non-positional localIds resolve through the map, never by index (KS2)") {
    // Every localId here is out of bounds as an index into a four-node vector. A `nodes[localId]`
    // read is an ASan heap-buffer-overflow, which is what this model exists to catch -- glTF, where
    // localId always equals the position, cannot see the seed at all.
    const ImportedModel model = fbxShapedModel();
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    REQUIRE(result.ok);
    REQUIRE(result.joints.size() == 4);
    const std::array<std::uint32_t, 4> expectedIds = {100, 205, 300, 407};
    CHECK(expectedIds.size() == 4);  // literal row count
    for (std::size_t k = 0; k < expectedIds.size(); ++k) {
        CHECK(result.joints[k].localId == expectedIds[k]);
        CHECK(result.joints[k].paletteSlot == static_cast<std::uint32_t>(k));
        // The translation is derived from the localId, so a resolution that landed on the wrong node
        // reads a wrong number here instead of merely a different pointer.
        CHECK(result.joints[k].translation.x == doctest::Approx(static_cast<float>(expectedIds[k])));
    }
    CHECK(result.joints[0].parentLocalId == SKELETON_INVALID_INDEX);
    CHECK(result.joints[1].parentLocalId == 100);
    CHECK(result.joints[3].parentLocalId == 300);
}

TEST_CASE("skeleton_cook_source: a non-joint node between two joints is carried hierarchy-only (KS3)") {
    // 10 (joint, root) -> 20 (NOT a joint) -> 30 (joint). glTF permits this, and dropping node 20
    // silently moves joint 30 by 20's whole transform.
    ImportedModel model;
    model.nodes.push_back(node(10, INVALID_SUBASSET));
    model.nodes.push_back(node(20, 10));
    model.nodes.push_back(node(30, 20));
    model.roots.push_back(10);
    model.skins.push_back(skin({10, 30}));

    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    REQUIRE(result.ok);
    REQUIRE(result.joints.size() == 3);  // two palette joints plus the closure's one ancestor
    const SkeletonCookJoint* between = findJoint(result, 20);
    REQUIRE(between != nullptr);
    CHECK(between->paletteSlot == SKELETON_INVALID_INDEX);  // hierarchy-only
    CHECK(between->parentLocalId == 10);                    // in its real ancestral position
    CHECK(between->translation.x == doctest::Approx(20.0F));
    // Its IBM is identity: it binds no vertex, so it has no bind matrix of its own to carry.
    CHECK(sameMatrixBits(between->inverseBind, Mat4::identity()));
    // The palette joints keep their slots and their own IBMs.
    const SkeletonCookJoint* leaf = findJoint(result, 30);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->paletteSlot == 1);
    CHECK(leaf->parentLocalId == 20);
    CHECK(leaf->inverseBind.columns[3].x == doctest::Approx(-2.0F));
}

TEST_CASE("skeleton_cook_source: a Structure-depth model is refused by name (KS4)") {
    // At Structure depth ImportedSkin::inverseBindMatrices is DELIBERATELY empty. Cooking it would
    // write identity matrices that look like a rig and deform like a bug.
    ImportedModel model = fbxShapedModel();
    model.skins[0].inverseBindMatrices.clear();
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    CHECK(!result.ok);
    CHECK(result.joints.empty());
    CHECK(result.error.find("Structure") != std::string::npos);

    // A mismatched count is refused too -- it is the same defect one step less obvious.
    ImportedModel shortModel = fbxShapedModel();
    shortModel.skins[0].inverseBindMatrices.pop_back();
    const SkeletonSourceResult shortResult = skeletonCookJoints(shortModel, 0);
    CHECK(!shortResult.ok);
    CHECK(!shortResult.error.empty());
}

TEST_CASE("skeleton_cook_source: an out-of-range skin index names the real count (KS5)") {
    const ImportedModel model = fbxShapedModel();
    const SkeletonSourceResult result = skeletonCookJoints(model, 3);
    CHECK(!result.ok);
    CHECK(result.joints.empty());
    CHECK(result.error.find("has 1 skin(s)") != std::string::npos);
    CHECK(result.error.find("index 3") != std::string::npos);
}

TEST_CASE("skeleton_cook_source: a model with no skins says so (KS6)") {
    ImportedModel model;
    model.nodes.push_back(node(10, INVALID_SUBASSET));
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    CHECK(!result.ok);
    CHECK(result.joints.empty());
    CHECK(result.error.find("has 0 skin(s)") != std::string::npos);
}

TEST_CASE("skeleton_cook_source: an out-of-range vertex joint index WARNs, never demotes (KS7)") {
    // A four-joint skin and one vertex bound to joint 7. The artifact pair would be memory-safe and
    // visually wrong, so the author gets the message at cook time -- and the vertex data is not
    // touched, because this adapter modifies nothing at all.
    ImportedModel model = fbxShapedModel();
    ImportedMesh mesh;
    mesh.localId = 0;
    ImportedPrimitive primitive;
    primitive.attributes = VertexAttribute::Position | VertexAttribute::Joints0 | VertexAttribute::Weights0;
    primitive.positions = {Vec3{}, Vec3{}};
    primitive.joints = {std::array<std::uint16_t, 4>{0, 1, 0, 0}, std::array<std::uint16_t, 4>{7, 0, 0, 0}};
    primitive.weights = {Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{1.0F, 0.0F, 0.0F, 0.0F}};
    primitive.indices = {0, 1, 0};
    mesh.primitives.push_back(primitive);
    model.meshes.push_back(mesh);
    // The node that USES the skin is what makes the mesh reachable from it.
    ImportedNode user = node(500, INVALID_SUBASSET);
    user.meshIndex = 0;
    user.skinIndex = 0;
    model.nodes.push_back(user);

    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    REQUIRE(result.ok);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("mesh 0") != std::string::npos);
    CHECK(result.warnings[0].find("primitive 0") != std::string::npos);
    CHECK(result.warnings[0].find("joint index 7") != std::string::npos);
    // The vertex data is untouched: the adapter took a const model and returns joints, never geometry.
    CHECK(model.meshes[0].primitives[0].joints[1][0] == 7);
    // And a mesh whose node does NOT use this skin is not scanned at all.
    ImportedModel unrelated = model;
    unrelated.nodes.back().skinIndex = INVALID_SUBASSET;
    CHECK(skeletonCookJoints(unrelated, 0).warnings.empty());
}

TEST_CASE("skeleton_cook_source: a hostile parent cycle is an error, never a hang (KS8)") {
    // 10 -> 20 -> 10. The closure walk is iterative and depth-capped, so this returns; a recursive or
    // uncapped walk would spin until the ctest timeout, which is also a red but a much worse one.
    ImportedModel model;
    model.nodes.push_back(node(10, 20));
    model.nodes.push_back(node(20, 10));
    model.skins.push_back(skin({10}));
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    CHECK(!result.ok);
    CHECK(result.joints.empty());
    CHECK(!result.error.empty());

    // A node that is its own parent is the same defect with one node.
    ImportedModel self;
    self.nodes.push_back(node(10, 10));
    self.skins.push_back(skin({10}));
    const SkeletonSourceResult selfResult = skeletonCookJoints(self, 0);
    CHECK(!selfResult.ok);
    CHECK(!selfResult.error.empty());
}

TEST_CASE("skeleton_cook_source: a chain deeper than MAX_NODE_DEPTH is refused (KS9)") {
    // MAX_NODE_DEPTH + 2 nodes, so the leaf carries MAX_NODE_DEPTH + 1 ancestors -- one past the cap
    // the importer itself uses.
    ImportedModel model;
    const std::uint32_t count = MAX_NODE_DEPTH + 2;
    for (std::uint32_t i = 0; i < count; ++i) {
        model.nodes.push_back(node(1000 + i, i == 0 ? INVALID_SUBASSET : 1000 + (i - 1)));
    }
    model.skins.push_back(skin({1000 + count - 1}));
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    CHECK(!result.ok);
    CHECK(result.error.find(std::to_string(MAX_NODE_DEPTH)) != std::string::npos);

    // One node shallower is legal and closes over the whole chain.
    ImportedModel legal;
    for (std::uint32_t i = 0; i < MAX_NODE_DEPTH; ++i) {
        legal.nodes.push_back(node(1000 + i, i == 0 ? INVALID_SUBASSET : 1000 + (i - 1)));
    }
    legal.skins.push_back(skin({1000 + MAX_NODE_DEPTH - 1}));
    const SkeletonSourceResult legalResult = skeletonCookJoints(legal, 0);
    REQUIRE(legalResult.ok);
    CHECK(legalResult.joints.size() == MAX_NODE_DEPTH);
}

TEST_CASE("skeleton_cook_source: cookImportedSkeleton runs adapter, cook and parse end to end (KS10)") {
    const ImportedModel model = fbxShapedModel();
    const Guid guid{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    const SkeletonCookResult cooked = cookImportedSkeleton(model, 0, guid);
    REQUIRE((cooked.status == SkeletonCookStatus::Ok));
    CHECK(cooked.message.empty());
    CHECK(cooked.warnings.empty());

    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    CHECK(parsed.skeleton.sourceGuid == guid);
    CHECK(parsed.skeleton.sourceSkinIndex == 0);
    CHECK(parsed.skeleton.paletteJointCount == 4);
    REQUIRE(parsed.skeleton.joints.size() == 4);
    // The chain's canonical order is its own depth order here, and the slots are the SOURCE order.
    const std::array<std::uint32_t, 4> expectedIds = {100, 205, 300, 407};
    CHECK(expectedIds.size() == 4);  // literal row count
    for (std::size_t i = 0; i < expectedIds.size(); ++i) {
        CHECK(parsed.skeleton.joints[i].sourceNodeLocalId == expectedIds[i]);
        CHECK(parsed.skeleton.joints[i].paletteSlot == static_cast<std::uint32_t>(i));
        CHECK(parsed.skeleton.joints[i].parent ==
              (i == 0 ? COOKED_SKELETON_INVALID_INDEX : static_cast<std::uint32_t>(i - 1)));
    }
    // The IBMs survive bit for bit from the import to the parsed artifact.
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(sameMatrixBits(parsed.skeleton.joints[i].inverseBind, model.skins[0].inverseBindMatrices[i]));
    }
    // Composing the two halves by hand produces the identical bytes -- the convenience is not a
    // second policy.
    const SkeletonSourceResult source = skeletonCookJoints(model, 0);
    REQUIRE(source.ok);
    engine::assets::SkeletonCookInput input;
    input.sourceGuid = guid;
    input.sourceSkinIndex = 0;
    input.joints = source.joints;
    CHECK(engine::assets::cookSkeleton(input).bytes == cooked.bytes);
}

TEST_CASE("skeleton_cook_source: a second skin is selectable and the model's total is WARNed (KS11)") {
    ImportedModel model;
    model.nodes.push_back(node(10, INVALID_SUBASSET));
    model.nodes.push_back(node(20, INVALID_SUBASSET));
    model.nodes.push_back(node(30, INVALID_SUBASSET));
    model.skins.push_back(skin({10}));
    model.skins.push_back(skin({20, 30}));

    const SkeletonSourceResult second = skeletonCookJoints(model, 1);
    REQUIRE(second.ok);
    REQUIRE(second.joints.size() == 2);
    CHECK(second.joints[0].localId == 20);  // skin 1's joints, not skin 0's
    CHECK(second.joints[1].localId == 30);
    REQUIRE(second.warnings.size() == 1);
    CHECK(second.warnings[0].find("2 skins") != std::string::npos);
    CHECK(second.warnings[0].find("skin 1") != std::string::npos);

    // Skin 0 selects skin 0 and warns the same way -- the index is forwarded, not ignored.
    const SkeletonSourceResult first = skeletonCookJoints(model, 0);
    REQUIRE(first.ok);
    REQUIRE(first.joints.size() == 1);
    CHECK(first.joints[0].localId == 10);
    REQUIRE(first.warnings.size() == 1);
    CHECK(first.warnings[0].find("skin 0") != std::string::npos);
    // The warning rides out on the cook result, which is how the CLI reports it.
    const SkeletonCookResult cooked = cookImportedSkeleton(model, 1, Guid{});
    REQUIRE((cooked.status == SkeletonCookStatus::Ok));
    REQUIRE(cooked.warnings.size() == 1);
    CHECK(cooked.warnings[0].find("2 skins") != std::string::npos);
    // And the SELECTED skin index reaches the bytes.
    const CookedSkeletonParseResult parsed = parseCookedSkeleton(std::span<const std::byte>(cooked.bytes));
    REQUIRE((parsed.status == CookedSkeletonStatus::Ok));
    CHECK(parsed.skeleton.sourceSkinIndex == 1);
}

TEST_CASE("skeleton_cook_source: a skin listing one node twice is refused by the cook (KS12)") {
    // The adapter passes the duplicate THROUGH deliberately -- refusing in one place keeps the two
    // layers telling the same story -- and the cook is where a duplicate localId is a refusal. This
    // case pins the composition, so the behaviour is a decision rather than an accident.
    ImportedModel model;
    model.nodes.push_back(node(10, INVALID_SUBASSET));
    model.nodes.push_back(node(20, 10));
    model.skins.push_back(skin({10, 20, 10}));

    const SkeletonSourceResult source = skeletonCookJoints(model, 0);
    REQUIRE(source.ok);
    CHECK(source.joints.size() == 3);  // the duplicate is still here

    const SkeletonCookResult cooked = cookImportedSkeleton(model, 0, Guid{});
    CHECK((cooked.status == SkeletonCookStatus::Invalid));
    CHECK(cooked.bytes.empty());
    CHECK(cooked.message.find("10") != std::string::npos);
}

TEST_CASE("skeleton_cook_source: the advisory is capped at MAX_IMPORT_WARNINGS (KS13)") {
    // Twenty-five bad vertices, twenty warnings: the same capped-list posture the importer uses, so a
    // pathological model produces a report rather than a wall of text.
    ImportedModel model = fbxShapedModel();
    ImportedMesh mesh;
    ImportedPrimitive primitive;
    primitive.attributes = VertexAttribute::Position | VertexAttribute::Joints0 | VertexAttribute::Weights0;
    constexpr std::size_t BAD_VERTICES = 25;
    for (std::size_t v = 0; v < BAD_VERTICES; ++v) {
        primitive.positions.push_back(Vec3{});
        primitive.joints.push_back(std::array<std::uint16_t, 4>{99, 0, 0, 0});
        primitive.weights.push_back(Vec4{1.0F, 0.0F, 0.0F, 0.0F});
        primitive.indices.push_back(static_cast<std::uint32_t>(v));
    }
    mesh.primitives.push_back(primitive);
    model.meshes.push_back(mesh);
    ImportedNode user = node(500, INVALID_SUBASSET);
    user.meshIndex = 0;
    user.skinIndex = 0;
    model.nodes.push_back(user);

    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    REQUIRE(result.ok);
    CHECK(result.warnings.size() == MAX_IMPORT_WARNINGS);
}

TEST_CASE("skeleton_cook_source: a skin joint naming no node is an error (KS14)") {
    ImportedModel model;
    model.nodes.push_back(node(10, INVALID_SUBASSET));
    model.skins.push_back(skin({10, 99}));  // 99 exists nowhere
    const SkeletonSourceResult result = skeletonCookJoints(model, 0);
    CHECK(!result.ok);
    CHECK(result.joints.empty());
    CHECK(result.error.find("99") != std::string::npos);

    // The same failure one level up: a node whose PARENT names nothing.
    ImportedModel orphan;
    orphan.nodes.push_back(node(10, 77));
    orphan.skins.push_back(skin({10}));
    const SkeletonSourceResult orphanResult = skeletonCookJoints(orphan, 0);
    CHECK(!orphanResult.ok);
    CHECK(orphanResult.error.find("77") != std::string::npos);
}
