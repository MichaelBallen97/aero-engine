// Aero Engine — the project format's <filesystem>-and-SDL half (task 2.6.1). THE only new SDL TU
// this task adds (besides the pre-existing file_dialog.cpp and imgui_layer.cpp -- the roster moves
// from two files to three, A4). NEVER THROWS: every std::filesystem call uses the std::error_code
// overload (project_files.cpp's E20 rule). NEVER LOGS except at exactly THREE call sites (code
// review: an earlier "exactly ONE exception" banner undercounted this by two): the pref-path CWD
// fallback inside defaultRecentProjectsPath (mirroring imgui_layer.cpp:46's WARN), the
// corrupt-recents-file WARN inside readRecentProjects (AC-23), and the recents-write-failure WARN
// inside writeRecentProjects (AC-24). NEVER DELETES, RENAMES or MOVES anything -- the only writes
// anywhere in this task are three create_directory calls and two atomic file writes (INV-P4/D7).
#include <aero/core/log.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/text_file.hpp>

#include <SDL3/SDL_filesystem.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// The TU-local pathFromUtf8/utf8FromPath pair, copied from project_files.cpp:29-38 (NEVER
// std::filesystem::u8path -- deprecated since C++20, and clang-tidy's --warnings-as-errors would
// flag it). Construct from UTF-8 BYTES so non-ASCII names resolve correctly on Windows, where
// path's native encoding is UTF-16 and the narrow-char constructor assumes the active code page.
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// If `p`'s filename is empty (a trailing separator), step up to the parent -- guarded so "/" never
// recurses to itself.
void stripTrailingSeparator(std::filesystem::path& p) {
    if (p.filename().empty() && p.has_parent_path() && p.parent_path() != p) {
        p = p.parent_path();
    }
}

void stepToParent(std::filesystem::path& p) {
    if (p.has_parent_path() && p.parent_path() != p) {
        p = p.parent_path();
    }
}

constexpr std::string_view RECENTS_FILE_NAME = "recent_projects.json";

}  // namespace

// MEASURED (A5): lexically_normal() PRESERVES a trailing separator ("/a/b/" -> "/a/b/") and turns
// "/a/b/." and "/a/b/c/.." into "/a/b/" -- and filename() is EMPTY for all of them. So the
// trailing-separator strip is mandatory, it must run BEFORE the manifest-name strip (filename() on
// ".../project.json/" is empty), and it must run again after it. Each strip is guarded by
// `parent_path() != p`, or "/" recurses to itself forever.
std::string projectRootFromPath(std::string_view pathUtf8) {
    if (pathUtf8.empty()) {
        return {};  // "" in, "" out (AC-17)
    }
    std::filesystem::path p = pathFromUtf8(pathUtf8).lexically_normal();
    stripTrailingSeparator(p);
    if (utf8FromPath(p.filename()) == PROJECT_FILE_NAME) {
        stepToParent(p);
    }
    stripTrailingSeparator(p);
    return utf8FromPath(p);
}

bool directoryExists(std::string_view utf8) {
    std::error_code ec;
    const std::filesystem::path p = pathFromUtf8(utf8);
    return std::filesystem::is_directory(p, ec) && !ec;
}

// A8: directory_iterator(p, ec) on a FILE sets ec ("Not a directory") -- so this returns false when
// it is not a directory, for free, BEFORE looking at the comparison.
bool directoryIsEmpty(std::string_view utf8) {
    std::error_code ec;
    const std::filesystem::directory_iterator it(pathFromUtf8(utf8), ec);
    if (ec) {
        return false;
    }
    return it == std::filesystem::directory_iterator();
}

ProjectLoadOutcome loadProjectFrom(std::string_view pathUtf8) {
    ProjectLoadOutcome out;
    const std::string root = projectRootFromPath(pathUtf8);
    if (root.empty()) {
        out.error = ProjectError::Unreadable;
        out.message = "no project path given";
        return out;
    }
    const std::string manifestPath = root + "/" + std::string(PROJECT_FILE_NAME);
    const FileReadResult read = readTextFile(manifestPath);
    if (!read.text.has_value()) {
        out.error = ProjectError::Unreadable;
        out.message = read.error;
        return out;
    }
    const ProjectParseResult parsed = parseProject(*read.text);
    if (!parsed.manifest.has_value()) {
        out.error = parsed.error;
        out.message = parsed.message;
        out.line = parsed.line;
        out.column = parsed.column;
        return out;
    }
    out.ok = true;
    out.manifest = *parsed.manifest;
    out.root = root;
    out.error = ProjectError::None;
    out.unknownKeys = parsed.unknownKeys;
    return out;
}

