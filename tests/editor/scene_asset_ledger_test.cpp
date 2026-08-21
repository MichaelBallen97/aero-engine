// tests/editor/scene_asset_ledger_test.cpp -- task 3.1.5, Step 13: the scene-asset ledger
// (LG1-LG24). A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0, and with NO FIXTURE OF ANY KIND: the ledger is pure, so every case below is a
// hand-built vector of LedgerAssetFacts and a sequence of service() calls. No device, no window, no
// database, no disk.
//
// LG9 AND LG10 ARE ONE CASE ON PURPOSE, and the reason is 3.5.1's SN8 lesson restated: the property
// under test is "not in pass N, AND in pass N+1", and a pair of independent cases both PASS under a
// ledger that destroys in BOTH passes. The sequence case asserts the pass-N destroy list is EMPTY and
// the pass-N+1 list holds exactly the retired handles, with the pass boundary explicit.
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md). Enum CHECKs use the
// DOUBLE-PAREN posture -- CHECK((a == b)) -- and no toString overload is added anywhere.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/scene_asset_ledger.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/render/material.hpp>
#include <aero/render/mesh.hpp>
#include <aero/rhi/handles.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::ContentHash;
using engine::Guid;
using engine::editor::Aabb;
using engine::editor::LedgerAssetClass;
using engine::editor::LedgerAssetFacts;
using engine::editor::LedgerDestroy;
using engine::editor::LedgerHandles;
using engine::editor::LedgerServiceInput;
using engine::editor::LedgerServiceOutput;
using engine::editor::LedgerSlotBinding;
using engine::editor::LedgerState;
using engine::editor::ledgerStateLabel;
using engine::editor::MeshBoundsKey;
using engine::editor::SceneAssetLedger;
using engine::editor::TextureRequest;

namespace {

[[nodiscard]] Guid guidOf(std::uint64_t lo) noexcept { return Guid{.hi = 0, .lo = lo}; }
[[nodiscard]] ContentHash hashOf(std::uint64_t lo) noexcept { return ContentHash{.hi = 0, .lo = lo}; }

[[nodiscard]] engine::render::MeshHandle meshHandle(std::uint32_t index) noexcept {
    return engine::render::MeshHandle{.index = index, .generation = 1};
}
[[nodiscard]] engine::render::MaterialHandle materialHandle(std::uint32_t index) noexcept {
    return engine::render::MaterialHandle{.index = index, .generation = 1};
}
[[nodiscard]] engine::rhi::TextureHandle textureHandle(std::uint32_t index) noexcept {
    return engine::rhi::TextureHandle{.index = index, .generation = 1};
}

// A model asset the caller can see, hash and classify -- the ordinary case.
[[nodiscard]] LedgerAssetFacts modelFacts(std::uint64_t lo, std::uint64_t hash = 1) noexcept {
    return LedgerAssetFacts{
        .guid = guidOf(lo), .isMaterial = false, .recordPresent = true, .hashUsable = true, .hash = hashOf(hash)};
}

[[nodiscard]] LedgerAssetFacts materialFacts(std::uint64_t lo, std::uint64_t hash = 1) noexcept {
    LedgerAssetFacts facts = modelFacts(lo, hash);
    facts.isMaterial = true;
    return facts;
}

[[nodiscard]] LedgerServiceInput inputOf(std::span<const LedgerAssetFacts> referenced, std::uint64_t generation,
                                         std::span<const Guid> nudged = {}) noexcept {
    return LedgerServiceInput{.referenced = referenced, .generation = generation, .nudged = nudged};
}

// Loads `guid` for real: one service pass to get the directive, then the report. Returns the pass's
// output so a caller can still assert on it.
LedgerServiceOutput issueAndLoad(SceneAssetLedger& ledger, std::span<const LedgerAssetFacts> referenced,
                                 std::uint64_t generation, Guid guid, const LedgerHandles& handles, ContentHash hash,
                                 std::span<const TextureRequest> requests = {}) {
    LedgerServiceOutput out = ledger.service(inputOf(referenced, generation));
    REQUIRE(out.directive.has_value());
    REQUIRE(out.directive->guid == guid);
    ledger.reportLoaded(guid, hash, handles, requests);
    return out;
}

[[nodiscard]] bool destroyListNames(const std::vector<LedgerDestroy>& list, engine::render::MeshHandle mesh) noexcept {
    for (const LedgerDestroy& entry : list) {
        if (entry.mesh == mesh) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::size_t countMaterialDestroys(const std::vector<LedgerDestroy>& list,
                                                engine::render::MaterialHandle material) noexcept {
    std::size_t hits = 0;
    for (const LedgerDestroy& entry : list) {
        if (entry.material == material) {
            ++hits;
        }
    }
    return hits;
}

}  // namespace

TEST_CASE("LG1: an empty input yields no directive and no entries") {
    SceneAssetLedger ledger;
    const LedgerServiceOutput out = ledger.service(inputOf({}, 1));
    CHECK_FALSE(out.directive.has_value());
    CHECK(out.retire.empty());
    CHECK(out.destroy.empty());
    CHECK(ledger.entryCount() == 0);
    CHECK(ledger.loadAttempts() == 0);
    // The label function, checked here so it has one owner: it must never be named toString.
    CHECK(ledgerStateLabel(LedgerState::Absent) == "absent");
    CHECK(ledgerStateLabel(LedgerState::Ready) == "ready");
    CHECK(ledgerStateLabel(LedgerState::Failed) == "failed");
}

TEST_CASE("LG2: one Absent model yields exactly one directive") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1)};
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(1));
    CHECK((out.directive->assetClass == LedgerAssetClass::Model));
    CHECK_FALSE(out.directive->ownerMaterial.valid());
    CHECK(ledger.entryCount() == 1);
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Absent));
    CHECK(ledger.loadAttempts() == 1);
    // The class comes from the caller's own classification, not from the guid.
    SceneAssetLedger second;
    const std::vector<LedgerAssetFacts> materialRef{materialFacts(1)};
    const LedgerServiceOutput materialOut = second.service(inputOf(materialRef, 1));
    REQUIRE(materialOut.directive.has_value());
    CHECK((materialOut.directive->assetClass == LedgerAssetClass::Material));
}

