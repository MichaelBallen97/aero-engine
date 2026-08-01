// tests/editor/project_test.cpp -- task 2.6.1: project.json v1's format, its validators, the
// filesystem half, the flow, and the three committed golden fixtures. SIXTEENTH TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, and that is the point (D4/AC-35/INV-P5). Unlike scene_io_test.cpp and
// scene_golden_test.cpp, which are ABSENT from the reflect-OFF build by design, every case in this
// file must be PRESENT and PASSING in all three configurations -- prove it with --list-test-cases,
// never with a skip. Tier-0: no GPU, no window, no ImGui context, so it must pass identically with
// AERO_REQUIRE_GPU unset and set.
#include <aero/editor/project.hpp>
#include <aero/editor/text_file.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using engine::editor::isLegalRelativePath;
using engine::editor::NameProblem;
using engine::editor::nameProblemMessage;
using engine::editor::parseProject;
using engine::editor::parseRecentProjects;
using engine::editor::ProjectError;
using engine::editor::ProjectLanguage;
using engine::editor::ProjectManifest;
using engine::editor::ProjectParseResult;
using engine::editor::ProjectSession;
using engine::editor::promoteRecent;
using engine::editor::RecentProjects;
using engine::editor::validateProjectName;
using engine::editor::writeProjectText;
using engine::editor::writeRecentProjectsText;

// ---- P1-P8: parseProject happy paths ---------------------------------------------------------------

TEST_CASE("project: parseProject accepts the minimal canonical document (P1)") {
    const std::string text =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\"\n"
        "  }\n"
        "}\n";
    const ProjectParseResult result = parseProject(text);
    REQUIRE(result.manifest.has_value());
    CHECK(result.error == ProjectError::None);
    CHECK(result.message.empty());
    CHECK(result.line == 0);
    CHECK(result.column == 0);
    CHECK(result.manifest->name == "MyGame");
    CHECK(result.manifest->engineVersion == "0.1.0");
    CHECK(result.manifest->language == ProjectLanguage::Ts);
    CHECK(result.manifest->assetsPath == "assets");
    CHECK(result.manifest->scenesPath == "scenes");
    CHECK(result.unknownKeys.empty());
}

TEST_CASE("project: parseProject accepts nested paths, \"cpp\" and a non-ASCII name (P2/P3/P4/D5)") {
    const std::string text =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"Caf\xC3\xA9 Rocket \xF0\x9F\x9A\x80\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"cpp\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"content/art\",\n"
        "    \"scenes\": \"content/levels\"\n"
        "  }\n"
        "}\n";
    const ProjectParseResult result = parseProject(text);
    REQUIRE(result.manifest.has_value());
    CHECK(result.manifest->name == "Caf\xC3\xA9 Rocket \xF0\x9F\x9A\x80");
    CHECK(result.manifest->language == ProjectLanguage::Cpp);
    CHECK(result.manifest->assetsPath == "content/art");
    CHECK(result.manifest->scenesPath == "content/levels");
}

TEST_CASE("project: parseProject collects unknown root and paths keys without erroring (P5/AC-6/S18)") {
    const std::string text =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\",\n"
        "    \"prefabs\": \"prefabs\"\n"
        "  },\n"
        "  \"author\": \"someone\",\n"
        "  \"editorLayout\": \"default\"\n"
        "}\n";
    const ProjectParseResult result = parseProject(text);
    REQUIRE(result.manifest.has_value());
    // "paths" precedes "author"/"editorLayout" among the root members, so its own unknown sub-key
    // is collected AT THAT POSITION in the single depth-first walk -- true document order, not
    // "every root unknown, then every paths unknown" (project.cpp's own comment on this).
    const std::vector<std::string> expected = {"prefabs", "author", "editorLayout"};
    CHECK(result.unknownKeys == expected);
    CHECK(result.manifest->assetsPath == "assets");
    CHECK(result.manifest->scenesPath == "scenes");
}

