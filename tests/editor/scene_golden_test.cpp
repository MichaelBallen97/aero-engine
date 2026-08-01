// tests/editor/scene_golden_test.cpp -- task 2.5.2: the three committed golden scenes, driven
// through the EDITOR's real disk path (openSceneFile -> saveSceneFile -> readTextFile) rather than
// through the engine's in-memory one. CONDITIONAL: appended to aero_editor_shell_test only inside
// if(AERO_REFLECT_TOOLS) (tests/CMakeLists.txt) -- with the tool off the generated component
// serializers do not exist, so this whole TU is ABSENT from that build, not skipped. SEVENTEENTH TU;
// do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
//
// WHY THIS EXISTS ALONGSIDE THE ENGINE BATTERY. The engine-side golden cases (G1-G10, a sibling
// tests/ target) already compare the same three files byte-for-byte, but they never touch a file on
// the WRITE side. This battery is the ONLY test in the tree that can catch the removal of
// std::ios::binary from editor/src/text_file.cpp:57 and :77 -- and it can only catch it ON THE
// WINDOWS LANE, where text mode would turn every newline into a carriage-return pair on the way out.
// On macOS and Linux EG3's hygiene assertion is a no-op that can never fail. That is not a weakness;
// it is the whole reason the assertion is written down.
//
// This TU names no type from the engine's serialization bridge, deliberately: every symbol it calls
// lives in aero_editor_core, which is what keeps 2.5.1's confinement grep honest.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::openSceneFile;
using engine::editor::readTextFile;
using engine::editor::RootOrder;
using engine::editor::saveSceneFile;
using engine::editor::SceneSession;
using engine::editor::Selection;

namespace {

constexpr std::string_view GOLDEN_EMPTY = AERO_GOLDEN_SCENES_DIR "/empty.scene.json";
constexpr std::string_view GOLDEN_FULL = AERO_GOLDEN_SCENES_DIR "/full.scene.json";
constexpr std::string_view GOLDEN_EDGE = AERO_GOLDEN_SCENES_DIR "/edge.scene.json";

// name, path, entity count -- one row per fixture, so every loop below is exhaustive by construction.
struct GoldenFixture {
    std::string_view name;
    std::string_view path;
    std::size_t entities;
};
constexpr std::array<GoldenFixture, 3> GOLDEN_FIXTURES{{
    {"empty", GOLDEN_EMPTY, 0},
    {"full", GOLDEN_FULL, 8},
    {"edge", GOLDEN_EDGE, 4},
}};

// Count by LEVEL, never records.size() (the command_stack_test.cpp precedent).
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

// A unique temp directory that removes itself on destruction -- the FIFTH TU-local copy of this
// shape (tests/vfs_test.cpp, tests/editor/project_files_test.cpp, tests/editor/scene_session_test.cpp,
// tests/editor/scene_io_test.cpp). A file-scope helper in another TU is TU-scoped and cannot be
// borrowed; scene_golden_support.hpp is the one deliberate exception to that convention, and it is
// the ASSERTION, not the scaffolding.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;
        dirPath = base / ("aero_scene_golden_test_" + std::to_string(++counter));
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
    // Creates <dir>/<utf8Leaf> and returns its UTF-8 path. The u8string construction is utf8()'s rule
    // in reverse and it is NOT optional on Windows, where path's native encoding is UTF-16 and the
    // narrow-char constructor assumes the ACTIVE CODE PAGE -- a non-ASCII leaf built from a plain
    // std::string would name a different directory. EG7 is the case that needs it.
    [[nodiscard]] std::string subdir(std::string_view utf8Leaf) const {
        const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8Leaf.data()), utf8Leaf.size());
        std::error_code ec;
        std::filesystem::create_directories(dirPath / std::filesystem::path(bytes), ec);
        return join(utf8Leaf);
    }

private:
    std::filesystem::path dirPath;
};

// Everything an open/save pair needs, in one place -- five objects that must travel together.
struct EditorFixture {
    engine::World world;
    Selection selection;
    RootOrder roots;
    CommandStack commands;
    SceneSession session;
    CommandContext context{world, selection, roots};
};

}  // namespace

