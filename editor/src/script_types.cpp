// Aero Engine -- the project's script declarations and tsconfig.json (task 4.2.2). The embedded texts' accessors
// and the ONE writer. All I/O goes through text_file.hpp's primitives, so this TU holds no <filesystem> and never
// logs: the caller (scene_session.cpp) turns a Failed outcome into one WARN.
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME / ASSET_CACHE_GITIGNORE_NAME / LIBRARY_GITIGNORE_TEXT
#include <aero/editor/script_types.hpp>
#include <aero/editor/text_file.hpp>  // readTextFile / writeTextFileAtomic / fileExists / ensureDirectory
#include <aero/reflect/json_writer.hpp>

#include "script_types_data.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

std::string_view scriptDeclarationsText() noexcept { return embedded::scriptDeclarations(); }

std::string_view tsconfigTemplateText() noexcept { return embedded::tsconfigTemplate(); }

std::string renderTsconfig(std::string_view assetsPath) {
    while (!assetsPath.empty() && assetsPath.back() == '/') {
        assetsPath.remove_suffix(1);
    }
    std::string glob(assetsPath);
    glob += "/**/*.ts";
    // A top-level value writes no separator, so str() is exactly the quoted, escaped string.
    JsonWriter writer;
    writer.value(std::string_view(glob));
    std::string text(tsconfigTemplateText());
    const std::size_t at = text.find(TSCONFIG_INCLUDE_PLACEHOLDER);
    if (at != std::string::npos) {
        text.replace(at, TSCONFIG_INCLUDE_PLACEHOLDER.size(), writer.str());
    }
    return text;
}

namespace {

struct FileOutcome {
    ScriptTypesWrite state = ScriptTypesWrite::Skipped;
    std::string error;
};

[[nodiscard]] std::string joinUnder(std::string_view parent, std::string_view leaf) {
    std::string path(parent);
    path += '/';
    path += leaf;
    return path;
}

// The order is the contract: compare first (an unchanged file touches nothing), then the cache directory and its
// ignore file, then the types directory, then the atomic write.
[[nodiscard]] FileOutcome writeDeclarations(std::string_view projectRootUtf8, const std::string& declarationsPath) {
    const std::string libraryDir = joinUnder(projectRootUtf8, ASSET_CACHE_DIR_NAME);
    const FileReadResult existing = readTextFile(declarationsPath);
    if (existing.text.has_value() && *existing.text == scriptDeclarationsText()) {
        return {.state = ScriptTypesWrite::Unchanged, .error = {}};
    }
    if (std::string error = ensureDirectory(libraryDir); !error.empty()) {
        return {.state = ScriptTypesWrite::Failed, .error = std::move(error)};
    }
    // The rule of project_state.cpp's writeProjectState, repeated with no string restated: the ignore file is
    // written only when absent, and its outcome is DISCARDED -- a missing .gitignore is a git annoyance the next
    // rescan fixes, and the declarations are the point.
    const std::string ignorePath = joinUnder(libraryDir, ASSET_CACHE_GITIGNORE_NAME);
    if (!fileExists(ignorePath)) {
        (void)writeTextFileAtomic(ignorePath, LIBRARY_GITIGNORE_TEXT);
    }
    const std::string typesDir = joinUnder(libraryDir, SCRIPT_TYPES_DIR_NAME);
    if (std::string error = ensureDirectory(typesDir); !error.empty()) {
        return {.state = ScriptTypesWrite::Failed, .error = std::move(error)};
    }
    if (std::string error = writeTextFileAtomic(declarationsPath, scriptDeclarationsText()); !error.empty()) {
        return {.state = ScriptTypesWrite::Failed, .error = std::move(error)};
    }
    return {.state = ScriptTypesWrite::Written, .error = {}};
}

// The file is the project's own once it exists (a directory wearing the name counts too), so it is only ever created.
[[nodiscard]] FileOutcome writeTsconfig(const std::string& tsconfigPath, std::string_view assetsPath) {
    if (fileExists(tsconfigPath)) {
        return {.state = ScriptTypesWrite::Unchanged, .error = {}};
    }
    if (std::string error = writeTextFileAtomic(tsconfigPath, renderTsconfig(assetsPath)); !error.empty()) {
        return {.state = ScriptTypesWrite::Failed, .error = std::move(error)};
    }
    return {.state = ScriptTypesWrite::Written, .error = {}};
}

}  // namespace

ScriptTypesReport writeScriptTypes(std::string_view projectRootUtf8, const ProjectManifest& manifest) {
    ScriptTypesReport report;
    if (projectRootUtf8.empty() || manifest.language != ProjectLanguage::Ts) {
        return report;
    }
    // NAMED LOCALS, each built from the project root (the amended INV-A1's grep-decidable form).
    const std::string declarationsPath =
        joinUnder(joinUnder(joinUnder(projectRootUtf8, ASSET_CACHE_DIR_NAME), SCRIPT_TYPES_DIR_NAME),
                  SCRIPT_DECLARATIONS_FILE_NAME);
    const std::string tsconfigPath = joinUnder(projectRootUtf8, TSCONFIG_FILE_NAME);

    FileOutcome declarations = writeDeclarations(projectRootUtf8, declarationsPath);
    FileOutcome tsconfig = writeTsconfig(tsconfigPath, manifest.assetsPath);
    report.declarations = declarations.state;
    report.declarationsPath = declarationsPath;
    report.declarationsError = std::move(declarations.error);
    report.tsconfig = tsconfig.state;
    report.tsconfigPath = tsconfigPath;
    report.tsconfigError = std::move(tsconfig.error);
    return report;
}

}  // namespace engine::editor