TEST_CASE("project: parseProject tolerates a BOM, CRLF and a missing trailing newline (P6/P7/P8/E3/E4)") {
    const std::string bomPrefixed =
        "\xEF\xBB\xBF"
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\"\n"
        "  }\n"
        "}\n";
    const ProjectParseResult bomResult = parseProject(bomPrefixed);
    REQUIRE(bomResult.manifest.has_value());
    CHECK(bomResult.manifest->name == "MyGame");

    const std::string crlf =
        "{\r\n"
        "  \"version\": 1,\r\n"
        "  \"name\": \"MyGame\",\r\n"
        "  \"engineVersion\": \"0.1.0\",\r\n"
        "  \"language\": \"ts\",\r\n"
        "  \"paths\": {\r\n"
        "    \"assets\": \"assets\",\r\n"
        "    \"scenes\": \"scenes\"\r\n"
        "  }\r\n"
        "}\r\n";
    const ProjectParseResult crlfResult = parseProject(crlf);
    REQUIRE(crlfResult.manifest.has_value());
    CHECK(crlfResult.manifest->name == "MyGame");

    const std::string noTrailingNewline =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\"\n"
        "  }\n"
        "}";  // no trailing '\n'
    const ProjectParseResult noNewlineResult = parseProject(noTrailingNewline);
    REQUIRE(noNewlineResult.manifest.has_value());
    CHECK(noNewlineResult.manifest->name == "MyGame");
}

// ---- P9-P24: parseProject rejections -----------------------------------------------------------

TEST_CASE("project: parseProject rejects unreadable JSON and a non-object root (P9/P10/AC-4)") {
    {
        const ProjectParseResult result = parseProject("{ not json");
        CHECK_FALSE(result.manifest.has_value());
        CHECK(result.error == ProjectError::BadJson);
        CHECK(result.line > 0);
        CHECK(result.column > 0);
    }
    {
        const ProjectParseResult result = parseProject("[]");
        CHECK_FALSE(result.manifest.has_value());
        CHECK(result.error == ProjectError::NotAnObject);
        CHECK(result.line == 0);
        CHECK(result.column == 0);
        CHECK(result.message == "project root must be a JSON object (found array)");
    }
}

TEST_CASE("project: parseProject validates \"version\" FIRST (P11/AC-5/S3)") {
    // Missing "name" too -- the version error must still be reported, not the name error.
    const ProjectParseResult result = parseProject(R"({"version": 2})");
    CHECK_FALSE(result.manifest.has_value());
    CHECK(result.error == ProjectError::UnsupportedVersion);
    CHECK(result.message == "unsupported project format version 2 (this build reads version 1)");

    const ProjectParseResult missingVersion = parseProject(R"({"name": "MyGame"})");
    CHECK(missingVersion.error == ProjectError::BadVersion);
    CHECK(missingVersion.message == "missing required key \"version\"");

    const ProjectParseResult nonIntegerVersion = parseProject(R"({"version": "1", "name": "MyGame"})");
    CHECK(nonIntegerVersion.error == ProjectError::BadVersion);
    CHECK(nonIntegerVersion.message == "\"version\" must be an integer (found string)");
}

TEST_CASE("project: parseProject rejects a bad \"name\" (P12/P13/AC-4)") {
    {
        const ProjectParseResult result = parseProject(R"({"version": 1})");
        CHECK(result.error == ProjectError::BadName);
        CHECK(result.message == "missing required key \"name\"");
    }
    {
        const ProjectParseResult result = parseProject(R"({"version": 1, "name": 5})");
        CHECK(result.error == ProjectError::BadName);
        CHECK(result.message == "\"name\" must be a string (found number)");
    }
    {
        const ProjectParseResult result = parseProject(R"({"version": 1, "name": ""})");
        CHECK(result.error == ProjectError::BadName);
        CHECK(result.message == "\"name\" must not be empty");
    }
}

TEST_CASE("project: parseProject rejects a bad \"engineVersion\" (P14/AC-4)") {
    {
        const ProjectParseResult result = parseProject(R"({"version": 1, "name": "MyGame"})");
        CHECK(result.error == ProjectError::BadEngineVersion);
        CHECK(result.message == "missing required key \"engineVersion\"");
    }
    {
        const ProjectParseResult result = parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": 5})");
        CHECK(result.error == ProjectError::BadEngineVersion);
        CHECK(result.message == "\"engineVersion\" must be a string (found number)");
    }
}

