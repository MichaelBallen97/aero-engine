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
        return SceneContainment::Outside;  // a bare leaf has no directory at all
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