TEST_CASE("LG3: two Absent entries yield one directive per call, the second on the next call") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(2)};
    const LedgerServiceOutput first = ledger.service(inputOf(referenced, 1));
    REQUIRE(first.directive.has_value());
    CHECK(first.directive->guid == guidOf(1));  // guid order
    CHECK(ledger.loadAttempts() == 1);
    // WITHOUT a report the same entry is re-issued -- that is the documented visible symptom of a
    // loader that forgets to report, and it is why the budget is one directive rather than a list.
    const LedgerServiceOutput second = ledger.service(inputOf(referenced, 1));
    REQUIRE(second.directive.has_value());
    CHECK(second.directive->guid == guidOf(1));
    CHECK(ledger.loadAttempts() == 2);
    ledger.reportLoaded(guidOf(1), hashOf(1), LedgerHandles{.mesh = meshHandle(7)});
    const LedgerServiceOutput third = ledger.service(inputOf(referenced, 1));
    REQUIRE(third.directive.has_value());
    CHECK(third.directive->guid == guidOf(2));
    CHECK(ledger.loadAttempts() == 3);
}

TEST_CASE("LG4: reportLoaded moves the entry to Ready and the next call issues the other one") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(2)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(1));
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Ready));
    CHECK(ledger.readyCount() == 1);
    REQUIRE(ledger.handlesOf(guidOf(1)) != nullptr);
    CHECK(ledger.handlesOf(guidOf(1))->mesh == meshHandle(7));
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(2));
}

TEST_CASE("LG5: a Failed entry issues no directive however many times service runs") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1)};
    const LedgerServiceOutput first = ledger.service(inputOf(referenced, 1));
    REQUIRE(first.directive.has_value());
    ledger.reportFailed(guidOf(1), "this model could not be cooked: nope");
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Failed));
    CHECK(ledger.failedCount() == 1);
    CHECK(ledger.messageOf(guidOf(1)) == "this model could not be cooked: nope");
    for (int i = 0; i < 10; ++i) {
        const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
        CHECK_FALSE(out.directive.has_value());
    }
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Failed));
}

