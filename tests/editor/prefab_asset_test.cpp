// tests/editor/prefab_asset_test.cpp -- task 4.4.4: InstantiatePrefabCommand and the prefab text bridge
// (PE1-PE8; Step 7 adds the save cases PE9-PE17 to this file). A TU of aero_editor_shell_test, which supplies main()
// from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED and tier-0: it compiles in every configuration, with no window, no GPU and no ImGui. The command cases
// drive a REAL CommandStack against a real CommandContext{world, selection, roots}, over a Prefab made through
// Prefab::capture, so they need no parse. The two text cases ask sceneIoAvailable() first and assert the
// tools-OFF answer when it is false, which is how this file holds no preprocessor conditional.
//
// <ostream> is included because std::string_view values are compared. No toString overload is added anywhere.
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/asset_commands.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::Entity;
using engine::Prefab;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::INSTANTIATE_PREFAB_COMMAND_LABEL;
using engine::editor::InstantiatePrefabCommand;
using engine::editor::prefabFromText;
using engine::editor::PrefabReadResult;
using engine::editor::prefabToText;
using engine::editor::RenameEntityCommand;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::Selection;

namespace {

[[nodiscard]] Entity makeNamed(World& world, std::string_view name, Entity parent, Vec3 position) {
    const Entity entity = world.create();
    REQUIRE(world.setName(entity, name));
    Transform transform;
    transform.position = position;
    REQUIRE(world.add<Transform>(entity, transform) != nullptr);
    if (parent.valid()) {
        REQUIRE(world.setParent(entity, parent));
    }
    return entity;
}

// Crate(Lid(Hinge), Latch): four entities. The root is rotated and scaled so the placement composition is visible, and
// it sits away from the origin so that capture's zeroing of a single root's position is visible too.
[[nodiscard]] Prefab makeCratePrefab() {
    World source;
    const Entity crate = makeNamed(source, "Crate", Entity{}, Vec3{5.0F, 0.0F, 0.0F});
    Transform* crateTransform = source.get<Transform>(crate);
    REQUIRE(crateTransform != nullptr);
    crateTransform->rotation = engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, engine::radians(30.0F));
    crateTransform->scale = Vec3{2.0F, 2.0F, 2.0F};
    const Entity lid = makeNamed(source, "Lid", crate, Vec3{0.0F, 1.0F, 0.0F});
    static_cast<void>(makeNamed(source, "Hinge", lid, Vec3{0.5F, 0.0F, 0.0F}));
    static_cast<void>(makeNamed(source, "Latch", crate, Vec3{1.0F, 0.0F, 0.0F}));
    const std::array<Entity, 1> roots{crate};
    std::optional<Prefab> prefab = Prefab::capture(source, roots, "unused");
    REQUIRE(prefab.has_value());
    return std::move(*prefab);
}

[[nodiscard]] Transform dropPlacement() {
    Transform placement;
    placement.position = Vec3{10.0F, 0.0F, -5.0F};
    placement.scale = Vec3{3.0F, 1.0F, 1.0F};
    return placement;
}

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

[[nodiscard]] std::vector<Entity> childrenOf(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&out](Entity child) { out.push_back(child); });
    return out;
}

[[nodiscard]] std::vector<std::string> childNames(const World& world, Entity parent) {
    std::vector<std::string> out;
    world.eachChild(parent, [&](Entity child) { out.emplace_back(world.name(child)); });
    return out;
}

// Counts every record a case provokes, through the public setLogCallback seam.
class LogCount {
public:
    LogCount() {
        engine::setLogCallback([this](const engine::LogRecord&) { ++records; });
    }
    LogCount(const LogCount&) = delete;
    LogCount& operator=(const LogCount&) = delete;
    LogCount(LogCount&&) = delete;
    LogCount& operator=(LogCount&&) = delete;
    ~LogCount() { engine::setLogCallback(nullptr); }

    [[nodiscard]] int count() const noexcept { return records; }

private:
    int records = 0;
};

}  // namespace

TEST_CASE("prefab_asset: the first redo copies the tree under placement o stored and selects the root (PE1)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    CHECK_FALSE(raw->createdRoot().valid());
    REQUIRE(stack.push(ctx, std::move(command)));

    CHECK(liveEntityCount(world) == 4);
    const Entity root = raw->createdRoot();
    REQUIRE(root.valid());
    CHECK(world.name(root) == "Crate");
    CHECK_FALSE(world.parent(root).valid());

    // capture stored the single root at position zero, so the composed position is the placement's own, and the scale
    // is the componentwise product. The stored rotation survives an identity placement exactly.
    const Transform* rootTransform = world.get<Transform>(root);
    REQUIRE(rootTransform != nullptr);
    CHECK(rootTransform->position == Vec3{10.0F, 0.0F, -5.0F});
    CHECK(rootTransform->scale == Vec3{6.0F, 2.0F, 2.0F});
    CHECK(rootTransform->rotation == engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, engine::radians(30.0F)));

    // The children are untouched by the placement.
    const Entity lid = childNamed(world, root, "Lid");
    REQUIRE(lid.valid());
    const Transform* lidTransform = world.get<Transform>(lid);
    REQUIRE(lidTransform != nullptr);
    CHECK(lidTransform->position == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(lidTransform->scale == Vec3{1.0F, 1.0F, 1.0F});
    CHECK(world.name(childNamed(world, lid, "Hinge")) == "Hinge");

    // The ONE selected entity is the root, never a child.
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == root);
    CHECK_FALSE(selection.contains(lid));

    CHECK(INSTANTIATE_PREFAB_COMMAND_LABEL == std::string_view("Instantiate Prefab"));
    CHECK(stack.undoLabel() == INSTANTIATE_PREFAB_COMMAND_LABEL);
    CHECK(raw->label() == std::string_view("Instantiate Prefab"));
    CHECK(raw->label() != engine::editor::INSTANTIATE_ASSET_COMMAND_LABEL);
}