TEST_CASE("project: parseProject rejects a bad or unknown \"language\" (P15/P16/P17/D5)") {
    {
        const ProjectParseResult result = parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0"})");
        CHECK(result.error == ProjectError::BadLanguage);
        CHECK(result.message == "missing required key \"language\"");
    }
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": 5})");
        CHECK(result.error == ProjectError::BadLanguage);
        CHECK(result.message == "\"language\" must be a string (found number)");
    }
    {
        // "rust" is a REJECT, never a silent fallback to "ts" (D5).
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "rust"})");
        CHECK(result.error == ProjectError::BadLanguage);
        CHECK(result.message == "unsupported language \"rust\" (expected \"ts\" or \"cpp\")");
    }
}

TEST_CASE("project: parseProject rejects a bad \"paths\" object (P18/P19/AC-4)") {
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts"})");
        CHECK(result.error == ProjectError::BadPaths);
        CHECK(result.message == "missing required key \"paths\"");
    }
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts", "paths": 5})");
        CHECK(result.error == ProjectError::BadPaths);
        CHECK(result.message == "\"paths\" must be an object (found number)");
    }
}

TEST_CASE("project: parseProject rejects illegal relative paths (P20-P24/E5)") {
    // absent
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
        CHECK(result.message == "missing required key \"paths.assets\"");
    }
    // non-string
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"assets": 5, "scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
        CHECK(result.message == "\"paths.assets\" must be a string (found number)");
    }
    // empty
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"assets": "", "scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
    }
    // absolute
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"assets": "/x", "scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
    }
    // backslash
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"assets": "a\\b", "scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
    }
    // ".." segment
    {
        const ProjectParseResult result =
            parseProject(R"({"version": 1, "name": "MyGame", "engineVersion": "0.1.0", "language": "ts",)"
                         R"( "paths": {"assets": "../shared", "scenes": "scenes"}})");
        CHECK(result.error == ProjectError::BadRelativePath);
        CHECK(result.message ==
              "\"paths.assets\" must be a non-empty relative path with no leading '/', no '\\', no ':' and no "
              "\"..\" segment (found \"../shared\")");
    }
}

// ---- P25-P32: validateProjectName --------------------------------------------------------------

TEST_CASE("project: validateProjectName -- one case per NameProblem (P25-P31/D6)") {
    CHECK(validateProjectName("MyGame") == NameProblem::Ok);
    CHECK(validateProjectName("") == NameProblem::Empty);
    CHECK(validateProjectName("   ") == NameProblem::Empty);  // whitespace-only, after trimming
    CHECK(validateProjectName(std::string(65, 'a')) == NameProblem::TooLong);
    CHECK(validateProjectName(std::string(64, 'a')) == NameProblem::Ok);  // exactly the limit is fine
    CHECK(validateProjectName("a/b") == NameProblem::Separator);
    CHECK(validateProjectName("a\\b") == NameProblem::Separator);
    CHECK(validateProjectName(".") == NameProblem::DotName);
    CHECK(validateProjectName("..") == NameProblem::DotName);
    CHECK(validateProjectName("a<b") == NameProblem::IllegalChar);
    CHECK(validateProjectName("a\x01"
                              "b") == NameProblem::IllegalChar);
    CHECK(validateProjectName("Foo ") == NameProblem::TrailingSpaceOrDot);
    CHECK(validateProjectName("Foo.") == NameProblem::TrailingSpaceOrDot);
    CHECK(validateProjectName("CON") == NameProblem::ReservedDeviceName);
}

