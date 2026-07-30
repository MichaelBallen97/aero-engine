// tests/editor/structural_commands_test.cpp -- task 2.4.2: the component-structure commands (step 5:
// X15-X16) and the five structural entity commands (step 6: X1-X14, X17-X20). Fourteenth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated, and REFLECTION-FREE (AC-8): no line below
// names entt::meta or reflect-gen -- every case drives a REAL CommandStack with a real
// CommandContext{world, selection, roots}, no panel, no ImGui.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_commands.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/transform_command.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::Camera;
using engine::ComponentTypeId;
using engine::Entity;
using engine::LogLevel;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::AddComponentCommand;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::CreateEntityCommand;
using engine::editor::DeleteEntitiesCommand;
using engine::editor::DuplicateEntitiesCommand;
using engine::editor::LogEntry;
using engine::editor::LogSinkScope;
using engine::editor::RemoveComponentCommand;
using engine::editor::RenameEntityCommand;
using engine::editor::ReparentCommand;
using engine::editor::RootOrder;
using engine::editor::Selection;
using engine::editor::TransformCommand;
using engine::scene::internal::registerComponent;

namespace {

// File-local copies, per the established one-per-TU rule.
[[nodiscard]] std::size_t countAtLevel(const std::vector<LogEntry>& records, LogLevel level) {
    return static_cast<std::size_t>(
        std::count_if(records.begin(), records.end(), [level](const LogEntry& e) { return e.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// X16(b)'s probe: a genuinely empty (tag) type the private ComponentSnapshot store cannot mirror
// (F10/H3 -- see scene_snapshot_test.cpp's N6/N11 comments for the full reasoning).
struct X16Tag {};

}  // namespace

TEST_CASE("structural_commands: AddComponentCommand (X15/AC-12..AC-15/A5)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    const ComponentTypeId cameraId = world.findComponentType("engine::Camera");
    REQUIRE(cameraId.valid());

    SUBCASE("push adds; undo removes; redo re-adds") {
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(world.has<Camera>(e));
        REQUIRE(stack.undo(ctx));
        CHECK_FALSE(world.has<Camera>(e));
        REQUIRE(stack.redo(ctx));
        CHECK(world.has<Camera>(e));
    }

    SUBCASE("the label is \"Add <Short>\", byte-identical across push/undo/redo") {
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(stack.undoLabel() == "Add Camera");
        REQUIRE(stack.undo(ctx));
        CHECK(stack.redoLabel() == "Add Camera");
        REQUIRE(stack.redo(ctx));
        CHECK(stack.undoLabel() == "Add Camera");
    }

    SUBCASE("push onto a type already present: one ERROR + one WARN (A5), nothing recorded, untouched") {
        const LogFixture fixture;
        const LogSinkScope scope;
        std::vector<LogEntry> records;
        REQUIRE(world.add<Camera>(e, Camera{.fovYRadians = 1.5F}) != nullptr);
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        CHECK_FALSE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Error) == 1);  // component_ops' own refusal ERROR
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);   // the stack's own WARN
        CHECK(stack.count() == 0);
        REQUIRE(world.has<Camera>(e));
        CHECK(world.get<Camera>(e)->fovYRadians == doctest::Approx(1.5F));  // untouched
    }

    SUBCASE("a dead target: false, nothing recorded") {
        REQUIRE(world.destroy(e));
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(stack.count() == 0);
    }
}

TEST_CASE("structural_commands: RemoveComponentCommand (X16/AC-12..AC-15/D18/E10)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());

    SUBCASE("undo restores BOTH non-default field values exactly (S14's discriminator)") {
        const Transform t{.position = Vec3{7.0F, 8.0F, 9.0F}, .scale = Vec3{3.0F, 3.0F, 3.0F}};
        REQUIRE(world.add<Transform>(e, t) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK_FALSE(world.has<Transform>(e));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        CHECK(*world.get<Transform>(e) == t);
    }

    // A5-shaped finding, own to this task rather than the spec's literal wording: D18's own "capture
    // BEFORE removing" reasoning means an UNMIRRORABLE type (F10 -- none of the five engine built-ins
    // is a tag; see scene_snapshot_test.cpp's N6/N11) is refused ENTIRELY, never silently zeroed. The
    // spec/plan's literal "(b) a tag component round-trips by presence" is unreachable today for a
    // non-built-in type (D24 says so of the sibling SubtreeSnapshot case); this proves the refusal is
    // safe rather than claiming a round trip the current architecture cannot deliver.
    SUBCASE("a component type this snapshot cannot mirror is refused entirely -- nothing removed (D18/H3)") {
        const ComponentTypeId tagId = registerComponent<X16Tag>(world, "X16.Tag");
        REQUIRE(tagId.valid());
        world.add<X16Tag>(e);
        REQUIRE(world.has<X16Tag>(e));
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, tagId, "X16.Tag")));
        CHECK(world.has<X16Tag>(e));  // untouched
        CHECK(stack.count() == 0);
    }

    SUBCASE("undo -> redo -> undo: each redo re-captures the CURRENT value, not the original") {
        REQUIRE(world.add<Transform>(e, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        world.get<Transform>(e)->position = Vec3{2.0F, 0.0F, 0.0F};  // an intervening write (A19)

        REQUIRE(stack.redo(ctx));  // must capture {2,0,0} fresh, not replay the original {1,0,0}
        CHECK_FALSE(world.has<Transform>(e));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        CHECK(world.get<Transform>(e)->position == Vec3{2.0F, 0.0F, 0.0F});
    }

    SUBCASE("an absent component: redo returns false, removes nothing") {
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK(stack.count() == 0);
    }

    SUBCASE("the label is \"Remove <Short>\"") {
        REQUIRE(world.add<Transform>(e) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK(stack.undoLabel() == "Remove Transform");
    }
}

// ---- CreateEntityCommand (X1-X3) ------------------------------------------------------------------

TEST_CASE(
    "structural_commands: X1 -- push CREATES with the seam's default Transform and the parent "
    "link; selection follows (D5/AC-16)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    const Entity other = world.create();
    selection.set(other);

    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<CreateEntityCommand>(parent, "Child", selection.entities())));
    REQUIRE(stack.count() == 1);
    // No accessor for `created()` off the stack -- read it back through the World's own effects, the
    // same shape a panel uses (the command's own identity is private to it).
    std::vector<Entity> children;
    world.eachChild(parent, [&children](Entity c) { children.push_back(c); });
    REQUIRE(children.size() == 1);
    const Entity created = children.front();
    CHECK(world.has<Transform>(created));
    CHECK(world.parent(created) == parent);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == created);
}

