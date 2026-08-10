#pragma once
// Aero Engine — the PURE half of the Blender CLI integration (task 3.2.4). PUBLIC, and the
// asset_view.hpp / project_files.hpp shape verbatim: free of ImGui, SDL, entt, <filesystem>,
// <fstream> and every build gate. NOTHING HERE LOGS (INV-B10). NOTHING HERE PERFORMS A FILE
// OPERATION OF ANY KIND (INV-B14) -- every rule below is provable from a string literal or an
// injected struct with no context at all, which is what makes all three host platforms' behaviour
// assertable from ONE test process (AC-4).
//
// Blender is an external PROCESS, never a library: no header, no archive, no vcpkg entry, ever
// (ADR-003 and the GPL boundary in docs/01-tech-stack.md). Nothing in this tree parses a .blend.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

enum class HostOs : std::uint8_t { Windows = 0, MacOs, Linux };

// The ONE place in first-party editor code that branches on the host (AC-5/INV-B6). Everything
// downstream -- including the entire candidate-path builder -- is a pure function of this enum plus
// injected environment strings, so Windows path generation is fully testable on macOS and vice versa.
//
// NOTE TO THE AUTHOR OF THIS COMMENT: do NOT spell the three platform macros in prose here. AC-5's
// grep does not strip comments (unlike the platform/rhi guards, which do), so a citation in this
// comment silently breaks the "exactly three lines" count. The three desktop hosts are named in
// docs/00's platform matrix; an unknown fourth is a compile error, never a silent fallback.
[[nodiscard]] HostOs currentHostOs() noexcept;

// Every input INJECTED. blenderCandidatePaths never reads getenv, never touches disk, never consults
// a clock or a global. That is what makes AC-4 -- three OSes asserted from ONE test process -- possible.
struct BlenderEnv {
    std::string overridePath;              // from editor_tools.json; "" == unset
    std::string envPath;                   // AERO_BLENDER_PATH;      "" == unset
    std::vector<std::string> pathEntries;  // PATH, ALREADY split on the OS separator by the caller
    std::string homeDir;                   // $HOME / %USERPROFILE%
    std::string programFiles;              // %PROGRAMFILES%  (Windows only; "" elsewhere)
    std::string localAppData;              // %LOCALAPPDATA%  (Windows only; "" elsewhere)
};

// D12's order, and it is fixed:
//   1. overridePath, if non-empty  -- ALONE. Steps 2-4 are skipped ENTIRELY, INCLUDING when the path
//      does not exist: an override that is wrong must produce "the Blender you configured is not
//      there", never a different Blender's output (AC-3).
//   2. envPath,      if non-empty  -- ALONE, same rule.
//   3. each pathEntries[i] + '/' + the per-OS executable name, skipping empty/whitespace-only entries
//      (E2), capped at MAX_PATH_ENTRIES_SCANNED (R3).
//   4. the per-OS well-known table, newest-version-first within each family.
// Deduplicated by exact string, FIRST-SEEN ORDER PRESERVED. Sorted std::vector only -- no std::set,
// no std::unordered_set (INV-B13).
//
// The well-known table is A CONVENIENCE AND NEVER THE ONLY ROUTE: `Locate...`, AERO_BLENDER_PATH and
// PATH all bypass it, so the list going stale DEGRADES, it does not break.
[[nodiscard]] std::vector<std::string> blenderCandidatePaths(HostOs os, const BlenderEnv& env);

struct BlenderVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    auto operator<=>(const BlenderVersion&) const noexcept = default;
};

// Reads the FIRST LINE ONLY. Requires the literal prefix "Blender " followed by a digit; parses
// <major>[.<minor>[.<patch>]]; IGNORES everything after the last parsed number (" LTS" on 5.2, a build
// suffix elsewhere) -- MEASURED against a real `blender --version` (plan §G-4). The tab-indented
// "build date:"/"build hash:" continuation lines are never reached.
// nullopt for anything else, INCLUDING a leading-whitespace form and a wrong-case prefix.
// NEVER THROWS: parses digit-by-digit with an overflow guard, never std::stoul. A component that does
// not fit in 32 bits yields nullopt rather than a saturated lie -- and nullopt already has a
// well-defined downstream meaning (D14 below: attempt, never refuse).
[[nodiscard]] std::optional<BlenderVersion> parseBlenderVersion(std::string_view versionOutput);

enum class BlenderSupport : std::uint8_t { Supported = 0, Warned, Refused };
// D14. Refused below 2.80 (bpy.ops.export_scene.gltf did not exist -- there is nothing to attempt).
// Warned in [2.80, 3.3). Supported at >= 3.3. Supported for nullopt: an unparseable version is far more
// likely to be a locale, a build suffix or a fork we have not seen than a Blender from 2011, and
// refusing on it would break installs that work (E4).
[[nodiscard]] BlenderSupport blenderSupport(const std::optional<BlenderVersion>& version) noexcept;

