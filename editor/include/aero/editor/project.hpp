#pragma once
// Aero Engine — the project model: project.json v1's format, its validators, the open project, and
// the recent-projects list (task 2.6.1). PUBLIC, and free of ImGui, SDL, entt and <filesystem> --
// the scene_session.hpp shape, so every rule below is reachable from the UNGATED tier-0
// aero_editor_shell_test with no window and no GPU. Held by FILE PLACEMENT (R12).
//
// THIS HEADER AND project.cpp ARE FREE OF EVERY BUILD GATE (D4/INV-P5). The project flow reads and
// writes no scene, so it never touches the engine's serialization bridge -- which is why
// tests/editor/project_test.cpp is present and passing in ALL THREE build configurations, unlike
// 2.5.1's and 2.5.2's editor test TUs, which are ABSENT from the reflect-OFF build by design.
// Anything that would make this header need a build gate is a design error, not a build problem.
//
// NOTHING HERE LOGS (INV-P6). Status is RETURNED -- `unknownKeys` is handed back and the CALLER
// WARNs (project_files.hpp:15-16's convention, applied a third time).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr int PROJECT_FORMAT_VERSION = 1;
inline constexpr std::string_view PROJECT_FILE_NAME = "project.json";
inline constexpr std::size_t MAX_RECENT_PROJECTS = 10;
inline constexpr std::size_t MAX_PROJECT_NAME_BYTES = 64;

// D5: "ts" | "cpp", chosen at creation and immutable by policy (enforcement is 4.6.1's). This build
// writes only "ts"; "cpp" is accepted on read so a hand-edited file opens and 4.6.1 is purely
// additive over a field that already round-trips. Any other value is a REJECT, never a fallback.
enum class ProjectLanguage : std::uint8_t { Ts = 0, Cpp };
[[nodiscard]] std::string_view languageKey(ProjectLanguage language) noexcept;  // "ts" / "cpp"
[[nodiscard]] std::optional<ProjectLanguage> languageFromKey(std::string_view key) noexcept;

// The parsed document. VALUES only -- no path to the file, no handles, nothing live.
struct ProjectManifest {
    std::string name;
    std::string engineVersion;
    ProjectLanguage language = ProjectLanguage::Ts;
    std::string assetsPath = "assets";  // project-relative, '/'-separated on every OS (A20)
    std::string scenesPath = "scenes";
};

// Why a manifest was refused. A VALUE TYPE, not an engine:: type -- the same reason SceneOpenOutcome
// is (scene_session.hpp:126-132): it keeps this header reachable in every configuration with no #if
// and no engine serialization include.
enum class ProjectError : std::uint8_t {
    None = 0,
    Unreadable,
    BadJson,
    NotAnObject,
    BadVersion,
    UnsupportedVersion,
    BadName,
    BadEngineVersion,
    BadLanguage,
    BadPaths,
    BadRelativePath,
};

struct ProjectParseResult {
    std::optional<ProjectManifest> manifest;  // engaged == success
    ProjectError error = ProjectError::None;
    // The exact docs/09 §4.7 text; "" iff `manifest` is engaged. line/column are > 0 ONLY for a
    // JSON-stage failure -- the SceneError contract, one layer up.
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::vector<std::string> unknownKeys;  // AC-6: WARNed by the CALLER, never here
};

[[nodiscard]] ProjectParseResult parseProject(std::string_view text);
[[nodiscard]] std::string writeProjectText(const ProjectManifest& manifest);  // canonical, trailing '\n' (F2)

// ---- pure validation, every rule its own testable function ---------------------------------------
enum class NameProblem : std::uint8_t {
    Ok = 0,
    Empty,
    TooLong,
    Separator,
    DotName,
    IllegalChar,
    TrailingSpaceOrDot,
    ReservedDeviceName,
};
[[nodiscard]] NameProblem validateProjectName(std::string_view utf8) noexcept;    // D6, all three OSes
[[nodiscard]] std::string_view nameProblemMessage(NameProblem problem) noexcept;  // the modal's inline text

// A pure STRING check -- deliberately NEVER std::filesystem::path::is_absolute() (A19): on Windows
// is_absolute("/shared") is FALSE (no root name), so a POSIX-rooted value would sail through the very
// check E5 exists for. Non-empty, no leading '/', no '\' anywhere, no ':' anywhere (covers "C:" and
// "C:/x"), no ".." segment. Keeps this function noexcept and <filesystem>-free in project.cpp.
[[nodiscard]] bool isLegalRelativePath(std::string_view relative) noexcept;