TEST_CASE(
    "structural_commands: X2 -- undo destroys the created entity and restores the previous "
    "selection exactly") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    const Entity other = world.create();
    selection.set(other);

    CommandStack stack;
    auto owned = std::make_unique<CreateEntityCommand>(parent, "Child", selection.entities());
    CreateEntityCommand* const cmdPtr = owned.get();
    REQUIRE(stack.push(ctx, std::move(owned)));
    const Entity created = cmdPtr->created();

    REQUIRE(stack.undo(ctx));
    CHECK_FALSE(world.alive(created));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == other);
}

TEST_CASE(
    "structural_commands: X3 -- redo restores the SAME handle, and the selection follows "
    "(S16's discriminator, D21)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();

    CommandStack stack;
    auto owned = std::make_unique<CreateEntityCommand>(parent, "Child", selection.entities());
    CreateEntityCommand* const cmdPtr = owned.get();
    REQUIRE(stack.push(ctx, std::move(owned)));
    const Entity created = cmdPtr->created();

    REQUIRE(stack.undo(ctx));
    REQUIRE(stack.redo(ctx));
    CHECK(cmdPtr->created() == created);
    CHECK(world.alive(created));
    CHECK(world.parent(created) == parent);
    CHECK(selection.primary() == created);
}

// ---- DeleteEntitiesCommand (X4-X7) ----------------------------------------------------------------

