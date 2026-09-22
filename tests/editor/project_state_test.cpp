// Aero Engine — <projectRoot>/Library/editor-state.json's codec, its path arithmetic, its two pure
// deciders and its two file operations (task E.4.1, PJ1-PJ40). Tier 0: aero_editor_shell_test, no
// ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING in all three
// configurations. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
#include <aero/editor/asset_cache.hpp>  // LIBRARY_GITIGNORE_TEXT / ASSET_CACHE_GITIGNORE_NAME -- PJ35/PJ36
                                        // read the SHIPPED constants, never a restated copy
#include <aero/editor/project_state.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>

using engine::editor::absoluteScenePath;
using engine::editor::parseProjectState;
using engine::editor::projectRelativeScenePath;
using engine::editor::ProjectState;
using engine::editor::projectStatePath;
using engine::editor::readProjectState;
using engine::editor::writeProjectState;
using engine::editor::writeProjectStateText;

namespace {

// The TENTH copy of this shape (editor_prefs_test.cpp:27-56 is the ninth, and states the rule:
// scaffolding is copied, the assertion is shared). A unique temp directory that removes itself.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_project_state_test_" + std::to_string(++counter));
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

// Reads a file's exact bytes. Returns nullopt when it does not exist or cannot be read.
[[nodiscard]] std::optional<std::string> readBytes(std::string_view path) {
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    return read.text;
}

}  // namespace

// ---- the format: PJ1-PJ10 ------------------------------------------------------------------------

TEST_CASE("project_state: parse(write(x)) == x for all three record states (PJ1)") {
    // camelBack, not SCREAMING_SNAKE: readability-identifier-naming reserves that for CONSTEXPR, and
    // ProjectState holds a std::string, so this table cannot be one.
    const std::array<ProjectState, 3> states = {
        ProjectState{},                                                                    // never recorded
        ProjectState{.lastScene = "", .lastSceneRecorded = true},                          // D2's third state
        ProjectState{.lastScene = "scenes/level1.scene.json", .lastSceneRecorded = true},  // recorded
    };
    for (const ProjectState& state : states) {
        CAPTURE(state.lastScene);
        CAPTURE(state.lastSceneRecorded);
        const std::optional<ProjectState> back = parseProjectState(writeProjectStateText(state));
        REQUIRE(back.has_value());
        CHECK(back->lastScene == state.lastScene);
        CHECK(back->lastSceneRecorded == state.lastSceneRecorded);
    }
}

TEST_CASE("project_state: writeProjectStateText is deterministic and canonical, byte for byte (PJ2)") {
    const ProjectState state{.lastScene = "scenes/level1.scene.json", .lastSceneRecorded = true};
    const std::string text = writeProjectStateText(state);
    CHECK(writeProjectStateText(state) == text);  // DETERMINISTIC

    // THE CANONICAL FORM ITSELF, as exact bytes. docs/09 section 4.10 says "JsonWriter's DEFAULT
    // configuration (pretty, 2-space), version first, exactly one trailing newline", and EP2's own
    // sabotage round proved that determinism, key order, the trailing newline and the round trip ALL
    // survive a different config -- this is the one arm that can tell the shipped form from a
    // restated one. The output is fully determined (json_writer.cpp:95-137), so it is spelled whole
    // rather than as three `find`s.
    CHECK(text == "{\n  \"version\": 1,\n  \"lastScene\": \"scenes/level1.scene.json\"\n}\n");

    REQUIRE_FALSE(text.empty());
    CHECK(text.back() == '\n');
    CHECK(text[text.size() - 2U] != '\n');  // exactly ONE
    const std::size_t versionAt = text.find("\"version\"");
    const std::size_t sceneAt = text.find("\"lastScene\"");
    REQUIRE(versionAt != std::string::npos);
    REQUIRE(sceneAt != std::string::npos);
    CHECK(versionAt < sceneAt);  // version FIRST, validated first
}

TEST_CASE("project_state: an unrecorded state OMITS the key entirely (PJ3)") {
    const std::string text = writeProjectStateText(ProjectState{});
    CHECK(text == "{\n  \"version\": 1\n}\n");
    CHECK(text.find("lastScene") == std::string::npos);  // not the key, not the token, nothing
    // ...and the omission survives the round trip as the ABSENT answer, not as "".
    const std::optional<ProjectState> back = parseProjectState(text);
    REQUIRE(back.has_value());
    CHECK_FALSE(back->lastSceneRecorded);
}

