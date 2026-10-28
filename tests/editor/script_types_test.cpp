// Aero Engine -- the project's script declarations and tsconfig.json (task 4.2.2, DT8-DT16). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING
// in all three configurations. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
//
// Paths are compared after a '\\' -> '/' normalisation and by suffix, never with == across layers.
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME / LIBRARY_GITIGNORE_TEXT -- the shipped constants
#include <aero/editor/asset_meta.hpp>   // ATOMIC_TEMP_SUFFIX -- writeTextFileAtomic's own temp name
#include <aero/editor/project.hpp>      // ProjectManifest / ProjectLanguage
#include <aero/editor/script_types.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>

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
