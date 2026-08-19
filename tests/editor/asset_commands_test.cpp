// tests/editor/asset_commands_test.cpp -- task 3.1.5, Step 9: InstantiateAssetCommand (IA1-IA14),
// the sixth structural command and the first that creates more than one entity. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED and REFLECTION-FREE (the structural_commands_test.cpp posture): every case drives a REAL
// CommandStack against a real CommandContext{world, selection, roots}, with no panel and no ImGui.
// Tier-0: no GPU, no window, no disk.
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md). No toString overload is added
// anywhere.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_commands.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/instantiate_plan.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::Entity;
using engine::Guid;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::INSTANTIATE_ASSET_COMMAND_LABEL;
using engine::editor::InstantiateAssetCommand;
using engine::editor::InstantiatePlan;
using engine::editor::InstantiatePlanNode;
using engine::editor::RenameEntityCommand;
using engine::editor::RootOrder;
using engine::editor::Selection;

namespace {

constexpr Guid MODEL_GUID{0x00FF00FF00FF00FFULL, 0x1234567890ABCDEFULL};

// A three-slot plan shaped exactly as the planner emits one: slot 0 the synthetic root with identity
// TRS and no mesh, then two children of it, the second carrying a mesh reference. Built by hand rather
// than through buildInstantiatePlan so this battery tests the COMMAND and not the planner.
[[nodiscard]] InstantiatePlan threeSlotPlan() {
    InstantiatePlan plan;
    plan.ok = true;
    plan.nodes.push_back(InstantiatePlanNode{.name = "chair", .parentSlot = 0});
    plan.nodes.push_back(InstantiatePlanNode{.name = "Frame",
                                             .parentSlot = 0,
                                             .translation = Vec3{1.0F, 2.0F, 3.0F},
                                             .rotation = engine::normalize(Quat{0.2F, 0.0F, 0.0F, 0.9F}),
                                             .scale = Vec3{2.0F, 0.5F, 4.0F}});
    plan.nodes.push_back(InstantiatePlanNode{.name = "Seat",
                                             .parentSlot = 1,
                                             .translation = Vec3{-4.0F, 0.25F, 0.0F},
                                             .scale = Vec3{1.0F, 1.0F, 1.0F},
                                             .mesh = MODEL_GUID,
                                             .meshIndex = 7});
    return plan;
}

[[nodiscard]] Transform dropPlacement() { return Transform{.position = Vec3{10.0F, 0.0F, -5.0F}}; }

[[nodiscard]] std::size_t liveEntityCount(const World& world) {
    std::size_t count = 0;
    world.eachEntity([&count](Entity) { ++count; });
    return count;
}

[[nodiscard]] Entity childNamed(const World& world, Entity parent, std::string_view name) {
    Entity found;
    world.eachChild(parent, [&](Entity child) {
        if (world.name(child) == name) {
            found = child;
        }
    });
    return found;
}

}  // namespace

TEST_CASE("asset_commands: a 3-slot plan creates 3 entities with the planned names (IA1/IA2)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const std::size_t before = liveEntityCount(world);
    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    CHECK(liveEntityCount(world) == before + 3);
    const Entity root = raw->createdRoot();
    REQUIRE(root.valid());
    CHECK(world.name(root) == "chair");
    CHECK_FALSE(world.parent(root).valid());  // a scene-root drop

    // IA2: parents match parentSlot.
    const Entity frame = childNamed(world, root, "Frame");
    REQUIRE(frame.valid());
    const Entity seat = childNamed(world, frame, "Seat");
    REQUIRE(seat.valid());
    CHECK(world.parent(frame) == root);
    CHECK(world.parent(seat) == frame);
}