TEST_CASE(
    "structural_commands: X4 -- one entity: redo destroys, undo restores handle + name + "
    "components (S11's discriminator)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    REQUIRE(world.setName(e, "Solo"));
    REQUIRE(world.add<Transform>(e, Transform{.position = Vec3{1.0F, 2.0F, 3.0F}}) != nullptr);

    CommandStack stack;
    const std::vector<Entity> targets{e};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{e})));
    CHECK_FALSE(world.alive(e));

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(e));
    CHECK(world.name(e) == "Solo");
    REQUIRE(world.has<Transform>(e));
    CHECK(world.get<Transform>(e)->position == Vec3{1.0F, 2.0F, 3.0F});
}

TEST_CASE(
    "structural_commands: X5 -- a whole three-level subtree: every handle, link and eachChild "
    "order back") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity root = world.create();
    const Entity c0 = world.create();
    const Entity c1 = world.create();
    REQUIRE(world.setParent(c0, root));
    REQUIRE(world.setParent(c1, root));
    const Entity g0 = world.create();
    REQUIRE(world.setParent(g0, c1));

    CommandStack stack;
    const std::vector<Entity> targets{root};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{root})));
    CHECK_FALSE(world.alive(root));

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(root));
    CHECK(world.alive(c0));
    CHECK(world.alive(c1));
    CHECK(world.alive(g0));
    std::vector<Entity> rootChildren;
    world.eachChild(root, [&rootChildren](Entity c) { rootChildren.push_back(c); });
    REQUIRE(rootChildren.size() == 2);
    CHECK(rootChildren[0] == c0);
    CHECK(rootChildren[1] == c1);
    std::vector<Entity> c1Children;
    world.eachChild(c1, [&c1Children](Entity c) { c1Children.push_back(c); });
    REQUIRE(c1Children.size() == 1);
    CHECK(c1Children[0] == g0);
}

TEST_CASE(
    "structural_commands: X6 -- a parent AND one of its own children in the target set: "
    "topMost collapses them") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    const Entity child = world.create();
    REQUIRE(world.setParent(child, parent));

    CommandStack stack;
    const std::vector<Entity> targets{parent, child};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
    CHECK_FALSE(world.alive(parent));
    CHECK_FALSE(world.alive(child));

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(parent));
    CHECK(world.alive(child));
    CHECK(world.parent(child) == parent);
}

TEST_CASE(
    "structural_commands: X7 -- a root delete: undo puts it back at its old RootOrder slot, "
    "selection exact (S12/S13's discriminator)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity a = world.create();
    const Entity middle = world.create();
    const Entity c = world.create();
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 3);
    REQUIRE(roots.indexOf(middle) == 1);
    selection.set(middle);
    const std::vector<Entity> selectionBefore(selection.entities().begin(), selection.entities().end());

    CommandStack stack;
    const std::vector<Entity> targets{middle};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, selectionBefore)));
    CHECK_FALSE(world.alive(middle));
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 2);

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(middle));
    REQUIRE(roots.entities().size() == 3);
    CHECK(roots.entities()[0] == a);
    CHECK(roots.entities()[1] == middle);  // back at slot 1, not appended at the end
    CHECK(roots.entities()[2] == c);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == middle);
}

// X21 -- code-review Gap 1 (task 2.4.2). X7 alone cannot catch this: it deletes exactly ONE root,
// so `restoreState`'s RootOrder-reinsert pass never has a second entry to corrupt. Two ROOTS deleted
// TOGETHER, given to the command in DESCENDING slot order -- exactly what selecting the lower one
// before the higher one produces -- must still come back in ascending display order, with the
// untouched roots on either side of them exactly where they were.
TEST_CASE(
    "structural_commands: X21 -- two roots deleted together in descending slot order restore "
    "in original RootOrder, untouched roots unmoved (code-review Gap 1)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity r0 = world.create();
    const Entity r1 = world.create();
    const Entity r2 = world.create();
    const Entity r3 = world.create();
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 4);
    REQUIRE(roots.indexOf(r1) == 1);
    REQUIRE(roots.indexOf(r2) == 2);

    CommandStack stack;
    // DESCENDING slot order: r2 (slot 2) named before r1 (slot 1).
    const std::vector<Entity> targets{r2, r1};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
    CHECK_FALSE(world.alive(r1));
    CHECK_FALSE(world.alive(r2));
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 2);

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(r1));
    CHECK(world.alive(r2));
    REQUIRE(roots.entities().size() == 4);
    CHECK(roots.entities()[0] == r0);
    CHECK(roots.entities()[1] == r1);
    CHECK(roots.entities()[2] == r2);
    CHECK(roots.entities()[3] == r3);  // the UNTOUCHED root -- this is what a wrong replay order moves
}