TEST_CASE("prefab_asset: undo removes the whole tree, and redo brings back the SAME handles (PE2)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity previouslySelected = world.create();
    selection.set(previouslySelected);
    const std::vector<Entity> before(selection.entities().begin(), selection.entities().end());
    const std::size_t baseline = liveEntityCount(world);

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(), before);
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    const Entity lid = childNamed(world, root, "Lid");
    const Entity hinge = childNamed(world, lid, "Hinge");
    const Entity latch = childNamed(world, root, "Latch");
    REQUIRE(hinge.valid());
    REQUIRE(latch.valid());
    CHECK(selection.primary() == root);

    REQUIRE(stack.undo(ctx));
    CHECK(liveEntityCount(world) == baseline);
    CHECK_FALSE(world.alive(root));
    CHECK_FALSE(world.alive(lid));
    CHECK_FALSE(world.alive(hinge));
    CHECK_FALSE(world.alive(latch));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == previouslySelected);

    REQUIRE(stack.redo(ctx));
    CHECK(liveEntityCount(world) == baseline + 4);
    CHECK(raw->createdRoot() == root);  // a re-instantiation would mint fresh handles
    CHECK(world.alive(root));
    CHECK(world.alive(lid));
    CHECK(world.alive(hinge));
    CHECK(world.alive(latch));
    CHECK(world.parent(lid) == root);
    CHECK(world.parent(hinge) == lid);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == root);
}

TEST_CASE("prefab_asset: a rename pushed after the drop survives undo, undo, redo, redo (S10, PE3)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    const Entity lid = childNamed(world, raw->createdRoot(), "Lid");
    REQUIRE(lid.valid());

    REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(lid, "Lid", "Lid Renamed")));
    CHECK(world.name(lid) == "Lid Renamed");

    REQUIRE(stack.undo(ctx));  // the rename
    REQUIRE(stack.undo(ctx));  // the drop
    CHECK_FALSE(world.alive(lid));
    REQUIRE(stack.redo(ctx));  // the drop
    REQUIRE(stack.redo(ctx));  // the rename, against the handle it recorded
    CHECK(world.alive(lid));
    CHECK(world.name(lid) == "Lid Renamed");
}

TEST_CASE("prefab_asset: under a parent the copy is linked and keeps its place among the siblings (PE4)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity parent = makeNamed(world, "Shelf", Entity{}, Vec3{});
    static_cast<void>(makeNamed(world, "A", parent, Vec3{}));
    static_cast<void>(makeNamed(world, "B", parent, Vec3{}));
    const Entity loose = makeNamed(world, "C", Entity{}, Vec3{});

    auto command =
        std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), parent, Transform{}, std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    const Entity root = raw->createdRoot();
    CHECK(world.parent(root) == parent);
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B", "Crate"});

    REQUIRE(stack.undo(ctx));
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B"});
    // A sibling attached behind the history's back after the undo (an existing entity, because a new one would take
    // the freed index): a restore that appends would land after it.
    REQUIRE(world.setParent(loose, parent));
    REQUIRE(stack.redo(ctx));
    CHECK(raw->createdRoot() == root);
    CHECK(world.parent(root) == parent);
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B", "Crate", "C"});
}

TEST_CASE("prefab_asset: a dead parent refuses, changing nothing and recording nothing (PE5)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity keep = world.create();
    selection.set(keep);
    const Entity doomed = world.create();
    world.destroy(doomed);
    const std::size_t before = liveEntityCount(world);
    const std::vector<Entity> selectionBefore(selection.entities().begin(), selection.entities().end());

    // Driven directly: a push that returns false destroys the command it was handed.
    InstantiatePrefabCommand command(makeCratePrefab(), doomed, dropPlacement(), selectionBefore);
    CHECK_FALSE(command.redo(ctx));
    CHECK_FALSE(command.createdRoot().valid());
    CHECK(liveEntityCount(world) == before);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == keep);

    CHECK_FALSE(stack.push(
        ctx, std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), doomed, dropPlacement(), selectionBefore)));
    CHECK(liveEntityCount(world) == before);
    CHECK(stack.count() == 0);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == keep);

    // Undo before any successful redo is a refusal that changes nothing either.
    CHECK_FALSE(command.undo(ctx));
    CHECK(liveEntityCount(world) == before);
}

