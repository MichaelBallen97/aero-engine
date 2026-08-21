// tests/editor/instantiate_plan_test.cpp -- task 3.1.5, Step 8: the ImportedModel -> entity-subtree
// planner (PL1-PL20). A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp --
// do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (the model_import_test.cpp precedent): instantiate_plan.hpp depends only on
// aero/core/{guid,math}.hpp and aero/editor/model_import.hpp, neither gated. Every case here must be
// PRESENT and PASSING in all three build configurations. Tier-0: no GPU, no window, no ImGui context.
// PL12 drives the three real Structure-depth fixtures through importModel and reaches them via
// AERO_ASSET_FIXTURES_DIR -- a path, not a flag, so a missing fixture is a REQUIRE failure.
//
// THE POINT OF HALF THIS FILE IS THE localId/position ASYMMETRY. Every hand-built model below uses
// NON-DENSE, NON-POSITIONAL localIds wherever the property under test allows it, because a fixture
// whose localIds happen to equal its positions cannot tell the map from an index (PL6/PL7 are the two
// cases written specifically so S5 and S6 redden).
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md). Enum CHECKs use the
// DOUBLE-PAREN posture -- CHECK((a == b)) -- and no toString overload is added anywhere.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/instantiate_plan.hpp>
#include <aero/editor/model_import.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::Guid;
using engine::Quat;
using engine::Vec3;
using engine::editor::buildInstantiatePlan;
using engine::editor::ImportDepth;
using engine::editor::ImportedModel;
using engine::editor::ImportedNode;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::InstantiatePlan;
using engine::editor::InstantiatePlanRefusal;
using engine::editor::instantiatePlanRefusalLabel;
using engine::editor::INVALID_SUBASSET;
using engine::editor::MAX_NODE_DEPTH;

namespace {

constexpr Guid ASSET_GUID{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT(*-reinterpret-cast)
}

// A node with a DELIBERATELY non-positional localId. Every helper below takes ids, never indices.
[[nodiscard]] ImportedNode node(std::string name, std::uint32_t localId, std::uint32_t parent,
                                std::vector<std::uint32_t> children, std::uint32_t meshIndex = INVALID_SUBASSET) {
    ImportedNode out;
    out.name = std::move(name);
    out.localId = localId;
    out.parent = parent;
    out.children = std::move(children);
    out.meshIndex = meshIndex;
    return out;
}

[[nodiscard]] std::size_t slotOfName(const InstantiatePlan& plan, std::string_view name) {
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        if (plan.nodes[i].name == name) {
            return i;
        }
    }
    return plan.nodes.size();  // "not present"; every caller REQUIREs against plan.nodes.size()
}

[[nodiscard]] bool anyWarningContains(const InstantiatePlan& plan, std::string_view needle) {
    return std::ranges::any_of(plan.warnings,
                               [needle](const std::string& w) { return w.find(needle) != std::string::npos; });
}

// The tree the FBX shape demands: ids 900, 17, 4321 -- sparse, unordered, and every one of them past
// nodes.size(). nodes[localId] is an out-of-bounds read on all three, which is what makes S5 an ASan
// report and not merely a wrong answer.
[[nodiscard]] ImportedModel sparseIdModel() {
    ImportedModel model;
    model.nodes.push_back(node("Root", 900, INVALID_SUBASSET, {17, 4321}));
    model.nodes.push_back(node("ChildA", 17, 900, {}, 0));
    model.nodes.push_back(node("ChildB", 4321, 900, {}, 1));
    model.roots = {900};
    model.meshes.resize(2);
    return model;
}

}  // namespace

TEST_CASE("instantiate_plan: a single-root two-node model plans three slots, root first (PL1)") {
    ImportedModel model;
    model.nodes.push_back(node("Root", 40, INVALID_SUBASSET, {41}));
    model.nodes.push_back(node("Leaf", 41, 40, {}));
    model.roots = {40};

    const InstantiatePlan plan = buildInstantiatePlan(model, "chair", ASSET_GUID);
    REQUIRE(plan.ok);
    CHECK((plan.refusal == InstantiatePlanRefusal::None));
    CHECK(plan.error.empty());
    REQUIRE(plan.nodes.size() == 3);
    CHECK(plan.nodes[0].name == "chair");
    CHECK(plan.nodes[1].name == "Root");
    CHECK(plan.nodes[2].name == "Leaf");
    CHECK(plan.warnings.empty());
}