// X23 -- X21's ASCENDING mirror, added by a SECOND code-review round (task 2.4.2) alongside N15:
// `restoreState`'s own `RootOrder::insert` sort-then-replay was independently verified correct in
// both directions (`RootOrder::insert` is a genuine positional `vector::insert` with no positional
// precondition, unlike `placeAt`), but the process that let N15's regression through a green gate
// mandates a mirror at every order-sensitive site regardless, so this direction is pinned too.
TEST_CASE(
    "structural_commands: X23 -- two roots deleted together in ASCENDING slot order restore "
    "in original RootOrder, untouched roots unmoved (X21's ascending mirror)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity r0 = world.create();
    const Entity r1 = world.create();
    const Entity r2 = world.create();
    const Entity r3 = world.create();
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 4);
    REQUIRE(roots.indexOf(r1) == 1);
    REQUIRE(roots.indexOf(r2) == 2);

    CommandStack stack;
    // ASCENDING slot order: r1 (slot 1) named before r2 (slot 2).
    const std::vector<Entity> targets{r1, r2};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
    CHECK_FALSE(world.alive(r1));
    CHECK_FALSE(world.alive(r2));
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 2);

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(r1));
    CHECK(world.alive(r2));
    REQUIRE(roots.entities().size() == 4);
    CHECK(roots.entities()[0] == r0);
    CHECK(roots.entities()[1] == r1);
    CHECK(roots.entities()[2] == r2);
    CHECK(roots.entities()[3] == r3);  // the UNTOUCHED root -- this is what a wrong replay order moves
}

// ---- DuplicateEntitiesCommand (X8-X9) -------------------------------------------------------------

TEST_CASE(
    "structural_commands: X8 -- redo duplicates as siblings; undo removes the copies; "
    "selection follows both ways (AC-18)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    const Entity source = world.create();
    REQUIRE(world.setParent(source, parent));
    REQUIRE(world.setName(source, "Source"));
    REQUIRE(world.add<Transform>(source, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
    selection.set(source);

    CommandStack stack;
    const std::vector<Entity> sources{source};
    const std::vector<Entity> selectionBefore(selection.entities().begin(), selection.entities().end());
    REQUIRE(stack.push(ctx, std::make_unique<DuplicateEntitiesCommand>(sources, selectionBefore)));
    std::vector<Entity> children;
    world.eachChild(parent, [&children](Entity c) { children.push_back(c); });
    REQUIRE(children.size() == 2);
    const Entity copy = (children[0] == source) ? children[1] : children[0];
    CHECK(world.parent(copy) == parent);
    CHECK(world.name(copy) == "Source");
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == copy);

    REQUIRE(stack.undo(ctx));
    CHECK_FALSE(world.alive(copy));
    CHECK(world.alive(source));
    CHECK(selection.primary() == source);
}

TEST_CASE(
    "structural_commands: X9 -- undo -> redo restores the copy WITH its original handle; an "
    "older command still applies (E12/D21)") {
    // A TransformCommand, not a SetFieldCommand: this TU is REFLECTION-FREE (AC-8), and
    // SetFieldCommand's write path needs entt::meta -- TransformCommand's transform_ops seam does
    // not, which is exactly what makes it the reflection-free "SetFieldCommand-shaped write" this
    // case needs.
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    const Entity source = world.create();
    REQUIRE(world.setParent(source, parent));
    REQUIRE(world.add<Transform>(source, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);

    CommandStack stack;
    stack.breakMergeChain();
    const std::vector<Entity> sources{source};
    REQUIRE(stack.push(ctx, std::make_unique<DuplicateEntitiesCommand>(sources, std::vector<Entity>{})));
    stack.breakMergeChain();
    std::vector<Entity> children;
    world.eachChild(parent, [&children](Entity c) { children.push_back(c); });
    REQUIRE(children.size() == 2);
    const Entity copy = (children[0] == source) ? children[1] : children[0];

    const Transform beforeMove = *world.get<Transform>(copy);
    const Transform afterMove{.position = Vec3{9.0F, 9.0F, 9.0F}};
    REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(copy, beforeMove, afterMove)));

    REQUIRE(stack.undo(ctx));  // undoes the TransformCommand
    REQUIRE(stack.undo(ctx));  // undoes the DuplicateEntitiesCommand -- `copy` is destroyed
    CHECK_FALSE(world.alive(copy));
    REQUIRE(stack.redo(ctx));  // re-duplicates -- `copy` comes back with the SAME handle (D21/E12)
    CHECK(world.alive(copy));
    REQUIRE(stack.redo(ctx));  // the TransformCommand's redo still names `copy` -- it must APPLY
    REQUIRE(world.has<Transform>(copy));
    CHECK(world.get<Transform>(copy)->position == Vec3{9.0F, 9.0F, 9.0F});
}