TEST_CASE("LG6: loadAttempts does not move across ten services of a Failed-only set") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(2)};
    const LedgerServiceOutput a = ledger.service(inputOf(referenced, 1));
    REQUIRE(a.directive.has_value());
    ledger.reportFailed(a.directive->guid, "no");
    const LedgerServiceOutput b = ledger.service(inputOf(referenced, 1));
    REQUIRE(b.directive.has_value());
    ledger.reportFailed(b.directive->guid, "no");
    const std::size_t settled = ledger.loadAttempts();
    CHECK(settled == 2);
    for (int i = 0; i < 10; ++i) {
        const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
        CHECK_FALSE(out.directive.has_value());
    }
    CHECK(ledger.loadAttempts() == settled);
    CHECK(ledger.readyCount() == 0);
}

TEST_CASE("LG7: a Failed entry whose hash changes returns to Absent and is re-issued") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    const LedgerServiceOutput first = ledger.service(inputOf(referenced, 1));
    REQUIRE(first.directive.has_value());
    ledger.reportFailed(guidOf(1), "broken");
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Failed));
    // NEW BYTES ARE A NEW QUESTION.
    referenced[0].hash = hashOf(12);
    const LedgerServiceOutput second = ledger.service(inputOf(referenced, 2));
    REQUIRE(second.directive.has_value());
    CHECK(second.directive->guid == guidOf(1));
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Absent));
    CHECK(ledger.messageOf(guidOf(1)).empty());
}

TEST_CASE("LG8: a Failed entry whose hash is unchanged stays Failed across a generation bump") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    const LedgerServiceOutput first = ledger.service(inputOf(referenced, 1));
    REQUIRE(first.directive.has_value());
    ledger.reportFailed(guidOf(1), "broken");
    const std::size_t settled = ledger.loadAttempts();
    for (std::uint64_t generation = 2; generation < 6; ++generation) {
        const LedgerServiceOutput out = ledger.service(inputOf(referenced, generation));
        CHECK_FALSE(out.directive.has_value());
    }
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Failed));
    CHECK(ledger.messageOf(guidOf(1)) == "broken");
    CHECK(ledger.loadAttempts() == settled);
}

TEST_CASE("LG9+LG10: a retire puts NOTHING in this pass's destroy list and everything in the next") {
    // ONE SEQUENCE, NOT TWO CASES. A ledger that destroys in BOTH passes satisfies a case that only
    // checks pass N+1, which is exactly seed S20's shape -- the pass-N emptiness assertion below is the
    // half that catches it.
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    LedgerHandles handles;
    handles.mesh = meshHandle(7);
    handles.materials = {materialHandle(3), materialHandle(4)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), handles, hashOf(11));
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Ready));

    // ---- PASS N: the bytes changed, so the entry retires HERE ------------------------------------
    referenced[0].hash = hashOf(12);
    const LedgerServiceOutput passN = ledger.service(inputOf(referenced, 2));
    REQUIRE(passN.retire.size() == 1);
    CHECK(passN.retire[0] == guidOf(1));
    CHECK(passN.destroy.empty());  // <-- the whole of INV-D2's GPU half

    // ---- PASS N+1: and dies HERE, exactly once ---------------------------------------------------
    const LedgerServiceOutput passNext = ledger.service(inputOf(referenced, 2));
    CHECK(passNext.retire.empty());
    REQUIRE(passNext.destroy.size() == 3);
    CHECK(destroyListNames(passNext.destroy, meshHandle(7)));
    CHECK(countMaterialDestroys(passNext.destroy, materialHandle(3)) == 1);
    CHECK(countMaterialDestroys(passNext.destroy, materialHandle(4)) == 1);

    // ---- PASS N+2: and never again ---------------------------------------------------------------
    const LedgerServiceOutput passAfter = ledger.service(inputOf(referenced, 2));
    CHECK(passAfter.destroy.empty());
}

TEST_CASE("LG11: an unreferenced guid is retired, Failed entries included") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> both{modelFacts(1), modelFacts(2)};
    (void)issueAndLoad(ledger, both, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(1));
    const LedgerServiceOutput second = ledger.service(inputOf(both, 1));
    REQUIRE(second.directive.has_value());
    ledger.reportFailed(guidOf(2), "broken");
    CHECK(ledger.entryCount() == 2);

    // Both leave the referenced set: the entity was deleted.
    const LedgerServiceOutput out = ledger.service(inputOf({}, 1));
    CHECK(ledger.entryCount() == 0);
    CHECK(out.retire.size() == 2);
    // Stickiness is per-session-WHILE-REFERENCED: delete, fix the file, re-add, and it re-asks.
    const std::vector<LedgerAssetFacts> again{modelFacts(2)};
    const LedgerServiceOutput reasked = ledger.service(inputOf(again, 1));
    REQUIRE(reasked.directive.has_value());
    CHECK(reasked.directive->guid == guidOf(2));
}

