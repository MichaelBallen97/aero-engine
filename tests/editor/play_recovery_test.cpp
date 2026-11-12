// Aero Engine — the play record (<projectRoot>/Library/play-recovery.json and its scene copy), its two pure
// deciders and its four disk functions (task 4.7.3, RV1-RV8), and the project flow's offer (RV10-RV16). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING in
// all three configurations. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
#include <aero/core/log.hpp>
#include <aero/editor/asset_cache.hpp>  // LIBRARY_GITIGNORE_TEXT / ASSET_CACHE_GITIGNORE_NAME -- the SHIPPED constants
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/play_recovery.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/project_state.hpp>    // MAX_PROJECT_STATE_SCENE_BYTES
#include <aero/editor/project_ui_view.hpp>  // task 4.7.3 (RV17, RV18): the recovery dialog's layout and label
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::parsePlayRecoveryState;
using engine::editor::planPlayAutosave;
using engine::editor::PlayAutosavePlan;
using engine::editor::PlayRecoveryFacts;
using engine::editor::playRecoveryOffered;
using engine::editor::PlayRecoveryRead;
using engine::editor::playRecoveryScenePath;
using engine::editor::PlayRecoveryState;
using engine::editor::playRecoveryStatePath;
using engine::editor::readPlayRecoveryState;
using engine::editor::writePlayRecoveryCopy;
using engine::editor::writePlayRecoveryState;
using engine::editor::writePlayRecoveryStateText;

namespace {

// Scaffolding is copied, the assertion is shared (project_state_test.cpp:43's posture): a unique temp
// directory that removes itself.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_play_recovery_test_" + std::to_string(++counter));
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
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dirPath; }

private:
    std::filesystem::path dirPath;
};

[[nodiscard]] std::optional<std::string> readBytes(std::string_view path) {
    return engine::editor::readTextFile(path).text;
}

// How many entries anywhere under `root` have a name starting with "play-recovery".
[[nodiscard]] std::size_t countRecoveryEntries(const std::filesystem::path& root) {
    std::size_t found = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.starts_with("play-recovery")) {
            ++found;
        }
    }
    return found;
}

constexpr std::string_view FULL_TEXT =
    "{\n  \"version\": 1,\n  \"playing\": true,\n  \"scene\": \"scenes/main.scene.json\",\n"
    "  \"copyWritten\": true\n}\n";

}  // namespace

TEST_CASE("play_recovery: the record's text is canonical, byte for byte, and re-parses equal (RV1)") {
    const PlayRecoveryState state{.playing = true, .scene = "scenes/main.scene.json", .copyWritten = true};
    const std::string text = writePlayRecoveryStateText(state);
    CHECK(text == FULL_TEXT);
    CHECK(writePlayRecoveryStateText(state) == text);  // two writes are byte-identical
    const std::optional<PlayRecoveryState> back = parsePlayRecoveryState(text);
    REQUIRE(back.has_value());
    CHECK((*back == state));

    // An untitled scene is WRITTEN as "", not omitted: the parser requires the key.
    const PlayRecoveryState untitled{.playing = false, .scene = "", .copyWritten = false};
    const std::string untitledText = writePlayRecoveryStateText(untitled);
    CHECK(untitledText ==
          "{\n  \"version\": 1,\n  \"playing\": false,\n  \"scene\": \"\",\n  \"copyWritten\": false\n}\n");
    const std::optional<PlayRecoveryState> untitledBack = parsePlayRecoveryState(untitledText);
    REQUIRE(untitledBack.has_value());
    CHECK((*untitledBack == untitled));
}