TEST_CASE("project: validateProjectName rejects every reserved DOS device name (P32/S4/E9)") {
    constexpr std::array<std::string_view, 22> RESERVED{{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    }};
    for (const std::string_view name : RESERVED) {
        CHECK(validateProjectName(name) == NameProblem::ReservedDeviceName);
    }
    CHECK(validateProjectName("con") == NameProblem::ReservedDeviceName);
    CHECK(validateProjectName("Con.txt") == NameProblem::ReservedDeviceName);
    CHECK(validateProjectName("LPT3.a.b") == NameProblem::ReservedDeviceName);
    // A non-reserved name that merely starts the same way is untouched.
    CHECK(validateProjectName("Console") == NameProblem::Ok);
}

TEST_CASE("project: nameProblemMessage is non-empty, distinct and specific (AC-12)") {
    constexpr std::array<NameProblem, 8> ALL{{
        NameProblem::Ok,
        NameProblem::Empty,
        NameProblem::TooLong,
        NameProblem::Separator,
        NameProblem::DotName,
        NameProblem::IllegalChar,
        NameProblem::TrailingSpaceOrDot,
        NameProblem::ReservedDeviceName,
    }};
    std::vector<std::string> messages;
    for (const NameProblem problem : ALL) {
        const std::string_view message = nameProblemMessage(problem);
        CHECK_FALSE(message.empty());
        CHECK(message.find("invalid name") == std::string_view::npos);
        messages.emplace_back(message);
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        for (std::size_t j = i + 1; j < messages.size(); ++j) {
            CHECK(messages[i] != messages[j]);
        }
    }
}

TEST_CASE("project: isLegalRelativePath (P33/P34/A19)") {
    CHECK(isLegalRelativePath("assets"));
    CHECK(isLegalRelativePath("content/art"));
    CHECK_FALSE(isLegalRelativePath(""));
    CHECK_FALSE(isLegalRelativePath("/shared"));  // is_absolute() would MISS this on Windows -- A19
    CHECK_FALSE(isLegalRelativePath("C:/x"));     // a Windows drive-rooted path
    CHECK_FALSE(isLegalRelativePath("a\\b"));
    CHECK_FALSE(isLegalRelativePath("../shared"));
    CHECK_FALSE(isLegalRelativePath("a/../b"));
    CHECK(isLegalRelativePath("a/./b"));  // a '.' segment is legal -- only ".." is forbidden
}

// ---- P37-P42: writeProjectText -----------------------------------------------------------------

TEST_CASE("project: writeProjectText emits the canonical bytes and key order (P37-P40/AC-1/AC-2)") {
    ProjectManifest manifest;
    manifest.name = "MyGame";
    manifest.engineVersion = "0.1.0";
    manifest.language = ProjectLanguage::Ts;
    manifest.assetsPath = "assets";
    manifest.scenesPath = "scenes";

    const std::string expected =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\"\n"
        "  }\n"
        "}\n";
    const std::string actual = writeProjectText(manifest);
    INFO(scene_golden::describeMismatch(expected, actual));
    CHECK(actual == expected);
    CHECK(scene_golden::hygieneComplaint(actual).empty());
}

TEST_CASE("project: writeProjectText escapes and passes UTF-8 through (P41/P42)") {
    ProjectManifest manifest;
    // '%', '"', '\', a C0 byte, then 2/3/4-byte UTF-8 -- adjacent-string-concatenation splits the
    // \x01 escape from the following \xC3 so neither can splice into the other (the hex-escape trap).
    manifest.name =
        "50% \"Q\" \\ \x01"
        "\xC3\xA9\xE2\x98\x83\xF0\x9F\x9A\x80";
    manifest.engineVersion = "0.1.0";
    manifest.language = ProjectLanguage::Ts;
    manifest.assetsPath = "assets";
    manifest.scenesPath = "scenes";

    const std::string actual = writeProjectText(manifest);
    // '%' needs no JSON escape; '"', '\' and the C0 byte do; the multi-byte UTF-8 passes through
    // RAW (json_writer.cpp:83-89 -- only bytes < 0x20 are escaped).
    CHECK(actual.find("50% \\\"Q\\\" \\\\ \\u0001") != std::string::npos);
    CHECK(actual.find("\xC3\xA9\xE2\x98\x83\xF0\x9F\x9A\x80") != std::string::npos);
    CHECK(actual.find("\\u00c3") == std::string::npos);  // the UTF-8 bytes are NEVER \u-escaped

    const ProjectParseResult reparsed = parseProject(actual);
    REQUIRE(reparsed.manifest.has_value());
    CHECK(reparsed.manifest->name == manifest.name);
}

