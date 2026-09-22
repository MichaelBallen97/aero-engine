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
#include <aero/editor/project.hpp>        // task E.4.1 (PJ44-PJ53): createProject, ProjectSession, the
                                          // ProjectContext the restore path needs
#include <aero/editor/project_state.hpp>  // task E.4.1: writeProjectState, to SEED a record
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/text_file.hpp>  // task E.4.1: writeTextFileAtomic / fileExists
#include <aero/editor/transform_ops.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::openSceneText;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::SceneSession;
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

// ---- task E.4.1's own fixtures (PJ44-PJ53) --------------------------------------------------------

// The FIFTH copy of project_test.cpp:166-184's shape. A TU-local helper is TU-scoped -- this file's
// own banner above already states the rule for TempDir, and it applies unchanged here.
struct FlowFixture {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::ProjectSession projectSession;
    engine::editor::ProjectFlow projectFlow;
    engine::editor::RecentProjects recents;
    engine::editor::ProjectContext project{projectSession, projectFlow, recents, ""};
};

// A REAL, loadable scene whose entity count is DISTINCT from the four-entity default seed, so "this
// scene opened" cannot be satisfied by adoptProject's own newScene. Four + `extra`.
[[nodiscard]] std::string sceneTextWithExtras(std::size_t extra) {
    engine::World world;
    engine::editor::seedDefaultScene(world);  // FOUR: Main Camera, Directional Light, Cube, Environment
    for (std::size_t i = 0; i < extra; ++i) {
        const engine::Entity e = engine::editor::createEntity(world, {}, "Extra" + std::to_string(i));
        REQUIRE(e.valid());
    }
    const std::optional<std::string> text = engine::editor::sceneToText(world);
    REQUIRE(text.has_value());
    return *text;
}

// A scaffolded project plus whatever scenes the caller names, each with a distinct entity count.
struct SceneProject {
    std::string root;
};

[[nodiscard]] SceneProject makeSceneProject(const TempDir& dir,
                                            std::initializer_list<std::pair<std::string_view, std::size_t>> scenes) {
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(dir.utf8(), "MyGame", "");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    for (const auto& [relative, extra] : scenes) {
        const std::string path = created.root + "/" + std::string(relative);
        REQUIRE(engine::editor::writeTextFileAtomic(path, sceneTextWithExtras(extra)).empty());
    }
    return SceneProject{.root = created.root};
}

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
    CHECK(outcome.entities == 4);
    CHECK(loaded.entityCount() == 4);

    std::vector<std::string> originalNames;
    original.eachEntity([&](engine::Entity e) { originalNames.emplace_back(original.name(e)); });
    std::vector<std::string> loadedNames;
    loaded.eachEntity([&](engine::Entity e) { loadedNames.emplace_back(loaded.name(e)); });
    std::sort(originalNames.begin(), originalNames.end());
    std::sort(loadedNames.begin(), loadedNames.end());
    CHECK(originalNames == loadedNames);

    // All four are roots in both Worlds (F8's seed contents parent nothing).
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
    REQUIRE(world.entityCount() == 5);

    REQUIRE(openSceneFile(ctx, commands, session, path));
    CHECK(world.entityCount() == 4);  // the mutation is gone
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

// ---- PJ44-PJ53: task E.4.1's startup-scene restore, driven through openProjectPath ----------------

TEST_CASE("scene_io: a recorded, existing scene is OPENED on project open (PJ44/AC-1/AC-36)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    REQUIRE(
        engine::editor::writeProjectState(
            project.root, engine::editor::ProjectState{.lastScene = "scenes/a.scene.json", .lastSceneRecorded = true})
            .empty());

    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

    CHECK(f.world.entityCount() == 6);  // FOUR seeded + two extras -- not the default's four
    CHECK(session.path() == project.root + "/scenes/a.scene.json");
    CHECK(f.commands.isClean());
    CHECK(f.selection.empty());

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 2);  // the project's, then the scene's
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    // AC-36: the PROJECT line comes FIRST. The only positional record read in either project TU, and
    // deliberate -- with the INFO last, a restore whose candidate failed to open reads as the project
    // having failed. `records` is in emission order (console_model.hpp's sink appends).
    std::vector<std::string> infos;
    for (const engine::editor::LogEntry& e : records) {
        if (e.level == engine::LogLevel::Info) {
            infos.push_back(e.message);
        }
    }
    REQUIRE(infos.size() == 2U);
    CHECK(infos[0].find("opened project") != std::string::npos);
    CHECK(infos[1].find("opened scene") != std::string::npos);
}

