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
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/scene/world.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::applyDialogResult;
using engine::editor::applyFileRequests;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::ConfirmChoice;
using engine::editor::createAndOpenProject;
using engine::editor::CreateProblem;
using engine::editor::createProject;
using engine::editor::DialogKind;
using engine::editor::DialogResult;
using engine::editor::directoryExists;
using engine::editor::directoryIsEmpty;
using engine::editor::discardsWork;
using engine::editor::FileAction;
using engine::editor::FileDialogHost;
using engine::editor::fileExists;
using engine::editor::FileFlow;
using engine::editor::FileReadResult;
using engine::editor::FileStep;
using engine::editor::guardFor;
using engine::editor::isLegalRelativePath;
using engine::editor::loadProjectFrom;
using engine::editor::NameProblem;
using engine::editor::nameProblemMessage;
using engine::editor::openProjectPath;
using engine::editor::parseProject;
using engine::editor::parseRecentProjects;
using engine::editor::ProjectContext;
using engine::editor::ProjectCreateOutcome;
using engine::editor::ProjectError;
using engine::editor::ProjectFlow;
using engine::editor::ProjectLanguage;
using engine::editor::ProjectLoadOutcome;
using engine::editor::ProjectManifest;
using engine::editor::ProjectParseResult;
using engine::editor::projectRootFromPath;
using engine::editor::ProjectSession;
using engine::editor::promoteRecent;
using engine::editor::readRecentProjects;
using engine::editor::readTextFile;
using engine::editor::RecentProjects;
using engine::editor::RootOrder;
using engine::editor::SceneSession;
using engine::editor::Selection;
using engine::editor::validateProjectName;
using engine::editor::writeProjectText;
using engine::editor::writeRecentProjects;
using engine::editor::writeRecentProjectsText;
using engine::editor::writeTextFileAtomic;

namespace {

// Count by LEVEL, never records.size() (the scene_session_test.cpp / command_stack_test.cpp
// precedent): AERO_LOG_DEBUG is compiled out under NDEBUG, so counting by level makes an assertion
// identical on both presets.
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

// A unique temp directory that removes itself (and its contents) on destruction -- the SIXTH
// TU-local copy of this shape (tests/vfs_test.cpp, tests/editor/project_files_test.cpp,
// tests/editor/scene_session_test.cpp, ...); scaffolding is copied, the ASSERTION is shared (F14).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_project_test_" + std::to_string(++counter));
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

// The scene_session_test.cpp FlowFixture shape, applied here so the project flow's own cases (43-53)
// need no scene_session_test.cpp coupling. `host.channel == nullptr`: the tier-0 seam (A17).
struct FlowFixture {
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

    void makeDirty() {
        const engine::Entity a = world.create();
        REQUIRE(commands.push(ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{a}, std::vector<engine::Entity>{})));
    }
};

}  // namespace

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

// ---- P35/P36: projectRootFromPath ----------------------------------------------------------------

TEST_CASE("project: projectRootFromPath accepts a directory or a project.json (P35/P36/AC-17/S17)") {
    CHECK(projectRootFromPath("") == "");
    CHECK(projectRootFromPath("/a/b") == "/a/b");
    CHECK(projectRootFromPath("/a/b/") == "/a/b");
    CHECK(projectRootFromPath("/a/b//") == "/a/b");
    CHECK(projectRootFromPath("/a/b/project.json") == "/a/b");
    CHECK(projectRootFromPath("/a/b/project.json/") == "/a/b");
    CHECK(projectRootFromPath("/a/b/./project.json") == "/a/b");
    CHECK(projectRootFromPath("/a/b/c/../project.json") == "/a/b");
    CHECK(projectRootFromPath("/") == "/");
    CHECK(projectRootFromPath("/project.json") == "/");
    CHECK(projectRootFromPath("relative/x") == "relative/x");
    CHECK(projectRootFromPath("/a/b/.") == "/a/b");
    CHECK(projectRootFromPath("/a/b/c/..") == "/a/b");
    CHECK(projectRootFromPath("/a/b/Project.json") == "/a/b/Project.json");  // NO case folding -- D9
}

