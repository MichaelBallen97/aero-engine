// tests/editor/scene_session_test.cpp -- task 2.5.1: the document model, the pure file-flow guard and
// THE scene swap (steps 1-2), the atomic file I/O (step 2), and the whole file-flow transition table
// driven with a NULL dialog channel (step 5). Fifteenth TU of aero_editor_shell_test, which supplies
// main() from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0 and
// UNGATED: this file must build and pass in BOTH tools-OFF configurations (AC-6/E21), and must pass
// identically with AERO_REQUIRE_GPU unset and set.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_containment.hpp>  // task E.4.2: the verdict SS43 asserts directly
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::ConfirmChoice;
using engine::editor::FileAction;
using engine::editor::FileStep;
using engine::editor::RootOrder;
using engine::editor::SceneSession;
using engine::editor::Selection;

namespace {

// Count by LEVEL, never records.size() (the command_stack_test.cpp precedent, plan A31):
// AERO_LOG_DEBUG is compiled out under NDEBUG, so counting by level makes an assertion identical on
// both presets.
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

// A unique temp directory that removes itself (and its contents) on destruction -- the THIRD TU-local
// copy of this shape (tests/vfs_test.cpp:20-60, tests/editor/project_files_test.cpp:41-60; plan
// A28/G12). Kept TU-local rather than shared: ~30 lines, no new header, no new
// target_include_directories.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_scene_session_test_" + std::to_string(++counter));
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

// ---- SS1-SS9: the pure guard + the document model ------------------------------------------------

TEST_CASE("scene_session: discardsWork / guardFor truth table (SS1/S9)") {
    using engine::editor::discardsWork;
    using engine::editor::guardFor;

    CHECK_FALSE(discardsWork(FileAction::None));
    CHECK(discardsWork(FileAction::NewScene));
    CHECK(discardsWork(FileAction::OpenScene));
    CHECK_FALSE(discardsWork(FileAction::SaveScene));
    CHECK_FALSE(discardsWork(FileAction::SaveSceneAs));
    CHECK(discardsWork(FileAction::Quit));

    // Confirm iff discarding AND dirty -- the FULL 6x2 matrix, including the None corner (S9's seed).
    for (const bool dirty : {false, true}) {
        CHECK(guardFor(FileAction::None, dirty) == FileStep::Perform);
        CHECK(guardFor(FileAction::SaveScene, dirty) == FileStep::Perform);
        CHECK(guardFor(FileAction::SaveSceneAs, dirty) == FileStep::Perform);
    }
    for (const FileAction action : {FileAction::NewScene, FileAction::OpenScene, FileAction::Quit}) {
        CHECK(guardFor(action, false) == FileStep::Perform);
        CHECK(guardFor(action, true) == FileStep::Confirm);
    }
}

TEST_CASE("scene_session: saveStep (SS2)") {
    using engine::editor::saveStep;
    CHECK(saveStep(true) == FileStep::AskWhereToSave);
    CHECK(saveStep(false) == FileStep::WriteNow);
}

TEST_CASE("scene_session: resolveConfirm (SS3/S10/S11)") {
    using engine::editor::resolveConfirm;
    CHECK(resolveConfirm(ConfirmChoice::Cancel, false) == FileStep::Nothing);
    CHECK(resolveConfirm(ConfirmChoice::Cancel, true) == FileStep::Nothing);
    CHECK(resolveConfirm(ConfirmChoice::Discard, false) == FileStep::Perform);
    CHECK(resolveConfirm(ConfirmChoice::Discard, true) == FileStep::Perform);
    CHECK(resolveConfirm(ConfirmChoice::Save, /*untitled=*/false) == FileStep::WriteNow);
    CHECK(resolveConfirm(ConfirmChoice::Save, /*untitled=*/true) == FileStep::AskWhereToSave);
}

TEST_CASE("scene_session: fileNameOf / directoryOf (SS4/S13)") {
    using engine::editor::directoryOf;
    using engine::editor::fileNameOf;

    CHECK(fileNameOf("/a/b/c.scene.json") == "c.scene.json");
    CHECK(directoryOf("/a/b/c.scene.json") == "/a/b");

    // The Windows back-slashed row -- S13's discriminator (a fileNameOf that only knows '/' fails here).
    CHECK(fileNameOf("C:\\a\\b\\c.scene.json") == "c.scene.json");
    CHECK(directoryOf("C:\\a\\b\\c.scene.json") == "C:\\a\\b");

    CHECK(fileNameOf("c.scene.json") == "c.scene.json");
    CHECK(directoryOf("c.scene.json") == "");

    CHECK(fileNameOf("/a/") == "");
    CHECK(directoryOf("/a/") == "/a");

    CHECK(fileNameOf("/c") == "c");
    CHECK(directoryOf("/c") == "");  // the root case

    CHECK(fileNameOf("") == "");
    CHECK(directoryOf("") == "");
}

TEST_CASE("scene_session: hasExtension / withSceneExtension (SS5/S12)") {
    using engine::editor::hasExtension;
    using engine::editor::withSceneExtension;

    CHECK_FALSE(hasExtension("/a/b"));
    CHECK(withSceneExtension("/a/b") == "/a/b.scene.json");

    CHECK(hasExtension("/a/b.json"));
    CHECK(withSceneExtension("/a/b.json") == "/a/b.json");  // unchanged -- S12's discriminator

    // A '.' in a DIRECTORY segment, not the last one -- extension-less (SS5's discriminator for S12).
    CHECK_FALSE(hasExtension("/a.b/c"));
    CHECK(withSceneExtension("/a.b/c") == "/a.b/c.scene.json");
}

TEST_CASE("scene_session: SceneSession defaults and path round trip (SS6)") {
    SceneSession session;
    CHECK(session.untitled());
    CHECK(session.documentName() == "Untitled");
    CHECK(session.path().empty());

    session.setPath("/tmp/level1.scene.json");
    CHECK_FALSE(session.untitled());
    CHECK(session.path() == "/tmp/level1.scene.json");
    CHECK(session.documentName() == "level1.scene.json");

    session.clearPath();
    CHECK(session.untitled());
    CHECK(session.path().empty());
    CHECK(session.documentName() == "Untitled");
}

TEST_CASE("scene_session: windowTitle -- the exact strings, character for character (SS7)") {
    SceneSession session;
    CHECK(session.windowTitle(false) == "Untitled - Aero Editor");
    CHECK(session.windowTitle(true) == "*Untitled - Aero Editor");

    session.setPath("/tmp/level1.scene.json");
    CHECK(session.windowTitle(false) == "level1.scene.json - Aero Editor");
    CHECK(session.windowTitle(true) == "*level1.scene.json - Aero Editor");
}

TEST_CASE("scene_session: dialogDirectory (SS8)") {
    SceneSession session;
    CHECK(session.dialogDirectory("/proj/root") == "/proj/root");
    CHECK(session.dialogDirectory("") == "");

    session.setPath("/tmp/scenes/level1.scene.json");
    CHECK(session.dialogDirectory("/proj/root") == "/tmp/scenes");  // the scene's OWN folder wins
    CHECK(session.dialogDirectory("") == "/tmp/scenes");

    // A titled path with no directory component falls back to the project root.
    session.setPath("level1.scene.json");
    CHECK(session.dialogDirectory("/proj/root") == "/proj/root");
    CHECK(session.dialogDirectory("") == "");
}

TEST_CASE("scene_session: saveSuggestion (SS9)") {
    SceneSession session;
    CHECK(session.saveSuggestion("/proj/root") == "/proj/root/Untitled.scene.json");
    CHECK(session.saveSuggestion("") == "Untitled.scene.json");  // no doubled separator

    session.setPath("/tmp/level1.scene.json");
    CHECK(session.saveSuggestion("/proj/root") == "/tmp/level1.scene.json");  // the CURRENT path wins
}

// ---- SS10-SS15: THE swap and newScene -------------------------------------------------------------

TEST_CASE("scene_session: resetSceneState clears world, selection, roots and history (SS10/S1/S2/S3)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;

    const engine::Entity a = world.create();
    const engine::Entity b = world.create();
    selection.setAll(std::vector<engine::Entity>{a, b});
    roots.reconcile(world);
    REQUIRE(roots.entities().size() == 2);

    CommandContext ctx{world, selection, roots};
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(std::vector<engine::Entity>{a},
                                                                                       std::vector<engine::Entity>{})));
    REQUIRE(commands.count() == 1);

    engine::editor::resetSceneState(ctx, commands);

    CHECK(world.entityCount() == 0);
    CHECK(selection.empty());
    CHECK(roots.entities().empty());
    CHECK(commands.count() == 0);
    CHECK(commands.isClean());
}

TEST_CASE("scene_session: resetSceneState leaves no history to resurrect a ghost entity (SS11/D2/F5)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;

    const engine::Entity a = world.create();
    CommandContext ctx{world, selection, roots};
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(std::vector<engine::Entity>{a},
                                                                                       std::vector<engine::Entity>{})));
    REQUIRE(commands.canUndo());

    engine::editor::resetSceneState(ctx, commands);

    // World::clear() would still let World::recreate(a) SUCCEED (F5, measured in scene_test.cpp W7) --
    // this is the proof that matters: there is no HISTORY left to drive that resurrection with. INV-1
    // rests on this, not on recreate() refusing.
    CHECK_FALSE(commands.canUndo());
    const engine::Entity recreated = world.recreate(a);
    CHECK(recreated == a);
    CHECK(world.alive(recreated));
}