TEST_CASE("project_state: a bad envelope is nullopt, five ways (PJ4)") {
    CHECK_FALSE(parseProjectState("").has_value());                     // unparseable
    CHECK_FALSE(parseProjectState("{").has_value());                    // unparseable
    CHECK_FALSE(parseProjectState("[1,2,3]").has_value());              // non-object root
    CHECK_FALSE(parseProjectState("\"hello\"").has_value());            // non-object root
    CHECK_FALSE(parseProjectState("{}").has_value());                   // missing version
    CHECK_FALSE(parseProjectState(R"({"version": 2})").has_value());    // wrong version
    CHECK_FALSE(parseProjectState(R"({"version": "1"})").has_value());  // non-integral version
    CHECK_FALSE(parseProjectState(R"({"version": 1.5})").has_value());  // non-integral version
}

TEST_CASE("project_state: an ABSENT lastScene parses and is NOT recorded (PJ5)") {
    const std::optional<ProjectState> state = parseProjectState(R"({"version": 1})");
    REQUIRE(state.has_value());  // ABSENT IS NOT A FAILURE -- this is what makes H4 one key
    CHECK(state->lastScene.empty());
    CHECK_FALSE(state->lastSceneRecorded);
}

TEST_CASE("project_state: a PRESENT empty lastScene parses AS RECORDED (PJ6)") {
    const std::optional<ProjectState> state = parseProjectState(R"({"version": 1, "lastScene": ""})");
    REQUIRE(state.has_value());
    CHECK(state->lastScene.empty());
    CHECK(state->lastSceneRecorded);  // THE DISCRIMINATOR D2 EXISTS FOR -- PJ5's exact mirror
}

TEST_CASE("project_state: a non-string lastScene is a parse FAILURE, never a coercion (PJ7)") {
    for (const std::string_view value : {"1", "true", "false", "null", "[]", "{}", "[\"a\"]"}) {
        CAPTURE(value);
        std::string text = R"({"version": 1, "lastScene": )";
        text += value;
        text += "}";
        CHECK_FALSE(parseProjectState(text).has_value());
    }
}

TEST_CASE("project_state: an ILLEGAL lastScene is a parse FAILURE, one subcase per rule (PJ8)") {
    // isLegalRelativePath's five rules (project.hpp:90-94), each with its own hostile value.
    constexpr std::array<std::string_view, 7> ILLEGAL = {
        "/abs/level.scene.json",       // leading '/'
        "scenes\\level.scene.json",    // any backslash
        "C:/scenes/level.scene.json",  // any colon -- covers "C:" and "C:/x"
        "C:level.scene.json",          // any colon, the driveless form
        "../outside.scene.json",       // a ".." segment, leading
        "a/../b.scene.json",           // a ".." segment, INTERIOR -- the one a prefix check misses
        "scenes/..",                   // a ".." segment, trailing
    };
    for (const std::string_view value : ILLEGAL) {
        CAPTURE(value);
        std::string text = R"({"version": 1, "lastScene": ")";
        text += value;
        text += R"("})";
        CHECK_FALSE(parseProjectState(text).has_value());
    }
    // ANTI-VACUITY: the same shape with a LEGAL value parses, so the loop above is rejecting the
    // values and not the surrounding text.
    CHECK(parseProjectState(R"({"version": 1, "lastScene": "a/b.scene.json"})").has_value());
}

TEST_CASE("project_state: MAX_PROJECT_STATE_SCENE_BYTES is a cap, not a truncation -- both directions (PJ9)") {
    const auto document = [](std::size_t length) {
        std::string text = R"({"version": 1, "lastScene": ")";
        text += std::string(length, 'a');
        text += R"("})";
        return text;
    };
    // EXACTLY the cap parses, and the value is returned WHOLE -- never shortened.
    const std::optional<ProjectState> atCap =
        parseProjectState(document(engine::editor::MAX_PROJECT_STATE_SCENE_BYTES));
    REQUIRE(atCap.has_value());
    CHECK(atCap->lastScene.size() == engine::editor::MAX_PROJECT_STATE_SCENE_BYTES);
    // ONE BYTE LONGER is a FAILURE. A truncation would silently name a different file.
    CHECK_FALSE(parseProjectState(document(engine::editor::MAX_PROJECT_STATE_SCENE_BYTES + 1U)).has_value());
}

