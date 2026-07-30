// tests/editor/hierarchy_test.cpp — task 2.2.1: the editor's scene-state layer, tier-0 and UNGATED.
// The second TU of aero_editor_shell_test (which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here). No GPU, no window, no ImGui context, no files, no
// randomness: it must pass identically with AERO_REQUIRE_GPU unset and set (AC-19).
//
// It covers Selection (selection.hpp), the pure structural ops + RootOrder + seedDefaultScene
// (entity_ops.hpp) and PanelContext (panel_context.hpp). The HierarchyPanel itself is src-private and
// ImGui-bound: its Begin/End + TreePop balance is proven by aero_editor_imgui_test on a real GPU,
// which is the only place that assertion means anything (I3).
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/tree_walk.hpp>
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- the E12/AC-13 proof
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::MeshRenderer;
using engine::Transform;
using engine::World;
using engine::editor::Selection;
using engine::scene::internal::registerComponent;

namespace {

// Every helper here is a free function over a local World -- no fixture class, matching
// scene_test.cpp / transform_test.cpp.
[[nodiscard]] std::vector<Entity> childrenOf(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&out](Entity c) { out.push_back(c); });
    return out;
}

[[nodiscard]] std::vector<Entity> rootsOf(const World& world) {
    std::vector<Entity> out;
    world.eachEntity([&](Entity e) {
        if (!world.parent(e).valid()) {
            out.push_back(e);
        }
    });
    return out;
}

}  // namespace

// ---- Selection ---------------------------------------------------------------------------------

TEST_CASE("editor: Selection starts empty and rejects the null entity (I5)") {
    Selection s;
    CHECK(s.empty());
    CHECK(s.count() == 0);
    CHECK(s.primary() == Entity{});
    CHECK(s.entities().empty());
    CHECK_FALSE(s.contains(Entity{}));

    s.add(Entity{});
    CHECK(s.empty());
    s.set(Entity{});
    CHECK(s.empty());
    s.toggle(Entity{});
    CHECK(s.empty());
    CHECK_FALSE(s.contains(Entity{}));
}

TEST_CASE("editor: Selection set/add/remove/toggle and the primary (D2)") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    Selection s;

    s.set(a);
    CHECK(s.count() == 1);
    CHECK(s.primary() == a);

    s.add(b);
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);  // most recently ADDED
    s.add(b);                 // no-op, and does NOT re-primary
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);

    s.add(c);
    CHECK(s.primary() == c);
    s.remove(c);  // removing the primary re-primaries to the last survivor
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);
    s.remove(a);  // removing a NON-primary leaves the primary alone
    CHECK(s.primary() == b);
    s.remove(a);  // absent -> no-op
    CHECK(s.count() == 1);

    s.toggle(a);
    CHECK(s.contains(a));
    CHECK(s.primary() == a);
    s.toggle(a);
    CHECK_FALSE(s.contains(a));
    CHECK(s.primary() == b);

    s.set(c);  // set REPLACES
    CHECK(s.count() == 1);
    CHECK(s.contains(c));
    CHECK_FALSE(s.contains(b));

    s.clear();
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection::setAll replaces wholesale, dedupes and skips nulls") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    Selection s;
    s.set(a);

    const std::vector<Entity> batch{b, a, b, Entity{}, a};
    s.setAll(batch);
    CHECK(s.count() == 2);
    CHECK(s.contains(a));
    CHECK(s.contains(b));
    CHECK(s.primary() == a);      // the LAST newly-added survivor
    CHECK(s.entities()[0] == b);  // input order preserved
    CHECK(s.entities()[1] == a);

    s.setAll(std::span<const Entity>{});
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection::prune drops dead handles and fixes the primary (I5)") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    Selection s;
    s.add(a);
    s.add(b);
    s.add(c);
    REQUIRE(s.primary() == c);

    REQUIRE(w.destroy(c));
    CHECK(s.prune(w) == 1);
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);  // the dropped primary follows to the last survivor
    CHECK(s.prune(w) == 0);   // idempotent

    REQUIRE(w.destroy(a));
    REQUIRE(w.destroy(b));
    CHECK(s.prune(w) == 2);
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection's shape and noexcept contract") {
    static_assert(std::is_nothrow_move_constructible_v<Selection>);
    static_assert(std::is_nothrow_move_assignable_v<Selection>);
    static_assert(std::is_default_constructible_v<Selection>);
    const Selection s;
    static_assert(noexcept(s.empty()));
    static_assert(noexcept(s.count()));
    static_assert(noexcept(s.contains(Entity{})));
    static_assert(noexcept(s.primary()));
    static_assert(noexcept(s.entities()));
}

// ---- clickSelectionAction (bugfix: multi-select drag, task 2.2.1) -------------------------------
//
// The regression this closes: the Hierarchy panel used to replace the WHOLE selection with a single
// row the instant IsItemClicked() fired -- the mouse-DOWN edge -- so pressing down on an
// already-selected row to BEGIN a multi-row drag collapsed the selection to that one row before
// BeginDragDropSource ever saw it. This function is the pure decision the panel's click/release
// sequencing now defers to; hierarchy_panel.cpp itself is src-private and ImGui-bound and cannot be
// driven from a tier-0 test (matching the walkForest / resolveCreateChildParent precedent, review
// round 2, Gaps 1 and 5) -- but no test that drives reparentTargets() directly could ever have caught
// this bug in the first place, since the defect was entirely in the panel's SEQUENCING, not in any
// function reparentTargets() touches. Hence: extract the decision, don't test the symptom.