TEST_CASE("scene_session: newScene contents (SS12/F8)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    engine::editor::newScene(ctx, commands);

    CHECK(world.entityCount() == 4);
    std::vector<std::string> names;
    world.eachEntity([&](engine::Entity e) { names.emplace_back(world.name(e)); });
    std::sort(names.begin(), names.end());
    const std::vector<std::string> expected = {"Cube", "Directional Light", "Environment", "Main Camera"};
    CHECK(names == expected);

    CHECK(commands.count() == 0);
    CHECK(commands.isClean());

    roots.reconcile(world);
    CHECK(roots.entities().size() == 4);  // a fresh forest -- four roots, no parenting
}

TEST_CASE("scene_session: newScene is idempotent (SS13/S5)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    engine::editor::newScene(ctx, commands);
    engine::editor::newScene(ctx, commands);

    CHECK(world.entityCount() == 4);  // NOT 8 -- S5's discriminator (seed before clear, not after)
}

TEST_CASE("scene_session: dirty tracking follows history arithmetic (SS14/D3)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    CHECK(commands.isClean());
    const engine::Entity a = world.create();
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(std::vector<engine::Entity>{a},
                                                                                       std::vector<engine::Entity>{})));
    CHECK_FALSE(commands.isClean());

    commands.setClean();
    CHECK(commands.isClean());

    const engine::Entity b = world.create();
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(std::vector<engine::Entity>{b},
                                                                                       std::vector<engine::Entity>{})));
    CHECK_FALSE(commands.isClean());

    CHECK(commands.undo(ctx));
    CHECK(commands.isClean());  // the row §H's human pass 2 exercises by eye
}

TEST_CASE("scene_session: resetSceneState on an empty World is a silent no-op (SS15/A31)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    scope.sink()->take(records);
    records.clear();  // LogSink::take requires `out` empty on entry

    engine::editor::resetSceneState(ctx, commands);

    scope.sink()->take(records);
    // The canary: a WARN/ERROR here would be a real defect, but an all-quiet sink is only meaningful if
    // it is proven to be LISTENING at all -- so this is a ZERO assertion, deliberately not `records.empty()`.
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
}

// ---- SS16-SS20: the atomic file I/O (task 2.5.1 step 2) -------------------------------------------

TEST_CASE("scene_session: readTextFile/writeTextFileAtomic round-trip is byte-exact (SS16/S8)") {
    using engine::editor::fileExists;
    using engine::editor::readTextFile;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string path = dir.join("scene.scene.json");
    // Embedded '\n', a trailing '\n' and an EXPLICIT "\r\n" pair -- the only possible discriminator for
    // S8 (both streams dropping std::ios::binary), and only on the Windows lane.
    const std::string content = "{\n  \"a\": 1\r\n}\n";

    CHECK(writeTextFileAtomic(path, content).empty());
    const engine::editor::FileReadResult result = readTextFile(path);
    REQUIRE(result.text.has_value());
    CHECK(result.error.empty());
    CHECK(*result.text == content);  // std::string equality -- never line by line
    CHECK(fileExists(path));
}

TEST_CASE("scene_session: writeTextFileAtomic overwrites and leaves no temp behind (SS17/S6)") {
    using engine::editor::fileExists;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string path = dir.join("scene.scene.json");
    CHECK(writeTextFileAtomic(path, "first").empty());
    CHECK(writeTextFileAtomic(path, "second").empty());

    const engine::editor::FileReadResult result = engine::editor::readTextFile(path);
    REQUIRE(result.text.has_value());
    CHECK(*result.text == "second");
    CHECK_FALSE(fileExists(path + ".aero-tmp"));
}

TEST_CASE("scene_session: writeTextFileAtomic to a non-existent directory fails and leaves no temp (SS18)") {
    using engine::editor::fileExists;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string path = dir.join("missing-subdir/scene.scene.json");
    const std::string reason = writeTextFileAtomic(path, "content");
    CHECK_FALSE(reason.empty());
    CHECK_FALSE(fileExists(path));
    CHECK_FALSE(fileExists(path + ".aero-tmp"));
}

TEST_CASE("scene_session: readTextFile failures -- missing path and a directory (SS19/E15)") {
    using engine::editor::readTextFile;

    const TempDir dir;
    {
        const engine::editor::FileReadResult missing = readTextFile(dir.join("nope.scene.json"));
        CHECK_FALSE(missing.text.has_value());
        CHECK_FALSE(missing.error.empty());
    }
    {
        const engine::editor::FileReadResult directory = readTextFile(dir.utf8());
        CHECK_FALSE(directory.text.has_value());
        CHECK_FALSE(directory.error.empty());
    }
}

TEST_CASE("scene_session: fileExists agrees with the filesystem (SS20)") {
    using engine::editor::fileExists;
    using engine::editor::writeTextFileAtomic;

    const TempDir dir;
    const std::string filePath = dir.join("scene.scene.json");
    CHECK_FALSE(fileExists(filePath));
    CHECK(writeTextFileAtomic(filePath, "x").empty());
    CHECK(fileExists(filePath));

    CHECK(fileExists(dir.utf8()));  // a directory
    CHECK_FALSE(fileExists(dir.join("definitely-missing")));
}

// ---- SS21-SS30: the flow, driven with a NULL dialog channel (task 2.5.1 step 5) --------------------
//
// Every case here drives applyFileRequests/applyDialogResult with `FileDialogHost{}` -- a null
// channel -- so no dialog is ever launched and no SDL is involved (A17's tier-0 test seam).

namespace {

using engine::editor::FileDialogHost;
using engine::editor::FileFlow;

// A fixture that owns the four objects every flow case needs, plus a helper to make the document
// dirty without going through a scene swap (the SS-case shape hierarchy_test.cpp already uses).
struct FlowFixture {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    FileFlow flow;
    const FileDialogHost host{};  // channel == nullptr: the tier-0 seam
    // task 2.6.1: the project half of the flow. Owned here so no case can accidentally share one.
    // NAMED projectSession, not `session`: every case below already declares a local
    // `SceneSession session;`, and a member of the same name would shadow confusingly. No trailing
    // underscore -- members are plain camelBack (docs/04).
    engine::editor::ProjectSession projectSession;
    engine::editor::ProjectFlow projectFlow;
    engine::editor::RecentProjects recents;
    engine::editor::ProjectContext project{projectSession, projectFlow, recents, ""};

    void makeDirty() {
        const engine::Entity a = world.create();
        REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{a}, std::vector<engine::Entity>{})));
    }
};

}  // namespace

TEST_CASE("scene_session: New performs immediately when clean, and produces four entities (SS21)") {
    FlowFixture f;
    SceneSession session;
    f.flow.requested = FileAction::NewScene;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.world.entityCount() == 4);
    CHECK(f.flow.requested == FileAction::None);
    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.confirmOpen);
}

TEST_CASE("scene_session: a request never survives its frame (SS22)") {
    FlowFixture f;
    SceneSession session;
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    CHECK(f.flow.requested == FileAction::None);
    CHECK_FALSE(f.flow.choice.has_value());

    const std::size_t countAfterFirst = f.world.entityCount();
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);  // a second call is a no-op
    CHECK(f.world.entityCount() == countAfterFirst);
}

TEST_CASE("scene_session: the guard raises on a dirty document (SS23)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    const std::size_t countBefore = f.world.entityCount();
    f.flow.requested = FileAction::NewScene;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.world.entityCount() == countBefore);  // UNCHANGED
    CHECK(f.flow.pending == FileAction::NewScene);
    CHECK(f.flow.confirmOpen);
}

TEST_CASE("scene_session: Cancel changes nothing and leaves no pending action (SS24/S10/S21)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    const std::size_t countBefore = f.world.entityCount();
    const bool cleanBefore = f.commands.isClean();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.confirmOpen);

    f.flow.choice = ConfirmChoice::Cancel;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.world.entityCount() == countBefore);
    CHECK(f.commands.isClean() == cleanBefore);
    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.confirmOpen);

    // A further frame does nothing -- no pending action survived (S21's discriminator).
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    CHECK(f.world.entityCount() == countBefore);
}

TEST_CASE("scene_session: Don't Save performs immediately (SS25)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.confirmOpen);

    f.flow.choice = ConfirmChoice::Discard;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.world.entityCount() == 4);  // the new scene
    CHECK(f.commands.count() == 0);
    CHECK(f.commands.isClean());
    CHECK_FALSE(f.flow.confirmOpen);
}

TEST_CASE("scene_session: Quit is guarded and confirmable; a clean quit needs no modal (SS26/S9)") {
    {
        FlowFixture f;
        SceneSession session;
        f.makeDirty();
        f.flow.requested = FileAction::Quit;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK_FALSE(f.flow.quitConfirmed);
        CHECK(f.flow.confirmOpen);

        f.flow.choice = ConfirmChoice::Discard;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.flow.quitConfirmed);
    }
    {
        FlowFixture f;  // clean
        SceneSession session;
        f.flow.requested = FileAction::Quit;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.flow.quitConfirmed);  // immediately -- no modal
        CHECK_FALSE(f.flow.confirmOpen);
    }
}