TEST_CASE("scene_golden/editor: the fixture directory resolves and all three files exist (EG1/D8)") {
    // REQUIRE, never a skip. A golden that can silently skip itself is the same failure mode as a
    // regeneration flag: green CI, zero coverage.
    for (const GoldenFixture& fixture : GOLDEN_FIXTURES) {
        INFO(std::string{fixture.name});
        const scene_golden::FileBytes file = scene_golden::readBytes(fixture.path);
        REQUIRE_MESSAGE(file.ok, file.error);
        CHECK_FALSE(file.text.empty());
        const std::string hygiene = scene_golden::hygieneComplaint(file.text);
        CHECK_MESSAGE(hygiene.empty(), hygiene);
    }
}

TEST_CASE("scene_golden/editor: openSceneFile loads each golden cleanly (EG2/AC-15)") {
    for (const GoldenFixture& fixture : GOLDEN_FIXTURES) {
        INFO(std::string{fixture.name});
        EditorFixture ed;

        // Made DIRTY first, never left at its already-clean default -- the scene_io_test.cpp:383-391
        // idiom, and for the same reason: a stack that STARTS clean stays clean whether or not
        // openSceneFile clears it, so the two history assertions below would pass with the clearing
        // removed entirely. Starting dirty is what makes them discriminate. AC-15's "history is
        // clean" clause is only worth asserting in this form.
        const engine::Entity probe = ed.world.create();
        const std::vector<engine::Entity> targets{probe};
        const std::vector<engine::Entity> noSelection;
        auto dirty = std::make_unique<engine::editor::DeleteEntitiesCommand>(targets, noSelection);
        REQUIRE(ed.commands.push(ed.context, std::move(dirty)));
        REQUIRE_FALSE(ed.commands.isClean());

        REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, fixture.path));
        CHECK(ed.world.entityCount() == fixture.entities);
        CHECK(ed.commands.isClean());  // a fresh load is a saved document by definition
        CHECK(ed.commands.count() == 0);
        CHECK(ed.session.path() == fixture.path);
    }
}

TEST_CASE("scene_golden/editor: a save writes the fixture's exact bytes back (EG3/AC-16/AC-17)") {
    // THE byte comparison of this battery. appendExtension is FALSE and the target already ends in
    // .scene.json (spec D11): with true, 2.5.1's refusal rule fires on the second write of the same
    // target and this case would silently be asserting the overwrite guard instead of byte
    // stability. IO13 already owns that rule.
    const TempDir dir;
    for (const GoldenFixture& fixture : GOLDEN_FIXTURES) {
        INFO(std::string{fixture.name});
        const scene_golden::FileBytes source = scene_golden::readBytes(fixture.path);
        REQUIRE_MESSAGE(source.ok, source.error);

        EditorFixture ed;
        REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, fixture.path));

        const std::string target = dir.join(std::string{fixture.name} + ".scene.json");
        REQUIRE(saveSceneFile(ed.context, ed.commands, ed.session, target, /*appendExtension=*/false));
        CHECK(ed.session.path() == target);
        // NO isClean() assertion here, deliberately. The open above already left the stack clean and
        // nothing between them dirties it, so the check would pass with saveSceneFile's setClean()
        // deleted -- vacuous, which is the one thing this task exists to forbid. It cannot be made
        // discriminating either: dirtying the stack means mutating the World, and this case's whole
        // point is that the saved bytes still equal the fixture's. "A save marks the document clean"
        // is owned by scene_io_test.cpp's IO cases and imgui_layer_test.cpp's I14.

        const engine::editor::FileReadResult written = readTextFile(target);
        REQUIRE_MESSAGE(written.text.has_value(), written.error);
        INFO(scene_golden::describeMismatch(source.text, *written.text));
        if (*written.text != source.text) {
            scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, fixture.name, *written.text);
        }
        CHECK(*written.text == source.text);

        // THE WINDOWS CANARY. On macOS and Linux this assertion is a NO-OP -- it cannot fail, because
        // nothing on those platforms rewrites a newline. Its ONLY possible failure is a Windows
        // text-mode regression: dropping std::ios::binary from editor/src/text_file.cpp:57 or :77
        // makes every newline in the written file a carriage-return pair, and this line is the only
        // thing anywhere in the tree that would notice. Do not delete it as untested; it is untestable
        // HERE and load-bearing THERE.
        const std::string hygiene = scene_golden::hygieneComplaint(*written.text);
        CHECK_MESSAGE(hygiene.empty(), hygiene);
    }
}