TEST_CASE("project_state: an UNKNOWN key is ignored and the document still parses (PJ10)") {
    // The property that makes handoff H4 (the editor camera pose) one appended key with NO version
    // bump -- docs/09 section 8.5's own rule, one format over.
    const std::optional<ProjectState> state = parseProjectState(
        R"({"version": 1, "lastScene": "scenes/a.scene.json", "cameraPose": [1,2,3], "future": {"k": true}})");
    REQUIRE(state.has_value());
    CHECK(state->lastScene == "scenes/a.scene.json");
    CHECK(state->lastSceneRecorded);
}

// ---- the paths: PJ11-PJ16 ------------------------------------------------------------------------

TEST_CASE("project_state: projectStatePath joins under Library with exactly one separator (PJ11)") {
    CHECK(projectStatePath("/p/Game") == "/p/Game/Library/editor-state.json");
    CHECK(projectStatePath("/p/Game/") == "/p/Game/Library/editor-state.json");    // no double separator
    CHECK(projectStatePath("/p/Game///") == "/p/Game/Library/editor-state.json");  // every trailing one
    CHECK(projectStatePath("C:/p/Game") == "C:/p/Game/Library/editor-state.json");
    CHECK(projectStatePath("").empty());  // "" in, "" out
}

TEST_CASE("project_state: projectRelativeScenePath, the ordinary shapes (PJ12)") {
    CHECK(projectRelativeScenePath("/p/Game", "/p/Game/a.scene.json") == "a.scene.json");
    CHECK(projectRelativeScenePath("/p/Game", "/p/Game/scenes/a.scene.json") == "scenes/a.scene.json");
    CHECK(projectRelativeScenePath("/p/Game", "/p/Game/a/b/c.scene.json") == "a/b/c.scene.json");
    CHECK(projectRelativeScenePath("/p/Game/", "/p/Game/scenes/a.scene.json") == "scenes/a.scene.json");
    // A WINDOWS-SHAPED path, on EVERY lane: this function is pure, so the case runs everywhere and a
    // regression that stopped unifying '\' would be Windows-only in production and visible here.
    CHECK(projectRelativeScenePath(R"(C:\p\Game)", R"(C:\p\Game\scenes\a.scene.json)") == "scenes/a.scene.json");
    CHECK(projectRelativeScenePath(R"(C:\p\Game\)", "C:/p/Game/scenes/a.scene.json") == "scenes/a.scene.json");
}

TEST_CASE("project_state: the separator-boundary test, BOTH directions (PJ13)") {
    // A prefix comparison without the '/' would accept a SIBLING directory whose name starts with the
    // root's. This is the single most likely regression in the whole path block.
    CHECK(projectRelativeScenePath("/p/Game", "/p/Game2/x.scene.json").empty());
    CHECK(projectRelativeScenePath("/p/Game", "/p/GameOther/x.scene.json").empty());
    // ...and the partner, so "returns empty" is not vacuously true for everything.
    CHECK(projectRelativeScenePath("/p/Game", "/p/Game/x.scene.json") == "x.scene.json");
}