TEST_CASE("asset_commands: non-root TRS matches the plan bit for bit (IA3)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;
    const InstantiatePlan plan = threeSlotPlan();

    auto command =
        std::make_unique<InstantiateAssetCommand>(plan, Entity{}, dropPlacement(), std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    const Entity frame = childNamed(world, root, "Frame");
    REQUIRE(frame.valid());
    const Transform* frameTransform = world.get<Transform>(frame);
    REQUIRE(frameTransform != nullptr);
    CHECK(frameTransform->position == plan.nodes[1].translation);
    CHECK(frameTransform->rotation == plan.nodes[1].rotation);
    CHECK(frameTransform->scale == plan.nodes[1].scale);
}

TEST_CASE("asset_commands: the root's Transform is the DROP placement, not identity (IA4)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Transform* rootTransform = world.get<Transform>(raw->createdRoot());
    REQUIRE(rootTransform != nullptr);
    CHECK(rootTransform->position == dropPlacement().position);
    // ... and the plan's OWN synthetic-root TRS (identity) was deliberately ignored, which is what
    // makes one plan reusable for a viewport drop and a hierarchy-row drop.
    CHECK_FALSE(rootTransform->position == Vec3{});
}

TEST_CASE("asset_commands: MeshRenderer exists exactly on planned nodes, with (mesh, meshIndex) (IA5)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    const Entity frame = childNamed(world, root, "Frame");
    REQUIRE(frame.valid());
    const Entity seat = childNamed(world, frame, "Seat");
    REQUIRE(seat.valid());

    CHECK_FALSE(world.has<MeshRenderer>(root));   // the synthetic root carries no mesh
    CHECK_FALSE(world.has<MeshRenderer>(frame));  // nor does a plain group node
    REQUIRE(world.has<MeshRenderer>(seat));
    const MeshRenderer* renderer = world.get<MeshRenderer>(seat);
    REQUIRE(renderer != nullptr);
    CHECK(renderer->mesh == MODEL_GUID);
    CHECK(renderer->meshIndex == 7);
    // primitive/color stay DEFAULT, so an unresolved reference draws nothing rather than a stray cube
    CHECK(renderer->primitive == 0);
    CHECK(renderer->color == Vec3::one());
}

TEST_CASE("asset_commands: the created root returns to its own display row on redo (IA6, S11)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity a = world.create();
    const Entity b = world.create();
    REQUIRE(world.setName(a, "A"));
    REQUIRE(world.setName(b, "B"));
    roots.reconcile(world);
    REQUIRE(roots.indexOf(a) == 0);
    REQUIRE(roots.indexOf(b) == 1);

    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    roots.reconcile(world);  // EditorApp::tick()'s own reconcile -- the first redo inserts NOTHING
    const Entity root = raw->createdRoot();
    REQUIRE(roots.indexOf(root) == 2);

    // A FOURTH root, created AFTER the instantiate, is what makes this case discriminating: with the
    // created root already last, a lost rootSlots capture would be indistinguishable from a correct
    // restore, because reconcile() appends.
    const Entity d = world.create();
    REQUIRE(world.setName(d, "D"));
    roots.reconcile(world);
    REQUIRE(roots.indexOf(d) == 3);

    REQUIRE(stack.undo(ctx));
    roots.reconcile(world);
    CHECK(roots.indexOf(a) == 0);
    CHECK(roots.indexOf(b) == 1);
    CHECK(roots.indexOf(d) == 2);

    REQUIRE(stack.redo(ctx));
    CHECK(roots.indexOf(raw->createdRoot()) == 2);  // back in its OWN row, not appended after D
    CHECK(roots.indexOf(a) == 0);
    CHECK(roots.indexOf(b) == 1);
    CHECK(roots.indexOf(d) == 3);
    roots.reconcile(world);  // and the reconcile preserves what the restore placed
    CHECK(roots.indexOf(raw->createdRoot()) == 2);
    CHECK(roots.indexOf(d) == 3);
}

TEST_CASE("asset_commands: identity survives undo -> redo -> undo -> redo (IA7, S10)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    const Entity frame = childNamed(world, root, "Frame");
    REQUIRE(frame.valid());
    const Entity seat = childNamed(world, frame, "Seat");
    REQUIRE(seat.valid());

    // A SECOND command, pushed against a handle the first one minted. This is the property
    // SubtreeSnapshot exists for: if a later redo re-CREATED rather than re-instated, `frame` would
    // name a dead handle and this rename would stop applying.
    REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(frame, "Frame", "Frame Renamed")));
    CHECK(world.name(frame) == "Frame Renamed");

    for (int cycle = 0; cycle < 2; ++cycle) {
        CAPTURE(cycle);
        REQUIRE(stack.undo(ctx));  // the rename
        REQUIRE(stack.undo(ctx));  // the instantiate
        CHECK_FALSE(world.alive(root));
        CHECK_FALSE(world.alive(frame));
        CHECK_FALSE(world.alive(seat));

        REQUIRE(stack.redo(ctx));  // the instantiate
        CHECK(raw->createdRoot() == root);
        CHECK(world.alive(root));
        CHECK(world.alive(frame));
        CHECK(world.alive(seat));
        CHECK(world.parent(frame) == root);
        CHECK(world.parent(seat) == frame);
        // and every payload came back with the handle
        const MeshRenderer* renderer = world.get<MeshRenderer>(seat);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh == MODEL_GUID);
        CHECK(renderer->meshIndex == 7);

        REQUIRE(stack.redo(ctx));  // the rename, against the SAME handle it recorded
        CHECK(world.name(frame) == "Frame Renamed");
    }
}