// ---- loadProjectFrom ------------------------------------------------------------------------------

TEST_CASE("project: loadProjectFrom reads, validates and reports (AC-4/AC-8)") {
    const TempDir dir;
    ProjectManifest manifest;
    manifest.name = "MyGame";
    manifest.engineVersion = "0.1.0";
    manifest.language = ProjectLanguage::Ts;
    REQUIRE(writeTextFileAtomic(dir.join("project.json"), writeProjectText(manifest)).empty());

    {
        const ProjectLoadOutcome outcome = loadProjectFrom(dir.utf8());
        REQUIRE(outcome.ok);
        CHECK(outcome.manifest.name == "MyGame");
        CHECK(outcome.root == dir.utf8());
    }
    {
        // Also accepts a direct path to project.json.
        const ProjectLoadOutcome outcome = loadProjectFrom(dir.join("project.json"));
        REQUIRE(outcome.ok);
        CHECK(outcome.manifest.name == "MyGame");
    }
    {
        // A missing file.
        const TempDir emptyDir;
        const ProjectLoadOutcome outcome = loadProjectFrom(emptyDir.utf8());
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == ProjectError::Unreadable);
    }
    {
        // E1: project.json is a DIRECTORY.
        const TempDir dirAsFile;
        std::error_code ec;
        std::filesystem::create_directory(std::filesystem::path(dirAsFile.utf8()) / "project.json", ec);
        REQUIRE_FALSE(ec);
        const ProjectLoadOutcome outcome = loadProjectFrom(dirAsFile.utf8());
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == ProjectError::Unreadable);
        CHECK(outcome.message == "path is a directory");
    }
    {
        // A v2 file.
        const TempDir v2Dir;
        REQUIRE(writeTextFileAtomic(v2Dir.join("project.json"), R"({"version": 2})").empty());
        const ProjectLoadOutcome outcome = loadProjectFrom(v2Dir.utf8());
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == ProjectError::UnsupportedVersion);
    }
}

// ---- P55-P62: createProject ------------------------------------------------------------------------

TEST_CASE("project: createProject scaffolds and writes the exact bytes (P55-P57/AC-9/AC-10/AC-11)") {
    const TempDir dir;
    const ProjectCreateOutcome outcome = createProject(dir.utf8(), "MyGame", "0.1.0");
    REQUIRE(outcome.problem == CreateProblem::Ok);
    CHECK(directoryExists(outcome.root));
    CHECK(directoryExists(outcome.root + "/assets"));
    CHECK(directoryExists(outcome.root + "/scenes"));

    const FileReadResult written = readTextFile(outcome.root + "/project.json");
    REQUIRE(written.text.has_value());
    CHECK(*written.text == writeProjectText(outcome.manifest));

    const ProjectParseResult reparsed = parseProject(*written.text);
    REQUIRE(reparsed.manifest.has_value());
    CHECK(reparsed.manifest->name == "MyGame");
    CHECK(reparsed.manifest->engineVersion == "0.1.0");
    CHECK(reparsed.manifest->language == ProjectLanguage::Ts);
}

TEST_CASE("project: createProject adopts an existing EMPTY directory (P58/E7/AC-13)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directory(std::filesystem::path(dir.utf8()) / "MyGame", ec);
    REQUIRE_FALSE(ec);

    const ProjectCreateOutcome outcome = createProject(dir.utf8(), "MyGame", "0.1.0");
    CHECK(outcome.problem == CreateProblem::Ok);
    CHECK(directoryExists(outcome.root + "/assets"));
    CHECK(directoryExists(outcome.root + "/scenes"));
}

TEST_CASE("project: createProject refuses a non-empty directory and a file target (P59/P60/E8/AC-13)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directory(std::filesystem::path(dir.utf8()) / "NonEmpty", ec);
    REQUIRE_FALSE(ec);
    REQUIRE(writeTextFileAtomic(dir.join("NonEmpty/keep.txt"), "x").empty());

    const ProjectCreateOutcome nonEmptyOutcome = createProject(dir.utf8(), "NonEmpty", "0.1.0");
    CHECK(nonEmptyOutcome.problem == CreateProblem::TargetNotEmpty);
    CHECK_FALSE(nonEmptyOutcome.message.empty());

    REQUIRE(writeTextFileAtomic(dir.join("AFile"), "x").empty());
    const ProjectCreateOutcome fileOutcome = createProject(dir.utf8(), "AFile", "0.1.0");
    CHECK(fileOutcome.problem == CreateProblem::TargetIsFile);
    CHECK_FALSE(fileOutcome.message.empty());
    CHECK(fileOutcome.message != nonEmptyOutcome.message);  // two DISTINCT messages
}