TEST_CASE("editor: clickSelectionAction's shape and noexcept contract") {
    using engine::editor::ClickPhase;
    using engine::editor::clickSelectionAction;
    static_assert(noexcept(clickSelectionAction(false, false, false, false, false, ClickPhase::Press)));
}

TEST_CASE(
    "editor: clickSelectionAction — PRESS: Ctrl/Shift resolve immediately; a plain click on an "
    "already-selected row defers (None)") {
    using engine::editor::ClickPhase;
    using engine::editor::clickSelectionAction;
    using engine::editor::ClickSelectionAction;
    constexpr ClickPhase PRESS = ClickPhase::Press;

    SUBCASE("unselected + plain -> Select (safe to apply immediately: nothing to collapse)") {
        CHECK(clickSelectionAction(false, false, false, false, false, PRESS) == ClickSelectionAction::Select);
    }
    SUBCASE(
        "selected + plain -> None (THE fix: deferred to RELEASE, so a same-press drag still reads "
        "the whole selection)") {
        CHECK(clickSelectionAction(true, false, false, false, false, PRESS) == ClickSelectionAction::None);
    }
    SUBCASE("Ctrl/Cmd -> Toggle, regardless of prior selection state (unaffected by the fix)") {
        CHECK(clickSelectionAction(false, true, false, false, false, PRESS) == ClickSelectionAction::Toggle);
        CHECK(clickSelectionAction(true, true, false, false, false, PRESS) == ClickSelectionAction::Toggle);
    }
    SUBCASE("Shift -> Range, regardless of prior selection state (unaffected by the fix)") {
        CHECK(clickSelectionAction(false, false, true, false, false, PRESS) == ClickSelectionAction::Range);
        CHECK(clickSelectionAction(true, false, true, false, false, PRESS) == ClickSelectionAction::Range);
    }
    SUBCASE("Shift beats Ctrl when (implausibly) both are held, matching the panel's own if/else order") {
        CHECK(clickSelectionAction(false, true, true, false, false, PRESS) == ClickSelectionAction::Range);
    }
    SUBCASE("the arrow-toggle guard takes precedence over every other input, on PRESS") {
        CHECK(clickSelectionAction(false, false, false, true, false, PRESS) == ClickSelectionAction::None);
        CHECK(clickSelectionAction(true, false, false, true, false, PRESS) == ClickSelectionAction::None);
        CHECK(clickSelectionAction(false, true, false, true, false, PRESS) == ClickSelectionAction::None);
        CHECK(clickSelectionAction(false, false, true, true, false, PRESS) == ClickSelectionAction::None);
    }
}

TEST_CASE("editor: clickSelectionAction — RELEASE: commits the deferred replace iff no drag occurred") {
    using engine::editor::ClickPhase;
    using engine::editor::clickSelectionAction;
    using engine::editor::ClickSelectionAction;
    constexpr ClickPhase RELEASE = ClickPhase::Release;

    SUBCASE("selected + plain + no drag -> Select (a press+release with no movement still commits)") {
        CHECK(clickSelectionAction(true, false, false, false, false, RELEASE) == ClickSelectionAction::Select);
    }
    SUBCASE(
        "selected + plain + a drag occurred -> None (never collapse the selection after a real "
        "drag, whether the drop landed elsewhere or back on the source row itself)") {
        CHECK(clickSelectionAction(true, false, false, false, true, RELEASE) == ClickSelectionAction::None);
    }
    SUBCASE(
        "RELEASE is only ever asked for a plain click's deferred candidate -- Ctrl/Shift never arm "
        "one (they resolve fully at PRESS), so the function must not select if they are somehow "
        "still set here") {
        CHECK(clickSelectionAction(true, true, false, false, false, RELEASE) == ClickSelectionAction::None);
        CHECK(clickSelectionAction(true, false, true, false, false, RELEASE) == ClickSelectionAction::None);
    }
    SUBCASE(
        "defensive: !alreadySelected never arms a deferred candidate either (PRESS already fires "
        "Select immediately in that case), so RELEASE must not select here") {
        CHECK(clickSelectionAction(false, false, false, false, false, RELEASE) == ClickSelectionAction::None);
    }
    SUBCASE("the arrow-toggle guard takes precedence on RELEASE too") {
        CHECK(clickSelectionAction(true, false, false, true, false, RELEASE) == ClickSelectionAction::None);
    }
}