TEST_CASE("scene_io: a recorded, MISSING scene cascades to the first scene under paths.scenes (PJ45/AC-3)") {
    const TempDir dir;
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    REQUIRE(engine::editor::writeProjectState(
                project.root,
                engine::editor::ProjectState{.lastScene = "scenes/gone.scene.json", .lastSceneRecorded = true})
                .empty());

    FlowFixture f;
    SceneSession session;
    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

    CHECK(session.path() == project.root + "/scenes/a.scene.json");  // the CASCADE fired
    CHECK(f.world.entityCount() == 6);
    // ...and the stale record is LEFT ALONE (D14, plan E13): nothing here prunes it.
    bool corrupt = false;
    const engine::editor::ProjectState after = engine::editor::readProjectState(project.root, corrupt);
    CHECK_FALSE(corrupt);
    CHECK(after.lastScene == "scenes/gone.scene.json");
}

TEST_CASE("scene_io: a recorded, PRESENT but BROKEN scene STOPS -- D6's whole content (PJ46/AC-4/AC-8)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    // TWO scenes under scenes/: `a` is the one the cascade WOULD choose, `b` is the recorded one.
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    REQUIRE(engine::editor::writeTextFileAtomic(project.root + "/scenes/b.scene.json", "{ not a scene").empty());
    // ANTI-VACUITY, and it is what makes this case an assertion at all: prove `a` EXISTS and is what
    // firstSceneUnder would pick, so "a was not opened" means "the cascade was refused" and not
    // "there was nothing to cascade to".
    REQUIRE(engine::editor::firstSceneUnder(project.root, "scenes") == "scenes/a.scene.json");
    REQUIRE(
        engine::editor::writeProjectState(
            project.root, engine::editor::ProjectState{.lastScene = "scenes/b.scene.json", .lastSceneRecorded = true})
            .empty());

    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

    CHECK(session.path().empty());      // NOTHING opened
    CHECK(f.world.entityCount() == 4);  // the fresh default scene adoptProject left
    CHECK(f.commands.isClean());
    CHECK(f.selection.empty());
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);  // openSceneFile's, naming b
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);   // the project's, and NO scene INFO
    // The ERROR names the RECORDED scene and not the cascade candidate.
    const auto namedB = std::count_if(records.begin(), records.end(), [](const engine::editor::LogEntry& e) {
        return e.level == engine::LogLevel::Error && e.message.find("b.scene.json") != std::string::npos;
    });
    CHECK(namedB == 1);
}

TEST_CASE("scene_io: a RECORDED-EMPTY lastScene lands on a new scene, whatever scenes/ holds (PJ47/AC-5)") {
    const TempDir dir;
    const SceneProject project =
        makeSceneProject(dir, {{"scenes/a.scene.json", 2}, {"scenes/b.scene.json", 3}, {"scenes/c.scene.json", 5}});
    REQUIRE(engine::editor::writeProjectState(project.root,
                                              engine::editor::ProjectState{.lastScene = "", .lastSceneRecorded = true})
                .empty());

    FlowFixture f;
    SceneSession session;
    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

    CHECK(session.path().empty());      // D2's third state
    CHECK(f.world.entityCount() == 4);  // not 6, not 7, not 9
}

TEST_CASE("scene_io: NO state file at all -- the first scene, or a new one (PJ48/AC-6)") {
    SUBCASE("scenes/ has content: the FIRST scene opens") {
        const TempDir dir;
        const SceneProject project = makeSceneProject(dir, {{"scenes/b.scene.json", 3}, {"scenes/a.scene.json", 2}});
        REQUIRE_FALSE(engine::editor::fileExists(project.root + "/Library/editor-state.json"));
        FlowFixture f;
        SceneSession session;
        REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));
        CHECK(session.path() == project.root + "/scenes/a.scene.json");  // `a`, by entryOrderLess
        CHECK(f.world.entityCount() == 6);
    }
    SUBCASE("scenes/ is empty: a NEW scene") {
        const LogFixture fixture;
        const engine::editor::LogSinkScope scope;
        std::vector<engine::editor::LogEntry> records;
        const TempDir dir;
        const SceneProject project = makeSceneProject(dir, {});
        FlowFixture f;
        SceneSession session;
        scope.sink()->take(records);
        records.clear();
        REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));
        CHECK(session.path().empty());
        CHECK(f.world.entityCount() == 4);
        scope.sink()->take(records);
        // SEED S21's witness at this tier: a decider that returns FirstUnderScenes with no first scene
        // makes the caller open "" -- which fails, logs one ERROR, and leaves BOTH assertions above
        // still passing. "No attempt was made" is the claim, and only this line says it.
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
        CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);  // the project's, and no scene line
    }
}

TEST_CASE("scene_io: a recorded path that names a DIRECTORY takes the STOP arm (PJ49)") {
    // fileExists is std::filesystem::exists, so a directory reads PRESENT, takes D6's stop arm, and
    // produces one ERROR from openSceneFile. Correct and honest; pinned so it is not later mistaken
    // for a bug (F8's second consequence).
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    std::error_code ec;
    std::filesystem::create_directories(project.root + "/scenes/dir.scene.json", ec);
    REQUIRE(
        engine::editor::writeProjectState(
            project.root, engine::editor::ProjectState{.lastScene = "scenes/dir.scene.json", .lastSceneRecorded = true})
            .empty());

    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();
    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

    CHECK(session.path().empty());
    CHECK(f.world.entityCount() == 4);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
}