TEST_CASE("project: writeProjectText(parseProject(text)) is a byte fixpoint (AC-3)") {
    const std::string canonical =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"MyGame\",\n"
        "  \"engineVersion\": \"0.1.0\",\n"
        "  \"language\": \"ts\",\n"
        "  \"paths\": {\n"
        "    \"assets\": \"assets\",\n"
        "    \"scenes\": \"scenes\"\n"
        "  }\n"
        "}\n";
    const ProjectParseResult parsed = parseProject(canonical);
    REQUIRE(parsed.manifest.has_value());
    CHECK(writeProjectText(*parsed.manifest) == canonical);

    // Idempotent over NON-canonical text too: two write cycles converge and stay converged.
    const std::string nonCanonical = R"({"version":1,"name":"MyGame","engineVersion":"0.1.0",)"
                                     R"("language":"ts","paths":{"assets":"assets","scenes":"scenes"}})";
    const ProjectParseResult parsedCompact = parseProject(nonCanonical);
    REQUIRE(parsedCompact.manifest.has_value());
    const std::string cycle1 = writeProjectText(*parsedCompact.manifest);
    const ProjectParseResult reparsed = parseProject(cycle1);
    REQUIRE(reparsed.manifest.has_value());
    const std::string cycle2 = writeProjectText(*reparsed.manifest);
    CHECK(cycle1 == cycle2);
    CHECK(cycle1 == canonical);
}

// ---- P43-P54: promoteRecent + the recents envelope ---------------------------------------------

TEST_CASE("project: promoteRecent dedups, moves to front and caps at 10 (P43-P47/AC-22/S5)") {
    RecentProjects recents;
    promoteRecent(recents, "/a");
    promoteRecent(recents, "/b");
    promoteRecent(recents, "/c");
    CHECK(recents.paths == std::vector<std::string>{"/c", "/b", "/a"});

    // Re-promoting an existing entry DEDUPS and MOVES it to the front, not appends a duplicate.
    promoteRecent(recents, "/a");
    CHECK(recents.paths == std::vector<std::string>{"/a", "/c", "/b"});
    CHECK(recents.paths.size() == 3);

    // The cap evicts the OLDEST (the back), not merely truncates arbitrarily.
    RecentProjects capped;
    for (int i = 0; i < 12; ++i) {
        promoteRecent(capped, "/p" + std::to_string(i));
    }
    CHECK(capped.paths.size() == engine::editor::MAX_RECENT_PROJECTS);
    CHECK(capped.paths.front() == "/p11");
    CHECK(capped.paths.back() == "/p2");  // /p0 and /p1 were evicted

    // An empty incoming path is a no-op.
    RecentProjects untouched;
    promoteRecent(untouched, "/only");
    promoteRecent(untouched, "");
    CHECK(untouched.paths == std::vector<std::string>{"/only"});
}

TEST_CASE("project: promoteRecent normalizes a trailing separator but does not case-fold (P48/D9)") {
    RecentProjects recents;
    promoteRecent(recents, "/a/b/");
    REQUIRE(recents.paths.size() == 1);
    CHECK(recents.paths[0] == "/a/b");  // the trailing separator was stripped

    promoteRecent(recents, "/a/b");  // same, unseparated -- dedups against the stripped entry
    CHECK(recents.paths.size() == 1);

    // No case folding: a different-case path is a DIFFERENT entry.
    promoteRecent(recents, "/A/b");
    CHECK(recents.paths.size() == 2);
}