TEST_CASE("instantiate_plan: the synthetic root is named from assetStem and carries no mesh (PL2)") {
    ImportedModel model;
    model.nodes.push_back(node("Mesh", 7, INVALID_SUBASSET, {}, 0));
    model.roots = {7};
    model.meshes.resize(1);

    const InstantiatePlan plan = buildInstantiatePlan(model, "sofa", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 2);
    CHECK(plan.nodes[0].name == "sofa");
    CHECK(plan.nodes[0].parentSlot == 0);  // the ONE slot that points at itself
    CHECK_FALSE(plan.nodes[0].mesh.valid());
    CHECK(plan.nodes[0].translation == Vec3{});
    CHECK(plan.nodes[0].scale == Vec3::one());
    CHECK(plan.nodes[0].rotation == Quat::identity());
    // ... while the real node under it DOES carry the reference
    CHECK(plan.nodes[1].mesh == ASSET_GUID);
}

TEST_CASE("instantiate_plan: every source root lands under slot 0 (PL3, S7)") {
    ImportedModel model;
    model.nodes.push_back(node("A", 5, INVALID_SUBASSET, {}));
    model.nodes.push_back(node("B", 9, INVALID_SUBASSET, {}));
    model.nodes.push_back(node("C", 3, INVALID_SUBASSET, {}));
    model.roots = {5, 9, 3};

    const InstantiatePlan plan = buildInstantiatePlan(model, "trio", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 4);
    CHECK(plan.nodes[0].name == "trio");
    // SOURCE ORDER is preserved -- the BFS seeds the queue from ImportedModel::roots as it stands.
    CHECK(plan.nodes[1].name == "A");
    CHECK(plan.nodes[2].name == "B");
    CHECK(plan.nodes[3].name == "C");
    for (std::size_t i = 1; i < plan.nodes.size(); ++i) {
        CHECK(plan.nodes[i].parentSlot == 0);
    }
}

TEST_CASE("instantiate_plan: an OBJ-shaped N-roots-one-mesh-each model plans N+1 slots (PL4)") {
    constexpr std::uint32_t N = 6;
    ImportedModel model;
    for (std::uint32_t i = 0; i < N; ++i) {
        // ids deliberately descending and offset, so they are neither dense nor positional
        model.nodes.push_back(node("group" + std::to_string(i), 500 - (i * 7), INVALID_SUBASSET, {}, i));
        model.roots.push_back(500 - (i * 7));
    }
    model.meshes.resize(N);

    const InstantiatePlan plan = buildInstantiatePlan(model, "scene", ASSET_GUID);
    REQUIRE(plan.ok);
    CHECK(plan.nodes.size() == N + 1);
    for (std::uint32_t i = 0; i < N; ++i) {
        CHECK(plan.nodes[i + 1].meshIndex == i);
        CHECK(plan.nodes[i + 1].mesh == ASSET_GUID);
    }
    CHECK(plan.warnings.empty());
}

TEST_CASE("instantiate_plan: TRS is copied bit for bit (PL5, S8)") {
    // A non-identity rotation AND a non-uniform scale: identity-emitting sabotage cannot hide behind
    // either one alone.
    const Vec3 translation{1.5F, -2.25F, 0.125F};
    const Quat rotation = engine::normalize(Quat{0.1F, 0.2F, 0.3F, 0.9F});
    const Vec3 scale{2.0F, 0.5F, 3.0F};

    ImportedModel model;
    ImportedNode only = node("Posed", 88, INVALID_SUBASSET, {});
    only.translation = translation;
    only.rotation = rotation;
    only.scale = scale;
    model.nodes.push_back(std::move(only));
    model.roots = {88};

    const InstantiatePlan plan = buildInstantiatePlan(model, "posed", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 2);
    CHECK(plan.nodes[1].translation == translation);
    CHECK(plan.nodes[1].rotation == rotation);
    CHECK(plan.nodes[1].scale == scale);
    // and the SYNTHETIC root keeps identity regardless -- the placement belongs to the drop, not here
    CHECK(plan.nodes[0].translation == Vec3{});
    CHECK(plan.nodes[0].scale == Vec3::one());
}