// AC-28. ASCII-case-folded SUFFIX test on the FULL name (the isMetaFileName / isImportableModelName
// shape): "a.tar.blend" true, "a.blend.bak" false, ".blend" alone false, ".blend1"/".blend2" false.
//
// DELIBERATELY SEPARATE FROM isImportableModelName, and that separation is the whole of D15: phase 7.5
// gates its probe on THAT predicate, so a .blend is skipped by code that already exists and
// asset_database.cpp is not modified by this task at all. If a future task adds ".blend" to that table,
// phase 7.5 will feed raw .blend bytes to importModel, which returns Unsupported, and every .blend in
// the project will report an import failure on every scan. MI133 pins both halves together.
[[nodiscard]] bool isBlendFileName(std::string_view fileName) noexcept;

// ---- argv builders ------------------------------------------------------------------------------
// Both are PURE, and both were executed VERBATIM against Blender 5.2.0 LTS (plan §G-4). NOTHING is
// quoted, escaped or substituted in either: SDL owns Windows quoting (SDL_windowsprocess.c's
// join_arguments, F2) and hand-quoting on top of it is a double-escape bug, while on POSIX there is
// no shell in the path at all -- SDL_CreateProcessWithProperties takes an argv ARRAY, never a
// command line. That is also why INV-B2 holds: no shell, no shell wrapper and no command-string
// launcher of any kind appears anywhere in this task.
//
// NOTE TO THE AUTHOR OF THIS COMMENT: INV-B2's gate grep does not strip comments (the AC-5 rule, one
// invariant over). Do NOT spell the shell-launcher tokens it searches for in prose here, or a
// citation silently turns a hard, empty-output gate into one that has to be read and judged.

[[nodiscard]] std::vector<std::string> buildVersionArgs(std::string_view binary);

// EXACTLY fifteen entries, in this order:
//   binary, "-b", blendAbs, "-X", "-Y", "-noaudio", "--python-exit-code", "42",
//   "--python", scriptAbs, "--", "--out", outAbs, "--status", statusAbs
// Everything after "--" is Blender's own convention for "stop parsing, hand the rest to the script",
// and the script reads it from sys.argv -- MEASURED to arrive verbatim (§G-4).
[[nodiscard]] std::vector<std::string> buildExportArgs(std::string_view binary, std::string_view blendAbs,
                                                       std::string_view scriptAbs, std::string_view outAbs,
                                                       std::string_view statusAbs);

// The export script's text, as a COMPILE-TIME CONSTANT with NO INTERPOLATION SITE OF ANY KIND
// (AC-13): byte-identical for every project, every asset and every platform, which is also what makes
// it assertable in tier-0.
//
// This is D8, and it is a DELIBERATE deviation from the roadmap subtask's own wording, on security
// grounds: the alternative -- handing Blender a Python EXPRESSION built by concatenating a
// user-controlled path into a string literal -- is a code injection. A file name that closes the
// quote and appends a statement is legal on macOS and Linux, and a backslash in a Windows path is a
// Python escape before it is a separator; every mitigation is a hand-rolled parser for a language we
// do not own. A FILE plus sys.argv after "--" has neither problem. The roadmap row is left unedited
// so the deviation stays visible.
[[nodiscard]] std::string_view blenderExportScriptText() noexcept;

// Bumped whenever the script text changes, which invalidates every cached artifact -- that is the
// intended behaviour, and it is why the version is recorded in the provenance record.
inline constexpr std::uint32_t BLENDER_SCRIPT_VERSION = 1;

// ---- constants ----------------------------------------------------------------------------------

inline constexpr std::string_view BLENDER_EXPORT_DIR_NAME = "BlenderExports";
inline constexpr std::string_view BLENDER_SCRIPT_FILE_NAME = "export_gltf.py";
inline constexpr int TOOL_PREFS_FORMAT_VERSION = 1;
inline constexpr int EXPORT_PROVENANCE_FORMAT_VERSION = 1;
inline constexpr float BLENDER_TIMEOUT_SECONDS = 300.0F;   // D6/R4 -- generous, finite, named
inline constexpr float PROCESS_FORCE_KILL_SECONDS = 5.0F;  // graceful, then forceful
// 256 KiB. A log is READ INTO MEMORY and rendered in a panel, which is why this is three orders of
// magnitude below the artifact cap rather than equal to it.
// The ULL is not decoration: a `256U * 1024U` product is computed in `unsigned int` and only THEN
// widened, which bugprone-implicit-widening-of-multiplication-result rejects on the Linux lane.
inline constexpr std::uint64_t MAX_TOOL_LOG_BYTES = 256ULL * 1024;
// 256 MiB -- the SAME NUMBER as 3.2.1's MAX_MODEL_FILE_BYTES, RESTATED rather than included (the
// MAX_REPORTED_PER_CATEGORY precedent). Including model_import.hpp for one integer would put
// aero::scene and the whole math umbrella on this pure header's compile line, which is exactly what
// import_settings.hpp exists to prevent.
inline constexpr std::uint64_t MAX_ARTIFACT_BYTES = 256ULL * 1024 * 1024;
inline constexpr std::size_t MAX_PATH_ENTRIES_SCANNED = 256;  // R3 -- bounds the one-per-session sweep
inline constexpr BlenderVersion BLENDER_MIN_SUPPORTED{3, 3, 0};
inline constexpr BlenderVersion BLENDER_ABSOLUTE_MIN{2, 80, 0};

}  // namespace engine::editor
