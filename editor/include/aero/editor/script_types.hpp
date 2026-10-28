#pragma once
// Aero Engine -- the TypeScript project's declarations and tsconfig.json (task 4.2.2). PUBLIC, and free of ImGui,
// SDL, entt and <filesystem>; held by FILE PLACEMENT (R12), like every other header under editor/include.
//
// Two texts are EMBEDDED at build time (cmake/embed.cmake's text mode): the build's aero-engine.d.ts -- the
// hand-written prelude joined with one reflect-gen --emit-dts block per built-in component header -- and
// editor/templates/tsconfig.json.in. writeScriptTypes puts them into a project: the declarations under
// <root>/Library/types/ (a cache, rewritten whenever its bytes differ), the tsconfig.json at the root (the project's
// own file once it exists, so never rewritten).
#include <aero/editor/project.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr std::string_view SCRIPT_TYPES_DIR_NAME = "types";  // under Library/ (ASSET_CACHE_DIR_NAME)
inline constexpr std::string_view SCRIPT_DECLARATIONS_FILE_NAME = "aero-engine.d.ts";
inline constexpr std::string_view TSCONFIG_FILE_NAME = "tsconfig.json";
inline constexpr std::string_view TSCONFIG_INCLUDE_PLACEHOLDER = "@AERO_SCRIPT_INCLUDE@";

[[nodiscard]] std::string_view scriptDeclarationsText() noexcept;  // the build's aero-engine.d.ts, embedded
[[nodiscard]] std::string_view tsconfigTemplateText() noexcept;    // editor/templates/tsconfig.json.in, embedded

// The template with its one placeholder replaced by the JSON string "<assetsPath>/**/*.ts", trailing '/' removed
// from assetsPath first, escaped by engine::JsonWriter::value(std::string_view).
[[nodiscard]] std::string renderTsconfig(std::string_view assetsPath);

enum class ScriptTypesWrite : std::uint8_t { Skipped = 0, Written, Unchanged, Failed };

struct ScriptTypesReport {
    ScriptTypesWrite declarations = ScriptTypesWrite::Skipped;
    ScriptTypesWrite tsconfig = ScriptTypesWrite::Skipped;
    std::string declarationsPath;  // '/'-joined under the root; empty when Skipped
    std::string tsconfigPath;
    std::string declarationsError;  // the OS reason when Failed, else empty
    std::string tsconfigError;
};

// Never throws, never logs, never deletes. Skipped for both files when the root is empty or the language is not ts.
// Each file's outcome is independent: one failing never skips the other.
[[nodiscard]] ScriptTypesReport writeScriptTypes(std::string_view projectRootUtf8, const ProjectManifest& manifest);

}  // namespace engine::editor
