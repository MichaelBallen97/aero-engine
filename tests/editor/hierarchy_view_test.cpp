// Aero Engine -- task E.6.3: the Hierarchy's chrome as VALUES (HY1-HY8). Tier 0: a real World, no ImGui, no #if.
#include <aero/editor/entity_ops.hpp>  // createEntity
#include <aero/editor/hierarchy_view.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <vector>

namespace {

namespace ed = engine::editor;
using engine::Entity;
using engine::World;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

// A forest whose deepest match sits under two parents: Alpha > Cube Child > Deep Cube, and a second root Beta Cube.
struct Forest {
    World world;
    Entity alpha{};
    Entity cubeChild{};
    Entity deepCube{};
    Entity betaCube{};
    std::vector<Entity> roots;
};
void seedForest(Forest& f) {
    f.alpha = ed::createEntity(f.world, {}, "Alpha");
    f.cubeChild = ed::createEntity(f.world, f.alpha, "Cube Child");
    f.deepCube = ed::createEntity(f.world, f.cubeChild, "Deep Cube");
    f.betaCube = ed::createEntity(f.world, {}, "Beta Cube");
    f.roots = {f.alpha, f.betaCube};
}

[[nodiscard]] std::vector<Entity> matchesFor(const Forest& f, std::string_view query) {
    std::vector<Entity> out;
    std::vector<ed::TreeWalkEntry> stack;
    std::vector<Entity> arena;
    std::string label;
    ed::collectHierarchyMatches(f.world, f.roots, query, out, stack, arena, label);
    return out;
}

}  // namespace

TEST_CASE("hierarchy view: matches are the whole forest, in pre-order (task E.6.3, HY1)") {
    Forest f;
    seedForest(f);
    const std::vector<Entity> found = matchesFor(f, "cube");
    REQUIRE(found.size() == 3U);  // compared ELEMENT BY ELEMENT against a hand-listed order
    CHECK((found[0] == f.cubeChild));
    CHECK((found[1] == f.deepCube));  // under two parents: the walk is total, open state is irrelevant (S10)
    CHECK((found[2] == f.betaCube));
}

TEST_CASE("hierarchy view: ASCII case-insensitive, byte-exact above ASCII, fallback labels (task E.6.3, HY2)") {
    Forest f;
    seedForest(f);
    CHECK(matchesFor(f, "CUBE").size() == 3U);
    const Entity cafe = ed::createEntity(f.world, {}, "Caf\xC3\xA9");
    const Entity unnamed = f.world.create();  // no name: entityLabel falls back to "Entity <index>"
    f.roots = {f.alpha, f.betaCube, cafe, unnamed};
    const std::vector<Entity> accented = matchesFor(f, "\xC3\xA9");
    REQUIRE(accented.size() == 1U);
    CHECK((accented[0] == cafe));
    CHECK(matchesFor(f, "\xC3\x89").empty());  // a different byte sequence: no case folding above ASCII
    const std::vector<Entity> fallback = matchesFor(f, "entity");
    REQUIRE(fallback.size() == 1U);
    CHECK((fallback[0] == unnamed));
    CHECK(matchesFor(f, "").empty());  // the flat mode is off
}

TEST_CASE("hierarchy view: the count badge (task E.6.3, HY3)") {
    std::string out = "stale";
    ed::hierarchyBadgeText(12U, std::nullopt, out);
    CHECK(out == "12");
    ed::hierarchyBadgeText(12U, 3U, out);
    CHECK(out == "3 / 12");
    ed::hierarchyBadgeText(12U, 0U, out);
    CHECK(out == "0 / 12");
    ed::hierarchyBadgeText(0U, std::nullopt, out);
    CHECK(out == "0");
    ed::hierarchyBadgeText(50000U, std::nullopt, out);
    CHECK(out == "50 000");  // groupThousands' own grouping, restated
    ed::hierarchyBadgeText(1284U, 96U, out);
    CHECK(out == "96 / 1 284");
}

TEST_CASE("hierarchy view: a row's dot, label and meta at scale 1 (task E.6.3, HY4)") {
    const ed::HierarchyRowLayout row = ed::hierarchyRowLayout(100.0F, 400.0F, 50.0F, 20.0F, true, 1.0F);
    CHECK(row.dotMin.x == 100.0F);
    CHECK(row.dotMin.y == 47.0F);  // a 6-point dot centred on the row
    CHECK(row.dotMax.x == 106.0F);
    CHECK(row.dotMax.y == 53.0F);
    CHECK(row.labelX == 114.0F);  // 8 after the dot
    CHECK(row.metaX == 374.0F);   // right edge at 400 - 6
    CHECK(row.metaShown);
    CHECK(row.labelMaxX == 366.0F);  // the meta's left minus the gap
    const ed::HierarchyRowLayout leaf = ed::hierarchyRowLayout(100.0F, 400.0F, 50.0F, 0.0F, false, 1.0F);
    CHECK_FALSE(leaf.metaShown);
    CHECK(leaf.labelMaxX == 400.0F);  // no meta: the label runs to the row's end
    const ed::HierarchyRowLayout narrow = ed::hierarchyRowLayout(100.0F, 160.0F, 50.0F, 20.0F, true, 1.0F);
    CHECK_FALSE(narrow.metaShown);  // 134 - 8 < 114 + 16: the meta goes before the label drops under one em
    CHECK(narrow.labelMaxX == 160.0F);
}