TEST_CASE("scene_session: a dialog in flight swallows every request (SS27/D8/AC-5/E4)") {
    // A LogSinkScope so this case can discriminate the Quit iteration's own E4 record (finding 6 of
    // the 2.5.1 code-review round: the original SS27 asserted only entityCount()/pending, which stays
    // green even with the swallow guard removed entirely, since nothing here mutated the World either
    // way -- the missing half was the SWALLOW itself, proven by the log record and by `quitConfirmed`).
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::Open;
    const std::size_t countBefore = f.world.entityCount();

    for (const FileAction action : {FileAction::NewScene, FileAction::OpenScene, FileAction::SaveScene,
                                    FileAction::SaveSceneAs, FileAction::Quit}) {
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        f.flow.requested = action;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        CHECK(f.world.entityCount() == countBefore);
        CHECK(f.flow.pending == FileAction::None);
        CHECK_FALSE(f.flow.quitConfirmed);  // a swallowed Quit must NOT confirm

        scope.sink()->take(records);
        const std::size_t expectedInfo = (action == FileAction::Quit) ? 1U : 0U;  // E4, Quit only
        CHECK(countAtLevel(records, engine::LogLevel::Info) == expectedInfo);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
        CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    }
}

TEST_CASE("scene_session: the unsaved-changes modal swallows every request too (SS27b/BLOCKING-2)") {
    // BLOCKING-2 (code review): `flow.confirmOpen` must gate a new request exactly like
    // `flow.dialog != None` does -- otherwise a Ctrl+S fired while the modal is showing falls straight
    // through to AskWhereToSave and can launch a NATIVE Save dialog on top of the still-open ImGui
    // modal (E18's violation). Seeded initial pending = Quit (not the actions under test) so an
    // unfixed swallow is caught the moment ANY of NewScene/OpenScene overwrites it via the guard.
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::Quit;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);  // raises the modal
    REQUIRE(f.flow.confirmOpen);
    REQUIRE(f.flow.pending == FileAction::Quit);
    const std::size_t countBefore = f.world.entityCount();

    for (const FileAction action : {FileAction::NewScene, FileAction::OpenScene, FileAction::SaveScene,
                                    FileAction::SaveSceneAs, FileAction::Quit}) {
        f.flow.requested = action;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.world.entityCount() == countBefore);
        CHECK(f.flow.dialog == engine::editor::DialogKind::None);  // nothing was launched
        CHECK(f.flow.confirmOpen);                                 // the modal itself is untouched
        CHECK(f.flow.pending == FileAction::Quit);                 // the ORIGINAL pending SURVIVES
    }
}

TEST_CASE("scene_session: a modal answer beats a new request carried in the same frame (SS28)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);  // raises the modal
    REQUIRE(f.flow.confirmOpen);

    // ONE call carrying BOTH the modal's answer AND a fresh request.
    f.flow.choice = ConfirmChoice::Discard;
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.world.entityCount() == 4);  // the pending New performed, then the fresh New performed too
    CHECK(f.flow.requested == FileAction::None);
    CHECK_FALSE(f.flow.choice.has_value());
    CHECK(f.flow.pending == FileAction::None);
}

TEST_CASE("scene_session: a modal answer resolves BEFORE a same-frame request re-raises it (SS28b/finding 5)") {
    // finding 5 of the 2.5.1 code-review round: the original SS28 only used `choice = Discard`, which
    // produces an IDENTICAL end state (pending == None) whether the modal or the request is processed
    // first, so it never actually proved the ordering the comment above claims. `choice = Cancel` is
    // order-sensitive: processed first (as the code does), Cancel clears the ORIGINAL pending action,
    // then the fresh request re-raises the guard on its own -- `pending == NewScene`, `confirmOpen ==
    // true`. Processed in the wrong order, the fresh request's guard-raise would itself be undone by
    // the (now second) Cancel -- both cleared.
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);  // raises the modal
    REQUIRE(f.flow.confirmOpen);
    REQUIRE(f.flow.pending == FileAction::NewScene);

    f.flow.choice = ConfirmChoice::Cancel;
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.flow.requested == FileAction::None);
    CHECK_FALSE(f.flow.choice.has_value());
    CHECK(f.flow.pending == FileAction::NewScene);  // the FRESH request's OWN guard-raise, not a leftover
    CHECK(f.flow.confirmOpen);
}

TEST_CASE("scene_session: applyDialogResult -- cancelled is silent, failed logs exactly one ERROR (SS29/AC-13)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::Open;
    f.flow.pending = FileAction::NewScene;
    scope.sink()->take(records);
    records.clear();

    engine::editor::DialogResult cancelled;
    cancelled.ready = true;
    cancelled.cancelled = true;
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, cancelled, f.project);
    scope.sink()->take(records);
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
    CHECK(f.flow.pending == FileAction::None);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
    records.clear();

    f.flow.dialog = engine::editor::DialogKind::Save;
    engine::editor::DialogResult failed;
    failed.ready = true;
    failed.failed = true;
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, failed, f.project);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
}

TEST_CASE(
    "scene_session: applyDialogResult abandons the pending action when the WRITE itself fails "
    "(SS29/D11/S23)") {
    // D11: a save that FAILS (not just a cancelled/failed DIALOG) must abandon the pending action --
    // performing it anyway would apply a "Save" answer the write never actually honoured.
    const TempDir dir;
    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::Save;
    f.flow.saveBeforePending = true;
    f.flow.pending = FileAction::NewScene;
    const std::size_t countBefore = f.world.entityCount();

    engine::editor::DialogResult result;
    result.ready = true;
    result.path = dir.join("missing-subdir/x.scene.json");  // the directory does not exist -> write fails
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);

    CHECK(f.flow.pending == FileAction::None);  // abandoned, not merely deferred
    CHECK_FALSE(f.flow.saveBeforePending);
    CHECK(f.world.entityCount() == countBefore);  // NewScene did NOT run
}

TEST_CASE("scene_session: performAction with no channel and no requestedPath is silent (SS30)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    FlowFixture f;  // clean, host.channel == nullptr, requestedPath empty
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    f.flow.requested = FileAction::OpenScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
}

TEST_CASE(
    "scene_session: applyDialogResult drops an orphaned result whose dialog kind is already None "
    "(SS35/SHOULD-FIX-5)") {
    // 2.6.1's code-review round: a second Browse click before the first project-location dialog
    // answers overwrites `flow.dialog` and (with a real channel) launches a SECOND native dialog.
    // `DialogChannel::take()` always resets its slot, so whichever result answers SECOND is consumed
    // with `flow.dialog` already reset to None by the FIRST -- this case simulates exactly that
    // ordering directly, with no real dialog needed: `flow.dialog` is already None (as if a prior
    // result already claimed and cleared it) when a second, orphaned, READY result with its own path
    // arrives. Before the fix, `applyDialogResult`'s kind chain had no arm for `kind == None` and fell
    // straight through into the Save arm (it never even checked `kind == DialogKind::Save` explicitly,
    // "by elimination"), silently saving the CURRENT scene to "<orphan path>.scene.json" and rebinding
    // the session path -- 2.5.1's BLOCKING-2 again, reached through 2.6.1's Browse button.
    const TempDir dir;
    FlowFixture f;
    SceneSession session;  // untitled
    REQUIRE(session.untitled());
    REQUIRE(f.flow.dialog == engine::editor::DialogKind::None);  // the precondition this case exists to prove

    engine::editor::DialogResult orphan;
    orphan.ready = true;
    orphan.path = dir.join("orphan-folder");  // NOT a scene path -- exactly what a folder dialog yields

    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, orphan, f.project);

    CHECK(session.untitled());  // NOTHING was written or bound
    CHECK_FALSE(engine::editor::fileExists(orphan.path + ".scene.json"));
    CHECK(f.flow.pending == FileAction::None);  // untouched -- this orphan owns no pending action
    CHECK_FALSE(f.flow.saveBeforePending);
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
}

// ---- SS31-SS34: BLOCKING-1, the 2.5.1 code-review round -------------------------------------------
//
// `flow.requestedPath` used to be read as the AskWhereToSave step's OWN save target -- but it is also
// where a DEFERRED OpenScene/SaveSceneAs request's own path lives while the guard's modal is up, and
// those are two different things that must not share one field. A guarded Open's target surviving
// across the modal, followed by the user answering "Save", let the modal's write land on the Open's
// own target instead of a real Save location -- concrete data loss, reachable through the public API.

