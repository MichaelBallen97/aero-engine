// tests/editor/hierarchy_test.cpp — task 2.2.1: the editor's scene-state layer, tier-0 and UNGATED.
// The second TU of aero_editor_shell_test (which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here). No GPU, no window, no ImGui context, no files, no
// randomness: it must pass identically with AERO_REQUIRE_GPU unset and set (AC-19).
//
// It covers Selection (selection.hpp), the pure structural ops + RootOrder + seedDefaultScene
// (entity_ops.hpp) and PanelContext (panel_context.hpp). The HierarchyPanel itself is src-private and
// ImGui-bound: its Begin/End + TreePop balance is proven by aero_editor_imgui_test on a real GPU,
// which is the only place that assertion means anything (I3).
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- the E12/AC-13 proof
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
    CHECK(s.primary() == a);  // the LAST newly-added survivor
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

// ---- PanelContext (AC-8/D7) ----------------------------------------------------------------------

TEST_CASE("editor: PanelContext binds references, it does not copy (D7)") {
    World w;
    Selection s;
    engine::editor::PanelContext context{w, s};

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

    CHECK(canReparent(w, b, a));               // legal
    CHECK(canReparent(w, aChild, Entity{}));   // detach is always legal (E17)
    CHECK(canReparent(w, aChild, a));          // already the parent -> a legal silent no-op (E15)
    CHECK_FALSE(canReparent(w, a, a));         // self
    CHECK_FALSE(canReparent(w, a, aChild));    // would close a cycle (E14)
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

    CHECK(w.entityCount() == 8);              // 4 originals + 4 copies
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
    using engine::editor::RootOrder;
    World w;
    RootOrder ro;
    const Entity first = w.create();
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 1);

    REQUIRE(w.destroy(first));
    const Entity recycled = w.create();  // same index, new generation
    REQUIRE(recycled.index == first.index);
    REQUIRE(!(recycled == first));
    ro.reconcile(w);
    REQUIRE(ro.entities().size() == 1);
    CHECK(ro.entities()[0] == recycled);  // generation is part of the stamp
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