// ---- ReparentCommand (X10-X12) --------------------------------------------------------------------

TEST_CASE(
    "structural_commands: X10 -- reparent to another parent: undo restores the old parent AND "
    "the old position (S15's discriminator)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity oldParent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 3; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, oldParent));
        kids.push_back(k);
    }
    const Entity target = kids[1];  // the middle child
    const Entity newParent = world.create();

    CommandStack stack;
    const std::vector<Entity> targets{target};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, newParent)));
    CHECK(world.parent(target) == newParent);

    REQUIRE(stack.undo(ctx));
    CHECK(world.parent(target) == oldParent);
    std::vector<Entity> after;
    world.eachChild(oldParent, [&after](Entity c) { after.push_back(c); });
    REQUIRE(after.size() == 3);
    CHECK(after[1] == target);  // back at its OLD index, never just appended at the end
}

TEST_CASE("structural_commands: X11 -- reparent to root: undo restores the parent link") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity oldParent = world.create();
    const Entity target = world.create();
    REQUIRE(world.setParent(target, oldParent));

    CommandStack stack;
    const std::vector<Entity> targets{target};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, Entity{})));
    CHECK_FALSE(world.parent(target).valid());

    REQUIRE(stack.undo(ctx));
    CHECK(world.parent(target) == oldParent);
}

TEST_CASE(
    "structural_commands: X12 -- multi-target reparent, undone in reverse: every target back "
    "at its own old parent and slot") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parentA = world.create();
    const Entity parentB = world.create();
    const Entity a0 = world.create();
    REQUIRE(world.setParent(a0, parentA));
    const Entity a1 = world.create();
    REQUIRE(world.setParent(a1, parentA));
    const Entity b0 = world.create();
    REQUIRE(world.setParent(b0, parentB));
    const Entity newParent = world.create();

    CommandStack stack;
    const std::vector<Entity> targets{a1, b0};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, newParent)));
    CHECK(world.parent(a1) == newParent);
    CHECK(world.parent(b0) == newParent);

    REQUIRE(stack.undo(ctx));
    CHECK(world.parent(a1) == parentA);
    CHECK(world.parent(b0) == parentB);
    std::vector<Entity> aChildren;
    world.eachChild(parentA, [&aChildren](Entity c) { aChildren.push_back(c); });
    REQUIRE(aChildren.size() == 2);
    CHECK(aChildren[0] == a0);
    CHECK(aChildren[1] == a1);
    std::vector<Entity> bChildren;
    world.eachChild(parentB, [&bChildren](Entity c) { bChildren.push_back(c); });
    REQUIRE(bChildren.size() == 1);
    CHECK(bChildren[0] == b0);
}