ProjectCreateOutcome createProject(std::string_view locationUtf8, std::string_view nameUtf8,
                                   std::string_view engineVersionUtf8) {
    ProjectCreateOutcome out;

    // 1. The name, LEXICALLY -- nothing touched yet.
    if (const NameProblem problem = validateProjectName(nameUtf8); problem != NameProblem::Ok) {
        out.problem = CreateProblem::BadName;
        out.message = std::string(nameProblemMessage(problem));
        return out;
    }

    // 2. The location.
    if (locationUtf8.empty()) {
        out.problem = CreateProblem::BadLocation;
        out.message = "no location given";
        return out;
    }
    if (!directoryExists(locationUtf8)) {
        out.problem = CreateProblem::LocationMissing;
        out.message = "location does not exist";
        return out;
    }

    // 3. The target: may not exist (created) or exist and be EMPTY (adopted, E7/AC-13). Normalized
    // ONCE, here -- SHOULD-FIX 8 (code review): `out.root` below used to be re-derived by running
    // `target` back through `projectRootFromPath`, whose manifest-name strip fires whenever the
    // FINAL path component reads "project.json". A project literally named "project.json" made that
    // strip fire on the root we just built, taking `out.root` one level too high (a directory
    // `<location>/project.json/` scaffolded, but `out.root` reported `<location>` itself -- a
    // silently broken project). `target` IS the true root by construction; it never needs
    // re-deriving through a helper meant for a caller-supplied, not-yet-known path.
    const std::filesystem::path target = (pathFromUtf8(locationUtf8) / pathFromUtf8(nameUtf8)).lexically_normal();
    std::error_code existsEc;
    const bool exists = std::filesystem::exists(target, existsEc);
    if (exists) {
        std::error_code dirEc;
        const bool isDir = std::filesystem::is_directory(target, dirEc);
        if (!isDir) {
            out.problem = CreateProblem::TargetIsFile;
            out.message = "a file already exists at that location";
            return out;
        }
        if (!directoryIsEmpty(utf8FromPath(target))) {
            out.problem = CreateProblem::TargetNotEmpty;
            out.message = "the target directory is not empty";
            return out;
        }
    }

    // 4. Scaffold. §A7: create_directory on an ALREADY-EXISTING directory returns `false` with NO
    // error_code (measured) -- decide CreateFailed from `ec` ALONE, never from the bool return, or
    // E7's legal "adopt an existing empty directory" case fails here every time (AC-13). Seed S22 is
    // the sabotage proof.
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        out.problem = CreateProblem::CreateFailed;
        out.message = ec.message();
        return out;
    }
    std::filesystem::create_directory(target / "assets", ec);
    if (ec) {
        out.problem = CreateProblem::CreateFailed;
        out.message = ec.message();
        return out;
    }
    std::filesystem::create_directory(target / "scenes", ec);
    if (ec) {
        out.problem = CreateProblem::CreateFailed;
        out.message = ec.message();
        return out;
    }

    // 5. The manifest. NOTHING IS REMOVED ON ANY FAILURE PATH, EVER (D7/INV-P4). No recursive
    // deletion, no rollback. The target may be a directory the user pre-created and cares about; it
    // may be a symlink; the failure that stopped us (a full disk, a permission change, an antivirus
    // lock) is exactly the condition under which a recursive delete is most likely to do something
    // surprising. The half-made directory is inert and the project is simply not opened. Seed S11
    // is what holds this line.
    ProjectManifest manifest;
    manifest.name = std::string(nameUtf8);
    manifest.engineVersion = std::string(engineVersionUtf8);
    manifest.language = ProjectLanguage::Ts;
    manifest.assetsPath = "assets";
    manifest.scenesPath = "scenes";
    const std::string manifestPath = utf8FromPath(target / std::string(PROJECT_FILE_NAME));
    const std::string writeReason = writeTextFileAtomic(manifestPath, writeProjectText(manifest));
    if (!writeReason.empty()) {
        out.problem = CreateProblem::WriteFailed;
        out.message = writeReason;
        return out;
    }

    out.problem = CreateProblem::Ok;
    out.root = utf8FromPath(target);  // verbatim -- SHOULD-FIX 8: never re-derived via
                                      // projectRootFromPath's manifest-name strip
    out.manifest = manifest;
    return out;
}

// D8: pref path FIRST, base path second, CWD last -- the OPPOSITE of imgui_layer.cpp:34-47's order
// for aero_editor.ini, deliberately. imgui.ini is APPLICATION state for a not-yet-installed dev
// build; next to the exe is convenient and disposable. The recents list is USER state that must
// survive rebuilding the editor into a fresh build/ directory, which base-path storage does not.
// Each file is stored where its own lifetime says it belongs. Do not "fix" the inconsistency.
//
// SDL_GetPrefPath() must be SDL_free()'d by the caller; SDL_GetBasePath() is SDL-CACHED and must NOT
// be freed (F16). Getting the first wrong is an ASan leak on both Debug lanes; getting the second
// wrong is a crash.
//
// This is the ONE exception to INV-P6 in this TU: one WARN, on the CWD fallback only, mirroring
// imgui_layer.cpp:46; nothing else in this file logs.
std::string defaultRecentProjectsPath() {
    if (char* const pref = SDL_GetPrefPath("AeroEngine", "AeroEditor"); pref != nullptr) {
        const std::string path = std::string(pref) + std::string(RECENTS_FILE_NAME);
        SDL_free(pref);
        return path;
    }
    if (const char* const base = SDL_GetBasePath(); base != nullptr) {
        return std::string(base) + std::string(RECENTS_FILE_NAME);
    }
    AERO_LOG_WARN("editor: could not resolve a pref/base path for {}; falling back to CWD", RECENTS_FILE_NAME);
    return std::string(RECENTS_FILE_NAME);
}

RecentProjects readRecentProjects(std::string_view pathUtf8) {
    const FileReadResult read = readTextFile(pathUtf8);
    if (!read.text.has_value()) {
        return {};  // a missing file is an empty list, SILENTLY (AC-23) -- not an error here
    }
    bool warn = false;
    RecentProjects recents = parseRecentProjects(*read.text, warn);
    if (warn) {
        AERO_LOG_WARN("editor: recents file '{}' is corrupt or unsupported; starting with an empty list", pathUtf8);
    }
    return recents;
}

void writeRecentProjects(std::string_view pathUtf8, const RecentProjects& recents) {
    const std::string reason = writeTextFileAtomic(pathUtf8, writeRecentProjectsText(recents));
    if (!reason.empty()) {
        AERO_LOG_WARN("editor: could not write recents file '{}' -- {}", pathUtf8, reason);
    }
}

}  // namespace engine::editor