TEST_CASE("scene_io: the history is CLEAN and the selection EMPTY after every arm (PJ50/AC-9)") {
    // One case over all four arms, driven from a DIRTY, NON-EMPTY starting state so "clean afterwards"
    // is a real assertion rather than an unchanged default (P73's own idiom).
    const TempDir dir;
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    struct Arm {
        std::string_view name;
        bool recorded;
        std::string_view value;
    };
    const std::array<Arm, 4> arms = {{{"no record", false, ""},
                                      {"recorded empty", true, ""},
                                      {"recorded present", true, "scenes/a.scene.json"},
                                      {"recorded missing", true, "scenes/gone.scene.json"}}};
    for (const Arm& arm : arms) {
        CAPTURE(arm.name);
        if (arm.recorded) {
            REQUIRE(engine::editor::writeProjectState(
                        project.root,
                        engine::editor::ProjectState{.lastScene = std::string(arm.value), .lastSceneRecorded = true})
                        .empty());
        }
        FlowFixture f;
        SceneSession session;
        // DIRTY and NON-EMPTY first.
        const engine::Entity extra = f.world.create();
        REQUIRE(f.commands.push(f.ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                           std::vector<engine::Entity>{extra}, std::vector<engine::Entity>{})));
        REQUIRE_FALSE(f.commands.isClean());
        f.selection.set(extra);
        REQUIRE_FALSE(f.selection.empty());

        REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));

        CHECK(f.commands.isClean());
        CHECK(f.commands.count() == 0);
        CHECK(f.selection.empty());
    }
}

TEST_CASE("scene_io: a project SWAP reads the NEW project's state, not the outgoing one (PJ51, seed S13)") {
    const TempDir dirA;
    const TempDir dirB;
    const SceneProject a = makeSceneProject(dirA, {{"scenes/a.scene.json", 2}});
    const SceneProject b = makeSceneProject(dirB, {{"scenes/b.scene.json", 5}});
    REQUIRE(engine::editor::writeProjectState(
                a.root, engine::editor::ProjectState{.lastScene = "scenes/a.scene.json", .lastSceneRecorded = true})
                .empty());
    REQUIRE(engine::editor::writeProjectState(
                b.root, engine::editor::ProjectState{.lastScene = "scenes/b.scene.json", .lastSceneRecorded = true})
                .empty());

    FlowFixture f;
    SceneSession session;
    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, a.root));
    REQUIRE(session.path() == a.root + "/scenes/a.scene.json");
    REQUIRE(f.world.entityCount() == 6);

    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, b.root));
    CHECK(session.path() == b.root + "/scenes/b.scene.json");  // B's record, read through B's root
    CHECK(f.world.entityCount() == 9);                         // four + five, not four + two
}

TEST_CASE("scene_io: createAndOpenProject lands on a NEW scene and reads no state (PJ52/R19)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    REQUIRE(engine::editor::createAndOpenProject(f.ctx, f.commands, session, f.project, dir.utf8(), "Fresh"));

    CHECK(session.path().empty());
    CHECK(f.world.entityCount() == 4);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);  // exactly ONE, still (A24)
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    // A NAMED LOCAL for root()'s view, exactly as everywhere else.
    const std::string root(f.projectSession.root());
    CHECK_FALSE(engine::editor::fileExists(root + "/Library/editor-state.json"));
}

TEST_CASE("scene_io: a recorded scene OUTSIDE paths.scenes but inside the root opens (PJ53)") {
    // The record is ROOT-relative, not scenes-relative. Without that, a project whose scenes live in
    // two places would forget half of them.
    const TempDir dir;
    const SceneProject project = makeSceneProject(dir, {{"scenes/a.scene.json", 2}});
    std::error_code ec;
    std::filesystem::create_directories(project.root + "/levels", ec);
    REQUIRE(
        engine::editor::writeTextFileAtomic(project.root + "/levels/deep.scene.json", sceneTextWithExtras(7)).empty());
    REQUIRE(engine::editor::writeProjectState(
                project.root,
                engine::editor::ProjectState{.lastScene = "levels/deep.scene.json", .lastSceneRecorded = true})
                .empty());

    FlowFixture f;
    SceneSession session;
    REQUIRE(engine::editor::openProjectPath(f.ctx, f.commands, session, f.project, project.root));
    CHECK(session.path() == project.root + "/levels/deep.scene.json");
    CHECK(f.world.entityCount() == 11);  // four + seven, NOT the first scene's six
}
