// tests/editor/scene_io_test.cpp -- task 2.5.1: the engine's serialization bridge, reached through
// scene_session.hpp's openSceneText/sceneToText (step 3: IO1-IO10; step 5: IO11-IO14, the flow
// through real files). CONDITIONAL: this TU is appended to aero_editor_shell_test only inside
// if(AERO_REFLECT_TOOLS) (tests/CMakeLists.txt) -- with the tool off, the generated component
// serializers do not EXIST (F9), so this whole TU is absent from that build, not skipped. Sixteenth
// TU; do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. This TU must NOT include the engine's
// serialization header directly (plan A29): every symbol it touches lives in aero_editor_core, which
// keeps §V7's boundary grep honest.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
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
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::openSceneText;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::sceneToText;
using engine::editor::Selection;

namespace {

// Count by LEVEL, never records.size() (the command_stack_test.cpp precedent, plan A31).
[[nodiscard]] std::size_t countAtLevel(const std::vector<engine::editor::LogEntry>& records, engine::LogLevel level) {
    return static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(), [level](const engine::editor::LogEntry& e) { return e.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// A unique temp directory that removes itself on destruction -- the FOURTH TU-local copy of this
// shape (plan A28/G12: tests/vfs_test.cpp, tests/editor/project_files_test.cpp,
// tests/editor/scene_session_test.cpp). These cases cannot borrow scene_session_test.cpp's copy: a
// file-scope/anonymous-namespace helper is TU-scoped.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;
        dirPath = base / ("aero_scene_io_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

}  // namespace

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

    // A non-empty World, deliberately: an empty one would leave world.entityCount() == 0 both before
    // and after a swap that should not have happened, making the assertion below trivially true
    // regardless of ordering (S4's second half needs the World to actually LOSE something to fail).
    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    const std::size_t countBefore = world.entityCount();
    REQUIRE(countBefore > 0);

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

// ---- IO11-IO14: the flow through real files (task 2.5.1 step 5) -----------------------------------

TEST_CASE("scene_io: save -> open round trip through the flow (IO11/AC-10/AC-15/AC-20)") {
    using engine::editor::openSceneFile;
    using engine::editor::saveSceneFile;

    const TempDir dir;
    const std::string path = dir.join("level1.scene.json");

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    REQUIRE(saveSceneFile(ctx, commands, session, path, /*appendExtension=*/false));
    CHECK(commands.isClean());
    CHECK(session.path() == path);

    // Mutate directly (bypassing the stack, the hierarchy_test.cpp shape): the document stays "saved"
    // as far as this test cares -- the point is that Open discards it regardless of the clean flag.
    const engine::Entity extra = engine::editor::createEntity(world, {}, "Extra");
    REQUIRE(extra.valid());
    REQUIRE(world.entityCount() == 4);

    REQUIRE(openSceneFile(ctx, commands, session, path));
    CHECK(world.entityCount() == 3);  // the mutation is gone
    CHECK(commands.isClean());
    CHECK(commands.count() == 0);
}

TEST_CASE("scene_io: a failed save does not lie (IO12/AC-21/S22)") {
    using engine::editor::saveSceneFile;

    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const std::string path = dir.join("missing-subdir/level1.scene.json");

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    // Made DIRTY first (not left at its already-clean default): a stack that starts clean would stay
    // clean whether or not the bug (S22: setClean() called before checking the write's own reason) is
    // present, which would make this assertion vacuous. Starting dirty is what makes "still dirty
    // afterwards" the real, discriminating check.
    const engine::Entity probe = world.create();
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                   std::vector<engine::Entity>{probe}, std::vector<engine::Entity>{})));
    REQUIRE_FALSE(commands.isClean());
    engine::editor::SceneSession session;
    scope.sink()->take(records);
    records.clear();

    const bool ok = saveSceneFile(ctx, commands, session, path, /*appendExtension=*/false);

    CHECK_FALSE(ok);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK_FALSE(commands.isClean());  // the single most valuable assertion in this TU
    CHECK(session.path().empty());    // the path did NOT change either
}

