#pragma once
// Aero Engine -- the PURE half of the esbuild integration (task 4.3.1). PUBLIC, and the blender_tool.hpp shape:
// free of ImGui, SDL, entt and every filesystem or stream header. NOTHING HERE LOGS, NOTHING HERE TOUCHES THE DISK,
// and NOTHING HERE READS THE ENVIRONMENT -- every rule below is a function of strings, which is what lets all three
// desktop hosts' behaviour be asserted from one test process.
//
// esbuild is an external PROCESS, never a library: no header, no archive, no vcpkg entry. This file only decides
// what its command line is, how its output is read and when a stored result is still current.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_meta.hpp>    // AssetRecord -- scriptSourceSetFingerprint's input
#include <aero/editor/blender_tool.hpp>  // HostOs

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// <projectRoot>/Library/<these>, beside BlenderExports/ (Library's one spelling is ASSET_CACHE_DIR_NAME).
inline constexpr std::string_view SCRIPT_ARTIFACT_DIR_NAME = "Scripts";
inline constexpr std::string_view SCRIPT_STAGING_DIR_NAME = "ScriptStaging";
inline constexpr int SCRIPT_RECORD_FORMAT_VERSION = 1;
inline constexpr float SCRIPT_RUN_LIMIT_SECONDS = 30.0F;
inline constexpr std::uint64_t MAX_SCRIPT_LOG_BYTES = 256ULL * 1024;
inline constexpr std::uint64_t MAX_SCRIPT_METAFILE_BYTES = 4ULL * 1024 * 1024;
inline constexpr std::uint64_t MAX_SCRIPT_OUTPUT_BYTES = 32ULL * 1024 * 1024;  // the .js and the .js.map, each
inline constexpr std::size_t MAX_SCRIPT_DIAGNOSTICS = 64;                      // stored and logged per run

// The command line's fixed part, in argv order. Changing any element changes every record's fingerprint.
// Why each is here: --bundle (one module per script), --format=esm (the VM evaluates modules), --platform=neutral
// (no Node or browser globals assumed), --target=es2023 (lowers syntax quickjs-ng refuses), --supported:using=false
// (lowers `using` whatever the target becomes), --preserve-symlinks (a symlinked directory inside the assets root
// keeps the spelling the asset database walks), --sourcemap=linked (the map beside the JS), --tsconfig-raw (the
// project's tsconfig never changes emit), and the three log flags (a parseable, colourless, complete log).
inline constexpr std::array<std::string_view, 11> ESBUILD_FIXED_FLAGS{
    "--bundle",
    "--format=esm",
    "--platform=neutral",
    "--target=es2023",
    "--supported:using=false",
    "--preserve-symlinks",
    "--sourcemap=linked",
    R"(--tsconfig-raw={"compilerOptions":{"verbatimModuleSyntax":true,)"
    R"("useDefineForClassFields":true,"experimentalDecorators":false}})",
    "--log-level=warning",
    "--color=false",
    "--log-limit=0",
};

[[nodiscard]] std::string_view esbuildPinnedVersion() noexcept;  // AERO_ESBUILD_VERSION, from CMake
// A `.ts` file that is not a `.d.ts`: exact-case suffix, at least one character before it. `.tsx`, `.mts` and `.cts`
// are not scripts.
[[nodiscard]] bool isScriptAssetName(std::string_view assetsRelativePath) noexcept;
[[nodiscard]] std::string_view esbuildExecutableName(HostOs os) noexcept;  // "esbuild.exe" | "esbuild"
// `configured` if non-empty; else `envValue` if non-empty; else `baseDirectory` + a separator + the executable name;
// "" when all three are empty.
[[nodiscard]] std::string resolveEsbuildPath(HostOs os, std::string_view configured, std::string_view envValue,
                                             std::string_view baseDirectory);
// The manifest's assets path as a '/'-joined run of segments, empty and "." segments dropped; "" == the project
// root itself.
[[nodiscard]] std::string assetsPrefixOf(std::string_view manifestAssetsPath);

struct EsbuildRunPaths {   // all project-relative, '/'-separated
    std::string entry;     // <prefix>/<assets-relative script>
    std::string outfile;   // Library/ScriptStaging/<guid>.js
    std::string metafile;  // Library/ScriptStaging/<guid>.meta.json
};
[[nodiscard]] EsbuildRunPaths esbuildRunPaths(std::string_view assetsPrefix, std::string_view scriptAssetsRelative,
                                              Guid guid);