TEST_CASE(
    "scene_session: a guarded Open + modal Save on an untitled document must not write to the Open "
    "target (SS31/BLOCKING-1)") {
    const TempDir dir;
    const std::string openTarget = dir.join("level1.scene.json");
    const std::string preexisting = R"({"version": 1, "entities": []})";
    REQUIRE(engine::editor::writeTextFileAtomic(openTarget, preexisting).empty());

    FlowFixture f;
    SceneSession session;  // untitled
    f.makeDirty();
    f.flow.requested = FileAction::OpenScene;
    f.flow.requestedPath = openTarget;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.confirmOpen);
    REQUIRE(f.flow.pending == FileAction::OpenScene);
    REQUIRE(f.flow.requestedPath == openTarget);  // still needed to eventually PERFORM the Open

    f.flow.choice = ConfirmChoice::Save;  // the modal's "Save" button
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    // host.channel == nullptr, so AskWhereToSave can launch no dialog and must ABANDON the pending
    // Open rather than silently writing the CURRENT (untitled) scene over the Open's own target -- the
    // exact defect reported: the old code read flow.requestedPath here as ITS OWN save target.
    const engine::editor::FileReadResult after = engine::editor::readTextFile(openTarget);
    REQUIRE(after.text.has_value());
    CHECK(*after.text == preexisting);  // byte-identical: nothing wrote to the Open target
    CHECK(session.untitled());          // nothing was saved at all
    CHECK(f.flow.pending == FileAction::None);
    CHECK(f.flow.requestedPath.empty());
}

TEST_CASE(
    "scene_session: a cancelled guarded Open clears requestedPath, so it cannot hijack a later Save "
    "As (SS32/BLOCKING-1)") {
    const TempDir dir;
    const std::string openTarget = dir.join("level1.scene.json");

    FlowFixture f;
    SceneSession session;  // untitled
    f.makeDirty();
    f.flow.requested = FileAction::OpenScene;
    f.flow.requestedPath = openTarget;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.confirmOpen);
    REQUIRE(f.flow.pending == FileAction::OpenScene);
    REQUIRE(f.flow.requestedPath == openTarget);

    f.flow.choice = ConfirmChoice::Cancel;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.confirmOpen);
    CHECK(f.flow.requestedPath.empty());  // the abandoned Open's own target must not survive

    // A LATER, unrelated Save As with NO explicit path (host.channel == nullptr too, so it can launch
    // no dialog either) must be a silent no-op -- not hijack the abandoned Open's target.
    f.flow.requested = FileAction::SaveSceneAs;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    CHECK(session.untitled());  // nothing was written to openTarget behind the session's back
}

TEST_CASE(
    "scene_session: a guarded Open + modal Save on a TITLED document that fails to write abandons "
    "the Open's own target too (SS33/BLOCKING-1)") {
    // The same leak, at the OTHER resolveConfirm arm: WriteNow (Save, titled) never touched
    // `flow.requestedPath` at all in the old code, on EITHER outcome -- so a failed write left the
    // deferred Open's own target alive indefinitely, same as the Cancel case above.
    const TempDir dir;
    const std::string currentScenePath = dir.join("missing-subdir/current.scene.json");  // the WRITE fails
    const std::string openTarget = dir.join("other.scene.json");

    FlowFixture f;
    SceneSession session;
    session.setPath(currentScenePath);  // TITLED -- resolveConfirm(Save, untitled=false) == WriteNow
    f.makeDirty();
    f.flow.requested = FileAction::OpenScene;
    f.flow.requestedPath = openTarget;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.confirmOpen);
    REQUIRE(f.flow.pending == FileAction::OpenScene);

    f.flow.choice = ConfirmChoice::Save;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);  // WriteNow to a missing directory

    CHECK(f.flow.pending == FileAction::None);  // abandoned, not performed
    CHECK(f.flow.requestedPath.empty());        // the abandoned Open's own target must not leak
}

// SS36 (Phase 2 audit): ONE definition of "a modal surface owns the input". Before this, the
// condition was written out by hand in two places -- `shell_ui.cpp`'s `fileEnabled` and
// `applyFileRequests`' refusal check -- and shell_ui.cpp's own banner names the cost of letting the
// two drift (2.5.1's BLOCKING-2: a chord reached AskWhereToSave while the modal was up and launched a
// SECOND native dialog on top of it). The audit found the drift had already happened in the other
// direction: `fileEnabled` gated every File chord and NEITHER history chord, so Ctrl+Z mutated the
// World behind the unsaved-changes modal that was still asking about the pre-undo document.
TEST_CASE("scene_session: modalInputActive is true iff some modal surface owns the input (SS36)") {
    using engine::editor::DialogKind;
    engine::editor::FileFlow flow;
    engine::editor::ProjectFlow projectFlow;

    SUBCASE("idle: nothing owns the input") { CHECK_FALSE(engine::editor::modalInputActive(flow, projectFlow)); }
    SUBCASE("a native dialog is in flight") {
        flow.dialog = DialogKind::Open;
        CHECK(engine::editor::modalInputActive(flow, projectFlow));
    }
    SUBCASE("the unsaved-changes modal is up") {
        flow.confirmOpen = true;
        CHECK(engine::editor::modalInputActive(flow, projectFlow));
    }
    SUBCASE("the New Project modal is up") {
        projectFlow.form.open = true;
        CHECK(engine::editor::modalInputActive(flow, projectFlow));
    }
    SUBCASE("it agrees with applyFileRequests' own refusal, which is the point of sharing it") {
        // The refusal check inside applyFileRequests is the same predicate; a request raised while a
        // modal is up must be refused, not performed.
        flow.confirmOpen = true;
        REQUIRE(engine::editor::modalInputActive(flow, projectFlow));
        flow.confirmOpen = false;
        flow.dialog = DialogKind::Save;
        CHECK(engine::editor::modalInputActive(flow, projectFlow));
    }
}

// ---- SS37-SS40: task 3.2.4, the Locate... arm ------------------------------------------------------
//
// `applyDialogResult`'s SIGNATURE is byte-identical after this task, and `scene_session.{hpp,cpp}`
// name no Blender type, include no Blender header and know nothing beyond "a string came back"
// (AC-46). These four cases prove the arm's four outcomes, and the FOURTH is the one that matters:
// the arm's PLACEMENT before the terminal Save fall-through is what stops a picked binary path from
// being saved over as a scene.

TEST_CASE(
    "scene_session: a BlenderBinary result lands in pickedBlenderPath and touches NOTHING else "
    "(SS37, task 3.2.4 AC-46)") {
    FlowFixture f;
    SceneSession session;  // untitled
    f.flow.dialog = engine::editor::DialogKind::BlenderBinary;
    f.flow.pending = FileAction::None;

    engine::editor::DialogResult result;
    result.ready = true;
    result.path = "/opt/homebrew/bin/blender";

    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);

    CHECK(f.flow.pickedBlenderPath == "/opt/homebrew/bin/blender");
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);  // consumed, exactly like every other kind
    CHECK(f.flow.pending == FileAction::None);
    // NOT the scene, NOT the project, NOT the save flow.
    CHECK(session.untitled());
    CHECK_FALSE(engine::editor::fileExists(std::string(result.path) + ".scene.json"));
    CHECK(f.project.flow.form.location.empty());
    CHECK(f.project.flow.requestedPath.empty());
    CHECK_FALSE(f.flow.saveBeforePending);
}

TEST_CASE("scene_session: a CANCELLED Locate... is silent and leaves pickedBlenderPath empty (SS38, task 3.2.4)") {
    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::BlenderBinary;

    engine::editor::DialogResult cancelled;
    cancelled.ready = true;
    cancelled.cancelled = true;
    cancelled.path = "/should/never/be/read";

    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, cancelled, f.project);

    CHECK(f.flow.pickedBlenderPath.empty());  // the commonest interaction in this feature, and silent
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
    CHECK(session.untitled());
}

TEST_CASE("scene_session: a FAILED Locate... leaves pickedBlenderPath empty (SS39, task 3.2.4)") {
    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::BlenderBinary;
    f.flow.pending = FileAction::NewScene;  // a pending action the failure must abandon

    engine::editor::DialogResult failed;
    failed.ready = true;
    failed.failed = true;
    failed.path = "/should/never/be/read";

    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, failed, f.project);

    CHECK(f.flow.pickedBlenderPath.empty());
    CHECK(f.flow.pending == FileAction::None);  // the pre-existing failed arm runs FIRST, unchanged
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
}

TEST_CASE(
    "scene_session: an ORPHANED Locate... result never reaches the Save fall-through (SS40, task 3.2.4 "
    "AC-46, seed S30)") {
    // THE IMPORTANT ONE. `DialogChannel` holds a SINGLE slot, so two dialogs cannot be in flight -- but
    // if one ever were, whichever result answers SECOND finds `flow.dialog` already reset to None by
    // the first. Before SHOULD-FIX 5's orphan guard, such a result fell straight through into the Save
    // arm, which is reached BY ELIMINATION and never checks its own kind. With a Blender binary as the
    // path, that means saving the current scene over `/usr/bin/blender.scene.json`.
    const TempDir dir;
    FlowFixture f;
    SceneSession session;
    REQUIRE(f.flow.dialog == engine::editor::DialogKind::None);  // as if a prior result already claimed it

    engine::editor::DialogResult orphan;
    orphan.ready = true;
    orphan.path = dir.join("blender");  // a BINARY path, not a scene path

    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, orphan, f.project);

    CHECK(f.flow.pickedBlenderPath.empty());  // an orphan owns no request, so it sets nothing
    CHECK(session.untitled());                // and NOTHING was written or bound
    CHECK_FALSE(engine::editor::fileExists(orphan.path + ".scene.json"));
    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.saveBeforePending);
}