// X22 -- code-review Gap 1 (task 2.4.2). X12 deliberately uses two DIFFERENT parents, which is
// exactly why it is blind here: reversing the walk order is invisible when nothing shares a parent.
// Two targets that are SIBLINGS OF ONE PARENT, given in the ORDINARY in-row-order a multi-select
// produces (ascending slot), must both come back at their own old slot once undone -- asserted over
// the WHOLE child list, including the two untouched siblings either side of them.
TEST_CASE(
    "structural_commands: X22 -- two-target reparent whose targets share ONE parent: both back "
    "at their own old slot on undo (code-review Gap 1)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 4; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity k0 = kids[0];
    const Entity k1 = kids[1];
    const Entity k2 = kids[2];
    const Entity k3 = kids[3];
    const Entity newParent = world.create();

    CommandStack stack;
    // ORDINARY in-row-order selection: k1 (slot 1) named before k2 (slot 2).
    const std::vector<Entity> targets{k1, k2};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, newParent)));
    CHECK(world.parent(k1) == newParent);
    CHECK(world.parent(k2) == newParent);

    REQUIRE(stack.undo(ctx));
    CHECK(world.parent(k1) == parent);
    CHECK(world.parent(k2) == parent);
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity c) { after.push_back(c); });
    REQUIRE(after.size() == 4);
    CHECK(after[0] == k0);
    CHECK(after[1] == k1);
    CHECK(after[2] == k2);
    CHECK(after[3] == k3);  // the UNTOUCHED sibling -- this is what a wrong replay order moves
}

// X24 -- X22's DESCENDING mirror, added by the same second code-review round as X23/N15/N16.
// `ReparentCommand::undo` was independently verified correct in both directions (its own `setParent`
// and `placeAt` calls are already adjacent, the shape N15's fix restores in `SubtreeSnapshot::
// restore`), but every order-sensitive site now gets a mirror in both directions rather than one.
TEST_CASE(
    "structural_commands: X24 -- two-target reparent whose targets share ONE parent, given in "
    "DESCENDING slot order: both back at their own old slot on undo (X22's descending mirror)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 4; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity k0 = kids[0];
    const Entity k1 = kids[1];
    const Entity k2 = kids[2];
    const Entity k3 = kids[3];
    const Entity newParent = world.create();

    CommandStack stack;
    // DESCENDING slot order: k2 (slot 2) named before k1 (slot 1).
    const std::vector<Entity> targets{k2, k1};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, newParent)));
    CHECK(world.parent(k1) == newParent);
    CHECK(world.parent(k2) == newParent);

    REQUIRE(stack.undo(ctx));
    CHECK(world.parent(k1) == parent);
    CHECK(world.parent(k2) == parent);
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity c) { after.push_back(c); });
    REQUIRE(after.size() == 4);
    CHECK(after[0] == k0);
    CHECK(after[1] == k1);
    CHECK(after[2] == k2);
    CHECK(after[3] == k3);  // the UNTOUCHED sibling -- this is what a wrong replay order moves
}

// ---- RenameEntityCommand (X13-X14) ----------------------------------------------------------------

TEST_CASE("structural_commands: X13 -- rename, both directions, exact strings") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    REQUIRE(world.setName(e, "Old"));

    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(e, std::string("Old"), std::string("New"))));
    CHECK(world.name(e) == "New");
    REQUIRE(stack.undo(ctx));
    CHECK(world.name(e) == "Old");
    REQUIRE(stack.redo(ctx));
    CHECK(world.name(e) == "New");
}

TEST_CASE(
    "structural_commands: X14 -- rename's empty-name clear, both directions, and a dead "
    "target (E13)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    SUBCASE("the empty-name clear, both directions") {
        const Entity e = world.create();
        REQUIRE(world.setName(e, "Named"));
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(e, std::string("Named"), std::string(""))));
        CHECK(world.name(e).empty());
        REQUIRE(stack.undo(ctx));
        CHECK(world.name(e) == "Named");
    }

    SUBCASE("a dead target: false, nothing mutated, exactly one WARN through the stack") {
        const LogFixture fixture;
        const LogSinkScope scope;
        std::vector<LogEntry> records;
        const Entity e = world.create();
        REQUIRE(world.destroy(e));
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        CHECK_FALSE(stack.push(ctx, std::make_unique<RenameEntityCommand>(e, std::string("A"), std::string("B"))));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(stack.count() == 0);
    }
}

