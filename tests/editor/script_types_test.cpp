// Aero Engine -- the project's script declarations and tsconfig.json (task 4.2.2, DT8-DT21). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING
// in all three configurations. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
//
// Paths are compared after a '\\' -> '/' normalisation and by suffix, never with == across layers.
#include <aero/core/log.hpp>
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME / LIBRARY_GITIGNORE_TEXT -- the shipped constants
#include <aero/editor/asset_meta.hpp>   // ATOMIC_TEMP_SUFFIX -- writeTextFileAtomic's own temp name
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>  // LogSinkScope / LogEntry -- the capture the project cases use
#include <aero/editor/entity_ops.hpp>     // RootOrder
#include <aero/editor/project.hpp>        // ProjectManifest / ProjectLanguage
#include <aero/editor/scene_session.hpp>  // openProjectPath / createAndOpenProject / ProjectContext
#include <aero/editor/script_types.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::editor {
// doctest streams a failing operand through an unqualified operator<< found by ADL.
std::ostream& operator<<(std::ostream& out, ScriptTypesWrite state) { return out << static_cast<unsigned>(state); }
}  // namespace engine::editor

namespace {

namespace ed = engine::editor;

// The declarations path relative to a project root: the contract DT8's template and DT11's writer must agree on.
[[nodiscard]] std::string declarationsRelative() {
    std::string relative(ed::ASSET_CACHE_DIR_NAME);
    relative += '/';
    relative += ed::SCRIPT_TYPES_DIR_NAME;
    relative += '/';
    relative += ed::SCRIPT_DECLARATIONS_FILE_NAME;
    return relative;
}

// A unique temp directory whose name carries the case id, removing itself.
class TempDir {
public:
    explicit TempDir(std::string_view caseId) {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        dirPath = base / ("aero_script_types_" + std::string(caseId));
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
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
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

[[nodiscard]] std::optional<std::string> readBytes(std::string_view path) { return ed::readTextFile(path).text; }

// Reads a file through the standard library, binary: the oracle for the two build-time files.
[[nodiscard]] std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void makeDirectories(std::string_view path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::string(path)), ec);
}

void writeBytes(std::string_view path, std::string_view bytes) {
    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool isDirectory(std::string_view path) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(std::string(path)), ec);
}

[[nodiscard]] std::size_t occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

[[nodiscard]] std::vector<std::string> stringArray(const engine::JsonValue* array) {
    std::vector<std::string> out;
    if (array == nullptr) {
        return out;
    }
    for (const engine::JsonValue& element : array->elements()) {
        out.emplace_back(element.asString().value_or("<not a string>"));
    }
    return out;
}

[[nodiscard]] ed::ProjectManifest manifestFor(ed::ProjectLanguage language, std::string assetsPath) {
    ed::ProjectManifest manifest;
    manifest.name = "demo";
    manifest.language = language;
    manifest.assetsPath = std::move(assetsPath);
    return manifest;
}

}  // namespace

TEST_CASE("script_types: the rendered tsconfig is the template with its one placeholder filled (DT8)") {
    const std::string text = ed::renderTsconfig("assets");
    const engine::JsonParseResult parsed = engine::parseJson(text);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value.has_value());
    const engine::JsonValue& root = *parsed.value;
    REQUIRE(root.isObject());

    const engine::JsonValue* options = root.find("compilerOptions");
    REQUIRE(options != nullptr);
    REQUIRE(options->isObject());
    CHECK(options->size() == 8U);
    const auto stringOption = [options](std::string_view key) {
        const engine::JsonValue* found = options->find(key);
        return found == nullptr ? std::string("<absent>") : std::string(found->asString().value_or("<not a string>"));
    };
    const auto boolOption = [options](std::string_view key) {
        const engine::JsonValue* found = options->find(key);
        return found != nullptr && found->asBool().value_or(false);
    };
    CHECK(stringOption("target") == "ES2023");
    CHECK(stringOption("module") == "ESNext");
    CHECK(stringOption("moduleResolution") == "bundler");
    CHECK(stringArray(options->find("lib")) == std::vector<std::string>{"ESNext"});
    REQUIRE(options->find("types") != nullptr);
    CHECK(options->find("types")->isArray());
    CHECK(options->find("types")->size() == 0U);
    CHECK(boolOption("strict"));
    CHECK(boolOption("noEmit"));
    CHECK(boolOption("verbatimModuleSyntax"));

    CHECK(stringArray(root.find("files")) == std::vector<std::string>{declarationsRelative()});
    CHECK(stringArray(root.find("include")) == std::vector<std::string>{"assets/**/*.ts"});
    CHECK(stringArray(root.find("exclude")) == std::vector<std::string>{std::string(ed::ASSET_CACHE_DIR_NAME)});

    CHECK(occurrences(ed::tsconfigTemplateText(), ed::TSCONFIG_INCLUDE_PLACEHOLDER) == 1U);
    CHECK(text.find(ed::TSCONFIG_INCLUDE_PLACEHOLDER) == std::string::npos);
}