// ---- SS41-SS46: task E.4.2, the containment refusal at the two choke points -----------------------

namespace {

// True iff ANY record's message contains `needle`. Used only for the NEGATIVE claim "no containment
// wording was logged", which must hold in every build configuration (SS43). TU-local, like every other
// helper in this file -- a free function at namespace scope here would have external linkage.
[[nodiscard]] bool anyMessageContains(const std::vector<engine::editor::LogEntry>& records, std::string_view needle) {
    return std::any_of(records.begin(), records.end(), [needle](const engine::editor::LogEntry& e) {
        return e.message.find(needle) != std::string::npos;
    });
}

// The u8-bytes path constructor, TU-local like every other helper here. NEVER the narrow-char
// std::filesystem::path constructor, which assumes the active code page on Windows.
[[nodiscard]] std::filesystem::path pathOfUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

}  // namespace

TEST_CASE("scene_session: a refused open changes nothing and logs exactly one ERROR (SS41, AC-3/AC-4)") {
    using engine::editor::openSceneFile;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;  // declared FIRST so it is destroyed LAST
    const TempDir tmp;
    const std::string root = tmp.join("ProjA");
    const std::string foreign = tmp.join("Other/x.scene.json");
    // ★ NEITHER DIRECTORY IS CREATED AND THE TARGET DOES NOT EXIST. If readTextFile were reached, the
    //   ERROR would carry the OS's own "no such file" reason instead of the containment reason -- which
    //   is how this case proves the check ran FIRST, on a consequence rather than a syscall counter.
    REQUIRE_FALSE(engine::editor::fileExists(foreign));

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;
    session.setPath("/previous/scene.scene.json");

    // CAPTURE BEFORE, compare AFTER -- never "it looks unchanged".
    const std::size_t entitiesBefore = world.entityCount();
    const std::size_t commandsBefore = commands.count();
    const bool cleanBefore = commands.isClean();
    const std::string pathBefore(session.path());
    const std::size_t selectedBefore = selection.count();

    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    CHECK_FALSE(openSceneFile(ctx, commands, session, foreign, SceneFileContext{root, nullptr}));

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
    // ★ THE ERROR IS THE CONTAINMENT ONE, NOT THE OS'S -- the "readTextFile was never reached"
    //   assertion, made on a consequence rather than on the call's absence.
    REQUIRE(records.size() >= 1U);
    CHECK(records.front().message.find("outside the open project") != std::string::npos);
    CHECK(records.front().message.find("No such file") == std::string::npos);

    CHECK(world.entityCount() == entitiesBefore);
    CHECK(commands.count() == commandsBefore);
    CHECK(commands.isClean() == cleanBefore);
    CHECK(session.path() == pathBefore);
    CHECK(selection.count() == selectedBefore);
}

TEST_CASE("scene_session: a refused save writes nothing, marks nothing clean and rebinds no path (SS42, AC-5)") {
    using engine::editor::saveSceneFile;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string root = tmp.join("ProjA");
    const std::string foreign = tmp.join("Other/x.scene.json");
    // ★ THE TARGET'S DIRECTORY IS CREATED ON PURPOSE, and it is what makes "nothing was written" an
    //   assertion rather than a coincidence: without it writeTextFileAtomic would fail whatever the
    //   containment check did, so CHECK_FALSE(fileExists(foreign)) below would be green for a reason
    //   unrelated to the rule. Measured: moving the containment check BELOW the write leaves that
    //   assertion green on a missing directory and is caught only by the message arm. With the
    //   directory present the write would SUCCEED if it were ever reached.
    std::error_code ec;
    std::filesystem::create_directories(pathOfUtf8(tmp.join("Other")), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    // THE STACK STARTS DIRTY, as IO12 does, so "setClean was not called" is not vacuous. A direct World
    // mutation leaves the stack clean and the assertion would say nothing.
    const engine::Entity probe = world.create();
    REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                   std::vector<engine::Entity>{probe}, std::vector<engine::Entity>{})));
    REQUIRE_FALSE(commands.isClean());
    engine::editor::SceneSession session;
    session.setPath("/previous/scene.scene.json");
    const std::string pathBefore(session.path());

    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    CHECK_FALSE(
        saveSceneFile(ctx, commands, session, foreign, /*appendExtension=*/false, SceneFileContext{root, nullptr}));

    scope.sink()->take(records);
    CHECK_FALSE(commands.isClean());                   // setClean was NOT called
    CHECK(session.path() == pathBefore);               // setPath was NOT called
    CHECK_FALSE(engine::editor::fileExists(foreign));  // nothing was written
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    REQUIRE(records.size() >= 1U);
    CHECK(records.front().message.find("must be saved inside the open project") != std::string::npos);
    CHECK(records.front().message.find("Save Scene As") != std::string::npos);
    // ★ D9 at the message level: a refused SAVE never names another project, so it can never offer one.
    CHECK(records.front().message.find("belongs to") == std::string::npos);
}

TEST_CASE("scene_session: NO_PROJECT_SCENE_CONTEXT permits and logs no containment wording (SS43, D5)") {
    using engine::editor::lexicalContainment;
    using engine::editor::NO_PROJECT_SCENE_CONTEXT;
    using engine::editor::openSceneFile;
    using engine::editor::saveSceneFile;
    using engine::editor::SceneContainment;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string path = tmp.join("level1.scene.json");

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    // DELIBERATELY NO CLAIM ABOUT THE ROUND TRIP SUCCEEDING: that needs the serialization bridge and
    // this TU is ungated, so in a tools-OFF build the save fails with its OWN ERROR. IO17 owns the "and
    // it really loaded" half. What must hold in EVERY configuration is that nothing refused on
    // containment grounds.
    (void)saveSceneFile(ctx, commands, session, path, /*appendExtension=*/false, NO_PROJECT_SCENE_CONTEXT);
    (void)openSceneFile(ctx, commands, session, path, NO_PROJECT_SCENE_CONTEXT);

    scope.sink()->take(records);
    CHECK_FALSE(anyMessageContains(records, "outside the open project"));
    CHECK_FALSE(anyMessageContains(records, "must be saved inside"));
    CHECK_FALSE(anyMessageContains(records, "could not be resolved"));
    CHECK_FALSE(anyMessageContains(records, "belongs to"));
    // ANTI-VACUITY: the sink really was listening -- a tools-OFF build logs one ERROR here and a
    // tools-ON build logs one INFO, so SOMETHING was recorded in either configuration.
    CHECK_FALSE(records.empty());
    // AND the permissive verdict, asserted directly at the predicate, which works in every configuration.
    CHECK((lexicalContainment(tmp.join("anywhere/x.scene.json"), "") == SceneContainment::NoProject));
}

TEST_CASE("scene_session: a refusal with a NULL offer still refuses and still logs (SS46, D12)") {
    // THE EVERY-TEST PATH, asserted rather than assumed: every test call site and every non-UI caller
    // passes offer == nullptr, so a null there must change NOTHING about the refusal's observable half.
    // The first half proves the null pointer is dereferenced nowhere on either choke point's refusal
    // path; the SECOND half (below) runs the SAME refusal twice, once with a null offer and once with
    // a real one, and compares the OBSERVABLE halves byte for byte -- without it, "nullptr raises
    // nothing" is compatible with "nullptr also does nothing else".
    using engine::editor::ContainmentOffer;
    using engine::editor::NO_PROJECT_SCENE_CONTEXT;
    using engine::editor::openSceneFile;
    using engine::editor::saveSceneFile;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string root = tmp.join("ProjA");
    const SceneFileContext refuseOnly{root, nullptr};
    REQUIRE(refuseOnly.offer == nullptr);
    // The permissive value carries no offer either, and names no root -- D12's whole point.
    CHECK(NO_PROJECT_SCENE_CONTEXT.offer == nullptr);
    CHECK(NO_PROJECT_SCENE_CONTEXT.projectRoot.empty());

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    CHECK_FALSE(openSceneFile(ctx, commands, session, tmp.join("Other/x.scene.json"), refuseOnly));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    records.clear();

    CHECK_FALSE(
        saveSceneFile(ctx, commands, session, tmp.join("Other/y.scene.json"), /*appendExtension=*/false, refuseOnly));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK(session.untitled());  // and neither call bound a path
    records.clear();

    // ---- the SECOND arm: the same refusal, null offer vs real offer, compared byte for byte -------
    const std::string sameTarget = tmp.join("Other/z.scene.json");

    CHECK_FALSE(openSceneFile(ctx, commands, session, sameTarget, refuseOnly));
    scope.sink()->take(records);
    REQUIRE(records.size() == 1U);
    const std::size_t nullErrors = countAtLevel(records, engine::LogLevel::Error);
    const std::string nullMessage = records.front().message;
    records.clear();

    ContainmentOffer offer;
    CHECK_FALSE(openSceneFile(ctx, commands, session, sameTarget, SceneFileContext{root, &offer}));
    scope.sink()->take(records);
    REQUIRE(records.size() == 1U);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == nullErrors);
    CHECK(records.front().message == nullMessage);  // the SAME bytes, not merely the same count
    CHECK(session.untitled());                      // and neither call bound a path
    // ANTI-VACUITY: the real offer really WAS filled, so "identical observables" is a statement about
    // two refusals that differ, not about two calls that both did nothing.
    CHECK(offer.open);
    CHECK(offer.refusalSerial == 1U);
    CHECK(offer.scenePath == sameTarget);
    CHECK_FALSE(offer.reason.empty());
}