TEST_CASE("LG12: a referenced guid whose record vanished is retired, not failed") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(1));
    referenced[0].recordPresent = false;
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 2));
    REQUIRE(out.retire.size() == 1);
    CHECK(out.retire[0] == guidOf(1));
    CHECK(ledger.entryCount() == 0);
    CHECK(ledger.failedCount() == 0);
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Absent));
}

TEST_CASE("LG13: an unhashed record leaves a Ready entry untouched") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(11));
    // An unhashed record's ALL-ZERO digest is the empty file's real value, never a sentinel: adopting
    // it would either re-load forever or freeze on a lie.
    referenced[0].hashUsable = false;
    referenced[0].hash = ContentHash{};
    for (std::uint64_t generation = 2; generation < 6; ++generation) {
        const LedgerServiceOutput out = ledger.service(inputOf(referenced, generation));
        CHECK(out.retire.empty());
        CHECK_FALSE(out.directive.has_value());
    }
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Ready));
    REQUIRE(ledger.handlesOf(guidOf(1)) != nullptr);
    CHECK(ledger.handlesOf(guidOf(1))->mesh == meshHandle(7));
}

TEST_CASE("LG14: a changed hash retires and re-issues") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(11));
    referenced[0].hash = hashOf(12);
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 2));
    REQUIRE(out.retire.size() == 1);
    CHECK(out.retire[0] == guidOf(1));
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Absent));
    // Re-issued in the SAME pass -- the entry is Absent by the time step 6 runs.
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(1));
    CHECK(ledger.handlesOf(guidOf(1))->mesh.valid() == false);
}

TEST_CASE("LG15: a nudge on a Ready entry retires and re-issues WITHOUT a generation bump") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{materialFacts(1, 11)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.materials = {materialHandle(3)}}, hashOf(11));
    const std::vector<Guid> nudged{guidOf(1)};
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1, nudged));  // SAME generation
    REQUIRE(out.retire.size() == 1);
    CHECK(out.retire[0] == guidOf(1));
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(1));
    CHECK((out.directive->assetClass == LedgerAssetClass::Material));
    const LedgerServiceOutput next = ledger.service(inputOf(referenced, 1));
    REQUIRE(next.destroy.size() == 1);
    CHECK(countMaterialDestroys(next.destroy, materialHandle(3)) == 1);
}

TEST_CASE("LG16: a nudge on an Absent entry is a no-op") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1)};
    const std::vector<Guid> nudged{guidOf(1), guidOf(99)};  // one Absent, one that has no entry at all
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1, nudged));
    CHECK(out.retire.empty());
    CHECK(out.destroy.empty());
    CHECK(ledger.entryCount() == 1);
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(1));
}

TEST_CASE("LG17: a nudge and a generation bump in one tick produce ONE retire, not two") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(11));
    referenced[0].hash = hashOf(12);  // the bytes changed AND the material panel applied
    const std::vector<Guid> nudged{guidOf(1)};
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 2, nudged));
    CHECK(out.retire.size() == 1);
    // ONE reload, so exactly one mesh handle is deferred, not two.
    const LedgerServiceOutput next = ledger.service(inputOf(referenced, 2));
    CHECK(next.destroy.size() == 1);
    CHECK(destroyListNames(next.destroy, meshHandle(7)));
}

TEST_CASE("LG18: a texture directive outranks a fresh model directive") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(2)};
    const std::vector<TextureRequest> requests{
        TextureRequest{.materialIndex = 0, .slot = 0, .guid = guidOf(50), .srgb = true}};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1),
                       LedgerHandles{.mesh = meshHandle(7), .materials = {materialHandle(3)}}, hashOf(1), requests);
    // guid 2 is still Absent, but the dressed model finishes dressing first.
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
    REQUIRE(out.directive.has_value());
    CHECK((out.directive->assetClass == LedgerAssetClass::Texture));
    CHECK(out.directive->guid == guidOf(50));
    CHECK(out.directive->ownerMaterial == guidOf(1));
    CHECK(out.directive->materialIndex == 0);
    CHECK(out.directive->slot == 0);
    CHECK(out.directive->srgb);
}