TEST_CASE("play_recovery: a damaged record is nullopt, never defaults (RV2)") {
    const auto doc = [](std::string_view version, std::string_view playing, std::string_view scene,
                        std::string_view copy) {
        std::string text = "{";
        const auto add = [&text](std::string_view key, std::string_view value) {
            if (value.empty()) {
                return;  // an empty value omits the key
            }
            if (text.size() > 1U) {
                text += ", ";
            }
            text += '"';
            text += key;
            text += "\": ";
            text += value;
        };
        add("version", version);
        add("playing", playing);
        add("scene", scene);
        add("copyWritten", copy);
        text += '}';
        return text;
    };
    CHECK(parsePlayRecoveryState(doc("1", "true", "\"a.scene.json\"", "true")).has_value());  // anti-vacuity

    CHECK_FALSE(parsePlayRecoveryState("").has_value());
    CHECK_FALSE(parsePlayRecoveryState("{").has_value());
    CHECK_FALSE(parsePlayRecoveryState("[1,2]").has_value());                           // an array root
    CHECK_FALSE(parsePlayRecoveryState(doc("", "true", "\"a\"", "true")).has_value());  // version absent
    CHECK_FALSE(parsePlayRecoveryState(doc("2", "true", "\"a\"", "true")).has_value());
    CHECK_FALSE(parsePlayRecoveryState(doc("\"1\"", "true", "\"a\"", "true")).has_value());
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "", "\"a\"", "true")).has_value());  // playing absent
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", "", "true")).has_value());   // scene absent
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", "\"a\"", "")).has_value());  // copyWritten absent
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "1", "\"a\"", "true")).has_value());
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", "\"a\"", "\"true\"")).has_value());
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", "7", "true")).has_value());

    const std::string tooLong(engine::editor::MAX_PROJECT_STATE_SCENE_BYTES + 1U, 'a');
    CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", '"' + tooLong + '"', "true")).has_value());
    const std::string longest(engine::editor::MAX_PROJECT_STATE_SCENE_BYTES, 'a');
    CHECK(parsePlayRecoveryState(doc("1", "true", '"' + longest + '"', "true")).has_value());

    for (const std::string_view illegal : {"/abs", "a/../b", "a\\\\b", "C:/x"}) {
        CAPTURE(illegal);
        const std::string quoted = '"' + std::string(illegal) + '"';
        CHECK_FALSE(parsePlayRecoveryState(doc("1", "true", quoted, "true")).has_value());
    }

    // An unknown extra key is ignored: append-only evolution.
    const std::optional<PlayRecoveryState> extra = parsePlayRecoveryState(
        R"({"version": 1, "playing": true, "scene": "a.scene.json", "copyWritten": false, "later": [1]})");
    REQUIRE(extra.has_value());
    CHECK(extra->playing);
    CHECK(extra->scene == "a.scene.json");
    CHECK_FALSE(extra->copyWritten);
}

TEST_CASE("play_recovery: the offer is true for exactly one of the 64 fact rows (RV3)") {
    std::size_t offered = 0;
    for (unsigned row = 0; row < 64U; ++row) {
        const PlayRecoveryFacts facts{
            .sceneIo = (row & 1U) != 0U,
            .found = (row & 2U) != 0U,
            .corrupt = (row & 4U) != 0U,
            .playing = (row & 8U) != 0U,
            .copyWritten = (row & 16U) != 0U,
            .copyExists = (row & 32U) != 0U,
        };
        const bool expected = row == (1U | 2U | 8U | 16U | 32U);  // everything true but `corrupt`
        CAPTURE(row);
        CHECK(playRecoveryOffered(facts) == expected);
        offered += playRecoveryOffered(facts) ? 1U : 0U;
    }
    CHECK(offered == 1U);
}

TEST_CASE("play_recovery: planPlayAutosave answers the four (dirty, sceneIo) rows (RV4)") {
    CHECK((planPlayAutosave(false, false) == PlayAutosavePlan{false, false, false}));
    CHECK((planPlayAutosave(false, true) == PlayAutosavePlan{false, false, false}));
    CHECK((planPlayAutosave(true, false) == PlayAutosavePlan{false, true, false}));
    CHECK((planPlayAutosave(true, true) == PlayAutosavePlan{true, false, true}));
}

TEST_CASE("play_recovery: both paths sit under <root>/Library/, '/'-joined, and \"\" gives \"\" (RV5)") {
    CHECK(playRecoveryStatePath("/p/Game") == "/p/Game/Library/play-recovery.json");
    CHECK(playRecoveryScenePath("/p/Game") == "/p/Game/Library/play-recovery.scene.json");
    CHECK(playRecoveryStatePath("/p/Game/") == "/p/Game/Library/play-recovery.json");  // no "//"
    CHECK(playRecoveryScenePath("/p/Game//") == "/p/Game/Library/play-recovery.scene.json");
    CHECK(playRecoveryStatePath("").empty());
    CHECK(playRecoveryScenePath("").empty());
}

