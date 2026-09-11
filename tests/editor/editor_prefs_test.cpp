// Aero Engine — editor_prefs.json's codec and file operations (task E.3.2, EP1-EP11). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window.
#include <aero/editor/editor_prefs.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>

using engine::editor::EditorPrefs;
using engine::editor::parseEditorPrefs;
using engine::editor::readEditorPrefs;
using engine::editor::writeEditorPrefs;
using engine::editor::writeEditorPrefsText;

namespace {

// The NINTH copy of this shape (text_file_test.cpp:50-86 is the eighth, and states the rule:
// scaffolding is copied, the assertion is shared). A unique temp directory that removes itself.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_editor_prefs_test_" + std::to_string(++counter));
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

    [[nodiscard]] std::string join(std::string_view leaf) const {
        const std::u8string bytes = dirPath.u8string();
        std::string result(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

}  // namespace

TEST_CASE("editor: EditorPrefs defaults to focus-follows-selection ON (EP1)") {
    const EditorPrefs prefs;
    CHECK(prefs.focusFollowsSelection);  // TRUE is the shipping behaviour, so it is the default
}

TEST_CASE("editor: writeEditorPrefsText is deterministic, version-first, one trailing newline (EP2)") {
    for (const bool value : {true, false}) {
        CAPTURE(value);
        const EditorPrefs prefs{.focusFollowsSelection = value};
        const std::string text = writeEditorPrefsText(prefs);
        CHECK(writeEditorPrefsText(prefs) == text);  // DETERMINISTIC

        const std::size_t versionAt = text.find("\"version\"");
        const std::size_t focusAt = text.find("\"focusFollowsSelection\"");
        REQUIRE(versionAt != std::string::npos);
        REQUIRE(focusAt != std::string::npos);
        CHECK(versionAt < focusAt);  // version FIRST, validated first

        REQUIRE_FALSE(text.empty());
        CHECK(text.back() == '\n');
        CHECK(text[text.size() - 2U] != '\n');  // exactly ONE

        // ...and it re-parses EQUAL, which is the only thing "canonical" means here.
        const std::optional<EditorPrefs> reparsed = parseEditorPrefs(text);
        REQUIRE(reparsed.has_value());
        CHECK(reparsed->focusFollowsSelection == value);
    }
}

TEST_CASE("editor: a wrong version is a MISS for the whole document (EP3)") {
    CHECK_FALSE(parseEditorPrefs(R"({"version":2,"focusFollowsSelection":false})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"version":0,"focusFollowsSelection":false})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"focusFollowsSelection":false})").has_value());  // absent
    CHECK_FALSE(parseEditorPrefs(R"({"version":"1","focusFollowsSelection":false})").has_value());
    // ANTI-VACUITY: the SAME document with version 1 parses.
    CHECK(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":false})").has_value());
}

TEST_CASE("editor: structurally wrong text is a MISS, never a partial value (EP4)") {
    SUBCASE("unparseable") {
        CHECK_FALSE(parseEditorPrefs("{not json").has_value());
        CHECK_FALSE(parseEditorPrefs("{\"version\":1,").has_value());
    }
    SUBCASE("a NON-OBJECT root") {
        CHECK_FALSE(parseEditorPrefs("[]").has_value());
        CHECK_FALSE(parseEditorPrefs("1").has_value());
        CHECK_FALSE(parseEditorPrefs("\"version\"").has_value());
        CHECK_FALSE(parseEditorPrefs("null").has_value());
    }
    SUBCASE("an EMPTY string") { CHECK_FALSE(parseEditorPrefs("").has_value()); }
}

TEST_CASE("editor: an ABSENT focusFollowsSelection is the DEFAULT, not a miss (EP5)") {
    // THE rule that makes appending a key to this file a non-breaking change: E.6.1's theme and the
    // per-user viewport toggles both land here, and neither may need a version bump.
    const std::optional<EditorPrefs> parsed = parseEditorPrefs(R"({"version":1})");
    REQUIRE(parsed.has_value());
    CHECK(parsed->focusFollowsSelection);  // the DEFAULT, not false

    SUBCASE("and an UNKNOWN key is ignored rather than refused") {
        const std::optional<EditorPrefs> forward =
            parseEditorPrefs(R"({"version":1,"somethingFromTheFuture":42,"focusFollowsSelection":false})");
        REQUIRE(forward.has_value());
        CHECK_FALSE(forward->focusFollowsSelection);
    }
}

TEST_CASE("editor: a PRESENT-but-non-bool focusFollowsSelection is a MISS, never coerced (EP6)") {
    CHECK_FALSE(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":0})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":1})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":"false"})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":null})").has_value());
    CHECK_FALSE(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":[]})").has_value());
    // ANTI-VACUITY: both real bools parse, so "miss" is about the TYPE and not about the key.
    CHECK(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":true})").has_value());
    CHECK(parseEditorPrefs(R"({"version":1,"focusFollowsSelection":false})").has_value());
}