TEST_CASE("project: createProject refuses a bad name and a missing location (P61/P62/AC-12)") {
    const TempDir dir;
    const ProjectCreateOutcome badName = createProject(dir.utf8(), "CON", "0.1.0");
    CHECK(badName.problem == CreateProblem::BadName);
    CHECK(badName.message == nameProblemMessage(NameProblem::ReservedDeviceName));

    const ProjectCreateOutcome missingLocation = createProject(dir.join("does-not-exist"), "MyGame", "0.1.0");
    CHECK(missingLocation.problem == CreateProblem::LocationMissing);

    const ProjectCreateOutcome emptyLocation = createProject("", "MyGame", "0.1.0");
    CHECK(emptyLocation.problem == CreateProblem::BadLocation);
}

// PLAN DEVIATION, recorded here and in the engineering log: the plan's own suggested seeds for this
// case -- a "project.json" directory pre-created inside the target, or the same for
// "project.json.aero-tmp" -- were TRIED FIRST and BOTH measured to redden the WRONG assertion: any
// pre-existing entry inside `target`, whatever its name, makes createProject's step-3 adoption check
// (`directoryIsEmpty`) see a NON-empty directory and refuse with TargetNotEmpty *before* step 4 ever
// runs -- so `WriteFailed` (E11: "the write fails AFTER the dirs exist") is structurally unreachable
// by pre-seeding *anything* inside `target`, because step 3's gate fires on ANY entry, not
// specifically on "project.json". Measured directly: both seeds produced
// `outcome.problem == CreateProblem::TargetNotEmpty`, never `WriteFailed`. This is the plan's own
// step order (S Step 3b) turned against its own step 3d test recipe, not an implementation bug --
// project_file.cpp matches the algorithm exactly as specified.
//
// The nearest REACHABLE proof of D7's "nothing is ever removed" invariant on a LATER-stage failure
// is CreateFailed at the "assets" step, forced by making an EMPTY, ADOPTABLE target read-only: step 3
// sees zero entries (permissions do not affect directory_iterator's entry count) and adopts it, then
// step 4's create_directory(target/"assets") fails for lack of write permission. Verified locally
// (non-root): create_directory under a chmod'd read-only parent returns ec == EACCES.
TEST_CASE("project: createProject deletes NOTHING when a later step fails (E11/D7/S11, adjusted -- see comment)") {
    const TempDir dir;
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::path(dir.utf8()) / "MyGame";
    std::filesystem::create_directory(target, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const ProjectCreateOutcome outcome = createProject(dir.utf8(), "MyGame", "0.1.0");

    // Restore permissions BEFORE any assertion can throw/fail loudly and BEFORE ~TempDir runs, or
    // cleanup itself fails (the project_files_test.cpp precedent for the identical hazard).
    std::error_code restoreEc;
    std::filesystem::permissions(target, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 restoreEc);

    if (outcome.problem == CreateProblem::Ok) {
        // Running as a user for whom the read-only bit does not block writes (e.g. root in some CI
        // containers) -- the seed did not land. Recorded rather than silently passed (the standing
        // "verify the seed landed" rule): this environment cannot discriminate this case.
        WARN("createProject: the read-only-target seed did not block a write on this account; skipping");
        return;
    }
    CHECK(outcome.problem == CreateProblem::CreateFailed);
    CHECK_FALSE(outcome.message.empty());

    // D7: NOTHING was removed. The target itself still exists (still empty -- "assets" never made it
    // in), and the project was not opened.
    CHECK(directoryExists(dir.join("MyGame")));
    CHECK(directoryIsEmpty(dir.join("MyGame")));
}

TEST_CASE("project: directoryExists / directoryIsEmpty agree with the filesystem (A8)") {
    const TempDir dir;
    CHECK(directoryExists(dir.utf8()));
    CHECK(directoryIsEmpty(dir.utf8()));

    REQUIRE(writeTextFileAtomic(dir.join("f.txt"), "x").empty());
    CHECK_FALSE(directoryIsEmpty(dir.utf8()));

    CHECK_FALSE(directoryExists(dir.join("does-not-exist")));
    CHECK_FALSE(directoryIsEmpty(dir.join("does-not-exist")));

    // A file is neither a directory nor "empty" in the directory sense.
    CHECK_FALSE(directoryExists(dir.join("f.txt")));
    CHECK_FALSE(directoryIsEmpty(dir.join("f.txt")));
}

// ---- readRecentProjects / writeRecentProjects --------------------------------------------------

TEST_CASE("project: readRecentProjects on a missing file is an empty list, silently (AC-23)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();

    const TempDir dir;
    const RecentProjects recents = readRecentProjects(dir.join("does-not-exist.json"));
    CHECK(recents.paths.empty());

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("project: writeRecentProjects round-trips atomically and leaves no temp behind (AC-22/AC-24)") {
    const TempDir dir;
    RecentProjects recents;
    recents.paths = {"/proj/one", "/proj/two"};
    const std::string path = dir.join("recent_projects.json");
    writeRecentProjects(path, recents);

    CHECK(fileExists(path));
    CHECK_FALSE(fileExists(path + ".aero-tmp"));

    const RecentProjects readBack = readRecentProjects(path);
    CHECK(readBack.paths == recents.paths);

    // A write failure (into a non-existent directory) is one WARN; nothing crashes.
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    scope.sink()->take(records);
    records.clear();
    writeRecentProjects(dir.join("missing-subdir/recent_projects.json"), recents);
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);
}

// ---- PG1-PG9: the three committed golden fixtures --------------------------------------------------

namespace {

constexpr std::string_view PROJECT_MINIMAL = AERO_PROJECT_FIXTURES_DIR "/minimal.project.json";
constexpr std::string_view PROJECT_FULL = AERO_PROJECT_FIXTURES_DIR "/full.project.json";
constexpr std::string_view PROJECT_UNKNOWN = AERO_PROJECT_FIXTURES_DIR "/unknown-keys.project.json";

// name, path, is-a-fixpoint -- unknown-keys is deliberately NOT one (it loses its three unknown keys
// on save; PG8 is the case that pins exactly which three).
struct ProjectFixture {
    std::string_view name;
    std::string_view path;
    bool fixpoint;
};
constexpr std::array<ProjectFixture, 3> PROJECT_FIXTURES{{
    {"minimal", PROJECT_MINIMAL, true},
    {"full", PROJECT_FULL, true},
    {"unknown-keys", PROJECT_UNKNOWN, false},
}};

}  // namespace

TEST_CASE("project_golden: the three fixtures resolve, are non-empty and hygienic (PG1/AC-41)") {
    for (const ProjectFixture& fixture : PROJECT_FIXTURES) {
        const scene_golden::FileBytes file = scene_golden::readBytes(fixture.path);
        REQUIRE_MESSAGE(file.ok, file.error);  // never a skip
        CHECK_FALSE(file.text.empty());
        CHECK(scene_golden::hygieneComplaint(file.text).empty());
    }
}

TEST_CASE("project_golden: minimal and full are exact byte fixpoints (PG2/AC-3/AC-41/S8)") {
    for (const ProjectFixture& fixture : PROJECT_FIXTURES) {
        if (!fixture.fixpoint) {
            continue;
        }
        const scene_golden::FileBytes file = scene_golden::readBytes(fixture.path);
        REQUIRE(file.ok);
        const ProjectParseResult parsed = parseProject(file.text);
        REQUIRE(parsed.manifest.has_value());
        const std::string written = writeProjectText(*parsed.manifest);
        INFO(scene_golden::describeMismatch(file.text, written));
        CHECK(written == file.text);
    }
}

TEST_CASE("project_golden: a second write cycle is byte-identical (PG3/AC-3)") {
    for (const ProjectFixture& fixture : PROJECT_FIXTURES) {
        if (!fixture.fixpoint) {
            continue;
        }
        const scene_golden::FileBytes file = scene_golden::readBytes(fixture.path);
        REQUIRE(file.ok);
        const ProjectParseResult parsed1 = parseProject(file.text);
        REQUIRE(parsed1.manifest.has_value());
        const std::string cycle1 = writeProjectText(*parsed1.manifest);
        const ProjectParseResult parsed2 = parseProject(cycle1);
        REQUIRE(parsed2.manifest.has_value());
        const std::string cycle2 = writeProjectText(*parsed2.manifest);
        CHECK(cycle2 == cycle1);
    }
}

TEST_CASE("project_golden: minimal.project.json is the exact bytes New Project writes (PG4/§4.5)") {
    ProjectManifest manifest;
    manifest.name = "MyGame";
    manifest.engineVersion = "0.1.0";
    manifest.language = ProjectLanguage::Ts;
    manifest.assetsPath = "assets";
    manifest.scenesPath = "scenes";
    const std::string written = writeProjectText(manifest);

    const scene_golden::FileBytes file = scene_golden::readBytes(PROJECT_MINIMAL);
    REQUIRE(file.ok);
    INFO(scene_golden::describeMismatch(file.text, written));
    CHECK(written == file.text);
}

// PG5-PG7 must NEVER be deleted as redundant with PG2-PG4. That is the exact hole 2.5.2's S12 proved
// open: a parseProject/writeProjectText pair that BOTH stopped handling a key would agree with each
// other, and a regenerated fixture would make the byte comparison pass. Seed S9 re-proves it for this
// format. These three cases read the FIXTURE directly, never a re-derived string.
TEST_CASE("project_golden: minimal's parsed model, field by field (PG5/AC-43/S9)") {
    const scene_golden::FileBytes file = scene_golden::readBytes(PROJECT_MINIMAL);
    REQUIRE(file.ok);
    const ProjectParseResult parsed = parseProject(file.text);
    REQUIRE(parsed.manifest.has_value());
    CHECK(parsed.manifest->name == "MyGame");
    CHECK(parsed.manifest->engineVersion == "0.1.0");
    CHECK(parsed.manifest->language == ProjectLanguage::Ts);
    CHECK(parsed.manifest->assetsPath == "assets");
    CHECK(parsed.manifest->scenesPath == "scenes");
    CHECK(parsed.unknownKeys.empty());
}

TEST_CASE("project_golden: full's parsed model -- nested paths, Cpp, the non-ASCII name (PG6/AC-43/S9/E29)") {
    const scene_golden::FileBytes file = scene_golden::readBytes(PROJECT_FULL);
    REQUIRE(file.ok);
    const ProjectParseResult parsed = parseProject(file.text);
    REQUIRE(parsed.manifest.has_value());
    CHECK(parsed.manifest->assetsPath == "content/art");
    CHECK(parsed.manifest->scenesPath == "content/levels");
    CHECK(parsed.manifest->language == ProjectLanguage::Cpp);
    CHECK(parsed.manifest->name == "Caf\xC3\xA9 Rocket \xF0\x9F\x9A\x80");
}

TEST_CASE("project_golden: unknown-keys' collected key list, read from the fixture (PG7/AC-6/AC-43)") {
    const scene_golden::FileBytes file = scene_golden::readBytes(PROJECT_UNKNOWN);
    REQUIRE(file.ok);
    const ProjectParseResult parsed = parseProject(file.text);
    REQUIRE(parsed.manifest.has_value());
    const std::vector<std::string> expected = {"prefabs", "author", "editorLayout"};
    CHECK(parsed.unknownKeys == expected);
    CHECK(parsed.manifest->name == "MyGame");
    CHECK(parsed.manifest->engineVersion == "0.1.0");
    CHECK(parsed.manifest->language == ProjectLanguage::Ts);
    CHECK(parsed.manifest->assetsPath == "assets");
    CHECK(parsed.manifest->scenesPath == "scenes");
}

TEST_CASE("project_golden: a save strips exactly the three unknown keys (PG8/AC-6)") {
    const scene_golden::FileBytes unknownFile = scene_golden::readBytes(PROJECT_UNKNOWN);
    REQUIRE(unknownFile.ok);
    const ProjectParseResult parsed = parseProject(unknownFile.text);
    REQUIRE(parsed.manifest.has_value());
    const std::string written = writeProjectText(*parsed.manifest);

    const scene_golden::FileBytes minimalFile = scene_golden::readBytes(PROJECT_MINIMAL);
    REQUIRE(minimalFile.ok);
    INFO(scene_golden::describeMismatch(minimalFile.text, written));
    CHECK(written == minimalFile.text);
}

TEST_CASE("project_golden: a created project.json on disk is a fixpoint (PG9/AC-11)") {
    const TempDir dir;
    const ProjectCreateOutcome outcome = createProject(dir.utf8(), "MyGame", "0.1.0");
    REQUIRE(outcome.problem == CreateProblem::Ok);

    const FileReadResult written = readTextFile(outcome.root + "/project.json");
    REQUIRE(written.text.has_value());
    // A Windows-lane canary: untestable HERE, load-bearing THERE (2.5.2's EG3 precedent). On macOS
    // and Linux this can never fail, because nothing here rewrites a newline; on Windows a text-mode
    // regression in text_file.cpp would flip it red. Do not delete it as untested.
    CHECK(scene_golden::hygieneComplaint(*written.text).empty());

    const ProjectParseResult reparsed = parseProject(*written.text);
    REQUIRE(reparsed.manifest.has_value());
    const std::string cycle = writeProjectText(*reparsed.manifest);
    CHECK(cycle == *written.text);
}

// ---- P69-P80: the flow, driven with a NULL dialog channel ------------------------------------------

TEST_CASE("project: windowTitle with and without a project name (P69/F11/AC-27/AC-28)") {
    SceneSession session;
    // EMPTY projectName -- byte-identical to today's four forms (F11/AC-28).
    CHECK(session.windowTitle(false) == "Untitled - Aero Editor");
    CHECK(session.windowTitle(true) == "*Untitled - Aero Editor");
    session.setPath("/tmp/level1.scene.json");
    CHECK(session.windowTitle(false) == "level1.scene.json - Aero Editor");
    CHECK(session.windowTitle(true) == "*level1.scene.json - Aero Editor");

    // WITH a project name (AC-27).
    session.clearPath();
    CHECK(session.windowTitle(false, "MyGame") == "Untitled - MyGame - Aero Editor");
    CHECK(session.windowTitle(true, "MyGame") == "*Untitled - MyGame - Aero Editor");
    session.setPath("/tmp/level1.scene.json");
    CHECK(session.windowTitle(false, "MyGame") == "level1.scene.json - MyGame - Aero Editor");
    CHECK(session.windowTitle(true, "MyGame") == "*level1.scene.json - MyGame - Aero Editor");
}

TEST_CASE("project: discardsWork and guardFor cover NewProject and OpenProject (P70/P71/AC-19/S12)") {
    CHECK(discardsWork(FileAction::NewProject));
    CHECK(discardsWork(FileAction::OpenProject));
    for (const FileAction action : {FileAction::NewProject, FileAction::OpenProject}) {
        CHECK(guardFor(action, false) == FileStep::Perform);
        CHECK(guardFor(action, true) == FileStep::Confirm);
    }
}

TEST_CASE("project: openProjectPath opens, resets the scene and promotes the recent entry (P73/AC-18/S1/S2)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    const ProjectCreateOutcome created = createProject(dir.utf8(), "MyGame", "0.1.0");
    REQUIRE(created.problem == CreateProblem::Ok);

    FlowFixture f;
    SceneSession session;
    // Start DIRTY, with an entity beyond the (not-yet-seeded) default -- a stack that starts clean
    // stays clean whether or not the code actually clears it (the scene_io_test.cpp:383-391 idiom).
    const engine::Entity extra = f.world.create();
    REQUIRE(f.commands.push(f.ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{extra}, std::vector<engine::Entity>{})));
    REQUIRE_FALSE(f.commands.isClean());
    session.setPath("/tmp/some-other-scene.scene.json");

    scope.sink()->take(records);
    records.clear();

    const bool ok = openProjectPath(f.ctx, f.commands, session, f.project, created.root);

    CHECK(ok);
    CHECK(f.world.entityCount() == 3);  // the fresh default scene (AC-18)
    CHECK(f.commands.isClean());
    CHECK(f.commands.count() == 0);
    CHECK(session.path().empty());
    CHECK(f.projectSession.isOpen());
    CHECK(f.projectSession.name() == "MyGame");
    CHECK(f.recents.paths.size() == 1);
    CHECK(f.projectFlow.recentsDirty);

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("project: a REJECTED project changes nothing (P74/INV-P3/AC-8/S13)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    REQUIRE(writeTextFileAtomic(dir.join("project.json"), R"({"version": 2})").empty());

    FlowFixture f;
    SceneSession session;
    const engine::Entity extra = f.world.create();
    REQUIRE(f.commands.push(f.ctx, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                       std::vector<engine::Entity>{extra}, std::vector<engine::Entity>{})));
    const std::size_t entitiesBefore = f.world.entityCount();
    const std::size_t countBefore = f.commands.count();

    scope.sink()->take(records);
    records.clear();

    const bool ok = openProjectPath(f.ctx, f.commands, session, f.project, dir.utf8());

    CHECK_FALSE(ok);
    CHECK_FALSE(f.projectSession.isOpen());  // session STILL closed
    CHECK(f.world.entityCount() == entitiesBefore);
    CHECK(f.commands.count() == countBefore);
    CHECK(f.recents.paths.empty());

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 0);
}