TEST_CASE("scene_io: D13's refusal -- appending the extension never overwrites silently (IO13/AC-22)") {
    using engine::editor::saveSceneFile;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string target = dir.join("x.scene.json");
    REQUIRE(writeTextFileAtomic(target, "PRE-EXISTING").empty());

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    const std::string bareName = dir.join("x");
    const bool refused = saveSceneFile(ctx, commands, session, bareName, /*appendExtension=*/true);
    CHECK_FALSE(refused);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);

    const engine::editor::FileReadResult afterRefusal = engine::editor::readTextFile(target);
    REQUIRE(afterRefusal.text.has_value());
    CHECK(*afterRefusal.text == "PRE-EXISTING");  // byte-identical -- nothing was written

    // The SAME call with appendExtension=false writes the bare name literally (D15's hook contract).
    CHECK(saveSceneFile(ctx, commands, session, bareName, /*appendExtension=*/false));
    CHECK(session.path() == bareName);
}

TEST_CASE("scene_io: openSceneFile logs exactly one INFO and zero WARN on a clean load (IO15/AC-14/D21)") {
    // Finding 4 of the 2.5.1 code-review round: AC-14's INFO and WARN records were asserted NOWHERE --
    // IO11 (which drives openSceneFile through a real file) installs no LogSinkScope at all, IO6 calls
    // openSceneText directly (which by design never logs), and IO14 counts ERRORs only. This closes the
    // gap for the clean-load half: exactly one INFO, zero WARN.
    using engine::editor::openSceneFile;
    using engine::editor::saveSceneFile;

    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const std::string path = dir.join("level1.scene.json");

    engine::World seedWorld;
    engine::editor::seedDefaultScene(seedWorld);
    Selection seedSelection;
    RootOrder seedRoots;
    CommandStack seedCommands;
    CommandContext seedCtx{seedWorld, seedSelection, seedRoots};  // a PRVALUE cannot bind to
                                                                  // saveSceneFile's CommandContext&
    engine::editor::SceneSession seedSession;
    REQUIRE(saveSceneFile(seedCtx, seedCommands, seedSession, path, /*appendExtension=*/false));

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    scope.sink()->take(records);
    records.clear();

    REQUIRE(openSceneFile(ctx, commands, session, path));

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE(
    "scene_io: openSceneFile logs one INFO AND at least one WARN when a component is skipped "
    "(IO16/AC-14/D21)") {
    using engine::editor::openSceneFile;
    using engine::editor::writeTextFileAtomic;

    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const std::string path = dir.join("mystery.scene.json");
    const std::string unknownComponent = R"({
  "version": 1,
  "entities": [
    { "id": 1, "name": "mystery", "components": { "not::a::real::Component": {} } }
  ]
})";
    REQUIRE(writeTextFileAtomic(path, unknownComponent).empty());

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    scope.sink()->take(records);
    records.clear();

    REQUIRE(openSceneFile(ctx, commands, session, path));

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    // Measured, not assumed: TWO WARNs, not one -- `engine::scene_serialize` itself already logs its
    // own WARN per skipped component ("scene: unknown component type ... skipped",
    // scene_serialize.cpp:109), and scene_session.cpp's D21 arm adds ONE MORE, aggregate WARN on top
    // ("scene '...' loaded with N skipped and M failed components") whenever skipped + failed > 0.
    // This scene has exactly one unknown component, so 1 (the loader's own) + 1 (D21's aggregate) == 2.
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 2);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("scene_io: a malformed file through the flow changes nothing (IO14/AC-11/AC-12)") {
    using engine::editor::openSceneFile;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string malformedPath = dir.join("broken.scene.json");
    REQUIRE(writeTextFileAtomic(malformedPath, "not json at all {{{").empty());

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;
    session.setPath("/some/other/path.scene.json");
    const std::size_t countBefore = world.entityCount();
    const std::size_t commandsBefore = commands.count();
    const bool cleanBefore = commands.isClean();

    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    CHECK_FALSE(openSceneFile(ctx, commands, session, malformedPath));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK(world.entityCount() == countBefore);
    CHECK(selection.empty());
    CHECK(roots.entities().empty());
    CHECK(commands.count() == commandsBefore);
    CHECK(commands.isClean() == cleanBefore);
    CHECK(session.path() == "/some/other/path.scene.json");

    records.clear();
    CHECK_FALSE(openSceneFile(ctx, commands, session, dir.join("definitely-missing.scene.json")));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK(world.entityCount() == countBefore);
    CHECK(session.path() == "/some/other/path.scene.json");
}