TEST_CASE("hierarchy view: the row layout doubles exactly at scale 2 (task E.6.3, HY5)") {
    const ed::HierarchyRowLayout one = ed::hierarchyRowLayout(100.0F, 400.0F, 50.0F, 20.0F, true, 1.0F);
    const ed::HierarchyRowLayout two = ed::hierarchyRowLayout(100.0F, 400.0F, 50.0F, 40.0F, true, 2.0F);
    CHECK(two.dotMax.x - two.dotMin.x == 2.0F * (one.dotMax.x - one.dotMin.x));
    CHECK(50.0F - two.dotMin.y == 2.0F * (50.0F - one.dotMin.y));
    CHECK(two.labelX - 100.0F == 2.0F * (one.labelX - 100.0F));
    CHECK(400.0F - two.metaX == 2.0F * (400.0F - one.metaX));
    CHECK(400.0F - two.labelMaxX == 2.0F * (400.0F - one.labelMaxX));
}

TEST_CASE("hierarchy view: the header row shares its width and floors the search (task E.6.3, HY6)") {
    const ed::HierarchyHeaderLayout wide = ed::hierarchyHeaderLayout(10.0F, 300.0F, 40.0F, 1.0F);
    CHECK(wide.searchWidth == 252.0F);  // 300 - 40 - 8
    CHECK(wide.badgeX == 270.0F);       // right-aligned: search + gap + badge == the row
    CHECK(wide.searchWidth + 8.0F + 40.0F == 300.0F);
    const ed::HierarchyHeaderLayout narrow = ed::hierarchyHeaderLayout(10.0F, 50.0F, 40.0F, 1.0F);
    CHECK(narrow.searchWidth == 24.0F);  // one frame height: body 16 + 2 x 4
    CHECK(narrow.badgeX == 42.0F);       // after the floored field, never over it
    for (const float bad : {NAN_F, 0.0F, -1.0F}) {
        CAPTURE(bad);
        CHECK((ed::hierarchyHeaderLayout(10.0F, 300.0F, 40.0F, bad) == wide));
    }
}

TEST_CASE("hierarchy view: the shortcut gate's truth table (task E.6.3, HY7)") {
    CHECK(ed::hierarchyShortcutsLive(false, false));
    CHECK_FALSE(ed::hierarchyShortcutsLive(true, false));
    CHECK_FALSE(ed::hierarchyShortcutsLive(false, true));  // seed S8: an || here goes red on this row
    CHECK_FALSE(ed::hierarchyShortcutsLive(true, true));
}

TEST_CASE("hierarchy view: an unwritten record is vacuous, and a frame resets it (task E.6.3, HY8)") {
    ed::HierarchyDrawRecord record{};
    CHECK(record.framesRecorded == 0U);
    CHECK(record.rowsDrawn == 0U);
    CHECK(record.rowRoles.empty());
    CHECK(record.rowMetas.empty());
    CHECK(record.badgeText.empty());
    CHECK_FALSE(record.shortcutsLive);  // unwritten reads "gated": a case asserting it live must see a drawn frame
    CHECK_FALSE(record.searchFocused);
    CHECK_FALSE(record.renameFieldDrawn);
    CHECK_FALSE(record.renameActive);
    CHECK_FALSE(record.renameFieldVisible);
    // Every per-frame member written, as a drawn frame leaves it: the panel only APPENDS to the two lists, so a
    // missing clear grows each by every row it records, every frame. HY8 pins the clear at tier 0; I309's size and
    // parent arms see it at the GPU tier (the code-review round's finding 13).
    record.rowsDrawn = 4U;
    record.dotsDrawn = 4U;
    record.metasDrawn = 1U;
    record.rowRoles.push_back(ed::HierarchyRowRole{});
    record.rowMetas.push_back(ed::HierarchyRowMeta{.entity = {}, .text = "2"});
    record.searchActive = true;
    record.shortcutsLive = true;
    record.searchFocused = true;
    record.flatMode = true;
    record.headerRuleDrawn = true;
    record.renameFieldDrawn = true;
    record.renameActive = true;
    record.renameFieldVisible = true;
    ed::beginHierarchyRecordFrame(record);
    CHECK(record.framesRecorded == 1U);
    CHECK(record.rowsDrawn == 0U);
    CHECK(record.dotsDrawn == 0U);
    CHECK(record.metasDrawn == 0U);
    CHECK(record.rowRoles.empty());
    CHECK(record.rowMetas.empty());
    CHECK_FALSE(record.searchActive);
    CHECK_FALSE(record.shortcutsLive);
    CHECK_FALSE(record.searchFocused);
    CHECK_FALSE(record.flatMode);
    CHECK_FALSE(record.headerRuleDrawn);
    // The rename field's two members (the code-review round, round 2): written only while a rename's row is walked,
    // so a missing reset keeps "drawn" and "active" true over every frame the field is gone -- the state I344 reads.
    CHECK_FALSE(record.renameFieldDrawn);
    CHECK_FALSE(record.renameActive);
    // ...and its visibility (round 3), written beside them: a missing reset would keep reporting a field in view over
    // every frame it is gone.
    CHECK_FALSE(record.renameFieldVisible);
}
