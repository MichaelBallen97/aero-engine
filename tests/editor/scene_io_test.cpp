// tests/editor/scene_io_test.cpp -- task 2.5.1: the scene_serialize bridge (step 3: IO1-IO10; step 5:
// IO11-IO14, the flow through real files). CONDITIONAL: this TU is appended to aero_editor_shell_test
// only inside if(AERO_REFLECT_TOOLS) (tests/CMakeLists.txt) -- with the tool off, engine/scene_serialize
// does not EXIST (F9), so this whole TU is absent from that build, not skipped. Sixteenth TU; do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. This TU must NOT include <aero/scene_serialize/...> (plan
// A29): every symbol it touches lives in aero_editor_core, which keeps §V7's boundary grep honest.
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::openSceneText;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::sceneToText;
using engine::editor::Selection;

TEST_CASE("scene_io: sceneIoAvailable is true in this configuration") { CHECK(sceneIoAvailable()); }

TEST_CASE("scene_io: seed -> text -> load into a SECOND World round-trips names, parents and Transform (IO2)") {
    engine::World original;
    engine::editor::seedDefaultScene(original);
    const std::optional<std::string> text = sceneToText(original);
    REQUIRE(text.has_value());

    engine::World loaded;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{loaded, selection, roots};
    const engine::editor::SceneOpenOutcome outcome = openSceneText(ctx, commands, *text);
    REQUIRE(outcome.ok);
    CHECK(outcome.entities == 3);
    CHECK(loaded.entityCount() == 3);

    std::vector<std::string> originalNames;
    original.eachEntity([&](engine::Entity e) { originalNames.emplace_back(original.name(e)); });
    std::vector<std::string> loadedNames;
    loaded.eachEntity([&](engine::Entity e) { loadedNames.emplace_back(loaded.name(e)); });
    std::sort(originalNames.begin(), originalNames.end());
    std::sort(loadedNames.begin(), loadedNames.end());
    CHECK(originalNames == loadedNames);

    // All three are roots in both Worlds (F8's seed contents parent nothing).
    engine::Entity cube{};
    loaded.eachEntity([&](engine::Entity e) {
        if (loaded.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    CHECK_FALSE(loaded.parent(cube).valid());
    const std::optional<engine::Transform> loadedTransform = engine::editor::readTransform(loaded, cube);
    REQUIRE(loadedTransform.has_value());
    CHECK(*loadedTransform == engine::Transform{});  // the Cube's seed Transform is the identity
}

TEST_CASE("scene_io: byte-stable round trip -- 2.5.2's precondition (IO3/AC-18/INV-8)") {
    engine::World original;
    engine::editor::seedDefaultScene(original);
    const std::optional<std::string> originalText = sceneToText(original);
    REQUIRE(originalText.has_value());

    engine::World loaded;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{loaded, selection, roots};
    REQUIRE(openSceneText(ctx, commands, *originalText).ok);

    const std::optional<std::string> loadedText = sceneToText(loaded);
    REQUIRE(loadedText.has_value());
    CHECK(*loadedText == *originalText);  // std::string equality, not a field walk
}

TEST_CASE("scene_io: malformed JSON changes nothing and reports line/column (IO4/S4)") {
    const std::string malformed = R"({"version": 1, "entities": [)";  // truncated -- a JSON-stage error

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    const engine::Entity keep = world.create();
    const engine::Entity toDelete = world.create();
    selection.set(keep);
    CommandContext ctx{world, selection, roots};
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                   std::vector<engine::Entity>{toDelete}, std::vector<engine::Entity>{keep})));
    const std::size_t countBefore = world.entityCount();
    const bool selectionBefore = selection.contains(keep);
    const std::size_t commandsBefore = commands.count();
    const bool cleanBefore = commands.isClean();

    const engine::editor::SceneOpenOutcome outcome = openSceneText(ctx, commands, malformed);

    CHECK_FALSE(outcome.ok);
    CHECK(outcome.line > 0);
    CHECK_FALSE(outcome.message.empty());
    CHECK(world.entityCount() == countBefore);
    CHECK(selection.contains(keep) == selectionBefore);
    CHECK(commands.count() == commandsBefore);
    CHECK(commands.isClean() == cleanBefore);
}