TEST_CASE("instantiate_plan: NON-DENSE, NON-POSITIONAL localIds plan correctly (PL6, S5)") {
    // THE CASE S5 REDDENS. Every id here (900, 17, 4321) is past nodes.size() == 3, so a
    // `nodes[localId]` walk is an out-of-bounds read -- an ASan heap overflow, not a wrong answer.
    const ImportedModel model = sparseIdModel();
    const InstantiatePlan plan = buildInstantiatePlan(model, "rig", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 4);
    CHECK(plan.nodes[0].name == "rig");
    CHECK(plan.nodes[1].name == "Root");
    CHECK(plan.nodes[1].parentSlot == 0);

    const std::size_t childA = slotOfName(plan, "ChildA");
    const std::size_t childB = slotOfName(plan, "ChildB");
    REQUIRE(childA < plan.nodes.size());
    REQUIRE(childB < plan.nodes.size());
    CHECK(plan.nodes[childA].parentSlot == 1);
    CHECK(plan.nodes[childB].parentSlot == 1);
    CHECK(plan.warnings.empty());
}

TEST_CASE("instantiate_plan: meshIndex is a POSITION and never crosses the id map (PL7, S6)") {
    // THE CASE S6 REDDENS. Each node's localId and its meshIndex are deliberately DIFFERENT numbers,
    // and each localId is ALSO a legal meshes[] position -- so resolving meshIndex through the id map
    // produces a plausible wrong answer rather than a crash, which is exactly the FBX failure shape.
    ImportedModel model;
    model.nodes.push_back(node("First", 1, INVALID_SUBASSET, {}, 2));
    model.nodes.push_back(node("Second", 2, INVALID_SUBASSET, {}, 0));
    model.nodes.push_back(node("Third", 0, INVALID_SUBASSET, {}, 1));
    model.roots = {1, 2, 0};
    model.meshes.resize(3);

    const InstantiatePlan plan = buildInstantiatePlan(model, "swap", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 4);
    // Asserted against LITERALS: the node named "First" carries mesh POSITION 2, not the position its
    // own localId (1) would resolve to, and not the map's answer for 2 (which is position 1).
    CHECK(plan.nodes[slotOfName(plan, "First")].meshIndex == 2);
    CHECK(plan.nodes[slotOfName(plan, "Second")].meshIndex == 0);
    CHECK(plan.nodes[slotOfName(plan, "Third")].meshIndex == 1);
}

TEST_CASE("instantiate_plan: a node with no mesh gets a NIL reference (PL8)") {
    ImportedModel model;
    model.nodes.push_back(node("Empty", 12, INVALID_SUBASSET, {}));  // meshIndex defaults to INVALID_SUBASSET
    model.roots = {12};
    model.meshes.resize(4);

    const InstantiatePlan plan = buildInstantiatePlan(model, "stem", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 2);
    CHECK_FALSE(plan.nodes[1].mesh.valid());
    // The plan-side meshIndex default is 0 and is a PLACEHOLDER: ImportedNode::meshIndex defaults to
    // INVALID_SUBASSET, never to 0, so it is only ever consulted when `mesh` is valid.
    CHECK(plan.nodes[1].meshIndex == 0);
}

TEST_CASE("instantiate_plan: a node with a valid meshIndex gets the asset guid (PL9)") {
    ImportedModel model;
    model.nodes.push_back(node("Body", 12, INVALID_SUBASSET, {}, 3));
    model.roots = {12};
    model.meshes.resize(4);

    const InstantiatePlan plan = buildInstantiatePlan(model, "stem", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 2);
    CHECK(plan.nodes[1].mesh == ASSET_GUID);
    CHECK(plan.nodes[1].meshIndex == 3);
}

TEST_CASE("instantiate_plan: an out-of-range meshIndex STILL plans, with a warning (PL10)") {
    ImportedModel model;
    model.nodes.push_back(node("Broken", 12, INVALID_SUBASSET, {}, 9));
    model.roots = {12};
    model.meshes.resize(2);  // 9 >= 2

    const InstantiatePlan plan = buildInstantiatePlan(model, "stem", ASSET_GUID);
    REQUIRE(plan.ok);  // the reference degrades at draw time, it does not remove the node
    REQUIRE(plan.nodes.size() == 2);
    CHECK(plan.nodes[1].mesh == ASSET_GUID);
    CHECK(plan.nodes[1].meshIndex == 9);
    REQUIRE(plan.warnings.size() == 1);
    CHECK(anyWarningContains(plan, "Broken"));
    CHECK(anyWarningContains(plan, "references mesh 9"));
}