TEST_CASE("project_state: projectRelativeScenePath's five refusals (PJ14)") {
    SUBCASE("an empty root") { CHECK(projectRelativeScenePath("", "/p/Game/a.scene.json").empty()); }
    SUBCASE("an empty scene") { CHECK(projectRelativeScenePath("/p/Game", "").empty()); }
    SUBCASE("the scene IS the root") { CHECK(projectRelativeScenePath("/p/Game", "/p/Game").empty()); }
    SUBCASE("the scene is the root DIRECTORY") { CHECK(projectRelativeScenePath("/p/Game", "/p/Game/").empty()); }
    SUBCASE("the scene is ABOVE the root") { CHECK(projectRelativeScenePath("/p/Game", "/p/a.scene.json").empty()); }
    SUBCASE("R25 -- an ILLEGAL remainder is refused rather than recorded") {
        // Nothing refuses a ".." segment at openSceneFile today (that is E.4.2's defect to close), so
        // session.path() CAN be this. Without the isLegalRelativePath gate the reconcile would WRITE
        // it and this build's own parseProjectState would REFUSE it on the next open: one WARN and a
        // forgotten position, on a round trip that never left this process. Sabotage seed S25.
        CHECK(projectRelativeScenePath("/p/Game", "/p/Game/scenes/../scenes/a.scene.json").empty());
        // ANTI-VACUITY: the same path without the ".." is accepted, so the refusal is the ".." and
        // not the length or the nesting.
        CHECK(projectRelativeScenePath("/p/Game", "/p/Game/scenes/a.scene.json") == "scenes/a.scene.json");
    }
}

TEST_CASE("project_state: absoluteScenePath is the inverse, and refuses every illegal relative (PJ15)") {
    CHECK(absoluteScenePath("/p/Game", "scenes/a.scene.json") == "/p/Game/scenes/a.scene.json");
    CHECK(absoluteScenePath("/p/Game/", "scenes/a.scene.json") == "/p/Game/scenes/a.scene.json");
    CHECK(absoluteScenePath("", "scenes/a.scene.json").empty());
    CHECK(absoluteScenePath("/p/Game", "").empty());
    for (const std::string_view value : {"/abs/a.scene.json", R"(a\b.scene.json)", "C:/a.scene.json", "C:a.scene.json",
                                         "../a.scene.json", "a/../b.scene.json", "a/.."}) {
        CAPTURE(value);
        CHECK(absoluteScenePath("/p/Game", value).empty());
    }
}

TEST_CASE("project_state: absolute -> relative -> absolute is the identity for a '/'-rooted path (PJ16)") {
    const std::string root = "/p/Game";
    for (const std::string_view abs :
         {"/p/Game/a.scene.json", "/p/Game/scenes/a.scene.json", "/p/Game/a/b/c/deep.scene.json"}) {
        CAPTURE(abs);
        const std::string relative = projectRelativeScenePath(root, abs);
        REQUIRE_FALSE(relative.empty());
        CHECK(absoluteScenePath(root, relative) == abs);
    }
}

// ---- the two file operations: PJ31-PJ40 ------------------------------------------------------------

TEST_CASE("project_state: a root with NO Library reads defaults, SILENTLY (PJ31)") {
    const TempDir dir;
    bool corrupt = true;  // seeded TRUE, so "set to false" is a real write and not a default
    const ProjectState state = readProjectState(dir.utf8(), corrupt);
    CHECK_FALSE(corrupt);  // the normal state on a machine that has never opened this project
    CHECK(state.lastScene.empty());
    CHECK_FALSE(state.lastSceneRecorded);
}

TEST_CASE("project_state: a file that EXISTS and does not parse is defaults PLUS corrupt (PJ32)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("Library"), ec);
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library/editor-state.json"), "not json").empty());
    bool corrupt = false;
    const ProjectState state = readProjectState(dir.utf8(), corrupt);
    CHECK(corrupt);
    CHECK_FALSE(state.lastSceneRecorded);
}

TEST_CASE("project_state: editor-state.json as a DIRECTORY reads corrupt (PJ33)") {
    // The portable stand-in for a permission-refused file (editor_prefs.hpp's EP13): readTextFile
    // disengages for a directory exactly as for a missing file, and fileExists is
    // std::filesystem::exists, so it is TRUE here. Without the discriminator this would read as the
    // NORMAL state and an unreadable record would silently start a new scene with no diagnostic.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("Library/editor-state.json"), ec);
    bool corrupt = false;
    const ProjectState state = readProjectState(dir.utf8(), corrupt);
    CHECK(corrupt);
    CHECK_FALSE(state.lastSceneRecorded);
}

TEST_CASE("project_state: an EMPTY root reads nothing and is not a failure (PJ34)") {
    bool corrupt = true;
    const ProjectState state = readProjectState("", corrupt);
    CHECK_FALSE(corrupt);
    CHECK_FALSE(state.lastSceneRecorded);
}