TEST_CASE("project: recent-projects text round-trips (P49/P50/AC-22)") {
    RecentProjects recents;
    recents.paths = {"/proj/one", "/proj/two"};
    const std::string text = writeRecentProjectsText(recents);
    bool warn = false;
    const RecentProjects parsed = parseRecentProjects(text, warn);
    CHECK_FALSE(warn);
    CHECK(parsed.paths == recents.paths);

    // An empty list round-trips too.
    const RecentProjects empty;
    bool warnEmpty = false;
    const RecentProjects parsedEmpty = parseRecentProjects(writeRecentProjectsText(empty), warnEmpty);
    CHECK_FALSE(warnEmpty);
    CHECK(parsedEmpty.paths.empty());
}

TEST_CASE(
    "project: parseRecentProjects tolerates corrupt, wrong-version and malformed files "
    "(P51-P54/AC-23/S16/E13/E14)") {
    bool warn = false;

    warn = false;
    CHECK(parseRecentProjects("not json", warn).paths.empty());
    CHECK(warn);

    warn = false;
    CHECK(parseRecentProjects(R"({"version": 2, "projects": ["/a"]})", warn).paths.empty());
    CHECK(warn);

    warn = false;
    CHECK(parseRecentProjects(R"({"version": 1})", warn).paths.empty());
    CHECK(warn);

    warn = false;
    CHECK(parseRecentProjects(R"({"version": 1, "projects": "nope"})", warn).paths.empty());
    CHECK(warn);

    // A non-string element is skipped; the REST of the list is kept -- a corrupt row must not cost
    // the whole list.
    warn = false;
    const RecentProjects mixed = parseRecentProjects(R"({"version": 1, "projects": ["/a", 5, "/b"]})", warn);
    CHECK(warn);
    CHECK(mixed.paths == std::vector<std::string>{"/a", "/b"});

    // A 500-entry list parses cleanly (E13) -- capping happens on the next promote, never on parse.
    std::string bigText = R"({"version": 1, "projects": [)";
    for (int i = 0; i < 500; ++i) {
        if (i != 0) {
            bigText += ',';
        }
        bigText += '"';
        bigText += "/p" + std::to_string(i);
        bigText += '"';
    }
    bigText += "]}";
    warn = false;
    const RecentProjects big = parseRecentProjects(bigText, warn);
    CHECK_FALSE(warn);
    CHECK(big.paths.size() == 500);
}

// ---- P63-P68: ProjectSession --------------------------------------------------------------------

TEST_CASE("project: ProjectSession defaults, joins, nested paths and close (P63-P68/E6)") {
    ProjectSession session;
    CHECK_FALSE(session.isOpen());
    CHECK(session.root().empty());
    CHECK(session.name().empty());
    CHECK(session.assetsRoot().empty());
    CHECK(session.scenesRoot().empty());
    CHECK(session.manifestPath().empty());

    ProjectManifest manifest;
    manifest.name = "MyGame";
    manifest.engineVersion = "0.1.0";
    manifest.language = ProjectLanguage::Ts;
    manifest.assetsPath = "assets";
    manifest.scenesPath = "scenes";
    session.set(manifest, "/Games/MyGame");

    CHECK(session.isOpen());
    CHECK(session.root() == "/Games/MyGame");
    CHECK(session.name() == "MyGame");
    CHECK(session.assetsRoot() == "/Games/MyGame/assets");
    CHECK(session.scenesRoot() == "/Games/MyGame/scenes");
    CHECK(session.manifestPath() == "/Games/MyGame/project.json");

    // Nested paths join correctly too.
    ProjectManifest nested = manifest;
    nested.assetsPath = "content/art";
    nested.scenesPath = "content/levels";
    session.set(nested, "/Games/MyGame");
    CHECK(session.assetsRoot() == "/Games/MyGame/content/art");
    CHECK(session.scenesRoot() == "/Games/MyGame/content/levels");

    // E6: a trailing separator in the stored path normalizes away at the join.
    ProjectManifest trailing = manifest;
    trailing.assetsPath = "assets/";
    session.set(trailing, "/Games/MyGame");
    CHECK(session.assetsRoot() == "/Games/MyGame/assets");

    session.close();
    CHECK_FALSE(session.isOpen());
    CHECK(session.name().empty());
    CHECK(session.assetsRoot().empty());
}