TEST_CASE("project: createAndOpenProject scaffolds and adopts in one operation (P75/AC-9/AC-21)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    const TempDir dir;
    FlowFixture f;
    SceneSession session;
    scope.sink()->take(records);
    records.clear();

    const bool ok = createAndOpenProject(f.ctx, f.commands, session, f.project, dir.utf8(), "MyGame");

    CHECK(ok);
    CHECK(directoryExists(dir.join("MyGame")));
    CHECK(directoryExists(dir.join("MyGame/assets")));
    CHECK(directoryExists(dir.join("MyGame/scenes")));
    CHECK(f.projectSession.isOpen());
    CHECK(f.world.entityCount() == 3);
    CHECK(f.commands.isClean());

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);  // exactly ONE (A24 -- never re-reads)
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("project: the New Project form's requests never survive their frame (P76/AC-46)") {
    FlowFixture f;
    SceneSession session;
    f.project.flow.form.open = true;
    f.project.flow.form.cancelRequested = true;
    f.project.flow.form.browseRequested = true;
    f.project.flow.clearRecentsRequested = true;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
    // cancelRequested resets the WHOLE form (form = {}), so browseRequested/createRequested/open are
    // ALSO cleared by that same assignment -- verify the flags, not merely `form.open`.
    CHECK_FALSE(f.project.flow.form.open);
    CHECK_FALSE(f.project.flow.form.cancelRequested);
    CHECK_FALSE(f.project.flow.form.browseRequested);
    CHECK_FALSE(f.project.flow.clearRecentsRequested);
}