// binary, entry, the fixed flags, --metafile=..., --outfile=... Nothing is quoted or escaped: the process layer
// owns the platform's quoting.
[[nodiscard]] std::vector<std::string> buildEsbuildArgs(std::string_view binary, const EsbuildRunPaths& paths);
// formatContentHash(hashBytes(the fixed flags, each followed by '\n')).
[[nodiscard]] std::string esbuildFlagsFingerprint();

enum class ScriptInputVerdict : std::uint8_t { Accepted = 0, OutsideAssetsRoot, PackageImport, InsideLibrary };
// Segment-wise, on '/'-separated project-relative paths (`assets2/x.ts` is not under `assets`).
[[nodiscard]] ScriptInputVerdict classifyScriptInput(std::string_view projectRelativeInput,
                                                     std::string_view assetsPrefix) noexcept;
// The assets-relative remainder of an ACCEPTED input; "" for any other verdict.
[[nodiscard]] std::string_view assetsRelativeOf(std::string_view projectRelativeInput,
                                                std::string_view assetsPrefix) noexcept;
[[nodiscard]] std::string projectRelativeOf(std::string_view assetsPrefix, std::string_view assetsRelative);

struct EsbuildMetafile {
    std::vector<std::string> inputs;  // the keys of "inputs": '/'-normalised, sorted, unique
};
[[nodiscard]] std::optional<EsbuildMetafile> parseEsbuildMetafile(std::string_view text);

enum class ScriptDiagnosticSeverity : std::uint8_t { Error = 0, Warning };
struct ScriptDiagnostic {
    ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
    std::string file;          // project-relative, '/'; "" when the message has no location
    std::uint32_t line = 0;    // 1-based; 0 when there is no location
    std::uint32_t column = 0;  // 1-BASED (esbuild prints 0-based); 0 when there is no location
    std::string text;          // the header text after "[ERROR] " / "[WARNING] ", verbatim
    bool operator==(const ScriptDiagnostic&) const = default;
};
struct EsbuildLogParse {
    std::vector<ScriptDiagnostic> diagnostics;  // at most MAX_SCRIPT_DIAGNOSTICS, in log order
    std::size_t total = 0;                      // every header seen, uncapped
    std::string firstLine;                      // the first non-blank line, for the unparseable-log message
};
[[nodiscard]] EsbuildLogParse parseEsbuildLog(std::string_view logText);
// "<file>:<line>:<column>: error: <text>" | "...: warning: <text>"; a location-less one names `scriptProjectRelative`
// alone: "<script>: error: <text>". No "scripts: " prefix -- the driver adds it.
[[nodiscard]] std::string formatScriptDiagnostic(const ScriptDiagnostic& diagnostic,
                                                 std::string_view scriptProjectRelative);

enum class ScriptRecordStatus : std::uint8_t { Ok = 0, Failed };
struct ScriptRecordInput {
    std::string path;                 // project-relative, '/'
    std::optional<ContentHash> hash;  // nullopt == the scan had no usable hash: never fresh
    bool operator==(const ScriptRecordInput&) const = default;
};
struct ScriptRecord {
    ScriptRecordStatus status = ScriptRecordStatus::Ok;
    std::string script;                         // project-relative             INFORMATIONAL
    std::string esbuildVersion;                 // the pinned version string    INFORMATIONAL
    std::string esbuildBinary;                  // the binary's content hash    compared when the current one is known
    std::string flags;                          // esbuildFlagsFingerprint()    compared always
    std::vector<ScriptRecordInput> inputs;      // sorted by path, unique       compared always
    std::string sourceSet;                      // Failed only, "" on Ok        compared on Failed
    std::vector<ScriptDiagnostic> diagnostics;  // Failed only                  INFORMATIONAL
    bool operator==(const ScriptRecord&) const = default;
};
// Any wrong version, missing key, wrong kind or unparseable text is nullopt: a record is derived and disposable, so
// a bad one is a miss (the script runs again), never a repair.
[[nodiscard]] std::optional<ScriptRecord> parseScriptRecord(std::string_view text);
[[nodiscard]] std::string writeScriptRecordText(const ScriptRecord& record);  // fixed key order, one trailing LF

struct ScriptFreshnessContext {
    std::string esbuildBinary;  // "" == unknown: not compared
    std::string flags;
    std::string sourceSet;
    std::function<std::optional<ContentHash>(std::string_view projectRelativePath)> currentHash;
};
[[nodiscard]] bool scriptRecordFresh(const ScriptRecord& record, std::string_view scriptProjectRelative,
                                     const ScriptFreshnessContext& context);
// formatContentHash(hashBytes(every record's relativePath, in the span's order, each followed by '\n')).
[[nodiscard]] std::string scriptSourceSetFingerprint(std::span<const AssetRecord> records);

}  // namespace engine::editor