TEST_CASE("LG19: texture directives are issued one per pass in slot order") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{materialFacts(1)};
    // DELIBERATELY OUT OF ORDER on the way in, so "slot order" is a property of the ledger and not of
    // the caller's vector.
    const std::vector<TextureRequest> requests{
        TextureRequest{.materialIndex = 0, .slot = 4, .guid = guidOf(54), .srgb = true},
        TextureRequest{.materialIndex = 0, .slot = 1, .guid = guidOf(51), .srgb = false},
        TextureRequest{.materialIndex = 0, .slot = 0, .guid = guidOf(50), .srgb = true}};
    LedgerHandles handles;
    handles.materials = {materialHandle(3)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), handles, hashOf(1), requests);
    // The parallel vector is sized on adoption, never trusted from the caller.
    REQUIRE(ledger.handlesOf(guidOf(1)) != nullptr);
    CHECK(ledger.handlesOf(guidOf(1))->materialStates.size() == ledger.handlesOf(guidOf(1))->materials.size());

    const std::vector<std::size_t> expectedSlots{0, 1, 4};
    const std::vector<Guid> expectedGuids{guidOf(50), guidOf(51), guidOf(54)};
    for (std::size_t i = 0; i < expectedSlots.size(); ++i) {
        const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
        REQUIRE(out.directive.has_value());
        CHECK((out.directive->assetClass == LedgerAssetClass::Texture));
        CHECK(out.directive->slot == expectedSlots[i]);
        CHECK(out.directive->guid == expectedGuids[i]);
        const LedgerSlotBinding bound = ledger.reportSlotTexture(*out.directive, textureHandle(20 + i));
        CHECK(bound.material == materialHandle(3));
        REQUIRE(bound.state != nullptr);
    }
    // Every slot dressed: nothing left to issue.
    const LedgerServiceOutput settled = ledger.service(inputOf(referenced, 1));
    CHECK_FALSE(settled.directive.has_value());
    // And the three uploads are OWNED by the entry, so they die with it.
    CHECK(ledger.handlesOf(guidOf(1))->textures.size() == 3);
}

TEST_CASE("LG20: boundsLookup answers the folded box for each reported meshIndex") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1)};
    LedgerHandles handles;
    handles.mesh = meshHandle(7);
    handles.bounds = {{0U, Aabb{.min = engine::Vec3{-1.0F, -2.0F, -3.0F}, .max = engine::Vec3{1.0F, 2.0F, 3.0F}}},
                      {2U, Aabb{.min = engine::Vec3{0.0F, 0.0F, 0.0F}, .max = engine::Vec3{4.0F, 4.0F, 4.0F}}}};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), handles, hashOf(1));
    const Aabb* first = ledger.boundsLookup().find(MeshBoundsKey{.mesh = guidOf(1), .meshIndex = 0});
    REQUIRE(first != nullptr);
    CHECK(first->min.x == doctest::Approx(-1.0F));
    CHECK(first->max.z == doctest::Approx(3.0F));
    const Aabb* third = ledger.boundsLookup().find(MeshBoundsKey{.mesh = guidOf(1), .meshIndex = 2});
    REQUIRE(third != nullptr);
    CHECK(third->max.y == doctest::Approx(4.0F));
    // A meshIndex nobody reported has no box at all -- localBoundsFor's nullopt path.
    CHECK(ledger.boundsLookup().find(MeshBoundsKey{.mesh = guidOf(1), .meshIndex = 1}) == nullptr);
    CHECK(ledger.boundsLookup().size() == 2);
}