// ---- SS44-SS49: task E.4.2 commit 4, the offer the refusal raises and the modal's two answers -----

TEST_CASE("scene_session: a refused OPEN fills the offer and names the owning project (SS44, AC-20)") {
    using engine::editor::ContainmentOffer;
    using engine::editor::containmentReason;
    using engine::editor::ContainmentVerdict;
    using engine::editor::CreateProblem;
    using engine::editor::createProject;
    using engine::editor::normalizeForContainment;
    using engine::editor::openSceneFile;
    using engine::editor::ProjectCreateOutcome;
    using engine::editor::resolveSceneContainment;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string rootA = tmp.join("ProjA");
    // A REAL sibling project on disk, built with createProject so the name comes from the same writer
    // loadProjectFrom reads -- never a hand-written manifest that could drift from the real one.
    const ProjectCreateOutcome b = createProject(tmp.utf8(), "ProjB", "0.1.0");
    REQUIRE(b.problem == CreateProblem::Ok);
    const std::string foreign = b.root + "/scenes/x.scene.json";
    REQUIRE(engine::editor::writeTextFileAtomic(foreign, "{}").empty());

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    ContainmentOffer offer;
    CHECK_FALSE(openSceneFile(ctx, commands, session, foreign, SceneFileContext{rootA, &offer}));

    CHECK(offer.open);
    CHECK_FALSE(offer.forSave);
    CHECK(offer.scenePath == foreign);
    CHECK(offer.projectRoot == normalizeForContainment(b.root));
    CHECK(offer.projectName == "ProjB");
    CHECK(offer.refusalSerial == 1U);
    CHECK_FALSE(offer.acceptRequested);  // a raise never answers itself
    CHECK_FALSE(offer.dismissRequested);

    // AC-20: the reason is containmentReason's OWN OUTPUT, not a second wording. Built here from the
    // SAME verdict the choke point resolved -- the only comparison that can catch a drift, because
    // comparing offer.reason against the LOG's text would compare two copies of one string.
    const ContainmentVerdict v = resolveSceneContainment(foreign, rootA, /*findOwningProject=*/true);
    CHECK(offer.reason == containmentReason(v, rootA, /*forSave=*/false));
    CHECK_FALSE(offer.reason.empty());  // anti-vacuity: "" == "" would pass otherwise
    CHECK(offer.reason.find("ProjB") != std::string::npos);
}

TEST_CASE("scene_session: a refused SAVE fills the offer but offers NO project (SS45, D9)") {
    using engine::editor::ContainmentOffer;
    using engine::editor::CreateProblem;
    using engine::editor::createProject;
    using engine::editor::openSceneFile;
    using engine::editor::ProjectCreateOutcome;
    using engine::editor::saveSceneFile;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string rootA = tmp.join("ProjA");
    // THE SAME sibling project exists and is perfectly findable. A save refusal must still offer
    // NOTHING -- accepting one routes through adoptProject -> newScene -> World::clear(), which would
    // discard the very work the user pressed Save to preserve.
    const ProjectCreateOutcome b = createProject(tmp.utf8(), "ProjB", "0.1.0");
    REQUIRE(b.problem == CreateProblem::Ok);

    engine::World world;
    engine::editor::seedDefaultScene(world);
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    engine::editor::SceneSession session;

    ContainmentOffer offer;
    CHECK_FALSE(saveSceneFile(ctx, commands, session, b.root + "/scenes/y.scene.json",
                              /*appendExtension=*/false, SceneFileContext{rootA, &offer}));
    CHECK(offer.open);
    CHECK(offer.forSave);
    CHECK(offer.refusalSerial == 1U);
    CHECK(offer.projectRoot.empty());  // EVEN THOUGH ProjB IS RIGHT THERE
    CHECK(offer.projectName.empty());
    CHECK(offer.reason.find("ProjB") == std::string::npos);
    CHECK(offer.reason.find("Save Scene As") != std::string::npos);

    // THE ANTI-VACUITY ARM: the identical path through openSceneFile DOES fill both fields, so this
    // case is not green merely because findEnclosingProject never works on this machine.
    const std::string openTarget = b.root + "/scenes/x.scene.json";
    REQUIRE(engine::editor::writeTextFileAtomic(openTarget, "{}").empty());
    ContainmentOffer openOffer;
    CHECK_FALSE(openSceneFile(ctx, commands, session, openTarget, SceneFileContext{rootA, &openOffer}));
    CHECK_FALSE(openOffer.forSave);
    CHECK_FALSE(openOffer.projectRoot.empty());
    CHECK(openOffer.projectName == "ProjB");
}

TEST_CASE("scene_session: modalInputActive's four disjuncts, each ALONE (SS47, D10)") {
    using engine::editor::DialogKind;
    using engine::editor::FileFlow;
    using engine::editor::modalInputActive;
    using engine::editor::ProjectFlow;

    FileFlow flow;
    ProjectFlow projectFlow;
    CHECK_FALSE(modalInputActive(flow, projectFlow));  // the baseline
    flow.dialog = DialogKind::Open;
    CHECK(modalInputActive(flow, projectFlow));
    flow = {};
    flow.confirmOpen = true;
    CHECK(modalInputActive(flow, projectFlow));
    flow = {};
    projectFlow.form.open = true;
    CHECK(modalInputActive(flow, projectFlow));
    projectFlow = {};
    // ★ the NEW disjunct, alone -- so S10 (dropping it) reddens exactly this assertion rather than
    //   being masked by a sibling that happened to be true at the same time.
    flow.containmentOffer.open = true;
    CHECK(modalInputActive(flow, projectFlow));
    flow.containmentOffer.open = false;
    CHECK_FALSE(modalInputActive(flow, projectFlow));  // and it clears

    // The OTHER ContainmentOffer fields must NOT make it true on their own -- a disjunct on `forSave`
    // or on a non-empty `scenePath` would keep every File chord dead after a dismissed modal.
    flow.containmentOffer.forSave = true;
    flow.containmentOffer.scenePath = "/w/x.scene.json";
    flow.containmentOffer.refusalSerial = 7U;
    CHECK_FALSE(modalInputActive(flow, projectFlow));
}