TEST_CASE("scene_golden/editor: a written file re-opens and re-saves identically (EG4/AC-18)") {
    // Disk-level idempotence: the engine battery proves this in memory, this proves it across two
    // real files and two real atomic renames.
    const TempDir dir;
    for (const GoldenFixture& fixture : GOLDEN_FIXTURES) {
        INFO(std::string{fixture.name});
        const scene_golden::FileBytes source = scene_golden::readBytes(fixture.path);
        REQUIRE_MESSAGE(source.ok, source.error);

        const std::string first = dir.join(std::string{fixture.name} + "-1.scene.json");
        const std::string second = dir.join(std::string{fixture.name} + "-2.scene.json");
        {
            EditorFixture ed;
            REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, fixture.path));
            REQUIRE(saveSceneFile(ed.context, ed.commands, ed.session, first, /*appendExtension=*/false));
        }
        {
            EditorFixture ed;
            REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, first));
            REQUIRE(saveSceneFile(ed.context, ed.commands, ed.session, second, /*appendExtension=*/false));
        }
        const engine::editor::FileReadResult a = readTextFile(first);
        const engine::editor::FileReadResult b = readTextFile(second);
        REQUIRE_MESSAGE(a.text.has_value(), a.error);
        REQUIRE_MESSAGE(b.text.has_value(), b.error);
        INFO(scene_golden::describeMismatch(*a.text, *b.text));
        CHECK(*a.text == source.text);
        CHECK(*b.text == *a.text);
    }
}

TEST_CASE("scene_golden/editor: a New Scene between two opens leaves no residue (EG5/AC-18)") {
    // INV-6's byte-level corollary. resetSceneState clears the World, the Selection, the RootOrder
    // and the CommandStack together; if it left anything behind, the SECOND open would append to a
    // non-empty World and the saved bytes would differ. This is the cheapest possible detector for a
    // whole class of scene-swap bug, and it is why the case exists as bytes rather than as a count.
    const TempDir dir;
    const scene_golden::FileBytes source = scene_golden::readBytes(GOLDEN_FULL);
    REQUIRE_MESSAGE(source.ok, source.error);

    EditorFixture ed;
    REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, GOLDEN_FULL));
    REQUIRE(ed.world.entityCount() == 8);

    engine::editor::newScene(ed.context, ed.commands);
    CHECK(ed.world.entityCount() == 3);  // the three seed entities, and nothing from `full`

    REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, GOLDEN_FULL));
    CHECK(ed.world.entityCount() == 8);  // 8, never 11

    const std::string target = dir.join("after-new.scene.json");
    REQUIRE(saveSceneFile(ed.context, ed.commands, ed.session, target, /*appendExtension=*/false));
    const engine::editor::FileReadResult written = readTextFile(target);
    REQUIRE_MESSAGE(written.text.has_value(), written.error);
    INFO(scene_golden::describeMismatch(source.text, *written.text));
    if (*written.text != source.text) {
        scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, "after-new", *written.text);
    }
    CHECK(*written.text == source.text);
}

TEST_CASE("scene_golden/editor: opening a golden logs exactly one INFO and no WARN (EG6/AC-19/D12)") {
    // A WARN here means the fixture named a component this build cannot resolve -- i.e. the fixture
    // silently degraded. It is a richness pin wearing a different hat, and it reuses IO15's shape.
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;

    for (const GoldenFixture& golden : GOLDEN_FIXTURES) {
        INFO(std::string{golden.name});
        EditorFixture ed;

        // take() ASSERTS `out` is empty and then SWAPS (console_model.cpp:231-234), so the drain
        // below leaves its haul IN `records` and the drain after the open must start from empty.
        // Hence take-then-clear, the scene_io_test.cpp:393-394/424-425/474-475/512-513 idiom -- NOT
        // clear-then-take, which leaves the pre-open residue in place: in Debug that trips the
        // assert and aborts all 304 cases, and in Release (NDEBUG compiles the assert out) it is
        // silently COUNTED below, failing for a reason unrelated to openSceneFile. `records` is
        // loop-scoped so iteration N+1 cannot inherit iteration N's haul either.
        std::vector<engine::editor::LogEntry> records;
        scope.sink()->take(records);
        records.clear();

        REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, golden.path));

        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
        CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }
}