TEST_CASE("project: a create FAILURE keeps the modal open with the typed name (AC-12)") {
    FlowFixture f;
    SceneSession session;
    f.project.flow.form.open = true;
    f.project.flow.form.name = "MyGame";
    f.project.flow.form.location = "/definitely/does/not/exist";  // LocationMissing
    f.project.flow.form.createRequested = true;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.project.flow.form.open);              // the modal STAYS UP
    CHECK(f.project.flow.form.name == "MyGame");  // the typed name is INTACT
    CHECK_FALSE(f.project.flow.form.error.empty());
    CHECK_FALSE(f.projectSession.isOpen());
}

TEST_CASE("project: a project request is refused while the form or a dialog is up (P77/R2/R3/S7)") {
    // Arm 1: the New Project form is open.
    {
        FlowFixture f;
        SceneSession session;
        f.project.flow.form.open = true;
        const std::size_t entitiesBefore = f.world.entityCount();
        f.flow.requested = FileAction::NewScene;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.world.entityCount() == entitiesBefore);
        CHECK(f.flow.requested == FileAction::None);
    }
    // Arm 2: a native dialog is in flight.
    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::Open;
        f.flow.requested = FileAction::NewProject;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK_FALSE(f.project.flow.form.open);  // NewProject's own form was never opened
    }
    // Arm 3: the unsaved-changes modal is up.
    {
        FlowFixture f;
        SceneSession session;
        f.flow.confirmOpen = true;
        f.flow.pending = FileAction::Quit;
        f.flow.requested = FileAction::OpenProject;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.flow.pending == FileAction::Quit);  // the ORIGINAL pending SURVIVES
        CHECK_FALSE(f.projectSession.isOpen());
    }
}