TEST_CASE("script_types: the include glob is escaped, and a trailing slash or a dot renders cleanly (DT9)") {
    const std::string quoted = "we\"ird/assets";
    const std::string quotedText = ed::renderTsconfig(quoted);
    const engine::JsonParseResult parsed = engine::parseJson(quotedText);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value.has_value());
    const std::vector<std::string> include = stringArray(parsed.value->find("include"));
    REQUIRE(include.size() == 1U);
    CHECK(include[0] == quoted + "/**/*.ts");

    const std::string slashed = "game/assets/";
    const std::string slashedText = ed::renderTsconfig(slashed);
    const engine::JsonParseResult slashedParsed = engine::parseJson(slashedText);
    REQUIRE(slashedParsed.ok());
    REQUIRE(slashedParsed.value.has_value());
    CHECK(stringArray(slashedParsed.value->find("include")) == std::vector<std::string>{"game/assets/**/*.ts"});

    // Every trailing slash goes, not only the first: the manifest reader accepts "assets//".
    for (const std::string& repeated : {std::string("game/assets//"), std::string("game/assets///")}) {
        const engine::JsonParseResult repeatedParsed = engine::parseJson(ed::renderTsconfig(repeated));
        REQUIRE(repeatedParsed.ok());
        REQUIRE(repeatedParsed.value.has_value());
        CHECK(stringArray(repeatedParsed.value->find("include")) == std::vector<std::string>{"game/assets/**/*.ts"});
    }

    const std::string dot = ".";
    const std::string dotText = ed::renderTsconfig(dot);
    const engine::JsonParseResult dotParsed = engine::parseJson(dotText);
    REQUIRE(dotParsed.ok());
    REQUIRE(dotParsed.value.has_value());
    CHECK(stringArray(dotParsed.value->find("include")) == std::vector<std::string>{"./**/*.ts"});
}

TEST_CASE("script_types: the embedded texts are exactly the two files they were built from (DT10)") {
    const std::string declarations = slurp(AERO_SCRIPT_DECLARATIONS_FILE);
    const std::string templateBytes = slurp(AERO_TSCONFIG_TEMPLATE_FILE);
    REQUIRE_FALSE(declarations.empty());
    REQUIRE_FALSE(templateBytes.empty());
    CHECK(ed::scriptDeclarationsText() == declarations);
    CHECK(ed::tsconfigTemplateText() == templateBytes);
    CHECK(ed::scriptDeclarationsText() != ed::tsconfigTemplateText());
}