TEST_CASE("scene_session: the step-0 drain applies the offer's two answers (SS48, D9/D10)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    // ---- (a) ACCEPT on an OPEN-shaped offer.
    f.flow.containmentOffer.open = true;
    f.flow.containmentOffer.projectRoot = "/w/ProjB";
    f.flow.containmentOffer.projectName = "ProjB";
    f.flow.containmentOffer.acceptRequested = true;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    // CAREFUL: step 2 consumes flow.requested in the SAME call, so asserting `requested ==
    // OpenProject` afterwards would be asserting the request SURVIVED -- which is the bug (S25), not
    // the feature. The observable is the CONSEQUENCE: openProjectPath ran against "/w/ProjB", which
    // does not exist, so it logged one ERROR and changed nothing.
    CHECK((f.flow.requested == FileAction::None));
    CHECK(f.projectFlow.requestedPath.empty());  // ...because OpenProject was PERFORMED
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    REQUIRE(records.size() >= 1U);
    CHECK(records.front().message.find("could not open project") != std::string::npos);
    CHECK(records.front().message.find("/w/ProjB") != std::string::npos);
    CHECK_FALSE(f.flow.containmentOffer.open);  // default-constructed
    CHECK(f.flow.containmentOffer.refusalSerial == 0U);
    CHECK(f.flow.containmentOffer.projectRoot.empty());
    CHECK_FALSE(f.flow.containmentOffer.acceptRequested);
    records.clear();

    // ---- (b) ACCEPT on a SAVE-shaped offer, set DIRECTLY (bypassing the button that would never be
    //          drawn). The re-test inside the drain is what must refuse it -- S12's value-level half.
    f.flow.containmentOffer = {};
    f.flow.containmentOffer.open = true;
    f.flow.containmentOffer.forSave = true;
    f.flow.containmentOffer.projectRoot = "/w/ProjB";  // set on purpose -- the ONLY guard is `forSave`
    f.flow.containmentOffer.acceptRequested = true;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK((f.flow.requested == FileAction::None));
    CHECK(f.projectFlow.requestedPath.empty());  // NOTHING was requested
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);  // openProjectPath never ran at all
    CHECK_FALSE(f.flow.containmentOffer.open);                   // but the modal still CLOSED
    records.clear();

    // ---- (c) DISMISS clears and requests nothing.
    f.flow.containmentOffer = {};
    f.flow.containmentOffer.open = true;
    f.flow.containmentOffer.projectRoot = "/w/ProjB";
    f.flow.containmentOffer.dismissRequested = true;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK_FALSE(f.flow.containmentOffer.open);
    CHECK((f.flow.requested == FileAction::None));
    CHECK(f.projectFlow.requestedPath.empty());
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("scene_session: the containment modal and the unsaved-changes modal are never both up (SS49, AC-14)") {
    using engine::editor::ProjectManifest;

    // ---- (a) A refusal raised INSIDE applyFileRequests leaves confirmOpen FALSE. A CLEAN document
    //          requesting an OpenScene on a foreign path: guardFor returns Perform, performAction
    //          calls openSceneFile, which refuses and raises through &flow.containmentOffer.
    const LogFixture fixture;
    const TempDir tmp;
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, tmp.join("ProjA"));
        REQUIRE(f.commands.isClean());

        f.flow.requested = FileAction::OpenScene;
        f.flow.requestedPath = tmp.join("Other/x.scene.json");
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        CHECK(f.flow.containmentOffer.open);
        CHECK_FALSE(f.flow.confirmOpen);  // AC-14
        CHECK((f.flow.pending == FileAction::None));
    }

    // ---- (b) The unsaved-changes modal's "Save" answer on a TITLED-but-outside scene refuses, and
    //          the containment offer takes the modal's place rather than joining it.
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, tmp.join("ProjA"));
        session.setPath(tmp.join("Other/current.scene.json"));  // titled, and outside the root
        f.makeDirty();
        const std::size_t entitiesBefore = f.world.entityCount();

        f.flow.requested = FileAction::OpenScene;
        f.flow.requestedPath = tmp.join("Other/next.scene.json");
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        REQUIRE(f.flow.confirmOpen);
        REQUIRE((f.flow.pending == FileAction::OpenScene));

        // resolveConfirm(Save, titled) -> WriteNow -> saveSceneFile, which refuses on containment.
        f.flow.choice = ConfirmChoice::Save;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        CHECK_FALSE(f.flow.confirmOpen);         // cleared BEFORE the save ran
        CHECK(f.flow.containmentOffer.open);     // and the offer is up in its place
        CHECK(f.flow.containmentOffer.forSave);  // in its SAVE shape (D9)
        CHECK(f.flow.containmentOffer.projectRoot.empty());
        CHECK((f.flow.pending == FileAction::None));  // the pending action is ABANDONED
        CHECK(f.flow.requestedPath.empty());          // BLOCKING-1's roster, BOTH halves
        CHECK(f.projectFlow.requestedPath.empty());
        CHECK(f.world.entityCount() == entitiesBefore);  // the OpenScene did NOT happen
        CHECK_FALSE(f.commands.isClean());               // and setClean did not run either

        // AC-14 stated as the invariant: never both. A NAMED BOOL, not `CHECK_FALSE(a && b)` --
        // doctest FORBIDS `&&` on a decomposed expression (doctest.h:2015, "Expression Too Complex"),
        // and CHECK_FALSE decomposes exactly as CHECK does.
        const bool bothModalsUp = f.flow.confirmOpen && f.flow.containmentOffer.open;
        CHECK_FALSE(bothModalsUp);
    }
}

TEST_CASE(
    "scene_session: every production call site builds the context from root(), never scenesRoot() (SS51, D1/AC-2)") {
    // ★ WHY THIS CASE EXISTS, AND WHY IO18 IS NOT IT. IO18 asserts the same rule at the PREDICATE, but
    //   it constructs its own SceneFileContext and hands it in, so it cannot see which root
    //   scene_session.cpp's own call sites chose. Measured directly: replacing
    //   `project.session.root()` with `project.session.scenesRoot()` at all six production sites --
    //   the exact value FileDialogHost::projectRoot is bound to (editor_app.cpp's two host
    //   constructions), and the single most likely accidental defect in this task -- left BOTH
    //   binaries entirely green. Everything below drives a PRODUCTION site instead, so a context built
    //   from the wrong root refuses a scene that is plainly inside the project.
    //
    //   The observable is `containmentOffer.open`, never the file or the World: containment is
    //   resolved BEFORE any I/O and before sceneIoAvailable(), so every arm reads the same in all
    //   three build configurations.
    using engine::editor::DialogKind;
    using engine::editor::DialogResult;
    using engine::editor::ProjectManifest;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string root = tmp.join("ProjA");
    const std::string inside = root + "/assets/levels/deep.scene.json";  // in the project, NOT in scenes/
    const std::string outside = tmp.join("Other/x.scene.json");          // outside the project entirely
    std::error_code ec;
    std::filesystem::create_directories(pathOfUtf8(root + "/assets/levels"), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));

    // ---- (a) performAction's OpenScene site, through applyFileRequests.
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, root);
        // THE PRECONDITION THAT MAKES EVERY ARM BELOW DISCRIMINATING: the two roots really do differ,
        // and `inside` is under one and not the other.
        REQUIRE(f.projectSession.scenesRoot() != root);
        REQUIRE(inside.rfind(f.projectSession.scenesRoot(), 0) != 0U);
        REQUIRE(inside.rfind(root, 0) == 0U);

        f.flow.requested = FileAction::OpenScene;
        f.flow.requestedPath = inside;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK_FALSE(f.flow.containmentOffer.open);
    }

    // ---- (b) performAction's SaveSceneAs site, through applyFileRequests.
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, root);
        f.flow.requested = FileAction::SaveSceneAs;
        f.flow.requestedPath = inside;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK_FALSE(f.flow.containmentOffer.open);
    }

    // ---- (c) applyFileRequests' own SaveScene/WriteNow site, on a TITLED session.
    {
        FlowFixture f;
        SceneSession session;
        session.setPath(inside);
        f.projectSession.set(ProjectManifest{}, root);
        f.flow.requested = FileAction::SaveScene;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK_FALSE(f.flow.containmentOffer.open);
    }

    // ---- (d) applyDialogResult's Open site.
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, root);
        f.flow.dialog = DialogKind::Open;
        DialogResult result;
        result.ready = true;
        result.path = inside;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);
        CHECK_FALSE(f.flow.containmentOffer.open);
    }

    // ---- (e) applyDialogResult's Save site -- the only one that appends the extension (D13).
    {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, root);
        f.flow.dialog = DialogKind::Save;
        DialogResult result;
        result.ready = true;
        result.path = inside;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);
        CHECK_FALSE(f.flow.containmentOffer.open);
    }

    // ---- ★ THE ANTI-VACUITY ARMS. Without them every CHECK_FALSE above is satisfied by a build in
    //      which containment is never consulted at all -- which is precisely what this file's own
    //      SceneFileContext-in-hand cases cannot rule out for the production sites. One OPEN and one
    //      SAVE, the same two functions, a path genuinely outside the project: the offer DOES rise,
    //      and it carries the containment wording.
    {
        const engine::editor::LogSinkScope scope;
        std::vector<engine::editor::LogEntry> records;
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, root);
        scope.sink()->take(records);
        records.clear();

        f.flow.requested = FileAction::OpenScene;
        f.flow.requestedPath = outside;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.flow.containmentOffer.open);
        CHECK_FALSE(f.flow.containmentOffer.forSave);
        scope.sink()->take(records);
        CHECK(anyMessageContains(records, "outside the open project"));
        records.clear();

        f.flow.containmentOffer = {};
        f.flow.requested = FileAction::SaveSceneAs;
        f.flow.requestedPath = outside;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.flow.containmentOffer.open);
        CHECK(f.flow.containmentOffer.forSave);
        scope.sink()->take(records);
        CHECK(anyMessageContains(records, "must be saved inside the open project"));
    }
}

// ---- SS52-SS54: the code-review round ------------------------------------------------------------

