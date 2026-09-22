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
using engine::editor::chooseStartupScene;
using engine::editor::firstSceneUnder;
using engine::editor::isSceneFileName;
using engine::editor::parseProjectState;
using engine::editor::projectRelativeScenePath;
using engine::editor::ProjectState;
using engine::editor::projectStatePath;
using engine::editor::projectStateStep;
using engine::editor::ProjectStateStep;
using engine::editor::readProjectState;
using engine::editor::StartupScene;
using engine::editor::StartupSceneFacts;
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

// A scoped enum inside a CHECK needs DOUBLE PARENTHESES, and `CHECK((a == b))` prints `CHECK( true )`
// on a FAILURE as well as on a pass. An operator<< in THIS anonymous namespace is invisible to
// doctest -- its streamability trait calls an UNQUALIFIED operator<< from inside doctest::detail, so
// only namespaces ASSOCIATED WITH THE ARGUMENTS are searched, and StartupScene's is engine::editor
// (task E.2.2's measured rule). Comparing ints instead prints both values and needs no printer.
[[nodiscard]] int sceneChoice(StartupScene value) noexcept { return static_cast<int>(value); }
[[nodiscard]] int stepChoice(ProjectStateStep value) noexcept { return static_cast<int>(value); }

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

// ---- isSceneFileName / firstSceneUnder: PJ17-PJ24 --------------------------------------------------

TEST_CASE("project_state: isSceneFileName, every arm pinned (PJ17)") {
    // ACCEPTED.
    CHECK(isSceneFileName("a.scene.json"));
    CHECK(isSceneFileName("A.SCENE.JSON"));  // ASCII-case-folded: a case-insensitive volume serves this
    CHECK(isSceneFileName("a.Scene.Json"));
    CHECK(isSceneFileName("my.level.scene.json"));  // the suffix is the LAST 11 bytes, not "the extension"
    CHECK(isSceneFileName(".hidden.scene.json"));   // it HAS a stem; PJ21 is what excludes it, not this
    // REFUSED.
    // THE STEM REQUIREMENT (plan 4.5). ".scene.json" alone is not a scene OF anything -- the
    // isMetaFileName / isBlendFileName / isImportableModelName shape, whose own comments say exactly
    // this about ".meta" (asset_meta.cpp:172) and ".blend" (blender_tool.cpp:48-49).
    CHECK_FALSE(isSceneFileName(".scene.json"));
    CHECK_FALSE(isSceneFileName("scene.json"));  // no leading '.', so it does not carry the suffix
    CHECK_FALSE(isSceneFileName("a.json"));
    CHECK_FALSE(isSceneFileName("a.scene.jsonx"));  // a SUFFIX test, not a "contains"
    CHECK_FALSE(isSceneFileName("a.scene.json.bak"));
    CHECK_FALSE(isSceneFileName("ascene.json"));
    CHECK_FALSE(isSceneFileName(""));
}

TEST_CASE("project_state: firstSceneUnder picks the first by entryOrderLess, not by creation order (PJ18)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("scenes"), ec);
    // Created c, a, b -- so "the first one written" and "the first in order" disagree.
    for (const std::string_view leaf : {"c.scene.json", "a.scene.json", "b.scene.json"}) {
        REQUIRE(
            engine::editor::writeTextFileAtomic(dir.join(std::string("scenes/") + std::string(leaf)), "{}").empty());
    }
    CHECK(firstSceneUnder(dir.utf8(), "scenes") == "scenes/a.scene.json");
}

TEST_CASE("project_state: firstSceneUnder SKIPS DIRECTORIES -- the single most likely regression (PJ19)") {
    // entryOrderLess sorts directories FIRST (project_files.hpp:109-113), so "take entries.front()" is
    // wrong in the most ordinary case there is: a scenes/ folder with a subfolder in it. The directory
    // is deliberately named so it sorts AHEAD of the real scene.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("scenes/aaa.scene.json"), ec);
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes/zzz.scene.json"), "{}").empty());
    CHECK(firstSceneUnder(dir.utf8(), "scenes") == "scenes/zzz.scene.json");
}