TEST_CASE("editor: multi-select drag survives the press -- the end-to-end regression (task 2.2.1 fix)") {
    // The exact bug: dragging a row that is PART OF a multi-selection used to collapse the selection to
    // that one row on mouse-DOWN, before the drag's own drop-time read of it. reparentTargets() itself
    // was always correct (review round 2, Gap 4's own test above proves it) -- the defect was entirely
    // in what the SELECTION held by the time reparentTargets() was asked.
    using engine::editor::ClickPhase;
    using engine::editor::clickSelectionAction;
    using engine::editor::ClickSelectionAction;
    using engine::editor::reparentTargets;
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    const Entity grabbed = b;  // the row physically pressed to start the drag -- part of the selection
    Selection selection;
    selection.setAll(std::vector<Entity>{a, b, c});
    REQUIRE(selection.count() == 3);

    // The OLD (buggy) sequencing: PRESS unconditionally replaced the selection with the grabbed row,
    // exactly what a bare `IsItemClicked() -> selection.set(entity)` did. Pinned here as the regression
    // this fix closes, NOT as current panel behaviour.
    Selection oldSequencing = selection;
    oldSequencing.set(grabbed);
    CHECK(reparentTargets(w, oldSequencing.entities(), grabbed) == std::vector<Entity>{grabbed});

    // The NEW sequencing: PRESS on an already-selected row resolves to None -- the panel applies
    // NOTHING for None, so `selection` is untouched by the time the drop reads it.
    const ClickSelectionAction pressAction = clickSelectionAction(
        /*alreadySelected=*/selection.contains(grabbed), false, false, false, false, ClickPhase::Press);
    REQUIRE(pressAction == ClickSelectionAction::None);
    REQUIRE(selection.count() == 3);  // still whole -- nothing was applied for None

    // Drop time: reparentTargets() now sees the WHOLE selection, exactly the fix's contract.
    CHECK(reparentTargets(w, selection.entities(), grabbed) == std::vector<Entity>{a, b, c});
}

// ---- PanelContext (AC-8/D7) ----------------------------------------------------------------------

TEST_CASE("editor: PanelContext binds references, it does not copy (D7)") {
    World w;
    Selection s;
    engine::editor::CommandStack commands;
    engine::editor::RootOrder roots;
    engine::editor::PanelContext context{w, s, commands, roots};

    CHECK(&context.world == &w);
    CHECK(&context.selection == &s);

    // A write through the context is visible through the originals -- the reference proof, not a
    // tautology: a by-value aggregate would pass the address check only by accident and fail here.
    const Entity e = context.world.create();
    context.selection.add(e);
    CHECK(w.alive(e));
    CHECK(s.contains(e));
    CHECK(s.primary() == e);

    static_assert(std::is_aggregate_v<engine::editor::PanelContext>);
    static_assert(!std::is_copy_assignable_v<engine::editor::PanelContext>);  // reference members
}

// ---- entity_ops: queries -------------------------------------------------------------------------

namespace {
// The E12/AC-13 proof material: a component type NO LINE OF EDITOR CODE HAS EVER HEARD OF. If
// duplicateEntities ever hardcodes the five built-ins, the marker's VALUE assertion below goes red.
// Asserting only that the copy exists would NOT catch that -- which is exactly why the value matters.
struct Marker {
    int payload = 0;
};
}  // namespace

TEST_CASE("editor: isDescendantOf walks upward, self included (I7/E31)") {
    using engine::editor::isDescendantOf;
    World w;
    const Entity root = w.create();
    const Entity child = w.create();
    const Entity grandchild = w.create();
    const Entity unrelated = w.create();
    REQUIRE(w.setParent(child, root));
    REQUIRE(w.setParent(grandchild, child));

    CHECK(isDescendantOf(w, root, root));  // self counts
    CHECK(isDescendantOf(w, child, root));
    CHECK(isDescendantOf(w, grandchild, root));
    CHECK_FALSE(isDescendantOf(w, root, child));  // the other direction
    CHECK_FALSE(isDescendantOf(w, unrelated, root));
    CHECK_FALSE(isDescendantOf(w, Entity{}, root));
    CHECK_FALSE(isDescendantOf(w, root, Entity{}));

    // 1000 deep: iterative, so this costs heap, not stack frames (E31/D13).
    Entity deep = w.create();
    const Entity chainRoot = deep;
    for (int i = 0; i < 1000; ++i) {
        const Entity next = w.create();
        REQUIRE(w.setParent(next, deep));
        deep = next;
    }
    CHECK(isDescendantOf(w, deep, chainRoot));
    CHECK_FALSE(isDescendantOf(w, chainRoot, deep));
}

TEST_CASE("editor: topMost drops covered entries and preserves input order (D19)") {
    using engine::editor::topMost;
    World w;
    const Entity a = w.create();
    const Entity aChild = w.create();
    const Entity aGrand = w.create();
    const Entity b = w.create();
    const Entity dead = w.create();
    REQUIRE(w.setParent(aChild, a));
    REQUIRE(w.setParent(aGrand, aChild));
    REQUIRE(w.destroy(dead));

    CHECK(topMost(w, std::vector<Entity>{a, aChild}) == std::vector<Entity>{a});
    CHECK(topMost(w, std::vector<Entity>{aChild, a}) == std::vector<Entity>{a});
    CHECK(topMost(w, std::vector<Entity>{aGrand, a}) == std::vector<Entity>{a});
    CHECK(topMost(w, std::vector<Entity>{b, a}) == std::vector<Entity>{b, a});  // order preserved
    CHECK(topMost(w, std::vector<Entity>{aChild, b}) == std::vector<Entity>{aChild, b});
    CHECK(topMost(w, std::vector<Entity>{dead, Entity{}, b}) == std::vector<Entity>{b});
    CHECK(topMost(w, std::vector<Entity>{a, a}) == std::vector<Entity>{a});  // deduped
    CHECK(topMost(w, std::span<const Entity>{}).empty());
}