// ---- the failure contract, one SUBCASE per command (X17) -------------------------------------------

TEST_CASE(
    "structural_commands: X17 -- the failure contract: dead/absent target, one WARN, nothing "
    "mutated, one SUBCASE per command (AC-23)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const LogSinkScope scope;
    std::vector<LogEntry> records;

    SUBCASE("CreateEntityCommand: a dead parent") {
        World world;
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        const Entity deadParent = world.create();
        REQUIRE(world.destroy(deadParent));
        const std::size_t before = world.entityCount();
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        CHECK_FALSE(stack.push(ctx, std::make_unique<CreateEntityCommand>(deadParent, "X", std::vector<Entity>{})));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(world.entityCount() == before);
        CHECK(stack.count() == 0);
    }

    SUBCASE("DeleteEntitiesCommand: an already-dead target") {
        World world;
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        const Entity e = world.create();
        REQUIRE(world.destroy(e));
        const std::size_t before = world.entityCount();
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        const std::vector<Entity> targets{e};
        CHECK_FALSE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(world.entityCount() == before);
        CHECK(stack.count() == 0);
    }

    SUBCASE("DuplicateEntitiesCommand: an already-dead source") {
        World world;
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        const Entity e = world.create();
        REQUIRE(world.destroy(e));
        const std::size_t before = world.entityCount();
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        const std::vector<Entity> sources{e};
        CHECK_FALSE(stack.push(ctx, std::make_unique<DuplicateEntitiesCommand>(sources, std::vector<Entity>{})));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(world.entityCount() == before);
        CHECK(stack.count() == 0);
    }

    SUBCASE("ReparentCommand: a dead target") {
        World world;
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        const Entity e = world.create();
        REQUIRE(world.destroy(e));
        const Entity newParent = world.create();
        const std::size_t before = world.entityCount();
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        const std::vector<Entity> targets{e};
        CHECK_FALSE(stack.push(ctx, std::make_unique<ReparentCommand>(targets, newParent)));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(world.entityCount() == before);
        CHECK(stack.count() == 0);
    }

    SUBCASE("RenameEntityCommand: a dead target") {
        World world;
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        const Entity e = world.create();
        REQUIRE(world.destroy(e));
        const std::size_t before = world.entityCount();
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();
        CHECK_FALSE(stack.push(ctx, std::make_unique<RenameEntityCommand>(e, std::string("A"), std::string("B"))));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);
        CHECK(world.entityCount() == before);
        CHECK(stack.count() == 0);
    }
}

// ---- X18: the Definition-of-Done case --------------------------------------------------------------