TEST_CASE("play_recovery: the two writes create Library/ and its ignore file, once, and keep a hand-made one (RV6)") {
    const TempDir fresh;
    const PlayRecoveryState state{.playing = true, .scene = "", .copyWritten = true};
    CHECK(writePlayRecoveryCopy(fresh.utf8(), "scene bytes\n").empty());
    CHECK(writePlayRecoveryState(fresh.utf8(), state).empty());
    const std::optional<std::string> copy = readBytes(fresh.join("Library/play-recovery.scene.json"));
    REQUIRE(copy.has_value());
    CHECK(*copy == "scene bytes\n");
    const std::optional<std::string> record = readBytes(fresh.join("Library/play-recovery.json"));
    REQUIRE(record.has_value());
    CHECK(*record == writePlayRecoveryStateText(state));
    const std::optional<std::string> ignore =
        readBytes(fresh.join("Library/" + std::string(engine::editor::ASSET_CACHE_GITIGNORE_NAME)));
    REQUIRE(ignore.has_value());
    CHECK(*ignore == engine::editor::LIBRARY_GITIGNORE_TEXT);

    // A pre-existing .gitignore is never overwritten, by either write.
    const TempDir kept;
    std::error_code ec;
    std::filesystem::create_directories(kept.path() / "Library", ec);
    constexpr std::string_view SENTINEL = "# hand-edited, do not touch\n*\n";
    REQUIRE(engine::editor::writeTextFileAtomic(kept.join("Library/.gitignore"), SENTINEL).empty());
    CHECK(writePlayRecoveryCopy(kept.utf8(), "x").empty());
    CHECK(writePlayRecoveryState(kept.utf8(), state).empty());
    const std::optional<std::string> keptIgnore = readBytes(kept.join("Library/.gitignore"));
    REQUIRE(keptIgnore.has_value());
    CHECK(*keptIgnore == SENTINEL);

    // An empty root writes nothing and is not a failure.
    CHECK(writePlayRecoveryCopy("", "x").empty());
    CHECK(writePlayRecoveryState("", state).empty());
}

TEST_CASE("play_recovery: a FILE named Library makes both writes return a reason and write nothing (RV7)") {
    const TempDir dir;
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library"), "not a directory").empty());
    CHECK_FALSE(writePlayRecoveryCopy(dir.utf8(), "x").empty());
    CHECK_FALSE(writePlayRecoveryState(dir.utf8(), PlayRecoveryState{.playing = true}).empty());
    CHECK(countRecoveryEntries(dir.path()) == 0U);
    CHECK_FALSE(engine::editor::fileExists(dir.join("Library/play-recovery.json")));
    CHECK_FALSE(engine::editor::fileExists(dir.join("Library/play-recovery.scene.json")));
}

TEST_CASE("play_recovery: a record or copy path that cannot be replaced makes its write return a reason (RV19)") {
    // Library/ is fine; the record's path and the copy's path are each a non-empty DIRECTORY, so only the atomic
    // write's own rename can fail -- the library is prepared, and the temp file is written before the rename refuses.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.path() / "Library" / "play-recovery.json", ec);
    std::filesystem::create_directories(dir.path() / "Library" / "play-recovery.scene.json", ec);
    REQUIRE(
        engine::editor::writeTextFileAtomic(dir.join("Library/play-recovery.json/keep.txt"), "record side").empty());
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library/play-recovery.scene.json/keep.txt"), "copy side")
                .empty());

    const std::string copyReason = writePlayRecoveryCopy(dir.utf8(), "x");
    const std::string stateReason = writePlayRecoveryState(dir.utf8(), PlayRecoveryState{.playing = true});
    CHECK_FALSE(copyReason.empty());
    CHECK_FALSE(stateReason.empty());

    // Each occupant is untouched and no temp file is left beside it.
    CHECK(readBytes(dir.join("Library/play-recovery.json/keep.txt")) == "record side");
    CHECK(readBytes(dir.join("Library/play-recovery.scene.json/keep.txt")) == "copy side");
    CHECK_FALSE(engine::editor::entryExists(dir.join("Library/play-recovery.json.aero-tmp")));
    CHECK_FALSE(engine::editor::entryExists(dir.join("Library/play-recovery.scene.json.aero-tmp")));
}