TEST_CASE("LG21: a retire removes that guid's bounds from the lookup") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11), modelFacts(2, 11)};
    LedgerHandles first;
    first.mesh = meshHandle(7);
    first.bounds = {{0U, Aabb{.min = engine::Vec3{-1.0F, -1.0F, -1.0F}, .max = engine::Vec3{1.0F, 1.0F, 1.0F}}}};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), first, hashOf(11));
    LedgerHandles second;
    second.mesh = meshHandle(8);
    second.bounds = {{0U, Aabb{.min = engine::Vec3{-2.0F, -2.0F, -2.0F}, .max = engine::Vec3{2.0F, 2.0F, 2.0F}}}};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(2), second, hashOf(11));
    CHECK(ledger.boundsLookup().size() == 2);

    const std::vector<LedgerAssetFacts> onlySecond{modelFacts(2, 11)};
    const LedgerServiceOutput out = ledger.service(inputOf(onlySecond, 1));
    CHECK(out.retire.size() == 1);
    CHECK(ledger.boundsLookup().find(MeshBoundsKey{.mesh = guidOf(1), .meshIndex = 0}) == nullptr);
    CHECK(ledger.boundsLookup().find(MeshBoundsKey{.mesh = guidOf(2), .meshIndex = 0}) != nullptr);
    CHECK(ledger.boundsLookup().size() == 1);
}

TEST_CASE("LG22: resetForProjectSwap returns every live handle exactly once and empties the ledger") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), materialFacts(2)};
    const std::vector<TextureRequest> requests{
        TextureRequest{.materialIndex = 0, .slot = 0, .guid = guidOf(50), .srgb = true}};
    LedgerHandles model;
    model.mesh = meshHandle(7);
    model.materials = {materialHandle(3)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), model, hashOf(1), requests);
    const LedgerServiceOutput textureIssue = ledger.service(inputOf(referenced, 1));
    REQUIRE(textureIssue.directive.has_value());
    CHECK((textureIssue.directive->assetClass == LedgerAssetClass::Texture));
    const LedgerSlotBinding bound = ledger.reportSlotTexture(*textureIssue.directive, textureHandle(20));
    REQUIRE(bound.state != nullptr);
    const LedgerServiceOutput materialIssue = ledger.service(inputOf(referenced, 1));
    REQUIRE(materialIssue.directive.has_value());
    CHECK(materialIssue.directive->guid == guidOf(2));
    ledger.reportLoaded(guidOf(2), hashOf(1), LedgerHandles{.materials = {materialHandle(4)}});

    const std::vector<LedgerDestroy> released = ledger.resetForProjectSwap();
    CHECK(ledger.entryCount() == 0);
    CHECK(ledger.readyCount() == 0);
    CHECK(ledger.boundsLookup().size() == 0);
    REQUIRE(released.size() == 4);  // one mesh, two materials, one texture -- each exactly once
    CHECK(destroyListNames(released, meshHandle(7)));
    CHECK(countMaterialDestroys(released, materialHandle(3)) == 1);
    CHECK(countMaterialDestroys(released, materialHandle(4)) == 1);
    std::size_t textures = 0;
    for (const LedgerDestroy& entry : released) {
        if (entry.texture == textureHandle(20)) {
            ++textures;
        }
    }
    CHECK(textures == 1);
}

TEST_CASE("LG23: two facts naming the same guid in one input collapse to one entry") {
    SceneAssetLedger ledger;
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(1), modelFacts(1)};
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
    CHECK(ledger.entryCount() == 1);
    REQUIRE(out.directive.has_value());
    CHECK(out.directive->guid == guidOf(1));
    ledger.reportLoaded(guidOf(1), hashOf(1), LedgerHandles{.mesh = meshHandle(7)});
    const LedgerServiceOutput settled = ledger.service(inputOf(referenced, 1));
    CHECK_FALSE(settled.directive.has_value());
    CHECK(ledger.readyCount() == 1);
}

TEST_CASE("LG24: the destroy deferral survives resetForProjectSwap -- nothing is returned twice") {
    SceneAssetLedger ledger;
    std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11), modelFacts(2, 11)};
    (void)issueAndLoad(ledger, referenced, 1, guidOf(1), LedgerHandles{.mesh = meshHandle(7)}, hashOf(11));
    (void)issueAndLoad(ledger, referenced, 1, guidOf(2), LedgerHandles{.mesh = meshHandle(8)}, hashOf(11));
    // guid 1's bytes change: it retires THIS pass and is deferred.
    referenced[0].hash = hashOf(12);
    const LedgerServiceOutput passN = ledger.service(inputOf(referenced, 2));
    CHECK(passN.destroy.empty());

    // The project swaps before the deferral was ever returned.
    const std::vector<LedgerDestroy> released = ledger.resetForProjectSwap();
    REQUIRE(released.size() == 2);
    CHECK(destroyListNames(released, meshHandle(7)));  // the deferred one
    CHECK(destroyListNames(released, meshHandle(8)));  // the still-live one
    // And the next pass returns nothing: the member was emptied, so no handle is handed back twice.
    const LedgerServiceOutput after = ledger.service(inputOf({}, 3));
    CHECK(after.destroy.empty());
    CHECK(after.retire.empty());
    CHECK(ledger.entryCount() == 0);
}