TEST_CASE("project_state: non-scene files are skipped, and a directory of them yields nothing (PJ20)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("scenes"), ec);
    for (const std::string_view leaf : {"a.json", "b.txt", "c.scene", "readme.md", ".scene.json"}) {
        REQUIRE(
            engine::editor::writeTextFileAtomic(dir.join(std::string("scenes/") + std::string(leaf)), "{}").empty());
    }
    CHECK(firstSceneUnder(dir.utf8(), "scenes").empty());
    // ANTI-VACUITY: add one real scene and the SAME directory now yields it.
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes/real.scene.json"), "{}").empty());
    CHECK(firstSceneUnder(dir.utf8(), "scenes") == "scenes/real.scene.json");
}

TEST_CASE("project_state: HIDDEN entries are excluded (PJ21)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("scenes"), ec);
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes/.hidden.scene.json"), "{}").empty());
    // PJ17 proves isSceneFileName ACCEPTS ".hidden.scene.json", so this refusal is listDirectory's
    // includeHidden=false and nothing else. The two facts are pinned independently on purpose: a
    // regression in either must not be maskable by the other.
    CHECK(firstSceneUnder(dir.utf8(), "scenes").empty());
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes/visible.scene.json"), "{}").empty());
    CHECK(firstSceneUnder(dir.utf8(), "scenes") == "scenes/visible.scene.json");
}

TEST_CASE("project_state: a missing, non-directory or rootless scenes path yields nothing (PJ22)") {
    const TempDir dir;
    SUBCASE("the directory does not exist") { CHECK(firstSceneUnder(dir.utf8(), "scenes").empty()); }
    SUBCASE("paths.scenes names a FILE") {
        REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes"), "not a directory").empty());
        CHECK(firstSceneUnder(dir.utf8(), "scenes").empty());
    }
    SUBCASE("an empty root") { CHECK(firstSceneUnder("", "scenes").empty()); }
    SUBCASE("an empty scenes path") { CHECK(firstSceneUnder(dir.utf8(), "").empty()); }
}

TEST_CASE("project_state: the result is ROOT-relative, not scenes-relative (PJ23)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("scenes"), ec);
    REQUIRE(engine::editor::writeTextFileAtomic(dir.join("scenes/a.scene.json"), "{}").empty());
    const std::string result = firstSceneUnder(dir.utf8(), "scenes");
    CHECK(result == "scenes/a.scene.json");  // NOT "a.scene.json"
    // ...and it round-trips through absoluteScenePath to a file that EXISTS, which is the property the
    // caller actually depends on.
    CHECK(engine::editor::fileExists(absoluteScenePath(dir.utf8(), result)));
}

TEST_CASE("project_state: a nested scenesPath and a scenesPath of \".\" both work (PJ24)") {
    SUBCASE("content/scenes") {
        const TempDir dir;
        std::error_code ec;
        std::filesystem::create_directories(dir.join("content/scenes"), ec);
        REQUIRE(engine::editor::writeTextFileAtomic(dir.join("content/scenes/a.scene.json"), "{}").empty());
        CHECK(firstSceneUnder(dir.utf8(), "content/scenes") == "content/scenes/a.scene.json");
    }
    SUBCASE(".") {
        // DELIBERATELY NOT SPECIAL-CASED (plan 6.6b): joinRelative(".", leaf) is "./leaf", which is
        // isLegalRelativePath, re-resolves to the same file, and round-trips. Pinned in BOTH
        // directions -- the exact bytes, and the fact that those bytes name the file.
        const TempDir dir;
        REQUIRE(engine::editor::writeTextFileAtomic(dir.join("a.scene.json"), "{}").empty());
        const std::string result = firstSceneUnder(dir.utf8(), ".");
        CHECK(result == "./a.scene.json");
        CHECK(engine::editor::fileExists(absoluteScenePath(dir.utf8(), result)));
    }
}

// ---- chooseStartupScene: PJ25, PJ26 ----------------------------------------------------------------