TEST_CASE("play_recovery: reading separates no record, a damaged record and a good one (RV8)") {
    const TempDir dir;
    const PlayRecoveryRead none = readPlayRecoveryState(dir.utf8());
    CHECK_FALSE(none.state.has_value());
    CHECK_FALSE(none.corrupt);  // a missing record is the normal state, never a warning

    std::error_code ec;
    std::filesystem::create_directories(dir.path() / "Library" / "play-recovery.json", ec);
    const PlayRecoveryRead directory = readPlayRecoveryState(dir.utf8());
    CHECK_FALSE(directory.state.has_value());
    CHECK(directory.corrupt);  // a directory wearing the name exists and cannot be read
    std::filesystem::remove(dir.path() / "Library" / "play-recovery.json", ec);

    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library/play-recovery.json"), "{ nope").empty());
    const PlayRecoveryRead malformed = readPlayRecoveryState(dir.utf8());
    CHECK_FALSE(malformed.state.has_value());
    CHECK(malformed.corrupt);

    const PlayRecoveryState good{.playing = true, .scene = "scenes/a.scene.json", .copyWritten = true};
    REQUIRE(writePlayRecoveryState(dir.utf8(), good).empty());
    const PlayRecoveryRead valid = readPlayRecoveryState(dir.utf8());
    REQUIRE(valid.state.has_value());
    CHECK((*valid.state == good));
    CHECK_FALSE(valid.corrupt);

    CHECK_FALSE(readPlayRecoveryState("").state.has_value());
    CHECK_FALSE(readPlayRecoveryState("").corrupt);
}

// ---- task 4.7.3 (RV10-RV16): the recovery offer in the project flow ------------------------------------------------
// Every case that needs a copy branches on sceneIoAvailable() at run time (this TU has no #if of any kind) and
// asserts the no-offer arm where it is false: the offer's decision requires scene I/O, the record's answer does not.
namespace {

using engine::editor::applyFileRequests;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::createProject;
using engine::editor::FileAction;
using engine::editor::FileDialogHost;
using engine::editor::FileFlow;
using engine::editor::modalInputActive;
using engine::editor::openProjectPath;
using engine::editor::PlayRecoveryOffer;
using engine::editor::ProjectContext;
using engine::editor::ProjectFlow;
using engine::editor::ProjectSession;
using engine::editor::RecentProjects;
using engine::editor::RootOrder;
using engine::editor::SceneSession;
using engine::editor::sceneToText;
using engine::editor::Selection;

constexpr std::string_view RECORDED_SCENE = "scenes/a.scene.json";
constexpr std::string_view EMPTY_SCENE_TEXT = "{\n  \"version\": 1,\n  \"entities\": []\n}\n";

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// How many records at `level` carry `needle`.
[[nodiscard]] std::size_t countLogged(const std::vector<engine::editor::LogEntry>& records, engine::LogLevel level,
                                      std::string_view needle) {
    return static_cast<std::size_t>(
        std::count_if(records.begin(), records.end(), [level, needle](const engine::editor::LogEntry& e) {
            return e.level == level && e.message.find(needle) != std::string::npos;
        }));
}
[[nodiscard]] std::size_t countAt(const std::vector<engine::editor::LogEntry>& records, engine::LogLevel level) {
    return countLogged(records, level, "");
}

// The recovery copy: a one-entity scene when scene I/O exists, the empty scene's exact bytes otherwise.
[[nodiscard]] std::string recoveryCopyText() {
    engine::World world;
    const engine::Entity e = engine::editor::createEntity(world, {}, "Recovered");
    REQUIRE(e.valid());
    const std::optional<std::string> text = sceneToText(world);
    return text.has_value() ? *text : std::string(EMPTY_SCENE_TEXT);
}

// A project with a loadable recorded scene, the editor-state naming it, a record and a copy: the whole of what
// openProjectPath reads, built over a TempDir.
struct OfferRig {
    TempDir dir;
    std::string root;
    std::string copyText;
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    CommandContext ctx{world, selection, roots};
    FileFlow flow;
    const FileDialogHost host{};
    ProjectSession projectSession;
    ProjectFlow projectFlow;
    RecentProjects recents;
    ProjectContext project{projectSession, projectFlow, recents, ""};
    SceneSession session;

    OfferRig() {
        const engine::editor::ProjectCreateOutcome created =
            createProject(dir.utf8(), "MyGame", "0.1.0", engine::editor::ProjectLanguage::Ts);
        REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
        root = created.root;
        REQUIRE(engine::editor::writeTextFileAtomic(root + "/scenes/a.scene.json", EMPTY_SCENE_TEXT).empty());
        REQUIRE(engine::editor::writeProjectState(root,
                                                  {.lastScene = std::string(RECORDED_SCENE), .lastSceneRecorded = true})
                    .empty());
        copyText = recoveryCopyText();
        writeCopy(copyText);
        writeRecord({.playing = true, .scene = std::string(RECORDED_SCENE), .copyWritten = true});
    }
    void writeCopy(std::string_view text) const { REQUIRE(writePlayRecoveryCopy(root, text).empty()); }
    void writeRecord(const PlayRecoveryState& state) const { REQUIRE(writePlayRecoveryState(root, state).empty()); }
    [[nodiscard]] bool open() { return openProjectPath(ctx, commands, session, project, root); }
    void apply() { applyFileRequests(ctx, commands, session, flow, host, project); }
    [[nodiscard]] std::optional<PlayRecoveryState> record() const { return readPlayRecoveryState(root).state; }
    // The offer a real open raises when scene I/O exists; set by hand where it does not, so the answer arms still run.
    void ensureOffer() {
        REQUIRE(open());
        if (!projectFlow.recoveryOffer.open) {
            REQUIRE_FALSE(engine::editor::sceneIoAvailable());
            projectFlow.recoveryOffer =
                PlayRecoveryOffer{.open = true, .projectRoot = root, .scene = std::string(RECORDED_SCENE)};
        }
    }
    // One command on the history, so "the history is unchanged" is never about an empty stack.
    void makeDirty() {
        const engine::Entity a = world.create();
        REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{a}, std::vector<engine::Entity>{})));
    }
};