TEST_CASE("prefab_asset: a root keeps its display slot across undo and redo with a reconcile between (PE6)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity a = world.create();
    const Entity b = world.create();
    roots.reconcile(world);
    REQUIRE(roots.indexOf(a) == 0);
    REQUIRE(roots.indexOf(b) == 1);

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    roots.reconcile(world);
    const Entity root = raw->createdRoot();
    REQUIRE(roots.indexOf(root) == 2);

    // A later root makes the case discriminating: with the copy already last, a lost slot would look like a restore.
    const Entity d = world.create();
    roots.reconcile(world);
    REQUIRE(roots.indexOf(d) == 3);

    REQUIRE(stack.undo(ctx));
    roots.reconcile(world);
    CHECK(roots.indexOf(a) == 0);
    CHECK(roots.indexOf(b) == 1);
    CHECK(roots.indexOf(d) == 2);

    REQUIRE(stack.redo(ctx));
    CHECK(roots.indexOf(raw->createdRoot()) == 2);  // its OWN row, not appended after d
    CHECK(roots.indexOf(d) == 3);
    roots.reconcile(world);
    CHECK(roots.indexOf(raw->createdRoot()) == 2);
    CHECK(roots.indexOf(d) == 3);
}

TEST_CASE("prefab_asset: the text bridge round-trips one root, and the text is a scene document (PE7)") {
    const Prefab prefab = makeCratePrefab();
    const std::optional<std::string> text = prefabToText(prefab);

    if (!sceneIoAvailable()) {
        // Tools OFF: no writer exists, and the reader says why in words.
        CHECK_FALSE(text.has_value());
        const PrefabReadResult refused = prefabFromText("{}");
        CHECK_FALSE(refused.prefab.has_value());
        CHECK_FALSE(refused.message.empty());
        return;
    }

    REQUIRE(text.has_value());
    // The text is a scene v1 document: the engine's own parser accepts it, with no prefab-specific key.
    const engine::SceneParseResult parsed = engine::parseScene(*text);
    CHECK(parsed.document.has_value());

    const PrefabReadResult read = prefabFromText(*text);
    REQUIRE(read.prefab.has_value());
    CHECK(read.message.empty());
    const Prefab& again = *read.prefab;
    CHECK(again.entityCount() == prefab.entityCount());
    const World& before = prefab.world();
    const World& after = again.world();
    CHECK(after.name(again.root()) == before.name(prefab.root()));
    CHECK(childNames(after, again.root()) == childNames(before, prefab.root()));

    const Transform* storedRoot = before.get<Transform>(prefab.root());
    const Transform* readRoot = after.get<Transform>(again.root());
    REQUIRE(storedRoot != nullptr);
    REQUIRE(readRoot != nullptr);
    CHECK(readRoot->position == storedRoot->position);
    CHECK(readRoot->rotation == storedRoot->rotation);
    CHECK(readRoot->scale == storedRoot->scale);

    const Entity lid = childNamed(after, again.root(), "Lid");
    REQUIRE(lid.valid());
    const Transform* readLid = after.get<Transform>(lid);
    REQUIRE(readLid != nullptr);
    CHECK(readLid->position == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(childNamed(after, lid, "Hinge").valid());
}

TEST_CASE("prefab_asset: a bad file is named by position or by the one-root rule, and nothing is logged (PE8)") {
    if (!sceneIoAvailable()) {
        const PrefabReadResult refused = prefabFromText("{");
        CHECK_FALSE(refused.prefab.has_value());
        CHECK_FALSE(refused.message.empty());
        return;
    }

    World none;
    World two;
    static_cast<void>(makeNamed(two, "One", Entity{}, Vec3{}));
    static_cast<void>(makeNamed(two, "Two", Entity{}, Vec3{}));
    const std::string zeroRoots = engine::editor::sceneToText(none).value_or(std::string{});
    const std::string twoRoots = engine::editor::sceneToText(two).value_or(std::string{});
    REQUIRE_FALSE(zeroRoots.empty());
    REQUIRE_FALSE(twoRoots.empty());

    const LogCount log;

    const PrefabReadResult broken = prefabFromText("{ \"entities\": [");
    CHECK_FALSE(broken.prefab.has_value());
    CHECK(broken.message.starts_with("line "));
    CHECK(broken.message.find(", column ") != std::string::npos);

    const PrefabReadResult empty = prefabFromText(zeroRoots);
    CHECK_FALSE(empty.prefab.has_value());
    CHECK(empty.message == "a prefab has exactly one root");

    const PrefabReadResult several = prefabFromText(twoRoots);
    CHECK_FALSE(several.prefab.has_value());
    CHECK(several.message == "a prefab has exactly one root");

    CHECK(log.count() == 0);  // a bridge that returns status and never prints it
}