TEST_CASE("scene_golden/editor: a non-ASCII target directory round-trips byte-identically (EG7/AC-20)") {
    // editor/src/text_file.cpp:34-37's pathFromUtf8 constructs from UTF-8 BYTES rather than from a
    // narrow std::string, which is what makes this work on Windows. Constructed from `char`, the
    // directory below resolves to mojibake or not at all, and the save fails or lands somewhere else.
    // Like EG3's hygiene check this is a Windows-lane assertion: on macOS and Linux both spellings
    // happen to work, so it can only ever fail there.
    const TempDir dir;
    // "caf\xC3\xA9-\xF0\x9F\x9A\x80" -- cafe-acute then a 4-byte rocket. Escaped rather than written
    // as glyphs because the tree sets no /utf-8 flag and MSVC reads this file in the active code page.
    const std::string sub = dir.subdir("caf\xC3\xA9-\xF0\x9F\x9A\x80");

    const scene_golden::FileBytes source = scene_golden::readBytes(GOLDEN_EDGE);
    REQUIRE_MESSAGE(source.ok, source.error);

    EditorFixture ed;
    REQUIRE(openSceneFile(ed.context, ed.commands, ed.session, GOLDEN_EDGE));
    const std::string target = sub + "/edge.scene.json";
    REQUIRE(saveSceneFile(ed.context, ed.commands, ed.session, target, /*appendExtension=*/false));

    const engine::editor::FileReadResult written = readTextFile(target);
    REQUIRE_MESSAGE(written.text.has_value(), written.error);
    INFO(scene_golden::describeMismatch(source.text, *written.text));
    CHECK(*written.text == source.text);
    CHECK(scene_golden::hygieneComplaint(*written.text).empty());
}

TEST_CASE("scene_golden/editor: the failure diagnostic itself works (EG8/D5)") {
    // A diagnostic nobody ever exercises is a diagnostic that segfaults the first time it fires --
    // and the first time it fires is on a remote lane, at the worst possible moment.
    CHECK(scene_golden::describeMismatch("abc", "abc").empty());

    const std::string report = scene_golden::describeMismatch("hello world", "hello worlx");
    CHECK_FALSE(report.empty());
    CHECK(report.find("offset 10") != std::string::npos);
    CHECK(report.find("expected:") != std::string::npos);
    CHECK(report.find("actual") != std::string::npos);

    // A C0 byte is RENDERED, never emitted raw. Note the adjacent-literal split: "a\x01b" would be
    // ONE byte (0x1b) because 'b' is a hex digit -- the splice trap, and no compiler warns.
    const std::string control = scene_golden::describeMismatch(
        "a\x01"
        "b",
        "azb");
    CHECK(control.find("\\x01") != std::string::npos);
    // And so is a UTF-8 lead byte, so a context window that cuts a sequence in half stays ASCII-safe.
    const std::string utf8 = scene_golden::describeMismatch("caf\xC3\xA9", "cafX");
    CHECK(utf8.find("\\xc3") != std::string::npos);

    // Truncation, both directions -- the loop must not run past the shorter string.
    CHECK_FALSE(scene_golden::describeMismatch("abc", "ab").empty());
    CHECK_FALSE(scene_golden::describeMismatch("ab", "abc").empty());

    // hygieneComplaint's own arms. Each message must NAME its property, because a bare "hygiene
    // failed" on a remote lane is no better than the byte diff it replaced.
    CHECK(scene_golden::hygieneComplaint("{}\n").empty());
    CHECK(scene_golden::hygieneComplaint("").find("empty") != std::string::npos);
    CHECK(scene_golden::hygieneComplaint("\xEF\xBB\xBF{}\n").find("BOM") != std::string::npos);
    CHECK(scene_golden::hygieneComplaint("{}\r\n").find("carriage return") != std::string::npos);
    CHECK(scene_golden::hygieneComplaint("{\t}\n").find("raw tab") != std::string::npos);
    CHECK(scene_golden::hygieneComplaint("{}").find("no trailing newline") != std::string::npos);
    CHECK(scene_golden::hygieneComplaint("{}\n\n").find("more than one") != std::string::npos);
}