TEST_CASE("project: projectFlow.requestedPath is cleared on every abandoning path (P78/F10)") {
    // Swallowed by a dialog already in flight.
    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::Open;
        f.project.flow.requestedPath = "/some/project";
        f.flow.requested = FileAction::OpenProject;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.project.flow.requestedPath.empty());
    }
    // A cancelled ProjectFolder dialog result.
    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::ProjectFolder;
        f.project.flow.requestedPath = "/some/project";
        DialogResult cancelled;
        cancelled.ready = true;
        cancelled.cancelled = true;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, cancelled, f.project);
        CHECK(f.project.flow.requestedPath.empty());
    }
    // A failed ProjectFolder dialog result.
    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::ProjectFolder;
        f.project.flow.requestedPath = "/some/project";
        DialogResult failed;
        failed.ready = true;
        failed.failed = true;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, failed, f.project);
        CHECK(f.project.flow.requestedPath.empty());
    }
    // Guard Cancel over a guarded OpenProject.
    {
        FlowFixture f;
        SceneSession session;
        f.makeDirty();
        f.project.flow.requestedPath = "/some/project";
        f.flow.requested = FileAction::OpenProject;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        REQUIRE(f.flow.confirmOpen);
        f.flow.choice = ConfirmChoice::Cancel;
        applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);
        CHECK(f.project.flow.requestedPath.empty());
    }
}