TEST_CASE("instantiate_plan: parents always precede children, over a five-level tree (PL11)") {
    ImportedModel model;
    // Five levels, ids descending so position order and depth order disagree everywhere.
    model.nodes.push_back(node("L4", 100, 200, {}));
    model.nodes.push_back(node("L3", 200, 300, {100}));
    model.nodes.push_back(node("L2", 300, 400, {200}));
    model.nodes.push_back(node("L1", 400, 500, {300}));
    model.nodes.push_back(node("L0", 500, INVALID_SUBASSET, {400}));
    model.roots = {500};

    const InstantiatePlan plan = buildInstantiatePlan(model, "deep", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 6);
    for (std::size_t i = 1; i < plan.nodes.size(); ++i) {
        CAPTURE(i);
        CHECK(plan.nodes[i].parentSlot < i);  // THE property InstantiateAssetCommand's one pass rests on
    }
    CHECK(plan.nodes[1].name == "L0");
    CHECK(plan.nodes[5].name == "L4");
}

TEST_CASE("instantiate_plan: an empty node list refuses with NoNodes (PL12, S37)") {
    SUBCASE("a hand-built empty model") {
        const InstantiatePlan plan = buildInstantiatePlan(ImportedModel{}, "empty", ASSET_GUID);
        CHECK_FALSE(plan.ok);
        CHECK((plan.refusal == InstantiatePlanRefusal::NoNodes));
        CHECK(plan.error == "this import produced no node hierarchy");
        CHECK(plan.nodes.empty());
    }

    // S37's WITNESS, and the reason 0.17 amends the spec's D5. The three formats the predicate was
    // written for produce zero nodes AND ZERO MESHES at Structure depth, so the spec's original
    // `nodes.empty() && !meshes.empty()` would never fire for any of them. Measured here through the
    // real importers rather than asserted.
    SUBCASE("the three measured Structure-depth formats: .obj, .ply, .stl") {
        struct Case {
            std::string_view fileName;
            std::string_view fixture;
        };
        const std::array<Case, 3> cases{{
            {"cube.obj", AERO_ASSET_FIXTURES_DIR "/cube.obj"},
            {"cube.ply", AERO_ASSET_FIXTURES_DIR "/cube.ply"},
            {"cube.stl", AERO_ASSET_FIXTURES_DIR "/cube.stl"},
        }};
        for (const Case& one : cases) {
            CAPTURE(one.fileName);
            const scene_golden::FileBytes bytes = scene_golden::readBytes(one.fixture);
            REQUIRE(bytes.ok);
            const ImportResult result =
                importModel(one.fileName, "", asBytes(bytes.text), ImportSettings{}, ImportDepth::Structure, {});
            // The measurement itself, recorded rather than assumed: BOTH lists are empty here.
            CHECK(result.model.nodes.empty());
            CHECK(result.model.meshes.empty());
            const InstantiatePlan plan = buildInstantiatePlan(result.model, "cube", ASSET_GUID);
            CHECK_FALSE(plan.ok);
            CHECK((plan.refusal == InstantiatePlanRefusal::NoNodes));
        }
    }

    SUBCASE("a .gltf at the SAME depth plans successfully -- the discriminating half") {
        const scene_golden::FileBytes bytes = scene_golden::readBytes(AERO_ASSET_FIXTURES_DIR "/hierarchy.gltf");
        REQUIRE(bytes.ok);
        const ImportResult result =
            importModel("hierarchy.gltf", "", asBytes(bytes.text), ImportSettings{}, ImportDepth::Structure, {});
        REQUIRE_FALSE(result.model.nodes.empty());
        const InstantiatePlan plan = buildInstantiatePlan(result.model, "hierarchy", ASSET_GUID);
        CHECK(plan.ok);
        CHECK(plan.nodes.size() == result.model.nodes.size() + 1);
    }

    SUBCASE("and so does a .dae, whose Structure pass DOES produce nodes") {
        const scene_golden::FileBytes bytes = scene_golden::readBytes(AERO_ASSET_FIXTURES_DIR "/cube.dae");
        REQUIRE(bytes.ok);
        const ImportResult result =
            importModel("cube.dae", "", asBytes(bytes.text), ImportSettings{}, ImportDepth::Structure, {});
        REQUIRE_FALSE(result.model.nodes.empty());
        const InstantiatePlan plan = buildInstantiatePlan(result.model, "cube", ASSET_GUID);
        CHECK(plan.ok);
    }
}

