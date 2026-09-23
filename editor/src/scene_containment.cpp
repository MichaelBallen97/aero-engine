// Aero Engine -- the containment predicate and its resolver (task E.4.2). The PURE half is
// <filesystem>-free by construction; the resolver below owns ALL <filesystem> for this pair, exactly
// as project_file.cpp does for the half of project.hpp that needs it. This TU never logs (INV: the
// caller logs exactly one ERROR, D6) and never writes, renames, copies or deletes anything -- which
// is why it is NOT in check-project-no-delete.sh's Check A allowlist and must never be added to it.
#include <aero/editor/project.hpp>        // PROJECT_FILE_NAME, loadProjectFrom, ProjectLoadOutcome
#include <aero/editor/project_files.hpp>  // canonicalDirectory (INV-C9, widened to a COMPARISON key)
#include <aero/editor/scene_containment.hpp>
#include <aero/editor/scene_session.hpp>  // directoryOf -- the tree's own "last '/' or '\\'" helper

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace engine::editor {

namespace {

// The TU-local pathFromUtf8/utf8FromPath pair, copied from project_file.cpp:34-42 -- the FOURTH copy
// in this tree (project_files.cpp:29-38, text_file.cpp, vfs.cpp:93-98 are the others). NEVER
// std::filesystem::u8path: deprecated since C++20, and clang-tidy's --warnings-as-errors flags it.
// Construct from UTF-8 BYTES so non-ASCII names resolve correctly on Windows, where path's native
// encoding is UTF-16 and the narrow-char constructor assumes the active code page.
//
// A fourth copy is correct here: a SHARED one would need a new header for two functions, and the two
// public headers that could hold it (project.hpp, project_files.hpp) are both <filesystem>-free by
// contract, which is the property that makes this whole pair tier-0 reachable.
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

constexpr bool isSeparator(char c) noexcept { return c == '/' || c == '\\'; }

// Count the leading separator run: 0 relative, 1 POSIX-absolute, 2 UNC (D2). Also the cursor's start.
[[nodiscard]] std::size_t leadingSeparatorRun(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && isSeparator(s[i])) {
        ++i;
    }
    return i;
}

// Advance `pos` past one segment of `s`, skipping separator runs and "." segments; returns false at
// the end of the string. `seg` is set to the segment found and POINTS INTO `s`. `dotDot` is set (never
// cleared) when the segment found is exactly "..".
//
// ALLOCATION-FREE ON PURPOSE (task E.4.2, U1): directoryWithin is noexcept and runs on every open and
// every save, and a bad_alloc inside a noexcept function is std::terminate (the project.hpp:106-108
// rule, pointed the other way). The vector-of-views form this replaced could not be both.
[[nodiscard]] bool nextSegment(std::string_view s, std::size_t& pos, std::string_view& seg, bool& dotDot) noexcept {
    while (true) {
        while (pos < s.size() && isSeparator(s[pos])) {
            ++pos;
        }
        if (pos >= s.size()) {
            return false;
        }
        const std::size_t start = pos;
        while (pos < s.size() && !isSeparator(s[pos])) {
            ++pos;
        }
        // The pointer+size constructor, NEVER substr(): substr is specified to throw std::out_of_range,
        // which would escape this noexcept function (fileNameOf's own reason, scene_session.cpp:122-125).
        const std::string_view found(s.data() + start, pos - start);
        if (found == "..") {
            dotDot = true;
            seg = found;
            return true;  // REPORTED, never dropped -- the caller must be able to refuse (D3)
        }
        if (found != ".") {
            seg = found;
            return true;
        }
        // a "." segment: skip it and keep going
    }
}

// True iff any segment of `s` is exactly "..". Deliberately NOT named hasDotDotSegment: that name is
// ALREADY TAKEN by engine/core/src/vfs.cpp:100, whose version splits on '/' only and takes a RELATIVE
// path (task E.4.2, C1). The two are file-local in different TUs, so there is no ODR question -- only a
// grep that would otherwise read two functions and suggest one calls the other.
[[nodiscard]] bool pathHasDotDotSegment(std::string_view s) noexcept {
    std::size_t pos = leadingSeparatorRun(s);
    std::string_view seg;
    bool dotDot = false;
    while (nextSegment(s, pos, seg, dotDot)) {
        if (dotDot) {
            return true;
        }
    }
    return dotDot;
}

// D8's upward walk, in the anonymous namespace: nothing outside this TU calls it. EXPLICITLY
// ITERATIVE -- misc-no-recursion is --warnings-as-errors in CI and a recursive walk would be
// rejected outright.
//
// `openRootUtf8` and `openRootCanonUtf8` are the OPEN project's two spellings, and they are what stop
// the walk OFFERING THE PROJECT THAT IS ALREADY OPEN (the code-review round, CN25). The walk climbs
// from the scene's own directory, so whenever the lexical test missed AND the rescue could not fire --
// a symlinked or case-differing spelling of the open root, plus a scene directory that does not exist,
// which canonicalDirectory answers "" for -- the first project.json above the scene is the open
// project's own. Accepting that offer routes through adoptProject -> newScene -> World::clear() +
// CommandStack::clear(), so a button labelled "Open ProjA" while ProjA is open silently RESETS a clean
// document for what reads as a no-op. The refusal itself still stands; only the offer is withheld.
//
// It STOPS rather than climbing past: the nearest enclosing project is the only one that owns this
// scene, and a grandparent is the "confidently wrong answer" the broken-manifest arm below refuses for
// the same reason.
void findEnclosingProject(std::string_view startDirUtf8, std::string_view openRootUtf8,
                          std::string_view openRootCanonUtf8, ContainmentVerdict& out) {
    if (startDirUtf8.empty()) {
        return;
    }
    std::filesystem::path dir = pathFromUtf8(startDirUtf8);
    for (std::size_t depth = 0; depth < MAX_PROJECT_SEARCH_DEPTH; ++depth) {
        std::error_code ec;
        const std::filesystem::path manifest = dir / std::string(PROJECT_FILE_NAME);
        // is_regular_file, NEVER editor::fileExists -- that is std::filesystem::exists
        // (text_file.hpp:32), which is TRUE FOR A DIRECTORY, so a directory named project.json would
        // stop the walk at a level holding no project at all (D8; CLAUDE.md records the same property
        // from the other side at E.3.2). CN18's directory arm is what proves it, and S19 is the seed.
        if (std::filesystem::is_regular_file(manifest, ec) && !ec) {
            const std::string foundRoot = utf8FromPath(dir);
            // IS THIS THE PROJECT THAT IS ALREADY OPEN? Both spellings are tested and the CANONICAL
            // one is the arm that matters: a byte comparison alone is very nearly unreachable here,
            // because a walk that reaches the open root byte-for-byte means the root's segments are a
            // prefix of the scene directory's, which directoryWithin would already have called
            // Contained. The differing spelling IS the defect's route, so resolving it is the fix.
            // One extra canonicalDirectory call, on the refused path only, and only once a manifest
            // was actually found.
            if (foundRoot == openRootUtf8 ||
                (!openRootCanonUtf8.empty() && canonicalDirectory(foundRoot) == openRootCanonUtf8)) {
                return;  // REFUSED, with NO offer -- D8's degradation, for a different reason
            }
            out.owningProjectRoot = foundRoot;
            const ProjectLoadOutcome loaded = loadProjectFrom(out.owningProjectRoot);
            if (loaded.ok) {
                out.owningProjectName = loaded.manifest.name;
            }
            // A present-but-BROKEN manifest STOPS the walk with a blank name (D8). Continuing upward
            // would offer a GRANDPARENT project that does not own this scene -- a confidently wrong
            // answer, where "there is a project here and I cannot read it" is the honest one. The
            // offer is still made; opening it fails with openProjectPath's own ERROR
            // (scene_session.cpp:306-307), which is the right place for that failure to surface.
            //
            // loaded.unknownKeys is DELIBERATELY NOT WARNED here: this NAMES a project, it does not
            // open one, and INV-P6 puts that WARN at the opening caller. Warning here would produce
            // messages about a project the user never opened.
            return;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return;  // stepToParent's own guard (project_file.cpp:76-80), or "/" recurses forever
        }
        // ★ UNMEASURED ON WINDOWS (the code-review round, recorded rather than fixed): if
        // path("C:/").parent_path() is "C:" rather than "C:/", neither term of the guard above fires
        // and the next iteration probes "C:/project.json" spelled DRIVE-RELATIVE -- resolved against
        // the process's current directory on that drive, not against its root. BOUNDED: the depth cap
        // still holds, the extra probe is one is_regular_file, and the worst outcome is an offer
        // naming a project the user did not expect rather than a permit. Windows validation row.
        dir = dir.parent_path();
    }
    // MAX_PROJECT_SEARCH_DEPTH exceeded: NO OFFER is made and the refusal still stands, with the
    // "outside the open project" wording. A degradation, never a wrong answer (D8).
}

}  // namespace