[[nodiscard]] std::string textOrEmpty(const engine::World& world) {
    const std::optional<std::string> text = sceneToText(world);
    return text.value_or(std::string{});
}

}  // namespace

TEST_CASE("play_recovery: a project open raises the offer after the restore, and only on the one row (RV10)") {
    const LogFixture fixture;
    const bool io = engine::editor::sceneIoAvailable();
    OfferRig rig;

    REQUIRE(rig.open());
    // The restore ran first: the session is on the recorded scene.
    CHECK((rig.session.path() == engine::editor::absoluteScenePath(rig.root, RECORDED_SCENE)) == io);
    CHECK(rig.projectFlow.recoveryOffer.open == io);
    if (io) {
        CHECK(rig.projectFlow.recoveryOffer.projectRoot == rig.root);
        CHECK(rig.projectFlow.recoveryOffer.scene == RECORDED_SCENE);
        CHECK_FALSE(rig.projectFlow.recoveryOffer.recoverRequested);
        CHECK_FALSE(rig.projectFlow.recoveryOffer.discardRequested);
    } else {
        CHECK(rig.projectFlow.recoveryOffer.projectRoot.empty());
    }

    // Each single-fact flip opens with no offer, and a reopen clears the previous one.
    std::error_code ec;
    std::filesystem::remove(playRecoveryScenePath(rig.root), ec);  // no copy file
    REQUIRE(rig.open());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    CHECK(rig.projectFlow.recoveryOffer.projectRoot.empty());
    rig.writeCopy(rig.copyText);
    rig.writeRecord({.playing = false, .scene = std::string(RECORDED_SCENE), .copyWritten = true});
    REQUIRE(rig.open());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    rig.writeRecord({.playing = true, .scene = std::string(RECORDED_SCENE), .copyWritten = false});
    REQUIRE(rig.open());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    // The full row again raises it (the flips were not a broken fixture), and a record-less reopen clears it.
    rig.writeRecord({.playing = true, .scene = std::string(RECORDED_SCENE), .copyWritten = true});
    REQUIRE(rig.open());
    CHECK(rig.projectFlow.recoveryOffer.open == io);
    std::filesystem::remove(playRecoveryStatePath(rig.root), ec);
    REQUIRE(rig.open());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    CHECK(rig.projectFlow.recoveryOffer.projectRoot.empty());

    // createAndOpenProject never offers: a new project has no record.
    const TempDir other;
    REQUIRE(engine::editor::createAndOpenProject(rig.ctx, rig.commands, rig.session, rig.project, other.utf8(), "Fresh",
                                                 engine::editor::ProjectLanguage::Ts));
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
}

TEST_CASE("play_recovery: an unparseable record opens the project with one WARN and no offer (RV11)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    OfferRig rig;
    rig.writeRecord({.playing = true, .scene = std::string(RECORDED_SCENE), .copyWritten = true});
    REQUIRE(engine::editor::writeTextFileAtomic(playRecoveryStatePath(rig.root), "{\"version\": 1}").empty());
    scope.sink()->take(records);
    records.clear();

    CHECK(rig.open());
    scope.sink()->take(records);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    CHECK(countAt(records, engine::LogLevel::Warn) == 1U);
    CHECK(countLogged(records, engine::LogLevel::Warn, "play-recovery.json") == 1U);
    CHECK(countAt(records, engine::LogLevel::Error) == 0U);
}