TEST_CASE("instantiate_plan: a parent cycle RETURNS with Cycle, and never hangs (PL13)") {
    // Two nodes each naming the other as parent, and NEITHER in roots -- which is what a cycle in a
    // source file actually looks like, since ImportedModel::roots holds only parentless nodes. The BFS
    // therefore emits nothing, and the post-pass's bounded parent walk is what names it.
    ImportedModel model;
    model.nodes.push_back(node("A", 11, 22, {22}));
    model.nodes.push_back(node("B", 22, 11, {11}));
    model.roots = {};

    const InstantiatePlan plan = buildInstantiatePlan(model, "loop", ASSET_GUID);
    CHECK_FALSE(plan.ok);
    CHECK((plan.refusal == InstantiatePlanRefusal::Cycle));
    CHECK(plan.error == "the node hierarchy contains a cycle");
    CHECK(plan.nodes.empty());

    SUBCASE("a self-parented node is the same refusal") {
        ImportedModel self;
        self.nodes.push_back(node("Self", 5, 5, {5}));
        self.roots = {};
        const InstantiatePlan selfPlan = buildInstantiatePlan(self, "self", ASSET_GUID);
        CHECK_FALSE(selfPlan.ok);
        CHECK((selfPlan.refusal == InstantiatePlanRefusal::Cycle));
    }

    SUBCASE("a CHILD-link loop reachable from a root terminates instead, with a revisit warning") {
        // The other half of "worklist exhaustion is the detector": a node already emitted is skipped
        // rather than re-walked, so the BFS cannot loop even when the links do.
        ImportedModel reachable;
        reachable.nodes.push_back(node("A", 11, INVALID_SUBASSET, {22}));
        reachable.nodes.push_back(node("B", 22, 11, {11}));
        reachable.roots = {11};
        const InstantiatePlan reachablePlan = buildInstantiatePlan(reachable, "loop", ASSET_GUID);
        CHECK(reachablePlan.ok);
        CHECK(reachablePlan.nodes.size() == 3);
        CHECK(anyWarningContains(reachablePlan, "revisits source id 11"));
    }
}

TEST_CASE("instantiate_plan: a chain deeper than MAX_NODE_DEPTH refuses with TooDeep (PL14, S9)") {
    const std::uint32_t depth = MAX_NODE_DEPTH + 40U;
    ImportedModel model;
    model.nodes.reserve(depth);
    for (std::uint32_t i = 0; i < depth; ++i) {
        const std::uint32_t id = 1000U + i;
        const std::uint32_t parent = (i == 0) ? INVALID_SUBASSET : (1000U + i - 1U);
        std::vector<std::uint32_t> children;
        if (i + 1U < depth) {
            children.push_back(1000U + i + 1U);
        }
        model.nodes.push_back(node("n" + std::to_string(i), id, parent, std::move(children)));
    }
    model.roots = {1000U};

    const InstantiatePlan plan = buildInstantiatePlan(model, "chain", ASSET_GUID);
    CHECK_FALSE(plan.ok);
    CHECK((plan.refusal == InstantiatePlanRefusal::TooDeep));
    CHECK(plan.error == "the node hierarchy is deeper than 256 levels");
    CHECK(plan.nodes.empty());

    SUBCASE("a chain exactly AT the bound is accepted") {
        ImportedModel atBound;
        atBound.nodes.reserve(MAX_NODE_DEPTH);
        for (std::uint32_t i = 0; i < MAX_NODE_DEPTH; ++i) {
            const std::uint32_t id = 1000U + i;
            const std::uint32_t parent = (i == 0) ? INVALID_SUBASSET : (1000U + i - 1U);
            std::vector<std::uint32_t> children;
            if (i + 1U < MAX_NODE_DEPTH) {
                children.push_back(1000U + i + 1U);
            }
            atBound.nodes.push_back(node("n" + std::to_string(i), id, parent, std::move(children)));
        }
        atBound.roots = {1000U};
        const InstantiatePlan ok = buildInstantiatePlan(atBound, "chain", ASSET_GUID);
        CHECK(ok.ok);
        CHECK(ok.nodes.size() == MAX_NODE_DEPTH + 1U);
    }
}