bool directoryWithin(std::string_view directoryUtf8, std::string_view rootUtf8) noexcept {
    if (rootUtf8.empty() || directoryUtf8.empty()) {
        return false;  // "is it under nothing" is not "yes"
    }
    const std::size_t dirLead = leadingSeparatorRun(directoryUtf8);
    const std::size_t rootLead = leadingSeparatorRun(rootUtf8);
    if (dirLead != rootLead) {
        return false;  // relative vs POSIX-absolute vs UNC (D2)
    }
    std::size_t dirPos = dirLead;
    std::size_t rootPos = rootLead;
    std::string_view dirSeg;
    std::string_view rootSeg;
    bool dirDotDot = false;
    bool rootDotDot = false;
    while (nextSegment(rootUtf8, rootPos, rootSeg, rootDotDot)) {
        if (rootDotDot) {
            return false;  // D3: refuse, never compare
        }
        if (!nextSegment(directoryUtf8, dirPos, dirSeg, dirDotDot)) {
            return false;  // the directory ran out first: it is SHORTER
        }
        if (dirDotDot) {
            return false;  // D3, on the other side
        }
        if (dirSeg != rootSeg) {
            return false;  // BYTE-EXACT, never case-folded (D4/F8)
        }
    }
    // The root is exhausted and every segment matched. The directory's REMAINING segments still have to
    // be checked for a ".." -- "/w/P/x/.." is NOT under "/w/P" in any useful sense, and stopping the
    // walk at the root's length would accept it.
    while (nextSegment(directoryUtf8, dirPos, dirSeg, dirDotDot)) {
        if (dirDotDot) {
            return false;
        }
    }
    return true;  // >= is right HERE: a directory may BE the root
}

