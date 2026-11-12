// Aero Engine — the play record (<projectRoot>/Library/play-recovery.json and its scene copy), its two pure
// deciders and its four disk functions (task 4.7.3, RV1-RV8). Tier 0: aero_editor_shell_test, no ImGui, no
// GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING in all three configurations.
// Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
#include <aero/editor/asset_cache.hpp>  // LIBRARY_GITIGNORE_TEXT / ASSET_CACHE_GITIGNORE_NAME -- the SHIPPED constants
#include <aero/editor/play_recovery.hpp>
#include <aero/editor/project_state.hpp>  // MAX_PROJECT_STATE_SCENE_BYTES
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