// ---- the open project ------------------------------------------------------------------------------
class ProjectSession {
public:
    [[nodiscard]] bool isOpen() const noexcept;  // == !rootPath.empty()
    // Absolute, normalized, no trailing separator. A VIEW into a member that lives as long as the
    // session -- copy it first if it must survive a project change (the scenePath() caveat,
    // editor_app.hpp:145-146).
    [[nodiscard]] std::string_view root() const noexcept;
    [[nodiscard]] const ProjectManifest& manifest() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;  // "" when closed
    // The three below ALLOCATE, so none is noexcept (A11): a bad_alloc inside a noexcept function is
    // std::terminate. The SceneSession::documentName()/windowTitle() precedent -- neither is noexcept
    // either, and for exactly this reason.
    //
    // The join rule (A20): <root> + '/' + <paths.assets>, with a single separator and no trailing
    // one. ALWAYS '/', on every OS, deliberately: root() is lexically_normal'd so on Windows it is
    // backslash-separated and the result is mixed -- which every consumer accepts
    // (std::filesystem::path::operator/, rootDisplayName's two-separator rule at
    // project_files.hpp:115-119, and SDL's default_location). A platform conditional is forbidden
    // (D4/AC-35), and '/' is already this tree's own separator for every editor-built relative path.
    [[nodiscard]] std::string assetsRoot() const;    // "" when closed  (D10 compares this)
    [[nodiscard]] std::string scenesRoot() const;    // "" when closed  (the dialog start dir)
    [[nodiscard]] std::string manifestPath() const;  // <root>/project.json

    // INV-P1: exactly ONE function under editor/src/ may call this setter -- scene_session.cpp's
    // adoptProject, which always calls newScene first. There is no path that changes the project
    // without resetting the scene, and none that resets the scene for a project change without
    // clearing the CommandStack in the same operation (INV-6, scene_session.hpp:92-113). A second call
    // site is an architecture bug, and this task's grep is what holds the line.
    void set(ProjectManifest manifest, std::string absoluteRootUtf8);
    void close() noexcept;

private:
    std::string rootPath;
    // Distinct from manifest(): MemberCase collides otherwise (docs/04's no-trailing-underscore rule).
    ProjectManifest projectManifest;
};

// ---- recents, pure over a vector -------------------------------------------------------------------
struct RecentProjects {
    std::vector<std::string> paths;  // absolute, normalized, most-recent FIRST
};

// Dedups on NORMALIZED BYTES ONLY -- every path reaching here is already normalized by
// projectRootFromPath upstream, and this function additionally strips a trailing '/' or '\' from
// both sides before comparing, so a hand-edited recents entry still matches. NO case folding (D9):
// APFS can be case-sensitive and so can a Windows volume with per-directory case sensitivity
// enabled; a duplicate row is a cosmetic annoyance, a MERGED row is data loss.
void promoteRecent(RecentProjects& recents, std::string absoluteRootUtf8);  // dedup + move-to-front + cap (AC-22)
[[nodiscard]] RecentProjects parseRecentProjects(std::string_view text, bool& warn);  // AC-23
[[nodiscard]] std::string writeRecentProjectsText(const RecentProjects& recents);

// ---- the filesystem half (project_file.cpp; ALL <filesystem> and ALL SDL for projects) ----------
// Every function below NEVER THROWS: each std::filesystem call uses the std::error_code overload
// (project_files.cpp's E20 rule). None of them logs (INV-P6). None of them DELETES, RENAMES or MOVES
// anything -- the only writes in this whole task are three create_directory calls and two atomic
// file writes (INV-P4/D7).

struct ProjectLoadOutcome {
    bool ok = false;
    ProjectManifest manifest;
    std::string root;  // normalized absolute root
    ProjectError error = ProjectError::None;
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::vector<std::string> unknownKeys;
};
// Read <root>/project.json, parse, validate. Touches NOTHING else. Never logs.
[[nodiscard]] ProjectLoadOutcome loadProjectFrom(std::string_view pathUtf8);

enum class CreateProblem : std::uint8_t {
    Ok = 0,
    BadName,
    BadLocation,
    LocationMissing,
    TargetIsFile,
    TargetNotEmpty,
    CreateFailed,
    WriteFailed,
};
struct ProjectCreateOutcome {
    CreateProblem problem = CreateProblem::Ok;
    std::string message;  // OS reason when there is one
    std::string root;     // the created root, on success
    ProjectManifest manifest;
};
// D6/D7/AC-9..AC-15. Creates nothing until every purely-lexical check has passed.
[[nodiscard]] ProjectCreateOutcome createProject(std::string_view locationUtf8, std::string_view nameUtf8,
                                                 std::string_view engineVersionUtf8);

[[nodiscard]] bool directoryExists(std::string_view utf8);
[[nodiscard]] bool directoryIsEmpty(std::string_view utf8);  // false when it is not a directory
[[nodiscard]] std::string defaultRecentProjectsPath();       // D8/F16: pref -> base -> CWD
[[nodiscard]] RecentProjects readRecentProjects(std::string_view pathUtf8);
void writeRecentProjects(std::string_view pathUtf8, const RecentProjects& recents);

// Accepts a project DIRECTORY or a .../project.json. Returns the ROOT with separators unified to the
// platform's own, `.`/`..` resolved LEXICALLY, and no trailing separator. "" in, "" out (AC-17).
// PURE -- it touches no disk -- but DEFINED IN project_file.cpp, because unifying separators without
// a platform conditional (forbidden, D4/AC-35) is exactly what std::filesystem::path's lexical
// machinery does for free, and THIS header must stay <filesystem>-free (AC-38).
[[nodiscard]] std::string projectRootFromPath(std::string_view pathUtf8);

}  // namespace engine::editor