TEST_CASE("scene_io: valid JSON, invalid envelope -- a cyclic parent fails with line == 0 (IO5/S4)") {
    const std::string cyclic = R"({
  "version": 1,
  "entities": [
    { "id": 1, "parent": 2 },
    { "id": 2, "parent": 1 }
  ]
})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    const std::size_t countBefore = world.entityCount();

    const engine::editor::SceneOpenOutcome outcome = openSceneText(ctx, commands, cyclic);

    CHECK_FALSE(outcome.ok);
    CHECK(outcome.line == 0);
    CHECK_FALSE(outcome.message.empty());
    CHECK(world.entityCount() == countBefore);
}

TEST_CASE("scene_io: an unknown component type is skipped, not fatal (IO6/D21/E9)") {
    const std::string unknownComponent = R"({
  "version": 1,
  "entities": [
    { "id": 1, "name": "mystery", "components": { "not::a::real::Component": {} } }
  ]
})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    const engine::editor::SceneOpenOutcome outcome = openSceneText(ctx, commands, unknownComponent);

    CHECK(outcome.ok);
    CHECK(outcome.entities == 1);
    CHECK(outcome.skipped > 0);
}

TEST_CASE("scene_io: clean history after a load (IO7/AC-10)") {
    const std::string empty = R"({"version": 1, "entities": []})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    REQUIRE(openSceneText(ctx, commands, empty).ok);

    CHECK(commands.count() == 0);
    CHECK(commands.isClean());
}

TEST_CASE("scene_io: the empty document loads to zero entities (IO8/E10)") {
    const std::string empty = R"({"version": 1, "entities": []})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    const engine::editor::SceneOpenOutcome outcome = openSceneText(ctx, commands, empty);

    CHECK(outcome.ok);
    CHECK(outcome.entities == 0);
    CHECK(world.entityCount() == 0);
}

TEST_CASE("scene_io: a three-level hierarchy round-trips with parents intact (IO9)") {
    const std::string chain = R"({
  "version": 1,
  "entities": [
    { "id": 1, "name": "grandparent" },
    { "id": 2, "name": "parent", "parent": 1 },
    { "id": 3, "name": "child", "parent": 2 }
  ]
})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    REQUIRE(openSceneText(ctx, commands, chain).ok);
    CHECK(world.entityCount() == 3);

    engine::Entity grandparent{};
    engine::Entity parent{};
    engine::Entity child{};
    world.eachEntity([&](engine::Entity e) {
        const std::string_view name = world.name(e);
        if (name == "grandparent") {
            grandparent = e;
        } else if (name == "parent") {
            parent = e;
        } else if (name == "child") {
            child = e;
        }
    });
    REQUIRE(grandparent.valid());
    REQUIRE(parent.valid());
    REQUIRE(child.valid());
    CHECK_FALSE(world.parent(grandparent).valid());
    CHECK(world.parent(parent) == grandparent);
    CHECK(world.parent(child) == parent);
}

TEST_CASE("scene_io: names survive, including an entity with no name (IO10)") {
    const std::string mixedNames = R"({
  "version": 1,
  "entities": [
    { "id": 1, "name": "named" },
    { "id": 2 }
  ]
})";

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    REQUIRE(openSceneText(ctx, commands, mixedNames).ok);
    CHECK(world.entityCount() == 2);

    std::size_t namedCount = 0;
    std::size_t unnamedCount = 0;
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "named") {
            ++namedCount;
        } else if (world.name(e).empty()) {
            ++unnamedCount;
        }
    });
    CHECK(namedCount == 1);
    CHECK(unnamedCount == 1);
}