TEST_CASE("play_recovery: the open offer owns input, and a request behind it writes nothing (RV12)") {
    const TempDir dir;
    CHECK_FALSE(modalInputActive(FileFlow{}, ProjectFlow{}));
    ProjectFlow open;
    open.recoveryOffer.open = true;
    CHECK(modalInputActive(FileFlow{}, open));

    OfferRig rig;
    REQUIRE(rig.open());
    rig.projectFlow.recoveryOffer.open = true;  // by hand: the refusal is the same with or without scene I/O
    const std::string target = dir.join("behind.scene.json");
    rig.flow.requested = FileAction::SaveSceneAs;
    rig.flow.requestedPath = target;
    rig.apply();
    CHECK_FALSE(engine::editor::fileExists(target));
    CHECK(rig.flow.requested == FileAction::None);
    CHECK(rig.flow.requestedPath.empty());
    CHECK(rig.projectFlow.recoveryOffer.open);  // nothing answered it
}

TEST_CASE("play_recovery: Recover swaps in the copy, binds the path and leaves the document dirty (RV13)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    const bool io = engine::editor::sceneIoAvailable();
    OfferRig rig;
    rig.ensureOffer();
    if (!io) {
        // Without scene I/O the swap cannot happen: the World stays and the record is still answered.
        const std::size_t entitiesBefore = rig.world.entityCount();
        rig.projectFlow.recoveryOffer.recoverRequested = true;
        rig.apply();
        CHECK(rig.world.entityCount() == entitiesBefore);
        const std::optional<PlayRecoveryState> answered = rig.record();
        REQUIRE(answered.has_value());
        CHECK_FALSE(answered->playing);
        return;
    }
    scope.sink()->take(records);
    records.clear();
    rig.projectFlow.recoveryOffer.recoverRequested = true;
    rig.apply();
    scope.sink()->take(records);

    CHECK(textOrEmpty(rig.world) == rig.copyText);
    CHECK(rig.world.entityCount() == 1U);
    CHECK(rig.session.path() == engine::editor::absoluteScenePath(rig.root, RECORDED_SCENE));
    CHECK(rig.commands.count() == 0U);
    CHECK_FALSE(rig.commands.isClean());
    const std::optional<PlayRecoveryState> after = rig.record();
    REQUIRE(after.has_value());
    CHECK((*after == PlayRecoveryState{.playing = false, .scene = std::string(RECORDED_SCENE), .copyWritten = true}));
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    CHECK(rig.projectFlow.recoveryOffer.projectRoot.empty());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.recoverRequested);
    CHECK(countLogged(records, engine::LogLevel::Info, "recovered the scene") == 1U);
    CHECK(countAt(records, engine::LogLevel::Error) == 0U);

    // The untitled arm: a record with no scene clears the path instead of binding one.
    OfferRig untitled;
    untitled.writeRecord({.playing = true, .scene = "", .copyWritten = true});
    untitled.ensureOffer();
    CHECK(untitled.projectFlow.recoveryOffer.scene.empty());
    untitled.projectFlow.recoveryOffer.recoverRequested = true;
    untitled.apply();
    CHECK(untitled.session.path().empty());
    CHECK(untitled.world.entityCount() == 1U);
    CHECK_FALSE(untitled.commands.isClean());
}

TEST_CASE("play_recovery: Discard keeps the document and the copy and only finishes the record (RV14)") {
    OfferRig rig;
    rig.ensureOffer();
    rig.makeDirty();
    const std::string worldBefore = textOrEmpty(rig.world);
    const std::string pathBefore(rig.session.path());
    const std::size_t entitiesBefore = rig.world.entityCount();
    const std::size_t countBefore = rig.commands.count();
    const bool cleanBefore = rig.commands.isClean();
    const std::optional<std::string> copyBefore = engine::editor::readTextFile(playRecoveryScenePath(rig.root)).text;

    rig.projectFlow.recoveryOffer.discardRequested = true;
    rig.apply();

    CHECK(textOrEmpty(rig.world) == worldBefore);
    CHECK(rig.world.entityCount() == entitiesBefore);
    CHECK(rig.session.path() == pathBefore);
    CHECK(rig.commands.count() == countBefore);
    CHECK(rig.commands.isClean() == cleanBefore);
    const std::optional<std::string> copyAfter = engine::editor::readTextFile(playRecoveryScenePath(rig.root)).text;
    REQUIRE(copyAfter.has_value());
    CHECK(copyAfter == copyBefore);
    const std::optional<PlayRecoveryState> after = rig.record();
    REQUIRE(after.has_value());
    CHECK_FALSE(after->playing);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);

    // Answered once: opening the same project again offers nothing.
    REQUIRE(rig.open());
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
}