TEST_CASE("project: applyDialogResult -- ProjectFolder opens, ProjectLocation only fills the form (P79/E18/E19)") {
    const TempDir dir;
    const ProjectCreateOutcome created = createProject(dir.utf8(), "MyGame", "0.1.0");
    REQUIRE(created.problem == CreateProblem::Ok);

    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::ProjectFolder;
        DialogResult result;
        result.ready = true;
        result.path = created.root;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);
        CHECK(f.projectSession.isOpen());
        CHECK(f.flow.dialog == DialogKind::None);
    }
    {
        FlowFixture f;
        SceneSession session;
        f.flow.dialog = DialogKind::ProjectLocation;
        f.project.flow.form.open = true;
        DialogResult result;
        result.ready = true;
        result.path = dir.utf8();
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, result, f.project);
        CHECK(f.project.flow.form.location == dir.utf8());  // and NOTHING else
        CHECK(f.project.flow.form.open);                    // the modal stays up; Create is still needed
        CHECK_FALSE(f.projectSession.isOpen());

        // A CANCELLED ProjectLocation result is silent and leaves form.location unchanged.
        f.flow.dialog = DialogKind::ProjectLocation;
        DialogResult cancelled;
        cancelled.ready = true;
        cancelled.cancelled = true;
        applyDialogResult(f.ctx, f.commands, session, f.flow, f.host, cancelled, f.project);
        CHECK(f.project.flow.form.location == dir.utf8());
    }
}

TEST_CASE("project: clearRecentProjects empties the list and marks it dirty (P80/AC-25)") {
    FlowFixture f;
    SceneSession session;
    f.recents.paths = {"/a", "/b"};
    f.project.flow.clearRecentsRequested = true;

    applyFileRequests(f.ctx, f.commands, session, f.flow, f.host, f.project);

    CHECK(f.recents.paths.empty());
    CHECK(f.projectFlow.recentsDirty);
    CHECK_FALSE(f.project.flow.clearRecentsRequested);
}