TEST_CASE("editor: entityLabel names an entity or falls back (D21/E22)") {
    using engine::editor::entityLabel;
    World w;
    const Entity named = w.create();
    const Entity unnamed = w.create();
    const Entity dead = w.create();
    REQUIRE(w.setName(named, "Player"));
    REQUIRE(w.destroy(dead));

    std::string label = "stale content that must be cleared";
    entityLabel(w, named, label);
    CHECK(label == "Player");
    entityLabel(w, unnamed, label);
    CHECK(label == "Entity " + std::to_string(unnamed.index));
    entityLabel(w, dead, label);
    CHECK(label == "Entity " + std::to_string(dead.index));
    REQUIRE(w.setName(named, ""));  // cleared -> falls back (E22)
    entityLabel(w, named, label);
    CHECK(label == "Entity " + std::to_string(named.index));
}

// ---- entity_ops: mutations -----------------------------------------------------------------------

TEST_CASE("editor: createEntity gives a Transform, an optional name and an optional parent (AC-11)") {
    using engine::editor::createEntity;
    World w;
    const Entity root = createEntity(w, {}, "Root");
    REQUIRE(root.valid());
    CHECK(w.has<Transform>(root));
    CHECK(w.name(root) == std::string_view{"Root"});
    CHECK(w.parent(root) == Entity{});

    const Entity child = createEntity(w, root);
    REQUIRE(child.valid());
    CHECK(w.has<Transform>(child));
    CHECK(w.name(child).empty());
    CHECK(w.parent(child) == root);
    CHECK(w.childCount(root) == 1);

    const Entity dead = w.create();
    REQUIRE(w.destroy(dead));
    const std::size_t before = w.entityCount();
    CHECK(createEntity(w, dead) == Entity{});  // a dead parent fails...
    CHECK(w.entityCount() == before);          // ...and leaves NO orphan behind
}

TEST_CASE("editor: resolveCreateChildParent follows Section O-2 (review round 2, Gap 5)") {
    using engine::editor::resolveCreateChildParent;
    World w;
    const Entity primary = w.create();
    const Entity sibling = w.create();   // also selected, alongside `primary`
    const Entity outsider = w.create();  // NOT in the selection

    const std::vector<Entity> selection{primary, sibling};

    // Right-clicking a row OUTSIDE the selection acts on that row alone (the Delete/Duplicate rule,
    // extended here) -- the child goes under `outsider`, never under `primary`.
    CHECK(resolveCreateChildParent(selection, primary, outsider) == outsider);
    // Right-clicking a row INSIDE the selection falls back to the primary.
    CHECK(resolveCreateChildParent(selection, primary, primary) == primary);
    CHECK(resolveCreateChildParent(selection, primary, sibling) == primary);
    // No specific row (a future keyboard-invoked CreateChild, `rightClicked` invalid) -> the primary.
    CHECK(resolveCreateChildParent(selection, primary, Entity{}) == primary);
    // An empty selection: any right-clicked row is trivially "outside" it.
    CHECK(resolveCreateChildParent(std::span<const Entity>{}, Entity{}, outsider) == outsider);
}

TEST_CASE("editor: destroyEntities takes whole subtrees, once each (AC-12/E8/E13)") {
    using engine::editor::destroyEntities;
    World w;
    const Entity a = w.create();
    const Entity aChild = w.create();
    const Entity b = w.create();
    REQUIRE(w.setParent(aChild, a));

    CHECK(destroyEntities(w, std::span<const Entity>{}) == 0);  // E13
    CHECK(w.entityCount() == 3);

    CHECK(destroyEntities(w, std::vector<Entity>{a, aChild}) == 1);  // E8: the child is not chased
    CHECK_FALSE(w.alive(a));
    CHECK_FALSE(w.alive(aChild));
    CHECK(w.alive(b));

    CHECK(destroyEntities(w, std::vector<Entity>{a}) == 0);  // already dead -> silent
    CHECK(destroyEntities(w, std::vector<Entity>{b}) == 1);
    CHECK(w.entityCount() == 0);
}

TEST_CASE("editor: canReparent predicts setParent, and reparentEntity never logs a predictable no (E14/E15)") {
    using engine::editor::canReparent;
    using engine::editor::reparentEntity;
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity aChild = w.create();
    const Entity dead = w.create();
    REQUIRE(w.setParent(aChild, a));
    REQUIRE(w.destroy(dead));

    CHECK(canReparent(w, b, a));              // legal
    CHECK(canReparent(w, aChild, Entity{}));  // detach is always legal (E17)
    CHECK(canReparent(w, aChild, a));         // already the parent -> a legal silent no-op (E15)
    CHECK_FALSE(canReparent(w, a, a));        // self
    CHECK_FALSE(canReparent(w, a, aChild));   // would close a cycle (E14)
    CHECK_FALSE(canReparent(w, dead, a));
    CHECK_FALSE(canReparent(w, Entity{}, a));
    CHECK_FALSE(canReparent(w, a, dead));

    CHECK(reparentEntity(w, b, a));
    CHECK(w.parent(b) == a);
    CHECK(w.childCount(a) == 2);
    CHECK_FALSE(reparentEntity(w, a, aChild));  // refused BEFORE World::setParent -> no ERROR
    CHECK(w.parent(a) == Entity{});
    CHECK(reparentEntity(w, b, Entity{}));  // unparent
    CHECK(w.parent(b) == Entity{});
    CHECK(w.childCount(a) == 1);
}