TEST_CASE("instantiate_plan: an orphan is emitted under slot 0 with a warning (PL15)") {
    ImportedModel model;
    model.nodes.push_back(node("Rooted", 10, INVALID_SUBASSET, {}));
    model.nodes.push_back(node("Orphan", 20, INVALID_SUBASSET, {21}));  // parentless, NOT in roots
    model.nodes.push_back(node("OrphanChild", 21, 20, {}));
    model.roots = {10};

    const InstantiatePlan plan = buildInstantiatePlan(model, "stray", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 4);
    const std::size_t orphan = slotOfName(plan, "Orphan");
    const std::size_t orphanChild = slotOfName(plan, "OrphanChild");
    REQUIRE(orphan < plan.nodes.size());
    REQUIRE(orphanChild < plan.nodes.size());
    CHECK(plan.nodes[orphan].parentSlot == 0);            // rescued at the top level
    CHECK(plan.nodes[orphanChild].parentSlot == orphan);  // and its SUBTREE came with it, whole
    CHECK(orphanChild > orphan);                          // parents still precede children
    CHECK(anyWarningContains(plan, "not reachable from any root"));
}

TEST_CASE("instantiate_plan: an unresolvable child id warns and skips the subtree (PL16)") {
    ImportedModel model;
    model.nodes.push_back(node("Root", 30, INVALID_SUBASSET, {31, 999}));
    model.nodes.push_back(node("Real", 31, 30, {}));
    // 999 names nothing. Its (non-existent) subtree goes with it: nothing was pushed for it.
    model.roots = {30};

    const InstantiatePlan plan = buildInstantiatePlan(model, "gap", ASSET_GUID);
    REQUIRE(plan.ok);
    CHECK(plan.nodes.size() == 3);
    CHECK(anyWarningContains(plan, "a node references source id 999, which this model does not contain"));
}

TEST_CASE("instantiate_plan: duplicate localIds warn and the second is skipped entirely (PL17)") {
    ImportedModel model;
    model.nodes.push_back(node("Root", 50, INVALID_SUBASSET, {60}));
    model.nodes.push_back(node("First", 60, 50, {}));
    model.nodes.push_back(node("SecondWithSameId", 60, 50, {}));  // ambiguous key
    model.roots = {50};

    const InstantiatePlan plan = buildInstantiatePlan(model, "dupe", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 3);  // synthetic root + Root + the FIRST claimant only
    CHECK(slotOfName(plan, "First") < plan.nodes.size());
    CHECK(slotOfName(plan, "SecondWithSameId") == plan.nodes.size());  // never emitted, not even as an orphan
    CHECK(anyWarningContains(plan, "two nodes share source id 60; the second was skipped"));
}

TEST_CASE("instantiate_plan: names are copied verbatim, '' and '##' included (PL18)") {
    ImportedModel model;
    model.nodes.push_back(node("", 70, INVALID_SUBASSET, {71, 72}));
    model.nodes.push_back(node("Mesh##hidden", 71, 70, {}));
    model.nodes.push_back(node("  spaced  ", 72, 70, {}));
    model.roots = {70};

    const InstantiatePlan plan = buildInstantiatePlan(model, "", ASSET_GUID);
    REQUIRE(plan.ok);
    REQUIRE(plan.nodes.size() == 4);
    CHECK(plan.nodes[0].name.empty());  // an empty assetStem stays empty -- names are never invented
    CHECK(plan.nodes[1].name.empty());
    CHECK(plan.nodes[2].name == "Mesh##hidden");
    CHECK(plan.nodes[3].name == "  spaced  ");
}