TEST_CASE("structural_commands: the Definition-of-Done case -- 5 commands, undo-to-start, redo-to-identical (X18)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    const Entity group = world.create();
    REQUIRE(world.setName(group, "Group"));
    const Entity cube = world.create();
    REQUIRE(world.setName(cube, "Cube"));
    REQUIRE(world.add<Transform>(cube, Transform{.position = Vec3{1.0F, 1.0F, 1.0F}}) != nullptr);
    roots.reconcile(world);
    const std::size_t startCount = world.entityCount();

    CommandStack stack;
    stack.breakMergeChain();

    auto createOwned = std::make_unique<CreateEntityCommand>(Entity{}, "Sphere", std::vector<Entity>{});
    CreateEntityCommand* const createCmd = createOwned.get();
    REQUIRE(stack.push(ctx, std::move(createOwned)));
    stack.breakMergeChain();
    const Entity sphere = createCmd->created();
    REQUIRE(sphere.valid());

    const Transform beforeMove = *world.get<Transform>(sphere);
    const Transform afterMove{.position = Vec3{5.0F, 0.0F, 0.0F}};
    REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(sphere, beforeMove, afterMove)));
    stack.breakMergeChain();

    REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(sphere, std::string("Sphere"), std::string("Ball"))));
    stack.breakMergeChain();

    const std::vector<Entity> deleteTargets{cube};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(deleteTargets, std::vector<Entity>{})));
    stack.breakMergeChain();

    const std::vector<Entity> reparentTargets{sphere};
    REQUIRE(stack.push(ctx, std::make_unique<ReparentCommand>(reparentTargets, group)));
    stack.breakMergeChain();

    CHECK(stack.count() == 5);
    CHECK(stack.appliedCount() == 5);

    REQUIRE(world.alive(group));
    REQUIRE(world.alive(sphere));
    CHECK_FALSE(world.alive(cube));
    CHECK(world.name(sphere) == "Ball");
    CHECK(world.parent(sphere) == group);
    REQUIRE(world.has<Transform>(sphere));
    CHECK(world.get<Transform>(sphere)->position == Vec3{5.0F, 0.0F, 0.0F});
    const std::size_t appliedEntityCount = world.entityCount();

    for (int i = 0; i < 5; ++i) {
        REQUIRE(stack.undo(ctx));
    }
    CHECK(stack.appliedCount() == 0);
    CHECK_FALSE(world.alive(sphere));
    REQUIRE(world.alive(group));
    REQUIRE(world.alive(cube));
    CHECK(world.name(group) == "Group");
    CHECK(world.name(cube) == "Cube");
    REQUIRE(world.has<Transform>(cube));
    CHECK(world.get<Transform>(cube)->position == Vec3{1.0F, 1.0F, 1.0F});
    CHECK_FALSE(world.parent(cube).valid());
    CHECK(world.entityCount() == startCount);

    for (int i = 0; i < 5; ++i) {
        REQUIRE(stack.redo(ctx));
    }
    CHECK(stack.appliedCount() == 5);
    REQUIRE(world.alive(group));
    REQUIRE(world.alive(sphere));
    CHECK_FALSE(world.alive(cube));
    CHECK(world.name(sphere) == "Ball");
    CHECK(world.parent(sphere) == group);
    REQUIRE(world.has<Transform>(sphere));
    CHECK(world.get<Transform>(sphere)->position == Vec3{5.0F, 0.0F, 0.0F});
    CHECK(world.entityCount() == appliedEntityCount);
}

// ---- X19: identity under the stack (S1) -------------------------------------------------------------

TEST_CASE("structural_commands: identity under the stack -- move, delete, undo, undo (X19/S1/D2)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    const Entity cube = world.create();
    REQUIRE(world.add<Transform>(cube, Transform{}) != nullptr);
    const Transform beforeMove = *world.get<Transform>(cube);
    const Transform afterMove{.position = Vec3{3.0F, 0.0F, 0.0F}};

    CommandStack stack;
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(cube, beforeMove, afterMove)));
    stack.breakMergeChain();
    const std::vector<Entity> targets{cube};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
    CHECK_FALSE(world.alive(cube));

    REQUIRE(stack.undo(ctx));  // undoes the delete -- cube comes back at afterMove, SAME handle (D2)
    REQUIRE(world.alive(cube));
    REQUIRE(world.has<Transform>(cube));
    CHECK(world.get<Transform>(cube)->position == afterMove.position);

    REQUIRE(stack.undo(ctx));  // undoes the move -- MUST apply, not WARN: the handle never changed
    REQUIRE(world.has<Transform>(cube));
    CHECK(world.get<Transform>(cube)->position == beforeMove.position);
}

// ---- X20: no RootOrder entries at all (E5) -----------------------------------------------------------

TEST_CASE("structural_commands: a delete with NO RootOrder ever reconciled is a tested degrade (X20/E5)") {
    World world;
    Selection selection;
    RootOrder roots;  // deliberately never reconciled -- E5's "the panel is not there"
    CommandContext ctx{world, selection, roots};

    const Entity e = world.create();
    CommandStack stack;
    const std::vector<Entity> targets{e};
    REQUIRE(stack.push(ctx, std::make_unique<DeleteEntitiesCommand>(targets, std::vector<Entity>{})));
    CHECK_FALSE(world.alive(e));

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(e));
    CHECK(roots.entities().empty());  // still never reconciled -- nothing crashed, nothing inserted
}