TEST_CASE("editor: reparentTargets filters a multi-select drag through topMost (review round 2, Gap 4)") {
    // The regression this closes: dragging a selected PARENT and one of its selected CHILDREN onto an
    // unrelated target must move the subtree intact -- never split, with the child peeled off into
    // the target directly instead of staying under the parent.
    using engine::editor::reparentEntity;
    using engine::editor::reparentTargets;
    World w;
    const Entity parent = w.create();
    const Entity child = w.create();
    const Entity target = w.create();
    const Entity other = w.create();
    REQUIRE(w.setParent(child, parent));

    // Both `parent` and `child` selected, `parent` is the one physically dragged: the whole selection
    // moves, but topMost() reduces it to `{parent}` alone -- `child` is COVERED by `parent`.
    const std::vector<Entity> selection{parent, child};
    CHECK(reparentTargets(w, selection, parent) == std::vector<Entity>{parent});
    // Same selection, `child` is the one physically dragged: E16 still pulls in the WHOLE selection
    // (since `child` is inside it), and topMost() still reduces it to `{parent}` -- dragging by the
    // child does not change what actually moves.
    CHECK(reparentTargets(w, selection, child) == std::vector<Entity>{parent});

    // Apply it exactly as applyPending's Reparent arm does, and confirm the subtree moved INTACT:
    // `parent` is now `target`'s child, and `child` is STILL `parent`'s child -- never re-parented to
    // `target` directly.
    for (const Entity e : reparentTargets(w, selection, parent)) {
        CHECK(reparentEntity(w, e, target));
    }
    CHECK(w.parent(parent) == target);
    CHECK(w.parent(child) == parent);  // never flattened
    CHECK(w.childCount(parent) == 1);
    CHECK(w.childCount(target) == 1);

    // Dragging a row OUTSIDE the selection moves only that row (E16), unaffected by Gap 4.
    CHECK(reparentTargets(w, selection, other) == std::vector<Entity>{other});
    // An empty selection still resolves to the dragged row alone.
    CHECK(reparentTargets(w, std::span<const Entity>{}, other) == std::vector<Entity>{other});
}

TEST_CASE("editor: duplicateEntities deep-copies subtree, name, components and order (AC-13)") {
    using engine::editor::duplicateEntities;
    World w;
    const Entity parent = w.create();
    REQUIRE(w.setName(parent, "Parent"));
    w.add<Transform>(parent, Transform{.position = {1.0F, 2.0F, 3.0F}});
    w.add<Camera>(parent, Camera{.fovYRadians = 0.5F, .nearPlane = 0.2F, .farPlane = 50.0F});

    const Entity first = w.create();
    REQUIRE(w.setName(first, "First"));
    w.add<Transform>(first, Transform{.position = {4.0F, 0.0F, 0.0F}});
    w.add<MeshRenderer>(first, MeshRenderer{.primitive = 2, .color = {0.1F, 0.2F, 0.3F}});
    REQUIRE(w.setParent(first, parent));

    const Entity second = w.create();  // deliberately UNNAMED
    w.add<DirectionalLight>(second, DirectionalLight{.color = {0.9F, 0.8F, 0.7F}, .intensity = 3.0F});
    REQUIRE(w.setParent(second, parent));

    const Entity grand = w.create();
    REQUIRE(w.setName(grand, "Grand"));
    REQUIRE(w.setParent(grand, first));

    const std::vector<Entity> copies = duplicateEntities(w, std::vector<Entity>{parent});
    REQUIRE(copies.size() == 1);
    const Entity copy = copies[0];
    CHECK(!(copy == parent));
    CHECK(w.parent(copy) == Entity{});  // E10: a root's copy is a root
    CHECK(w.name(copy) == std::string_view{"Parent"});
    REQUIRE(w.get<Transform>(copy) != nullptr);
    CHECK(w.get<Transform>(copy)->position == engine::Vec3{1.0F, 2.0F, 3.0F});
    REQUIRE(w.get<Camera>(copy) != nullptr);
    CHECK(w.get<Camera>(copy)->fovYRadians == 0.5F);
    CHECK(w.get<Camera>(copy)->farPlane == 50.0F);

    const std::vector<Entity> copyChildren = childrenOf(w, copy);
    REQUIRE(copyChildren.size() == 2);
    CHECK(w.name(copyChildren[0]) == std::string_view{"First"});  // E11 -- attach order preserved
    CHECK(w.name(copyChildren[1]).empty());
    REQUIRE(w.get<MeshRenderer>(copyChildren[0]) != nullptr);
    CHECK(w.get<MeshRenderer>(copyChildren[0])->primitive == 2);
    REQUIRE(w.get<DirectionalLight>(copyChildren[1]) != nullptr);
    CHECK(w.get<DirectionalLight>(copyChildren[1])->intensity == 3.0F);

    const std::vector<Entity> grandCopies = childrenOf(w, copyChildren[0]);
    REQUIRE(grandCopies.size() == 1);
    CHECK(w.name(grandCopies[0]) == std::string_view{"Grand"});

    CHECK(w.entityCount() == 8);               // 4 originals + 4 copies
    CHECK(childrenOf(w, parent).size() == 2);  // the source is untouched
}