TEST_CASE("project_state: chooseStartupScene's WHOLE 16-row truth table (PJ25)") {
    struct Row {
        bool recorded;
        bool recordedEmpty;
        bool recordedExists;
        bool firstSceneFound;
        StartupScene expected;
    };
    // All sixteen combinations, INCLUDING the four `!recorded && recordedExists` rows that cannot
    // happen in practice: a decider that reads a fact it should ignore is a real defect, and these
    // four are the only thing in the tree that can catch it. They are also why restoreLastScene
    // computes firstSceneUnder UNCONDITIONALLY (plan 4.1) -- a false-but-unread fact would turn this
    // case from an assertion into a coincidence.
    constexpr std::array<Row, 16> ROWS = {{
        {false, false, false, false, StartupScene::NewScene},
        {false, false, false, true, StartupScene::FirstUnderScenes},
        {false, false, true, false, StartupScene::NewScene},         // recordedExists IGNORED
        {false, false, true, true, StartupScene::FirstUnderScenes},  // recordedExists IGNORED
        {false, true, false, false, StartupScene::NewScene},
        {false, true, false, true, StartupScene::FirstUnderScenes},
        {false, true, true, false, StartupScene::NewScene},         // recordedExists IGNORED
        {false, true, true, true, StartupScene::FirstUnderScenes},  // recordedExists IGNORED
        {true, false, false, false, StartupScene::NewScene},
        {true, false, false, true, StartupScene::FirstUnderScenes},  // D6's CASCADE arm
        {true, false, true, false, StartupScene::Recorded},
        {true, false, true, true, StartupScene::Recorded},  // the record BEATS the first scene
        {true, true, false, false, StartupScene::NewScene},
        {true, true, false, true, StartupScene::NewScene},  // D2's third state BEATS the first scene
        {true, true, true, false, StartupScene::NewScene},
        {true, true, true, true, StartupScene::NewScene},
    }};
    for (const Row& row : ROWS) {
        CAPTURE(row.recorded);
        CAPTURE(row.recordedEmpty);
        CAPTURE(row.recordedExists);
        CAPTURE(row.firstSceneFound);
        const StartupSceneFacts facts{.recorded = row.recorded,
                                      .recordedEmpty = row.recordedEmpty,
                                      .recordedExists = row.recordedExists,
                                      .firstSceneFound = row.firstSceneFound};
        CHECK(sceneChoice(chooseStartupScene(facts)) == sceneChoice(row.expected));
    }
}

TEST_CASE("project_state: chooseStartupScene's three arms, as the user stories they are (PJ26)") {
    SUBCASE("I was editing scenes/level1 -- give it back") {
        const StartupSceneFacts facts{
            .recorded = true, .recordedEmpty = false, .recordedExists = true, .firstSceneFound = true};
        CHECK(sceneChoice(chooseStartupScene(facts)) == sceneChoice(StartupScene::Recorded));
    }
    SUBCASE("I chose File > New Scene and quit -- do NOT hand me somebody else's scene") {
        const StartupSceneFacts facts{
            .recorded = true, .recordedEmpty = true, .recordedExists = false, .firstSceneFound = true};
        CHECK(sceneChoice(chooseStartupScene(facts)) == sceneChoice(StartupScene::NewScene));
    }
    SUBCASE("I just cloned this project -- show me something rather than an empty box") {
        const StartupSceneFacts facts{
            .recorded = false, .recordedEmpty = true, .recordedExists = false, .firstSceneFound = true};
        CHECK(sceneChoice(chooseStartupScene(facts)) == sceneChoice(StartupScene::FirstUnderScenes));
    }
}

// ---- projectStateStep: PJ27-PJ30 -------------------------------------------------------------------

TEST_CASE("project_state: a ROOT change ADOPTS, whatever the scenes are (PJ27)") {
    // Including when the scenes are EQUAL, and including a close (root -> ""): the state was just
    // READ, not changed, and writing here would re-record the value it was read from (D5). The two
    // produce byte-identical files, so projectStateWriteCount() is the only thing that can see it.
    CHECK(stepChoice(projectStateStep("/a", "", "", "")) == stepChoice(ProjectStateStep::AdoptBaseline));
    CHECK(stepChoice(projectStateStep("/a", "/b", "x", "x")) == stepChoice(ProjectStateStep::AdoptBaseline));
    CHECK(stepChoice(projectStateStep("/a", "/b", "x", "y")) == stepChoice(ProjectStateStep::AdoptBaseline));
    CHECK(stepChoice(projectStateStep("", "/b", "", "y")) == stepChoice(ProjectStateStep::AdoptBaseline));
}