TEST_CASE("instantiate_plan: a 1000-node model plans in one bounded pass (PL19)") {
    constexpr std::uint32_t COUNT = 1000;
    ImportedModel model;
    model.nodes.reserve(COUNT);
    // One root with 999 direct children, all with sparse ids: wide rather than deep, so nothing here
    // is bounded by MAX_NODE_DEPTH and the worklist itself is what is being exercised.
    std::vector<std::uint32_t> children;
    children.reserve(COUNT - 1U);
    for (std::uint32_t i = 1; i < COUNT; ++i) {
        children.push_back(90000U - (i * 3U));
    }
    model.nodes.push_back(node("Root", 1U, INVALID_SUBASSET, children));
    for (std::uint32_t i = 1; i < COUNT; ++i) {
        model.nodes.push_back(node("n" + std::to_string(i), 90000U - (i * 3U), 1U, {}));
    }
    model.roots = {1U};

    const InstantiatePlan plan = buildInstantiatePlan(model, "wide", ASSET_GUID);
    REQUIRE(plan.ok);
    CHECK(plan.nodes.size() == COUNT + 1U);
    CHECK(plan.warnings.empty());
    for (std::size_t i = 2; i < plan.nodes.size(); ++i) {
        CHECK(plan.nodes[i].parentSlot == 1);
    }
}

TEST_CASE("instantiate_plan: the plan is DETERMINISTIC across two calls (PL20)") {
    const ImportedModel model = sparseIdModel();
    const InstantiatePlan first = buildInstantiatePlan(model, "rig", ASSET_GUID);
    const InstantiatePlan second = buildInstantiatePlan(model, "rig", ASSET_GUID);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.nodes.size() == second.nodes.size());
    for (std::size_t i = 0; i < first.nodes.size(); ++i) {
        CAPTURE(i);
        CHECK(first.nodes[i].name == second.nodes[i].name);
        CHECK(first.nodes[i].parentSlot == second.nodes[i].parentSlot);
        CHECK(first.nodes[i].mesh == second.nodes[i].mesh);
        CHECK(first.nodes[i].meshIndex == second.nodes[i].meshIndex);
        CHECK(first.nodes[i].translation == second.nodes[i].translation);
        CHECK(first.nodes[i].rotation == second.nodes[i].rotation);
        CHECK(first.nodes[i].scale == second.nodes[i].scale);
    }
    CHECK(first.warnings == second.warnings);

    SUBCASE("and so is a model whose every path produces warnings") {
        ImportedModel noisy;
        noisy.nodes.push_back(node("Root", 800, INVALID_SUBASSET, {801, 4242}));
        noisy.nodes.push_back(node("Dup", 801, 800, {}, 12));
        noisy.nodes.push_back(node("DupTwin", 801, 800, {}));
        noisy.nodes.push_back(node("Stray", 802, INVALID_SUBASSET, {}));
        noisy.roots = {800};
        noisy.meshes.resize(1);
        const InstantiatePlan a = buildInstantiatePlan(noisy, "noisy", ASSET_GUID);
        const InstantiatePlan b = buildInstantiatePlan(noisy, "noisy", ASSET_GUID);
        REQUIRE(a.ok);
        CHECK(a.warnings == b.warnings);
        CHECK(a.warnings.size() >= 4);  // duplicate id, missing child id, out-of-range mesh, orphan
    }
}

TEST_CASE("instantiate_plan: instantiatePlanRefusalLabel is total and injective (PL21)") {
    // Against LITERALS, never against each other.
    CHECK(instantiatePlanRefusalLabel(InstantiatePlanRefusal::None) == std::string_view("none"));
    CHECK(instantiatePlanRefusalLabel(InstantiatePlanRefusal::NoNodes) == std::string_view("no nodes"));
    CHECK(instantiatePlanRefusalLabel(InstantiatePlanRefusal::Cycle) == std::string_view("cycle"));
    CHECK(instantiatePlanRefusalLabel(InstantiatePlanRefusal::TooDeep) == std::string_view("too deep"));

    const std::array<InstantiatePlanRefusal, 4> all{InstantiatePlanRefusal::None, InstantiatePlanRefusal::NoNodes,
                                                    InstantiatePlanRefusal::Cycle, InstantiatePlanRefusal::TooDeep};
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK_FALSE(instantiatePlanRefusalLabel(all[i]).empty());
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            CHECK(instantiatePlanRefusalLabel(all[i]) != instantiatePlanRefusalLabel(all[j]));
        }
    }
}