TEST_CASE("editor: duplicateEntities copies a component the editor has never heard of (AC-13/E12, the D4 proof)") {
    World w;
    REQUIRE(registerComponent<Marker>(w, "test::Marker").valid());  // AFTER the five built-ins
    CHECK(w.componentTypeCount() == 6);

    const Entity source = w.create();
    REQUIRE(w.setName(source, "Marked"));
    w.add<Transform>(source, Transform{});
    REQUIRE(w.add<Marker>(source, Marker{4242}) != nullptr);

    const std::vector<Entity> copies = engine::editor::duplicateEntities(w, std::vector<Entity>{source});
    REQUIRE(copies.size() == 1);
    REQUIRE(w.has<Marker>(copies[0]));
    REQUIRE(w.get<Marker>(copies[0]) != nullptr);
    // THE assertion. Checking only has<Marker>() would still pass against a duplicate that hardcoded
    // the built-ins and happened to default-construct the rest; the VALUE is what proves the copy.
    CHECK(w.get<Marker>(copies[0])->payload == 4242);
    CHECK(w.get<Marker>(source)->payload == 4242);
}

TEST_CASE("editor: duplicateEntities filters through topMost (E9) and tolerates an empty input (E13)") {
    using engine::editor::duplicateEntities;
    World w;
    const Entity parent = w.create();
    const Entity child = w.create();
    w.add<Transform>(parent, Transform{});
    w.add<Transform>(child, Transform{});
    REQUIRE(w.setParent(child, parent));

    CHECK(duplicateEntities(w, std::span<const Entity>{}).empty());
    CHECK(w.entityCount() == 2);

    // Selecting BOTH must copy the child ONCE, inside the parent's subtree copy -- never twice.
    const std::vector<Entity> copies = duplicateEntities(w, std::vector<Entity>{parent, child});
    REQUIRE(copies.size() == 1);
    CHECK(w.entityCount() == 4);  // 2 originals + parent copy + child copy
    CHECK(childrenOf(w, copies[0]).size() == 1);

    // A non-root source's copy becomes a SIBLING of the source.
    const std::vector<Entity> siblingCopies = duplicateEntities(w, std::vector<Entity>{child});
    REQUIRE(siblingCopies.size() == 1);
    CHECK(w.parent(siblingCopies[0]) == parent);
}

// ---- RootOrder -----------------------------------------------------------------------------------

TEST_CASE("editor: RootOrder appends new roots and is stable across deletes (AC-16/I6)") {
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    ro.reconcile(w);
    CHECK(ro.entities().empty());

    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 3);
    const std::vector<Entity> first(ro.entities().begin(), ro.entities().end());

    ro.reconcile(w);  // idempotent -- no duplicates (I6)
    CHECK(std::vector<Entity>(ro.entities().begin(), ro.entities().end()) == first);

    const Entity d = w.create();  // a new root always lands LAST
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 4);
    CHECK(ro.entities()[3] == d);

    // Deleting a MIDDLE root must not reorder the survivors -- the whole reason RootOrder exists
    // (World::eachEntity walks a swap-and-popped packed array).
    REQUIRE(w.destroy(b));
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.entities()[0] == a);
    CHECK(ro.entities()[1] == c);
    CHECK(ro.entities()[2] == d);
}

TEST_CASE("editor: RootOrder tracks reparenting and survives a wholesale repopulation (E27)") {
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity a = w.create();
    const Entity b = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);

    REQUIRE(w.setParent(b, a));  // b stops being a root
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 1);
    CHECK(ro.entities()[0] == a);

    REQUIRE(w.setParent(b, Entity{}));  // and comes back, at the END
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);
    CHECK(ro.entities()[0] == a);
    CHECK(ro.entities()[1] == b);

    // E27: the World is replaced behind RootOrder's back (2.5.1 load / 2.4.2 undo). ONE reconcile
    // must produce exactly the live roots, with no dead and no duplicate entries.
    w.clear();
    for (int i = 0; i < 5; ++i) {
        (void)w.create();
    }
    ro.reconcile(w);
    const std::vector<Entity> after(ro.entities().begin(), ro.entities().end());
    CHECK(after.size() == 5);
    CHECK(after == rootsOf(w));
    for (const Entity e : after) {
        CHECK(w.alive(e));
        CHECK(w.parent(e) == Entity{});
        CHECK(std::count(after.begin(), after.end(), e) == 1);
    }

    ro.clear();
    CHECK(ro.entities().empty());
    ro.reconcile(w);  // clear() must not corrupt the stamp table
    CHECK(ro.entities().size() == 5);
}