SceneContainment lexicalContainment(std::string_view scenePathUtf8, std::string_view projectRootUtf8) noexcept {
    if (projectRootUtf8.empty()) {
        return SceneContainment::NoProject;  // D5, FIRST -- before every other test
    }
    if (scenePathUtf8.empty()) {
        return SceneContainment::Unresolvable;
    }
    if (pathHasDotDotSegment(scenePathUtf8) || pathHasDotDotSegment(projectRootUtf8)) {
        return SceneContainment::Unresolvable;  // D3: refused, never compared
    }
    const std::string_view dir = directoryOf(scenePathUtf8);  // scene_session.hpp:113
    if (dir.empty()) {
        // A bare leaf has no directory at all. It also catches a scene sitting DIRECTLY at a POSIX
        // filesystem root -- directoryOf("/x.scene.json") is "", not "/" -- which is a FALSE REFUSAL
        // for a project rooted at "/" (the code-review round, recorded rather than fixed). It is in
        // the safe direction (D3), it needs a project at the filesystem root to reach, and closing it
        // would mean teaching directoryOf that "" and "/" differ, which is a change to a helper five
        // other callers share.
        return SceneContainment::Outside;
    }
    return directoryWithin(dir, projectRootUtf8) ? SceneContainment::Contained : SceneContainment::Outside;
}