TEST_CASE("project_state: one root, a changed scene WRITES; an unchanged one does NOTHING (PJ28)") {
    CHECK(stepChoice(projectStateStep("/a", "/a", "x", "y")) == stepChoice(ProjectStateStep::Write));
    CHECK(stepChoice(projectStateStep("/a", "/a", "x", "x")) == stepChoice(ProjectStateStep::Nothing));
}

TEST_CASE("project_state: no project means nothing to record, even with differing scenes (PJ29)") {
    CHECK(stepChoice(projectStateStep("", "", "x", "y")) == stepChoice(ProjectStateStep::Nothing));
    CHECK(stepChoice(projectStateStep("", "", "", "")) == stepChoice(ProjectStateStep::Nothing));
}

TEST_CASE("project_state: CLEARING the scene is a change, in both directions (PJ30)") {
    CHECK(stepChoice(projectStateStep("/a", "/a", "", "x")) == stepChoice(ProjectStateStep::Write));
    CHECK(stepChoice(projectStateStep("/a", "/a", "x", "")) == stepChoice(ProjectStateStep::Write));
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

// ---- the end-of-tick DRAIN's own predicate: PJ56 ---------------------------------------------------

TEST_CASE("project_state: the outgoing-pair DRAIN is projectStateStep, asked about a DIFFERENT pair (PJ56)") {
    // EditorApp::persistProjectState drains ProjectFlow's outgoing (root, scene) pair -- published by
    // openProjectPath immediately before the adopt -- by asking THIS decider the same question it asks
    // about the live session: "does this pair differ from the baseline in a way that must be written?"
    // No second predicate is spelled anywhere, deliberately: a hand-written three-term condition beside
    // this function is a second rule that can drift from it, and the drain and the reconcile must never
    // disagree about what a change IS.
    //
    // These are the drain's four rows, in the drain's own vocabulary. They are the same function
    // PJ27-PJ30 cover, and that is the point of the case.
    //
    // (1) THE ROW THE HANDOFF EXISTS FOR: the baseline still names the outgoing project, and the pair
    // says its scene moved while this tick ran -- a guarded Save that chained into a project open.
    CHECK(stepChoice(projectStateStep("/p/A", "/p/A", "scenes/saved.scene.json", "")) ==
          stepChoice(ProjectStateStep::Write));
    // (2) A PLAIN SWAP: the pair is exactly what the last reconcile already recorded, so nothing is
    // written -- which is what keeps I176 and I180 ("opening or swapping a project writes nothing")
    // true with the drain in place.
    CHECK(stepChoice(projectStateStep("/p/A", "/p/A", "scenes/a.scene.json", "scenes/a.scene.json")) ==
          stepChoice(ProjectStateStep::Nothing));
    // (3) A PAIR THE BASELINE CANNOT SPEAK FOR -- two opens in one tick, so the pending root is no
    // longer the one the baseline names. DROPPED, never written: writing it would record a scene
    // against a baseline that was never reconciled for it.
    CHECK(stepChoice(projectStateStep("/p/A", "/p/B", "scenes/saved.scene.json", "")) !=
          stepChoice(ProjectStateStep::Write));
    // (4) NOTHING PENDING -- the ordinary tick, and the reason the pending root's emptiness is the
    // whole of the "is a pair pending" flag. A separate bool would be a second spelling of this row.
    CHECK(stepChoice(projectStateStep("", "", "", "")) == stepChoice(ProjectStateStep::Nothing));
    CHECK(stepChoice(projectStateStep("", "/p/A", "", "scenes/a.scene.json")) != stepChoice(ProjectStateStep::Write));
}