TEST_CASE("play_recovery: a Recover on a damaged copy logs one ERROR, swaps nothing and answers the record (RV15)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    OfferRig rig;
    rig.writeCopy(R"({"version": 1, "entities": [)");
    rig.ensureOffer();
    rig.makeDirty();
    const std::string worldBefore = textOrEmpty(rig.world);
    const std::size_t entitiesBefore = rig.world.entityCount();
    const std::string pathBefore(rig.session.path());
    const std::size_t countBefore = rig.commands.count();
    scope.sink()->take(records);
    records.clear();

    rig.projectFlow.recoveryOffer.recoverRequested = true;
    rig.apply();
    scope.sink()->take(records);

    CHECK(countAt(records, engine::LogLevel::Error) == 1U);
    CHECK(countLogged(records, engine::LogLevel::Error, "play-recovery.scene.json") == 1U);
    CHECK(textOrEmpty(rig.world) == worldBefore);
    CHECK(rig.world.entityCount() == entitiesBefore);
    CHECK(rig.session.path() == pathBefore);
    CHECK(rig.commands.count() == countBefore);
    const std::optional<PlayRecoveryState> after = rig.record();
    REQUIRE(after.has_value());
    CHECK_FALSE(after->playing);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
}

TEST_CASE("play_recovery: a Recover that fails on a clean document logs no recovery and leaves it clean (RV20)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    OfferRig rig;
    rig.writeCopy(R"({"version": 1, "entities": [)");
    rig.ensureOffer();
    REQUIRE(rig.commands.isClean());  // NOT dirtied: a failure that falls through into forgetClean would show here
    const std::string worldBefore = textOrEmpty(rig.world);
    const std::string pathBefore(rig.session.path());
    const std::size_t countBefore = rig.commands.count();
    scope.sink()->take(records);
    records.clear();

    rig.projectFlow.recoveryOffer.recoverRequested = true;
    rig.apply();
    scope.sink()->take(records);

    CHECK(countLogged(records, engine::LogLevel::Error, "play-recovery.scene.json") == 1U);
    CHECK(countLogged(records, engine::LogLevel::Info, "recovered the scene") == 0U);
    CHECK(rig.commands.isClean());
    CHECK(textOrEmpty(rig.world) == worldBefore);
    CHECK(rig.session.path() == pathBefore);
    CHECK(rig.commands.count() == countBefore);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
}

TEST_CASE("play_recovery: Discard wins a frame with both answers, and another project's answer is dropped (RV16)") {
    OfferRig rig;
    rig.ensureOffer();
    const std::string worldBefore = textOrEmpty(rig.world);
    const std::size_t entitiesBefore = rig.world.entityCount();

    rig.projectFlow.recoveryOffer.recoverRequested = true;
    rig.projectFlow.recoveryOffer.discardRequested = true;
    rig.apply();
    CHECK(textOrEmpty(rig.world) == worldBefore);
    CHECK(rig.world.entityCount() == entitiesBefore);
    const std::optional<PlayRecoveryState> answered = rig.record();
    REQUIRE(answered.has_value());
    CHECK_FALSE(answered->playing);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);

    // An offer raised for a different directory, answered Recover: neither World nor either record moves.
    const TempDir other;
    rig.writeRecord({.playing = true, .scene = std::string(RECORDED_SCENE), .copyWritten = true});
    rig.projectFlow.recoveryOffer = PlayRecoveryOffer{
        .open = true, .projectRoot = other.utf8(), .scene = std::string(RECORDED_SCENE), .recoverRequested = true};
    rig.apply();
    CHECK(textOrEmpty(rig.world) == worldBefore);
    CHECK(rig.world.entityCount() == entitiesBefore);
    const std::optional<PlayRecoveryState> kept = rig.record();
    REQUIRE(kept.has_value());
    CHECK(kept->playing);  // this project's record was not touched
    CHECK_FALSE(engine::editor::fileExists(playRecoveryStatePath(other.utf8())));
    CHECK_FALSE(rig.projectFlow.recoveryOffer.open);
    CHECK_FALSE(rig.projectFlow.recoveryOffer.recoverRequested);
}