TEST_CASE("asset_commands: the selection is the created root, and comes back on undo (IA8, S12)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity previouslySelected = world.create();
    selection.set(previouslySelected);
    const std::vector<Entity> before(selection.entities().begin(), selection.entities().end());

    auto command = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(), before);
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    CHECK(selection.count() == 1);
    CHECK(selection.primary() == raw->createdRoot());

    REQUIRE(stack.undo(ctx));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == previouslySelected);

    REQUIRE(stack.redo(ctx));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == raw->createdRoot());
}

TEST_CASE("asset_commands: a dead parent target refuses, creating NOTHING (IA9/IA10)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity doomed = world.create();
    world.destroy(doomed);
    const std::size_t before = liveEntityCount(world);

    // Driven DIRECTLY rather than through push(), deliberately: a push that returns false destroys the
    // command it was handed (it was not recorded), so a raw pointer kept across the call dangles --
    // command_stack.hpp says as much about label views, and it is true of the whole object. The push
    // arm below asserts what the STACK does; this arm asserts what the COMMAND does.
    InstantiateAssetCommand command(threeSlotPlan(), doomed, dropPlacement(), std::span<const Entity>{});
    CHECK_FALSE(command.redo(ctx));
    CHECK_FALSE(command.createdRoot().valid());

    // IA10: the World is exactly where it was. createEntity destroys the entity it could not parent,
    // and this command's own rollback runs over an EMPTY `created` without touching anything.
    CHECK(liveEntityCount(world) == before);

    CHECK_FALSE(stack.push(ctx, std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), doomed, dropPlacement(),
                                                                          std::span<const Entity>{})));
    CHECK(liveEntityCount(world) == before);
    CHECK(stack.count() == 0);

    // A STATED COVERAGE GAP, recorded rather than pretended away: a createEntity failure PART WAY
    // through the loop -- the branch whose rollback destroys what this pass already made -- is
    // unreachable from any tier in this tree. createEntity fails only for a moved-from World or a
    // dead parent, and slot 0 is the only slot whose parent this loop did not itself just create and
    // never destroys. The rollback is therefore written by construction and proven only for the
    // zero-created case above.

    SUBCASE("an empty plan refuses the same way") {
        auto empty = std::make_unique<InstantiateAssetCommand>(InstantiatePlan{}, Entity{}, dropPlacement(),
                                                               std::span<const Entity>{});
        CHECK_FALSE(stack.push(ctx, std::move(empty)));
        CHECK(liveEntityCount(world) == before);
    }

    SUBCASE("undo before any successful redo returns false and changes nothing") {
        auto never = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), doomed, dropPlacement(),
                                                               std::span<const Entity>{});
        CHECK_FALSE(never->undo(ctx));
        CHECK(liveEntityCount(world) == before);
    }
}

