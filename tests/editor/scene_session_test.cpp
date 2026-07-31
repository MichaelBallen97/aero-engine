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

    CHECK(world.entityCount() == 3);
    std::vector<std::string> names;
    world.eachEntity([&](engine::Entity e) { names.emplace_back(world.name(e)); });
    std::sort(names.begin(), names.end());
    const std::vector<std::string> expected = {"Cube", "Directional Light", "Main Camera"};
    CHECK(names == expected);

    CHECK(commands.count() == 0);
    CHECK(commands.isClean());

    roots.reconcile(world);
    CHECK(roots.entities().size() == 3);  // a fresh forest -- three roots, no parenting
}

TEST_CASE("scene_session: newScene is idempotent (SS13/S5)") {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};

    engine::editor::newScene(ctx, commands);
    engine::editor::newScene(ctx, commands);

    CHECK(world.entityCount() == 3);  // NOT 6 -- S5's discriminator (seed before clear, not after)
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

    void makeDirty() {
        const engine::Entity a = world.create();
        REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{a}, std::vector<engine::Entity>{})));
    }
};

}  // namespace

TEST_CASE("scene_session: New performs immediately when clean, and produces three entities (SS21)") {
    FlowFixture f;
    SceneSession session;
    f.flow.requested = FileAction::NewScene;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

    CHECK(f.world.entityCount() == 3);
    CHECK(f.flow.requested == FileAction::None);
    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.confirmOpen);
}

TEST_CASE("scene_session: a request never survives its frame (SS22)") {
    FlowFixture f;
    SceneSession session;
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
    CHECK(f.flow.requested == FileAction::None);
    CHECK_FALSE(f.flow.choice.has_value());

    const std::size_t countAfterFirst = f.world.entityCount();
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);  // a second call is a no-op
    CHECK(f.world.entityCount() == countAfterFirst);
}

TEST_CASE("scene_session: the guard raises on a dirty document (SS23)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    const std::size_t countBefore = f.world.entityCount();
    f.flow.requested = FileAction::NewScene;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

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
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
    REQUIRE(f.flow.confirmOpen);

    f.flow.choice = ConfirmChoice::Cancel;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

    CHECK(f.world.entityCount() == countBefore);
    CHECK(f.commands.isClean() == cleanBefore);
    CHECK(f.flow.pending == FileAction::None);
    CHECK_FALSE(f.flow.confirmOpen);

    // A further frame does nothing -- no pending action survived (S21's discriminator).
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
    CHECK(f.world.entityCount() == countBefore);
}

TEST_CASE("scene_session: Don't Save performs immediately (SS25)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
    REQUIRE(f.flow.confirmOpen);

    f.flow.choice = ConfirmChoice::Discard;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

    CHECK(f.world.entityCount() == 3);  // the new scene
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
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
        CHECK_FALSE(f.flow.quitConfirmed);
        CHECK(f.flow.confirmOpen);

        f.flow.choice = ConfirmChoice::Discard;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
        CHECK(f.flow.quitConfirmed);
    }
    {
        FlowFixture f;  // clean
        SceneSession session;
        f.flow.requested = FileAction::Quit;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
        CHECK(f.flow.quitConfirmed);  // immediately -- no modal
        CHECK_FALSE(f.flow.confirmOpen);
    }
}

TEST_CASE("scene_session: a dialog in flight swallows every request (SS27/D8/AC-5)") {
    FlowFixture f;
    SceneSession session;
    f.flow.dialog = engine::editor::DialogKind::Open;
    const std::size_t countBefore = f.world.entityCount();

    for (const FileAction action : {FileAction::NewScene, FileAction::OpenScene, FileAction::SaveScene,
                                    FileAction::SaveSceneAs, FileAction::Quit}) {
        f.flow.requested = action;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);
        CHECK(f.world.entityCount() == countBefore);
        CHECK(f.flow.pending == FileAction::None);
    }
}

TEST_CASE("scene_session: a modal answer beats a new request carried in the same frame (SS28)") {
    FlowFixture f;
    SceneSession session;
    f.makeDirty();
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);  // raises the modal
    REQUIRE(f.flow.confirmOpen);

    // ONE call carrying BOTH the modal's answer AND a fresh request.
    f.flow.choice = ConfirmChoice::Discard;
    f.flow.requested = FileAction::NewScene;
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

    CHECK(f.world.entityCount() == 3);  // the pending New performed, then the fresh New performed too
    CHECK(f.flow.requested == FileAction::None);
    CHECK_FALSE(f.flow.choice.has_value());
    CHECK(f.flow.pending == FileAction::None);
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
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, cancelled);
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
    applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, failed);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
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
    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host);

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
    CHECK(f.flow.dialog == engine::editor::DialogKind::None);
}