TEST_CASE("script_types: a fresh TypeScript project gets the declarations, the tsconfig and the ignore file (DT11)") {
    const TempDir dir("DT11");
    const ed::ScriptTypesReport report =
        ed::writeScriptTypes(dir.utf8(), manifestFor(ed::ProjectLanguage::Ts, "game/assets"));
    CHECK(report.declarations == ed::ScriptTypesWrite::Written);
    CHECK(report.tsconfig == ed::ScriptTypesWrite::Written);
    CHECK(report.declarationsError.empty());
    CHECK(report.tsconfigError.empty());
    CHECK(report.declarationsPath == dir.join(declarationsRelative()));
    CHECK(report.tsconfigPath == dir.join(ed::TSCONFIG_FILE_NAME));

    CHECK(readBytes(dir.join(declarationsRelative())) ==
          std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
    CHECK(readBytes(dir.join("tsconfig.json")) == std::optional<std::string>(ed::renderTsconfig("game/assets")));
    const std::string ignorePath =
        dir.join(std::string(ed::ASSET_CACHE_DIR_NAME) + "/" + std::string(ed::ASSET_CACHE_GITIGNORE_NAME));
    CHECK(readBytes(ignorePath) == std::optional<std::string>(std::string(ed::LIBRARY_GITIGNORE_TEXT)));
}

TEST_CASE("script_types: an unchanged project is never rewritten (DT12)") {
    const TempDir dir("DT12");
    const ed::ProjectManifest manifest = manifestFor(ed::ProjectLanguage::Ts, "assets");
    REQUIRE(ed::writeScriptTypes(dir.utf8(), manifest).declarations == ed::ScriptTypesWrite::Written);

    // A directory where each atomic write would put its temp file blocks any write at all.
    const std::string declarationsPath = dir.join(declarationsRelative());
    const std::string tsconfigPath = dir.join(ed::TSCONFIG_FILE_NAME);
    makeDirectories(declarationsPath + std::string(ed::ATOMIC_TEMP_SUFFIX));
    makeDirectories(tsconfigPath + std::string(ed::ATOMIC_TEMP_SUFFIX));

    const ed::ScriptTypesReport again = ed::writeScriptTypes(dir.utf8(), manifest);
    CHECK(again.declarations == ed::ScriptTypesWrite::Unchanged);
    CHECK(again.tsconfig == ed::ScriptTypesWrite::Unchanged);
    CHECK(again.declarationsError.empty());
    CHECK(again.tsconfigError.empty());

    // The block is real: change one byte of the declarations and the same call fails to write them.
    std::string changed(ed::scriptDeclarationsText());
    changed.push_back(' ');
    writeBytes(declarationsPath, changed);
    const ed::ScriptTypesReport blocked = ed::writeScriptTypes(dir.utf8(), manifest);
    CHECK(blocked.declarations == ed::ScriptTypesWrite::Failed);
    CHECK_FALSE(blocked.declarationsError.empty());
    CHECK(blocked.tsconfig == ed::ScriptTypesWrite::Unchanged);
}

TEST_CASE("script_types: an existing tsconfig is never touched and a stale declarations file is replaced (DT13)") {
    const TempDir dir("DT13");
    const std::string sentinel = "{ \"mine\": true }\n";
    writeBytes(dir.join("tsconfig.json"), sentinel);
    makeDirectories(dir.join(std::string(ed::ASSET_CACHE_DIR_NAME) + "/" + std::string(ed::SCRIPT_TYPES_DIR_NAME)));
    const std::string declarationsPath = dir.join(declarationsRelative());
    writeBytes(declarationsPath, "declare const stale: 1;\n");

    const ed::ScriptTypesReport report =
        ed::writeScriptTypes(dir.utf8(), manifestFor(ed::ProjectLanguage::Ts, "assets"));
    CHECK(report.tsconfig == ed::ScriptTypesWrite::Unchanged);
    CHECK(report.declarations == ed::ScriptTypesWrite::Written);
    CHECK(readBytes(dir.join("tsconfig.json")) == std::optional<std::string>(sentinel));
    CHECK(readBytes(declarationsPath) == std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
}

TEST_CASE("script_types: only a TypeScript project with a root is written (DT14)") {
    const TempDir cppDir("DT14");
    const ed::ScriptTypesReport cpp =
        ed::writeScriptTypes(cppDir.utf8(), manifestFor(ed::ProjectLanguage::Cpp, "assets"));
    CHECK(cpp.declarations == ed::ScriptTypesWrite::Skipped);
    CHECK(cpp.tsconfig == ed::ScriptTypesWrite::Skipped);
    CHECK(cpp.declarationsPath.empty());
    CHECK(cpp.tsconfigPath.empty());
    CHECK_FALSE(ed::fileExists(cppDir.join(ed::ASSET_CACHE_DIR_NAME)));
    CHECK_FALSE(ed::fileExists(cppDir.join(ed::TSCONFIG_FILE_NAME)));

    const ed::ScriptTypesReport empty = ed::writeScriptTypes("", manifestFor(ed::ProjectLanguage::Ts, "assets"));
    CHECK(empty.declarations == ed::ScriptTypesWrite::Skipped);
    CHECK(empty.tsconfig == ed::ScriptTypesWrite::Skipped);
}

TEST_CASE("script_types: one file failing never skips the other, and nothing throws (DT15)") {
    {
        const TempDir dir("DT15a");
        writeBytes(dir.join(ed::ASSET_CACHE_DIR_NAME), "a file where the cache directory belongs");
        const ed::ScriptTypesReport report =
            ed::writeScriptTypes(dir.utf8(), manifestFor(ed::ProjectLanguage::Ts, "assets"));
        CHECK(report.declarations == ed::ScriptTypesWrite::Failed);
        CHECK_FALSE(report.declarationsError.empty());
        CHECK(report.tsconfig == ed::ScriptTypesWrite::Written);
        CHECK(report.tsconfigError.empty());
        CHECK(readBytes(dir.join("tsconfig.json")) == std::optional<std::string>(ed::renderTsconfig("assets")));
    }
    {
        const TempDir dir("DT15b");
        makeDirectories(dir.join(ed::TSCONFIG_FILE_NAME));
        const ed::ScriptTypesReport report =
            ed::writeScriptTypes(dir.utf8(), manifestFor(ed::ProjectLanguage::Ts, "assets"));
        CHECK(report.tsconfig == ed::ScriptTypesWrite::Unchanged);
        CHECK(report.declarations == ed::ScriptTypesWrite::Written);
        CHECK(isDirectory(dir.join(ed::TSCONFIG_FILE_NAME)));
    }
}

TEST_CASE("script_types: an existing Library/.gitignore survives the write (DT16)") {
    const TempDir dir("DT16");
    makeDirectories(dir.join(ed::ASSET_CACHE_DIR_NAME));
    const std::string ignorePath =
        dir.join(std::string(ed::ASSET_CACHE_DIR_NAME) + "/" + std::string(ed::ASSET_CACHE_GITIGNORE_NAME));
    const std::string sentinel = "# mine\n";
    writeBytes(ignorePath, sentinel);

    const ed::ScriptTypesReport report =
        ed::writeScriptTypes(dir.utf8(), manifestFor(ed::ProjectLanguage::Ts, "assets"));
    CHECK(report.declarations == ed::ScriptTypesWrite::Written);
    CHECK(readBytes(ignorePath) == std::optional<std::string>(sentinel));
    CHECK(readBytes(dir.join(declarationsRelative())) ==
          std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
}

// ---- DT17-DT21: both adoption paths write the two files -----------------------------------------------------

namespace {

// Count by LEVEL, never records.size(): the Debug-only records are compiled out under NDEBUG.
[[nodiscard]] std::size_t countAtLevel(const std::vector<ed::LogEntry>& records, engine::LogLevel level) {
    return static_cast<std::size_t>(std::count_if(records.begin(), records.end(),
                                                  [level](const ed::LogEntry& entry) { return entry.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// project_test.cpp's FlowFixture shape, without the dirty-scene helper.
struct FlowFixture {
    engine::World world;
    ed::Selection selection;
    ed::RootOrder roots;
    ed::CommandStack commands;
    ed::CommandContext ctx{world, selection, roots};
    ed::ProjectSession projectSession;
    ed::ProjectFlow projectFlow;
    ed::RecentProjects recents;
    ed::ProjectContext project{projectSession, projectFlow, recents, ""};
};

// The source gate's helpers (the asset_actions_test.cpp shape): line comments are dropped so a comment that names a
// call is not a call.
[[nodiscard]] std::string stripLineComments(const std::string& body) {
    std::string out;
    out.reserve(body.size());
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/') {
            while (i < body.size() && body[i] != '\n') {
                ++i;
            }
            continue;
        }
        out.push_back(body[i]);
        ++i;
    }
    return out;
}

// The text from the first occurrence of `signature` to the first "\n}\n" after it: every function this gate reads
// closes at column 0.
[[nodiscard]] std::string functionBody(const std::string& body, std::string_view signature) {
    const std::size_t begin = body.find(signature);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = body.find("\n}\n", begin);
    return body.substr(begin, end == std::string::npos ? std::string::npos : end + 3 - begin);
}

[[nodiscard]] std::string readSource(const std::filesystem::path& path) {
    return stripLineComments(slurp(path.string().c_str()));
}

[[nodiscard]] ed::ProjectManifest cppManifestOf(const ed::ProjectManifest& base) {
    ed::ProjectManifest manifest = base;
    manifest.language = ed::ProjectLanguage::Cpp;
    return manifest;
}

}  // namespace

TEST_CASE("script_types: createAndOpenProject writes the declarations and the tsconfig, silently (DT17)") {
    const LogFixture fixture;
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;
    const TempDir dir("DT17");
    FlowFixture f;
    ed::SceneSession session;
    scope.sink()->take(records);
    records.clear();

    REQUIRE(ed::createAndOpenProject(f.ctx, f.commands, session, f.project, dir.utf8(), "demo"));
    REQUIRE(f.projectSession.isOpen());
    const std::string root(f.projectSession.root());

    CHECK(readBytes(root + "/" + declarationsRelative()) ==
          std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
    CHECK(readBytes(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)) ==
          std::optional<std::string>(ed::renderTsconfig("assets")));

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("script_types: openProjectPath adds both files to an older project and keeps the tsconfig (DT18)") {
    const LogFixture fixture;
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;
    const TempDir dir("DT18");
    const ed::ProjectCreateOutcome created = ed::createProject(dir.utf8(), "demo", "0.1.0");
    REQUIRE(created.problem == ed::CreateProblem::Ok);
    const std::string root = created.root;
    CHECK_FALSE(ed::fileExists(root + "/" + std::string(ed::ASSET_CACHE_DIR_NAME)));
    CHECK_FALSE(ed::fileExists(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)));

    FlowFixture f;
    ed::SceneSession session;
    scope.sink()->take(records);
    records.clear();
    REQUIRE(ed::openProjectPath(f.ctx, f.commands, session, f.project, root));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(readBytes(root + "/" + declarationsRelative()) ==
          std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
    CHECK(readBytes(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)) ==
          std::optional<std::string>(ed::renderTsconfig("assets")));

    // A second open: the project's own tsconfig is never rewritten, a stale declarations file is replaced.
    const std::string sentinel = "{ \"mine\": true }\n";
    writeBytes(root + "/" + std::string(ed::TSCONFIG_FILE_NAME), sentinel);
    writeBytes(root + "/" + declarationsRelative(), "declare const stale: 1;\n");
    FlowFixture g;
    ed::SceneSession second;
    records.clear();
    scope.sink()->take(records);  // drains what the setup logged
    records.clear();
    REQUIRE(ed::openProjectPath(g.ctx, g.commands, second, g.project, root));
    CHECK(readBytes(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)) == std::optional<std::string>(sentinel));
    CHECK(readBytes(root + "/" + declarationsRelative()) ==
          std::optional<std::string>(std::string(ed::scriptDeclarationsText())));
    // A kept tsconfig is a success (Unchanged), not a failure: the reopen stays as silent as the first open.
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Info) == 1);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
}

TEST_CASE("script_types: a C++ project is opened without either file (DT19)") {
    const LogFixture fixture;
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;
    const TempDir dir("DT19");
    const ed::ProjectCreateOutcome created = ed::createProject(dir.utf8(), "demo", "0.1.0");
    REQUIRE(created.problem == ed::CreateProblem::Ok);
    const std::string root = created.root;
    const std::string manifestPath = root + "/" + std::string(ed::PROJECT_FILE_NAME);
    REQUIRE(ed::writeTextFileAtomic(manifestPath, ed::writeProjectText(cppManifestOf(created.manifest))).empty());

    FlowFixture f;
    ed::SceneSession session;
    scope.sink()->take(records);
    records.clear();
    REQUIRE(ed::openProjectPath(f.ctx, f.commands, session, f.project, root));
    CHECK(f.projectSession.manifest().language == ed::ProjectLanguage::Cpp);
    CHECK_FALSE(ed::fileExists(root + "/" + std::string(ed::ASSET_CACHE_DIR_NAME)));
    CHECK_FALSE(ed::fileExists(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 0);
}

TEST_CASE("script_types: a failed write is one WARN that names the file, and the project still opens (DT20)") {
    const LogFixture fixture;
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;
    const TempDir dir("DT20");
    const ed::ProjectCreateOutcome created = ed::createProject(dir.utf8(), "demo", "0.1.0");
    REQUIRE(created.problem == ed::CreateProblem::Ok);
    const std::string root = created.root;
    writeBytes(root + "/" + std::string(ed::ASSET_CACHE_DIR_NAME), "a file where the cache directory belongs");

    FlowFixture f;
    ed::SceneSession session;
    scope.sink()->take(records);
    records.clear();
    REQUIRE(ed::openProjectPath(f.ctx, f.commands, session, f.project, root));
    CHECK(f.projectSession.isOpen());

    scope.sink()->take(records);
    REQUIRE(countAtLevel(records, engine::LogLevel::Warn) == 1);
    for (const ed::LogEntry& entry : records) {
        if (entry.level == engine::LogLevel::Warn) {
            CHECK(entry.message.find(ed::SCRIPT_DECLARATIONS_FILE_NAME) != std::string::npos);
        }
    }
    CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    CHECK(readBytes(root + "/" + std::string(ed::TSCONFIG_FILE_NAME)) ==
          std::optional<std::string>(ed::renderTsconfig("assets")));
}

TEST_CASE("script_types: the writer has two callers, both reached once per adoption (DT21)") {
    const std::filesystem::path src{AERO_EDITOR_SRC_DIR};
    std::size_t scanned = 0;
    std::vector<std::string> files;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(src, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".cpp") || name.ends_with(".hpp")) {
            const std::string body = readSource(entry.path());
            ++scanned;
            if (occurrences(body, "writeScriptTypes(") != 0U) {
                files.push_back(name);
            }
        }
    }
    REQUIRE(scanned > 50U);  // ANTI-VACUITY: the tree was really read
    std::sort(files.begin(), files.end());
    CHECK(files == std::vector<std::string>{"scene_session.cpp", "script_types.cpp"});

    const std::string session = readSource(src / "scene_session.cpp");
    REQUIRE(session.size() > 3000U);
    CHECK(occurrences(session, "writeScriptTypes(") == 1U);
    CHECK(occurrences(session, "adoptScriptTypes(") == 3U);

    const std::string adopter = functionBody(session, "void adoptScriptTypes(");
    REQUIRE_FALSE(adopter.empty());
    CHECK(occurrences(adopter, "writeScriptTypes(") == 1U);
    const std::string opener = functionBody(session, "bool openProjectPath(");
    REQUIRE_FALSE(opener.empty());
    CHECK(occurrences(opener, "adoptScriptTypes(") == 1U);
    const std::string creator = functionBody(session, "bool createAndOpenProject(");
    REQUIRE_FALSE(creator.empty());
    CHECK(occurrences(creator, "adoptScriptTypes(") == 1U);
    const std::string adopted = functionBody(session, "void adoptProject(");
    REQUIRE_FALSE(adopted.empty());
    CHECK(occurrences(adopted, "adoptScriptTypes(") == 0U);
    CHECK(occurrences(adopted, "writeScriptTypes(") == 0U);
}