TEST_CASE("asset_commands: the label is the constant, unchanged across the whole cycle (IA11)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    REQUIRE(stack.push(ctx, std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                                      std::span<const Entity>{})));
    CHECK(INSTANTIATE_ASSET_COMMAND_LABEL == std::string_view("Instantiate Asset"));
    CHECK(stack.undoLabel() == INSTANTIATE_ASSET_COMMAND_LABEL);
    REQUIRE(stack.undo(ctx));
    CHECK(stack.redoLabel() == INSTANTIATE_ASSET_COMMAND_LABEL);
    REQUIRE(stack.redo(ctx));
    CHECK(stack.undoLabel() == INSTANTIATE_ASSET_COMMAND_LABEL);
}

TEST_CASE("asset_commands: createdRoot() is Entity{} before the first redo (IA12)") {
    const InstantiateAssetCommand command(threeSlotPlan(), Entity{}, dropPlacement(), std::span<const Entity>{});
    CHECK_FALSE(command.createdRoot().valid());
    CHECK(command.createdRoot() == Entity{});
    CHECK(command.label() == INSTANTIATE_ASSET_COMMAND_LABEL);
}

TEST_CASE("asset_commands: a 1-slot plan creates exactly the root (IA13)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    InstantiatePlan plan;
    plan.ok = true;
    plan.nodes.push_back(InstantiatePlanNode{.name = "lonely", .parentSlot = 0});

    const std::size_t before = liveEntityCount(world);
    auto command = std::make_unique<InstantiateAssetCommand>(std::move(plan), Entity{}, dropPlacement(),
                                                             std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    CHECK(liveEntityCount(world) == before + 1);
    const Entity root = raw->createdRoot();
    REQUIRE(root.valid());
    CHECK(world.name(root) == "lonely");
    CHECK_FALSE(world.has<MeshRenderer>(root));
    std::size_t children = 0;
    world.eachChild(root, [&children](Entity) { ++children; });
    CHECK(children == 0);
}

TEST_CASE("asset_commands: two instantiates of the SAME plan are independent subtrees (IA14)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const std::size_t before = liveEntityCount(world);
    auto first = std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), Entity{}, dropPlacement(),
                                                           std::span<const Entity>{});
    const InstantiateAssetCommand* firstRaw = first.get();
    REQUIRE(stack.push(ctx, std::move(first)));

    auto second = std::make_unique<InstantiateAssetCommand>(
        threeSlotPlan(), Entity{}, Transform{.position = Vec3{-1.0F, -2.0F, -3.0F}}, std::span<const Entity>{});
    const InstantiateAssetCommand* secondRaw = second.get();
    REQUIRE(stack.push(ctx, std::move(second)));

    CHECK(liveEntityCount(world) == before + 6);
    const Entity rootA = firstRaw->createdRoot();
    const Entity rootB = secondRaw->createdRoot();
    REQUIRE(rootA.valid());
    REQUIRE(rootB.valid());
    CHECK_FALSE(rootA == rootB);
    CHECK(world.get<Transform>(rootA)->position == dropPlacement().position);
    CHECK(world.get<Transform>(rootB)->position == Vec3{-1.0F, -2.0F, -3.0F});

    // Undoing only the SECOND leaves the first entirely alone.
    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(rootA));
    CHECK_FALSE(world.alive(rootB));
    CHECK(liveEntityCount(world) == before + 3);
}

TEST_CASE("asset_commands: a hierarchy-row drop parents the whole subtree under the target (IA15)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity target = world.create();
    REQUIRE(world.setName(target, "Target"));

    auto command =
        std::make_unique<InstantiateAssetCommand>(threeSlotPlan(), target, Transform{}, std::span<const Entity>{});
    const InstantiateAssetCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    REQUIRE(root.valid());
    CHECK(world.parent(root) == target);
    // A row drop's placement is LOCAL identity, and the command carries it verbatim.
    CHECK(world.get<Transform>(root)->position == Vec3{});

    REQUIRE(stack.undo(ctx));
    CHECK(world.alive(target));  // the target is not part of the captured subtree
    CHECK_FALSE(world.alive(root));
    REQUIRE(stack.redo(ctx));
    CHECK(world.parent(raw->createdRoot()) == target);
}