TEST_CASE("project_state: writing creates Library AND its .gitignore, from the shipped constants (PJ35)") {
    const TempDir dir;
    REQUIRE_FALSE(engine::editor::fileExists(dir.join("Library")));
    const std::string reason =
        writeProjectState(dir.utf8(), ProjectState{.lastScene = "scenes/a.scene.json", .lastSceneRecorded = true});
    CHECK(reason.empty());
    CHECK(engine::editor::fileExists(dir.join("Library/editor-state.json")));
    REQUIRE(engine::editor::fileExists(dir.join("Library/.gitignore")));
    // The SHIPPED text, read off asset_cache.hpp -- never a restated copy. A restated literal one byte
    // off is invisible to every automated tier (E.1.4's sabotage row 20).
    const std::optional<std::string> ignore = readBytes(dir.join("Library/.gitignore"));
    REQUIRE(ignore.has_value());
    CHECK(*ignore == engine::editor::LIBRARY_GITIGNORE_TEXT);
}

TEST_CASE("project_state: writing NEVER overwrites an existing .gitignore (PJ36)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("Library"), ec);
    constexpr std::string_view SENTINEL = "# hand-edited, do not touch\n*\n";
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library/.gitignore"), SENTINEL).empty());
    const std::string reason =
        writeProjectState(dir.utf8(), ProjectState{.lastScene = "a.scene.json", .lastSceneRecorded = true});
    CHECK(reason.empty());
    const std::optional<std::string> ignore = readBytes(dir.join("Library/.gitignore"));
    REQUIRE(ignore.has_value());
    CHECK(*ignore == SENTINEL);                                                // the sentinel SURVIVED
    CHECK(engine::editor::fileExists(dir.join("Library/editor-state.json")));  // and the state landed
}

TEST_CASE("project_state: Library occupied by a FILE fails with a reason and writes nothing (PJ37)") {
    const TempDir dir;
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("Library"), "not a directory").empty());
    const std::string reason =
        writeProjectState(dir.utf8(), ProjectState{.lastScene = "a.scene.json", .lastSceneRecorded = true});
    CHECK_FALSE(reason.empty());  // the OS reason, returned rather than logged
    CHECK_FALSE(engine::editor::fileExists(dir.join("Library/editor-state.json")));
}

TEST_CASE("project_state: writing with an EMPTY root writes nothing and succeeds (PJ38)") {
    const std::string reason =
        writeProjectState("", ProjectState{.lastScene = "a.scene.json", .lastSceneRecorded = true});
    CHECK(reason.empty());  // "this instance has no project" is not a failure
}

TEST_CASE("project_state: write -> read round-trips through the DISK, all three states (PJ39)") {
    const std::array<ProjectState, 3> states = {
        ProjectState{},
        ProjectState{.lastScene = "", .lastSceneRecorded = true},
        ProjectState{.lastScene = "scenes/level1.scene.json", .lastSceneRecorded = true},
    };
    for (const ProjectState& state : states) {
        CAPTURE(state.lastScene);
        CAPTURE(state.lastSceneRecorded);
        const TempDir dir;  // a FRESH directory per state -- a leftover file would make this vacuous
        REQUIRE(writeProjectState(dir.utf8(), state).empty());
        bool corrupt = true;
        const ProjectState back = readProjectState(dir.utf8(), corrupt);
        CHECK_FALSE(corrupt);
        CHECK(back.lastScene == state.lastScene);
        CHECK(back.lastSceneRecorded == state.lastSceneRecorded);
    }
}

TEST_CASE("project_state: two identical writes leave a BYTE-identical file (PJ40)") {
    const TempDir dir;
    const ProjectState state{.lastScene = "scenes/a.scene.json", .lastSceneRecorded = true};
    REQUIRE(writeProjectState(dir.utf8(), state).empty());
    const std::optional<std::string> first = readBytes(dir.join("Library/editor-state.json"));
    REQUIRE(first.has_value());
    REQUIRE(writeProjectState(dir.utf8(), state).empty());
    const std::optional<std::string> second = readBytes(dir.join("Library/editor-state.json"));
    REQUIRE(second.has_value());
    CHECK(*second == *first);  // the BYTES, not the parse
}