std::string normalizeForContainment(std::string_view pathUtf8) {
    if (pathUtf8.empty()) {
        return {};  // P84, and BEFORE absolutization: absolute("") is the CWD, not "".
    }
    std::error_code ec;
    std::filesystem::path p = pathFromUtf8(pathUtf8).lexically_normal();
    const std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (!ec) {
        p = abs.lexically_normal();  // absolute() alone does NOT normalize; re-normalize after it
    }
    std::string out = utf8FromPath(p);
    // canonicalDirectory:418's rule, for the identical reason: the comparison must be valid on a
    // platform whose native separator is '\\'.
    std::replace(out.begin(), out.end(), '\\', '/');
    // lexically_normal PRESERVES a trailing separator (project_file.cpp:95-99's A5 measurement).
    // The size() > 1 guard is why "/" survives as "/" -- stripping it to "" would turn a project at
    // the filesystem root into NoProject, which PERMITS. CN11 pins it; S21 seeds it away.
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

ContainmentVerdict resolveSceneContainment(std::string_view scenePathUtf8, std::string_view projectRootUtf8,
                                           bool findOwningProject) {
    ContainmentVerdict out;
    if (projectRootUtf8.empty()) {
        out.state = SceneContainment::NoProject;
        return out;  // D5 -- ZERO syscalls, ZERO allocations, before anything else happens
    }
    const std::string normScene = normalizeForContainment(scenePathUtf8);
    const std::string normRoot = normalizeForContainment(projectRootUtf8);

    out.state = lexicalContainment(normScene, normRoot);
    if (containmentPermits(out.state)) {
        return out;  // D4 -- THE COMMON PATH for every open and every save. Still zero syscalls.
    }
    if (out.state == SceneContainment::Unresolvable) {
        return out;  // no rescue for a path we could not even parse
    }

    // ---- THE RESCUE (D4 step 4). Reachable ONLY from Outside, and it can only ever produce
    // Contained -- it WIDENS, it never narrows, which is what bounds the untested Windows behaviour
    // to a false refusal with a readable ERROR rather than a false accept (R3).
    //
    // It canonicalises the scene's PARENT DIRECTORY, never the scene file. Three reasons, all
    // measured or cited: canonicalDirectory returns "" for anything that is not an EXISTING
    // DIRECTORY (project_files.cpp:405-408); a SAVE target routinely does not exist yet, and
    // canonical() on a missing path fails; and the question is about a directory's identity in the
    // first place. Canonicalising the file would make every Save-to-a-new-name outside the lexical
    // root take the "" arm and be refused for the wrong reason.
    //
    // `directoryOf` returns a VIEW into its argument (scene_session.cpp:129-135), so it is called on
    // the NAMED `normScene`, never on a temporary.
    const std::string canonRoot = canonicalDirectory(normRoot);
    const std::string canonDir = canonicalDirectory(directoryOf(normScene));
    if (!canonRoot.empty() && !canonDir.empty() && directoryWithin(canonDir, canonRoot)) {
        out.state = SceneContainment::Contained;
        return out;
    }

    // ---- D8's upward walk. NOT performed for a SAVE (D9): accepting a project offer routes through
    // adoptProject -> newScene (scene_session.cpp:261) -> World::clear() + CommandStack::clear(),
    // which discards the very work being saved. There is nothing to offer, so there is nothing to
    // look up, and a refused save therefore costs no extra filesystem call at all.
    if (findOwningProject) {
        findEnclosingProject(directoryOf(normScene), normRoot, canonRoot, out);
    }
    return out;  // Outside
}

std::string containmentReason(const ContainmentVerdict& v, std::string_view projectRootUtf8, bool forSave) {
    switch (v.state) {
        case SceneContainment::Contained:
        case SceneContainment::NoProject:
            return {};
        case SceneContainment::Unresolvable:
            return "the path could not be resolved.";
        case SceneContainment::Outside:
            break;
    }
    if (forSave) {
        return "a scene must be saved inside the open project at '" + std::string(projectRootUtf8) +
               "'; use Save Scene As... to choose a location inside it.";
    }
    if (!v.owningProjectRoot.empty() && !v.owningProjectName.empty()) {
        return "it belongs to project '" + v.owningProjectName + "' at '" + v.owningProjectRoot + "'.";
    }
    if (!v.owningProjectRoot.empty()) {
        return "it belongs to another project at '" + v.owningProjectRoot + "'.";
    }
    return "it is outside the open project at '" + std::string(projectRootUtf8) + "'.";
}

}  // namespace engine::editor