TEST_CASE("editor: RootOrder does not mistake a recycled slot for its predecessor") {
    // Review round 2, Gap 3: `stamp` is only ever grown (in reconcile()'s pass 2) for indices STILL
    // in `order` after pass 1's dead-entry prune -- so a lone entity, reconciled once then destroyed
    // and recycled, never actually grows `stamp` past size 0, and the named generation comparison
    // (`stamp[e.index] == e.generation`) is never reached (short-circuited by `e.index < stamp.size()`
    // being trivially false both times). `keep` (index 0) survives across BOTH reconciles so its
    // presence in `order` after pass 1 of the SECOND reconcile grows `stamp` to size 2 in pass 2 --
    // which incidentally covers `victim`'s lower index (1) too, even though `victim` itself fell out
    // of `order` (and so was never re-stamped) in that same pass 1. That is what finally makes
    // `recycled.index < stamp.size()` true for a genuine reason, and puts weight on the actual value
    // comparison: `stamp[1]` is 0 (never touched this call), which must NOT equal `recycled`'s real
    // (non-zero) generation for `recycled` to be correctly re-admitted as a NEW root.
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity victim = w.create();  // index 0 -- destroyed and recycled below
    const Entity keep = w.create();    // index 1 -- stays alive through both reconciles
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);

    REQUIRE(w.destroy(victim));
    const Entity recycled = w.create();  // same index as `victim`, new generation
    REQUIRE(recycled.index == victim.index);
    REQUIRE(!(recycled == victim));

    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);
    CHECK(ro.entities()[0] == keep);      // untouched -- pass 1 preserved its position
    CHECK(ro.entities()[1] == recycled);  // appended -- generation mismatch correctly re-admits it
}

// ---- RootOrder::indexOf / RootOrder::insert (task 2.4.2, D9) --------------------------------------

TEST_CASE("editor: RootOrder::indexOf reports a tracked root's slot, or NO_ROOT_SLOT (task 2.4.2 R1)") {
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.indexOf(a) == 0);
    CHECK(ro.indexOf(b) == 1);
    CHECK(ro.indexOf(c) == 2);

    const Entity child = w.create();
    REQUIRE(w.setParent(child, a));
    CHECK(ro.indexOf(child) == engine::editor::NO_ROOT_SLOT);  // a child -- never entered `order`

    const Entity dead = w.create();
    REQUIRE(w.destroy(dead));
    CHECK(ro.indexOf(dead) == engine::editor::NO_ROOT_SLOT);  // dead -- never reconciled in, so untracked

    CHECK(ro.indexOf(Entity{}) == engine::editor::NO_ROOT_SLOT);
}

TEST_CASE("editor: RootOrder::insert restores a destroyed root to its captured slot (task 2.4.2 R2)") {
    // The shape a real root-delete undo produces: capture the slot BEFORE destroying, destroy, then
    // put the entity's IDENTITY back via World::recreate (task 2.4.2's own engine primitive) before
    // reinserting it at the captured slot -- never a fresh create().
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity a = w.create();
    const Entity middle = w.create();
    const Entity c = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 3);
    const std::size_t slot = ro.indexOf(middle);
    REQUIRE(slot == 1);

    REQUIRE(w.destroy(middle));
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);  // the dead root dropped out

    const Entity restored = w.recreate(middle);
    REQUIRE(restored == middle);
    ro.insert(restored, slot);

    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.entities()[0] == a);
    CHECK(ro.entities()[1] == middle);  // back where it was, not appended at the end
    CHECK(ro.entities()[2] == c);
}

TEST_CASE(
    "editor: RootOrder::insert clamps out-of-range indices and no-ops on invalid/duplicate input "
    "(task 2.4.2 R3/D25)") {
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity a = w.create();
    const Entity b = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);

    const Entity c = w.create();
    ro.insert(c, 999);  // clamps to size() -- an append
    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.entities()[2] == c);

    const Entity d = w.create();
    ro.insert(d, engine::editor::NO_ROOT_SLOT);  // the sentinel clamps to size() too -- an append
    REQUIRE(ro.entities().size() == 4);
    CHECK(ro.entities()[3] == d);

    const std::vector<Entity> before(ro.entities().begin(), ro.entities().end());
    ro.insert(a, 0);  // already tracked -- a no-op, not a move
    CHECK(ro.entities().size() == before.size());
    CHECK(std::vector<Entity>(ro.entities().begin(), ro.entities().end()) == before);

    ro.insert(Entity{}, 0);  // invalid handle -- a no-op
    CHECK(ro.entities().size() == before.size());
    CHECK(std::vector<Entity>(ro.entities().begin(), ro.entities().end()) == before);
}

TEST_CASE("editor: reconcile preserves an entry insert() placed, it does not re-append it (task 2.4.2 R4/F12)") {
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity a = w.create();
    const Entity b = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 2);

    const Entity c = w.create();  // a genuine root, but NOT yet reconciled in
    ro.insert(c, 1);
    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.entities()[1] == c);

    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 3);
    CHECK(ro.entities()[0] == a);
    CHECK(ro.entities()[1] == c);  // stays at index 1 -- not re-appended at the end
    CHECK(ro.entities()[2] == b);
}