// ---- the code-review round -----------------------------------------------------------------------

TEST_CASE("LG25: reportLoaded ADOPTS a guid with no entry yet, instead of dropping the handles") {
    // The drop path reports from tick()'s reconcile block, which runs BEFORE serviceSceneAssets has
    // inserted anything -- so on the tick a drop lands there is no entry for the guid. This used to
    // early-return, stranding a live MeshHandle and every MaterialHandle: never adopted, never
    // deferred, never destroyed, while the binding table already named them, and the ledger then
    // re-imported the same bytes on the next pass. Reachable for .blend/.obj/.ply/.stl, where the drop
    // performs a Full import; a .gltf takes the Structure path and never reaches it, which is why the
    // .gltf-based drop cases were blind.
    SceneAssetLedger ledger;
    REQUIRE(ledger.entryCount() == 0);

    ledger.reportLoaded(guidOf(1), hashOf(11), LedgerHandles{.mesh = meshHandle(7)});

    REQUIRE(ledger.entryCount() == 1);
    CHECK((ledger.stateOf(guidOf(1)) == LedgerState::Ready));

    // ...and the adopted entry is a REAL one: a later pass that references the same guid issues NO
    // directive for it, which is the half that proves the second import is gone.
    const std::vector<LedgerAssetFacts> referenced{modelFacts(1, 11)};
    const LedgerServiceOutput out = ledger.service(inputOf(referenced, 1));
    CHECK_FALSE(out.directive.has_value());
    CHECK(ledger.loadAttempts() == 0);  // nothing was ever issued for it

    // The handles are genuinely held: retiring the entry hands them back for destruction.
    const LedgerServiceOutput retired = ledger.service(inputOf({}, 2));
    CHECK(retired.destroy.empty());  // deferred, never in the retiring pass
    const LedgerServiceOutput next = ledger.service(inputOf({}, 3));
    CHECK(destroyListNames(next.destroy, meshHandle(7)));
}

TEST_CASE("LG26: an Absent entry whose record vanished never starves the one-per-pass budget") {
    // Step 5 refuses to INSERT an entry for a guid with no record, and says why: the loader can do
    // nothing with a guid the database cannot resolve, so issuing for it burns the whole budget. Step
    // 6b had no such guard, and step 3 re-examines only Ready and Failed -- so an entry inserted while
    // the record existed and orphaned afterwards was picked forever, climbing loadAttempts() while
    // readyCount() never moved and every other asset waited.
    SceneAssetLedger ledger;
    // Two models referenced; guid 1 sorts first, so it takes the pass. Neither is reported.
    std::vector<LedgerAssetFacts> referenced{modelFacts(1), modelFacts(2)};
    const LedgerServiceOutput first = ledger.service(inputOf(referenced, 1));
    REQUIRE(first.directive.has_value());
    REQUIRE(first.directive->guid == guidOf(1));
    REQUIRE(ledger.entryCount() == 2);

    // guid 1's asset is deleted from disk. Its entry is still Absent, so step 3 never looks at it.
    referenced[0].recordPresent = false;

    // The budget must move ON to guid 2 rather than re-picking the orphan forever.
    const LedgerServiceOutput second = ledger.service(inputOf(referenced, 2));
    REQUIRE(second.directive.has_value());
    CHECK(second.directive->guid == guidOf(2));

    // And it stays moved on: a third pass does not fall back to the orphan either.
    ledger.reportLoaded(guidOf(2), hashOf(1), LedgerHandles{.mesh = meshHandle(8)});
    const LedgerServiceOutput third = ledger.service(inputOf(referenced, 3));
    CHECK_FALSE(third.directive.has_value());
}