// ---- task 4.7.3 (RV17, RV18): the recovery dialog's pure model ------------------------------------------------------

TEST_CASE("play_recovery: the dialog layout is the Welcome's width, clamped, and every length scales (RV17)") {
    using engine::editor::playRecoveryDialogLayout;
    using engine::editor::PlayRecoveryDialogLayout;
    using engine::editor::playRecoveryFooterLayout;
    using engine::editor::PlayRecoveryFooterLayout;
    const engine::editor::ThemeDialogs& d = engine::editor::EDITOR_THEME.dialogs;

    const PlayRecoveryDialogLayout wide = playRecoveryDialogLayout(1600.0F, 1.0F);
    CHECK(wide.width == 560.0F);
    CHECK(wide.width == std::floor(d.welcomeWidth));
    CHECK(wide.paddingX == d.paddingX);
    CHECK(wide.contentWidth == wide.width - (2.0F * wide.paddingX));
    CHECK(wide.buttonPadding.x == d.buttonPaddingX);
    CHECK(wide.buttonPadding.y > 0.0F);
    CHECK(wide.buttonPadding.y < wide.buttonHeight * 0.5F);

    // A narrow work area: the work width less the margin on each side, never below the floor.
    const PlayRecoveryDialogLayout narrow = playRecoveryDialogLayout(300.0F, 1.0F);
    CHECK(narrow.width == std::floor(std::max(d.minWidth, 300.0F - (2.0F * d.screenMargin))));
    CHECK(narrow.width < wide.width);
    const PlayRecoveryDialogLayout floored = playRecoveryDialogLayout(100.0F, 1.0F);
    CHECK(floored.width == std::floor(d.minWidth));

    // Scale 2 doubles every length (the theme's dp are whole numbers, and 2 is exact in binary).
    const PlayRecoveryDialogLayout twice = playRecoveryDialogLayout(1600.0F, 2.0F);
    CHECK(twice.width == 2.0F * wide.width);
    CHECK(twice.paddingX == 2.0F * wide.paddingX);
    CHECK(twice.contentWidth == 2.0F * wide.contentWidth);
    CHECK(twice.headerHeight == 2.0F * wide.headerHeight);
    CHECK(twice.footerHeight == 2.0F * wide.footerHeight);
    CHECK(twice.windowRounding == 2.0F * wide.windowRounding);
    CHECK(twice.controlRounding == 2.0F * wide.controlRounding);
    CHECK(twice.buttonHeight == 2.0F * wide.buttonHeight);
    CHECK(twice.buttonPadding.x == 2.0F * wide.buttonPadding.x);
    CHECK(twice.bodyTop == 2.0F * wide.bodyTop);
    CHECK(twice.lineGap == 2.0F * wide.lineGap);
    CHECK(twice.footerButtonGap == 2.0F * wide.footerButtonGap);
    CHECK_FALSE(wide == twice);
    // A scale that is not a positive finite number lays out as 1.
    CHECK(playRecoveryDialogLayout(1600.0F, 0.0F) == wide);
    CHECK(playRecoveryDialogLayout(1600.0F, -3.0F) == wide);

    // The footer: Recover's right edge on the padding, Discard a gap to its left, both centred in the band.
    constexpr float DISCARD_WIDTH = 80.0F;
    constexpr float RECOVER_WIDTH = 100.0F;
    const PlayRecoveryFooterLayout footer = playRecoveryFooterLayout(wide, DISCARD_WIDTH, RECOVER_WIDTH);
    CHECK(footer.recoverX + RECOVER_WIDTH == wide.width - wide.paddingX);
    CHECK(footer.discardX + DISCARD_WIDTH == footer.recoverX - wide.footerButtonGap);
    CHECK(footer.buttonY == (wide.footerHeight - wide.buttonHeight) / 2.0F);
    CHECK(footer.discardX > 0.0F);
}

TEST_CASE("play_recovery: the scene label is the leaf, or Untitled (RV18)") {
    using engine::editor::playRecoverySceneLabel;
    CHECK(playRecoverySceneLabel("") == "Untitled");
    CHECK(playRecoverySceneLabel("scenes/a.scene.json") == "a.scene.json");
    CHECK(playRecoverySceneLabel("a.scene.json") == "a.scene.json");
    CHECK(playRecoverySceneLabel("x/y/100%s.scene.json") == "100%s.scene.json");
    CHECK(playRecoverySceneLabel("scenes/") == "Untitled");
}