// ---- walkForest (review round 2, Gap 1) -----------------------------------------------------------
//
// hierarchy_panel.cpp's drawTree is src-private and ImGui-bound, so it cannot be driven from a tier-0
// test -- but the pure explicit-stack traversal it delegates to, engine::editor::walkForest, is
// public, ImGui-free, and engine-typed, which is exactly what makes these cases able to walk a real
// multi-level forest and pin the invariants a mouse-driven GPU test never reaches (no row is ever
// actually EXPANDED anywhere else in this codebase's test suite -- see imgui_layer_test.cpp's own
// updated comment).

TEST_CASE("editor: walkForest visits a 3-level forest exactly once each, front-to-back, depth-first") {
    using engine::editor::TreeWalkEntry;
    using engine::editor::walkForest;
    World w;
    // Two roots; r0 has two children (c0, c1); c0 has one grandchild (g0) -- three levels.
    const Entity r0 = w.create();
    const Entity r1 = w.create();
    const Entity c0 = w.create();
    const Entity c1 = w.create();
    const Entity g0 = w.create();
    REQUIRE(w.setParent(c0, r0));
    REQUIRE(w.setParent(c1, r0));
    REQUIRE(w.setParent(g0, c0));

    const std::vector<Entity> roots{r0, r1};
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> arena;
    std::vector<Entity> entered;
    std::vector<Entity> unwound;
    const std::function<bool(Entity)> enter = [&entered](Entity e) {
        entered.push_back(e);
        return true;  // always descend -- ImGui's own _Leaf contract (every node reports `open`)
    };
    const std::function<void(Entity, bool)> unwind = [&unwound](Entity e, bool /*open*/) { unwound.push_back(e); };

    walkForest(w, roots, stack, arena, enter, unwind);

    // Depth-first, each node visited EXACTLY once, roots front-to-back (r0 before r1).
    CHECK(entered == std::vector<Entity>{r0, c0, g0, c1, r1});
    CHECK(unwound == std::vector<Entity>{g0, c0, c1, r0, r1});  // children unwind before their parent
    CHECK(arena.empty());  // I4: back to its pre-call size (0) once every pushed node has unwound
    CHECK(stack.empty());
}

TEST_CASE("editor: walkForest never descends into a node `enter` declines to open") {
    using engine::editor::TreeWalkEntry;
    using engine::editor::walkForest;
    World w;
    const Entity r0 = w.create();
    const Entity c0 = w.create();
    REQUIRE(w.setParent(c0, r0));

    const std::vector<Entity> roots{r0};
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> arena;
    std::vector<Entity> entered;
    std::vector<std::pair<Entity, bool>> unwound;
    const std::function<bool(Entity)> enter = [&entered](Entity e) {
        entered.push_back(e);
        return false;  // never expand
    };
    const std::function<void(Entity, bool)> unwind = [&unwound](Entity e, bool open) { unwound.emplace_back(e, open); };

    walkForest(w, roots, stack, arena, enter, unwind);

    CHECK(entered == std::vector<Entity>{r0});  // c0 is never visited -- r0 declined to open
    REQUIRE(unwound.size() == 1);
    CHECK(unwound[0] == std::pair<Entity, bool>{r0, false});
    CHECK(arena.empty());
    CHECK(stack.empty());
}

TEST_CASE("editor: walkForest is a no-op over an empty forest") {
    using engine::editor::TreeWalkEntry;
    using engine::editor::walkForest;
    const World w;
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> arena;
    int calls = 0;
    const std::function<bool(Entity)> enter = [&calls](Entity) {
        ++calls;
        return true;
    };
    const std::function<void(Entity, bool)> unwind = [&calls](Entity, bool) { ++calls; };

    walkForest(w, std::span<const Entity>{}, stack, arena, enter, unwind);

    CHECK(calls == 0);
    CHECK(stack.empty());
    CHECK(arena.empty());
}

// ---- seedDefaultScene ---------------------------------------------------------------------------

TEST_CASE("editor: seedDefaultScene builds the documented three entities (AC-18/D9/E32)") {
    using engine::editor::seedDefaultScene;
    World w;
    seedDefaultScene(w);
    CHECK(w.entityCount() == 3);

    std::vector<std::string> names;
    w.eachEntity([&](Entity e) { names.emplace_back(w.name(e)); });
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Main Camera");
    CHECK(names[1] == "Directional Light");
    CHECK(names[2] == "Cube");

    const std::vector<Entity> roots = rootsOf(w);
    REQUIRE(roots.size() == 3);
    for (const Entity e : roots) {
        CHECK(w.has<Transform>(e));  // every seeded entity is placeable
    }
    CHECK(w.componentCount<Camera>() == 1);
    CHECK(w.componentCount<DirectionalLight>() == 1);
    CHECK(w.componentCount<MeshRenderer>() == 1);
    REQUIRE(w.get<MeshRenderer>(roots[2]) != nullptr);
    CHECK(w.get<MeshRenderer>(roots[2])->primitive == 0);  // 0 == Cube (F23)

    seedDefaultScene(w);  // E32: it APPENDS, it does not reset
    CHECK(w.entityCount() == 6);
}