TEST_CASE("editor: the text round trip preserves both values (EP7)") {
    for (const bool value : {true, false}) {
        CAPTURE(value);
        const EditorPrefs written{.focusFollowsSelection = value};
        const std::optional<EditorPrefs> read = parseEditorPrefs(writeEditorPrefsText(written));
        REQUIRE(read.has_value());
        CHECK(read->focusFollowsSelection == value);
    }
    // The writer emits a JSON BOOL, not the string "true": a string form would round-trip through
    // writeEditorPrefsText only to be REFUSED by the parser's own optionalBool.
    CHECK(writeEditorPrefsText(EditorPrefs{.focusFollowsSelection = true}).find("\"true\"") == std::string::npos);
    CHECK(writeEditorPrefsText(EditorPrefs{.focusFollowsSelection = true}).find("true") != std::string::npos);
}

TEST_CASE("editor: a MISSING preferences file is defaults, SILENTLY (EP8)") {
    const TempDir tmp;
    bool corrupt = true;  // seeded WRONG on purpose: the function must clear it
    const EditorPrefs prefs = readEditorPrefs(tmp.join("nothing-here.json"), corrupt);
    CHECK(prefs.focusFollowsSelection);
    CHECK_FALSE(corrupt);  // a missing file is the NORMAL state -- never a warning

    SUBCASE("an EMPTY path is the same: read nothing, warn about nothing") {
        bool emptyCorrupt = true;
        const EditorPrefs none = readEditorPrefs("", emptyCorrupt);
        CHECK(none.focusFollowsSelection);
        CHECK_FALSE(emptyCorrupt);
    }
}

TEST_CASE("editor: a file that EXISTS and does not parse is defaults PLUS corrupt (EP9)") {
    const TempDir tmp;
    const std::string path = tmp.join("editor_prefs.json");
    REQUIRE(engine::editor::writeTextFileAtomic(path, "{ not json at all").empty());

    bool corrupt = false;
    const EditorPrefs prefs = readEditorPrefs(path, corrupt);
    CHECK(prefs.focusFollowsSelection);  // defaults
    CHECK(corrupt);                      // ...PLUS the out-flag the caller turns into ONE warning

    SUBCASE("a wrong VERSION is corrupt too, not silently ignored") {
        REQUIRE(engine::editor::writeTextFileAtomic(path, "{\"version\":99}\n").empty());
        bool versionCorrupt = false;
        const EditorPrefs wrongVersion = readEditorPrefs(path, versionCorrupt);
        CHECK(wrongVersion.focusFollowsSelection);
        CHECK(versionCorrupt);
    }
    SUBCASE("a VALID file is not corrupt -- the flag is about the document, not about existing") {
        REQUIRE(
            engine::editor::writeTextFileAtomic(path, writeEditorPrefsText(EditorPrefs{.focusFollowsSelection = false}))
                .empty());
        bool validCorrupt = true;
        const EditorPrefs valid = readEditorPrefs(path, validCorrupt);
        CHECK_FALSE(valid.focusFollowsSelection);
        CHECK_FALSE(validCorrupt);
    }
}

TEST_CASE("editor: writeEditorPrefs round-trips through a real file, and reports a real failure (EP10)") {
    const TempDir tmp;
    const std::string path = tmp.join("editor_prefs.json");

    CHECK(writeEditorPrefs(path, EditorPrefs{.focusFollowsSelection = false}).empty());
    bool corrupt = true;
    const EditorPrefs read = readEditorPrefs(path, corrupt);
    CHECK_FALSE(read.focusFollowsSelection);
    CHECK_FALSE(corrupt);

    SUBCASE("an UNWRITABLE path returns a non-empty reason and creates nothing") {
        // A path whose PARENT does not exist -- portable across all three lanes, unlike a permission
        // bit. writeTextFileAtomic writes <path>.aero-tmp first, so neither file may appear.
        const std::string bad = tmp.join("no-such-directory/editor_prefs.json");
        const std::string reason = writeEditorPrefs(bad, EditorPrefs{});
        CHECK_FALSE(reason.empty());
        CHECK_FALSE(engine::editor::fileExists(bad));
        CHECK_FALSE(engine::editor::fileExists(bad + ".aero-tmp"));
    }
    SUBCASE("an EMPTY path writes nothing and is NOT a failure") { CHECK(writeEditorPrefs("", EditorPrefs{}).empty()); }
}

TEST_CASE("editor: two writes of the same value produce BYTE-IDENTICAL files (EP11)") {
    const TempDir tmp;
    const std::string first = tmp.join("a.json");
    const std::string second = tmp.join("b.json");
    const EditorPrefs prefs{.focusFollowsSelection = false};
    REQUIRE(writeEditorPrefs(first, prefs).empty());
    REQUIRE(writeEditorPrefs(second, prefs).empty());

    const engine::editor::FileReadResult a = engine::editor::readTextFile(first);
    const engine::editor::FileReadResult b = engine::editor::readTextFile(second);
    REQUIRE(a.text.has_value());
    REQUIRE(b.text.has_value());
    CHECK(*a.text == *b.text);
    // ANTI-VACUITY: the two DIFFERENT values are not byte-identical, so the check above is about
    // determinism and not about the writer emitting a constant.
    const std::string other = tmp.join("c.json");
    REQUIRE(writeEditorPrefs(other, EditorPrefs{.focusFollowsSelection = true}).empty());
    const engine::editor::FileReadResult c = engine::editor::readTextFile(other);
    REQUIRE(c.text.has_value());
    CHECK(*c.text != *a.text);
}