TEST_CASE(
    "scene_session: a containment-refused SAVE abandons the pending action's own target, whichever "
    "flow object it lives in (SS52/BLOCKING-1)") {
    // THE CHAIN THIS CLOSES, end to end, every step reachable from the UI: an open is refused and the
    // user accepts the offer -> the step-0 drain writes ProjB into project.flow.requestedPath and
    // requests OpenProject -> the document is DIRTY, so guardFor raises the unsaved-changes modal and
    // the OpenProject becomes flow.pending with its target parked -> the user answers "Save" on an
    // UNTITLED document -> AskWhereToSave launches the native Save panel -> the user picks a folder
    // OUTSIDE the open project -> containment REFUSES the write. The pending action is abandoned at
    // that point, so its target must go with it: the Save arm cleared flow.pending and
    // flow.saveBeforePending but left BOTH requestedPath fields set, and the next File > Open
    // Project... then took performAction's no-dialog seam and adopted ProjB with no dialog and no
    // click. Every other abandon site in this file clears both; this arm was the hole.
    using engine::editor::CreateProblem;
    using engine::editor::createProject;
    using engine::editor::DialogKind;
    using engine::editor::DialogResult;
    using engine::editor::ProjectCreateOutcome;
    using engine::editor::ProjectManifest;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string rootA = tmp.join("ProjA");
    // A REAL project on disk, so the leak's consequence is an ACTUAL adopt rather than a failed one:
    // with the defect present the second applyFileRequests below opens ProjB for real.
    const ProjectCreateOutcome b = createProject(tmp.utf8(), "ProjB", "0.1.0");
    REQUIRE(b.problem == CreateProblem::Ok);

    FlowFixture f;
    SceneSession session;  // UNTITLED -- the AskWhereToSave arm's own precondition
    f.projectSession.set(ProjectManifest{}, rootA);
    REQUIRE(session.untitled());

    // The state AskWhereToSave leaves behind, seeded directly: the native Save panel is in flight, it
    // is the modal's "Save" answer, and the deferred OpenProject's target is parked where that action
    // keeps it -- project.flow.requestedPath, a DIFFERENT flow object's field from flow.requestedPath.
    f.flow.dialog = DialogKind::Save;
    f.flow.saveBeforePending = true;
    f.flow.pending = FileAction::OpenProject;
    f.projectFlow.requestedPath = b.root;

    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    DialogResult result;
    result.ready = true;
    result.path = tmp.join("Elsewhere/x.scene.json");  // outside ProjA -> the containment refusal
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);

    // The refusal really was a CONTAINMENT one, not a write failure -- without this the case would
    // pass for the wrong reason on any path the filesystem happened to reject.
    REQUIRE(f.flow.containmentOffer.open);
    REQUIRE(f.flow.containmentOffer.forSave);
    CHECK(session.untitled());  // nothing was written or bound
    CHECK((f.flow.pending == FileAction::None));
    CHECK_FALSE(f.flow.saveBeforePending);
    CHECK(f.flow.requestedPath.empty());
    CHECK(f.projectFlow.requestedPath.empty());  // ★ the hole: the abandoned action's OWN target

    // ---- ★ THE CONSEQUENCE, and it is what makes the assertion above more than a field read: the
    //      user dismisses the modal and later asks for File > Open Project... A leaked target makes
    //      performAction take its no-dialog seam and ADOPT ProjB silently.
    //
    //      The observable is the ADOPT, never `flow.dialog == DialogKind::ProjectFolder`: this tier
    //      has no DialogChannel (FileDialogHost{} is the null-channel seam, and file_dialog.hpp is
    //      src-private), so with the target correctly cleared the arm is A17's silent no-op and
    //      `flow.dialog` stays None in BOTH directions. The project root does not.
    f.flow.containmentOffer = {};
    records.clear();
    f.flow.requested = FileAction::OpenProject;
    REQUIRE(f.commands.isClean());  // so step 2 PERFORMS rather than raising the guard's modal
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.projectSession.root() == rootA);  // NOT ProjB -- no project was adopted
    scope.sink()->take(records);
    CHECK_FALSE(anyMessageContains(records, "opened project"));
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
}

TEST_CASE("scene_session: a NEW refusal clears the previous offer's unanswered buttons (SS53, §7.3)") {
    // WHY A ONE-SHOT CAN OUTLIVE ITS OFFER AT ALL, measured in the product's own ordering:
    // applyDialogResult runs at editor_app.cpp:640 and the step-0 drain runs inside drawShellUi at
    // :1134, so a refusal raised by a dialog result lands BETWEEN the button (or the request hook)
    // recording an answer and the drain consuming it. Without the two clears, that answer is applied
    // to the NEW offer: the user pressed "Open ProjB" and the editor opens ProjC. A wrong-target
    // action, not merely a lost click -- which is why the raise clears them rather than preserving
    // them.
    using engine::editor::CreateProblem;
    using engine::editor::createProject;
    using engine::editor::normalizeForContainment;
    using engine::editor::openSceneFile;
    using engine::editor::ProjectCreateOutcome;
    using engine::editor::ProjectManifest;
    using engine::editor::saveSceneFile;
    using engine::editor::SceneFileContext;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string rootA = tmp.join("ProjA");
    const ProjectCreateOutcome b = createProject(tmp.utf8(), "ProjB", "0.1.0");
    const ProjectCreateOutcome c = createProject(tmp.utf8(), "ProjC", "0.1.0");
    REQUIRE(b.problem == CreateProblem::Ok);
    REQUIRE(c.problem == CreateProblem::Ok);
    REQUIRE(b.root != c.root);  // anti-vacuity: the two targets really are different projects
    const std::string sceneInC = c.root + "/scenes/x.scene.json";

    SUBCASE("an ACCEPT pressed on offer A never opens offer B's project") {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, rootA);
        f.flow.containmentOffer.open = true;
        f.flow.containmentOffer.projectRoot = b.root;  // offer A: "Open ProjB"
        f.flow.containmentOffer.projectName = "ProjB";
        f.flow.containmentOffer.acceptRequested = true;

        CHECK_FALSE(openSceneFile(f.ctx, f.commands, session, sceneInC,
                                  SceneFileContext{f.projectSession.root(), &f.flow.containmentOffer}));
        // The raise really did overwrite the offer -- otherwise the drain below would be answering the
        // same offer it was pressed on and the case would assert nothing.
        REQUIRE(f.flow.containmentOffer.projectRoot == normalizeForContainment(c.root));

        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        CHECK(f.flow.containmentOffer.open);      // the NEW modal is still up, awaiting its own answer
        CHECK(f.projectSession.root() == rootA);  // and NOTHING was adopted -- neither ProjB nor ProjC
        CHECK(f.projectFlow.requestedPath.empty());
    }

    SUBCASE("an ACCEPT pressed on offer A never silently dismisses a SAVE-shaped offer B") {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, rootA);
        f.flow.containmentOffer.open = true;
        f.flow.containmentOffer.projectRoot = b.root;
        f.flow.containmentOffer.acceptRequested = true;

        CHECK_FALSE(saveSceneFile(f.ctx, f.commands, session, tmp.join("Elsewhere/x.scene.json"),
                                  /*appendExtension=*/false,
                                  SceneFileContext{f.projectSession.root(), &f.flow.containmentOffer}));
        REQUIRE(f.flow.containmentOffer.forSave);
        REQUIRE(f.flow.containmentOffer.projectRoot.empty());  // D9: a save offers nothing...

        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        // ...so the stale accept would fail the drain's `offerable` re-test and close the modal
        // WITHOUT opening anything -- a refusal the user never saw and never answered.
        CHECK(f.flow.containmentOffer.open);
        CHECK(f.flow.containmentOffer.forSave);
        CHECK(f.projectSession.root() == rootA);
    }

    SUBCASE("a DISMISS pressed on offer A never swallows offer B") {
        FlowFixture f;
        SceneSession session;
        f.projectSession.set(ProjectManifest{}, rootA);
        f.flow.containmentOffer.open = true;
        f.flow.containmentOffer.projectRoot = b.root;
        f.flow.containmentOffer.dismissRequested = true;  // the OTHER one-shot, on its own

        CHECK_FALSE(openSceneFile(f.ctx, f.commands, session, sceneInC,
                                  SceneFileContext{f.projectSession.root(), &f.flow.containmentOffer}));
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

        CHECK(f.flow.containmentOffer.open);
        CHECK(f.projectSession.root() == rootA);
    }
}

TEST_CASE("scene_session: the drain preserves refusalSerial, so a refusal in the same tick is not lost (SS54)") {
    // refusalSerial is MONOTONIC FOR THE LIFETIME OF THE FileFlow and EditorApp mirrors it as an
    // ABSOLUTE value. A drain that reset it to 0 made the second of "dismiss + a fresh refusal in one
    // applyFileRequests call" invisible to every counter above it: 1 -> 0 -> 1 is indistinguishable
    // from "nothing happened", and sceneContainmentRefusalCount() is the GPU tier's ONLY window into a
    // refusal. I191 is the same claim one tier up.
    using engine::editor::ProjectManifest;

    const LogFixture fixture;
    const TempDir tmp;
    const std::string rootA = tmp.join("ProjA");

    FlowFixture f;
    SceneSession session;
    f.projectSession.set(ProjectManifest{}, rootA);

    f.flow.requested = FileAction::OpenScene;
    f.flow.requestedPath = tmp.join("Other/one.scene.json");
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    REQUIRE(f.flow.containmentOffer.open);
    REQUIRE(f.flow.containmentOffer.refusalSerial == 1U);

    // ONE call carrying BOTH answers-then-request: the drain runs at step 0 and the new refusal at
    // step 2, so the serial has to survive the drain or the second refusal reads as the first.
    f.flow.containmentOffer.dismissRequested = true;
    f.flow.requested = FileAction::OpenScene;
    f.flow.requestedPath = tmp.join("Other/two.scene.json");
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.flow.containmentOffer.open);  // the second refusal DID raise...
    CHECK(f.flow.containmentOffer.scenePath == tmp.join("Other/two.scene.json"));
    CHECK(f.flow.containmentOffer.refusalSerial == 2U);  // ...and it is distinguishable from the first

    // The OTHER drain site preserves it too. This offer names no project (nothing above
    // <tmp>/Other holds a project.json), so the accept requests nothing and only closes the modal.
    REQUIRE(f.flow.containmentOffer.projectRoot.empty());
    f.flow.containmentOffer.acceptRequested = true;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    CHECK_FALSE(f.flow.containmentOffer.open);
    CHECK(f.flow.containmentOffer.refusalSerial == 2U);
}
