// tests/editor/scene_containment_test.cpp -- task E.4.2: the containment predicate, its resolver and
// its one reason sentence. UNCONDITIONAL and tier-0: no window, no GPU, no ImGui context, no
// serialization bridge -- so this TU is present and its cases run in ALL THREE build configurations,
// which is what AC-19 asks and what --list-test-cases proves (CN1 must appear in both reduced ones).
// Fifty-third TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. NO `#if` OF ANY KIND anywhere in this file (3.6.3's rule).
//
// A scoped-enum comparison inside a CHECK needs DOUBLE parentheses, and this file adds NO printer for
// SceneContainment: doctest's streamability trait calls an unqualified operator<< from inside
// doctest::detail, so only namespaces ASSOCIATED WITH THE ARGUMENT are searched (E.2.2's rule) -- a
// printer would have to live in engine::editor, not here. Every enum assertion is therefore written
// CHECK((x == SceneContainment::Outside)), which prints `CHECK( true )` on a failure, which is why the
// cases CAPTURE their inputs instead.
#include <aero/editor/project.hpp>
#include <aero/editor/scene_containment.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view / std::string
#include <string>
#include <string_view>
#include <system_error>

using engine::editor::containmentPermits;
using engine::editor::containmentReason;
using engine::editor::ContainmentVerdict;
using engine::editor::directoryWithin;
using engine::editor::lexicalContainment;
using engine::editor::normalizeForContainment;
using engine::editor::SceneContainment;

namespace {

// A unique temp directory that removes itself on destruction -- the Nth TU-local copy of this shape
// (scene_session_test.cpp:60-92, project_files_test.cpp:43-90). Kept TU-local: ~30 lines, no new
// header, no new target_include_directories.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_scene_containment_test_" + std::to_string(++counter));
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
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }
    [[nodiscard]] std::filesystem::path pathOf(std::string_view leaf) const {
        const std::string joined = join(leaf);
        const std::u8string bytes(reinterpret_cast<const char8_t*>(joined.data()), joined.size());
        return std::filesystem::path(bytes);
    }

private:
    std::filesystem::path dirPath;
};

// The '/'-unified UTF-8 spelling of a std::filesystem::path, for the few cases that need to compare
// against one the resolver did not produce. NEVER named toString: doctest's DOCTEST_STRINGIFY expands
// to an UNQUALIFIED toString(...), which ADL would find here and then fail to decompose.
[[nodiscard]] std::string genericUtf8Of(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    std::string out(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

}  // namespace

// ---- CN1-CN10: the pure predicate --------------------------------------------------------------

TEST_CASE("scene_containment: directoryWithin accepts the root and anything under it (CN1, AC-7)") {
    CHECK(directoryWithin("/w/P/scenes", "/w/P"));
    CHECK(directoryWithin("/w/P", "/w/P"));  // a DIRECTORY may BE the root
    CHECK(directoryWithin("/w/P/a/b/c/d", "/w/P"));
    CHECK_FALSE(directoryWithin("/w", "/w/P"));  // the parent is not under the child
    CHECK_FALSE(directoryWithin("/x/P/scenes", "/w/P"));
}

TEST_CASE("scene_containment: a sibling sharing a byte prefix is REFUSED at every spelling (CN2, AC-7)") {
    // `"/w/ProjA"` IS a byte prefix of every string below. A starts_with() implementation accepts
    // all four; the segment-wise one refuses all four, on every OS, with no extra guard. S2 seeds
    // starts_with() back in, and if THIS case does not go red the whole battery is worthless.
    CHECK_FALSE(directoryWithin("/w/ProjA-evil", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("/w/ProjAx", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("/w/ProjA.", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("/w/ProjA ", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("/w/ProjA-evil/scenes", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("\\w\\ProjA-evil", "\\w\\ProjA"));  // and the Windows spelling
    // THE ANTI-VACUITY ARM: the same root with the REAL directory is accepted, so a
    // directoryWithin that returned false for everything would not pass this case.
    CHECK(directoryWithin("/w/ProjA/scenes", "/w/ProjA"));
}

TEST_CASE("scene_containment: every separator spelling of one root agrees (CN3, D2)") {
    // FIVE spellings of the SAME root, against both spellings of the same directory.
    for (const std::string_view root : {"/w/P", "/w/P/", "\\w\\P", "/w//P", "/w/./P"}) {
        CAPTURE(root);
        CHECK(directoryWithin("/w/P/s", root));
        CHECK(directoryWithin("\\w\\P\\s", root));
    }
    // The negative half, in the same five spellings -- what stops a "return true always" body.
    for (const std::string_view root : {"/w/Q", "/w/Q/", "\\w\\Q", "/w//Q", "/w/./Q"}) {
        CAPTURE(root);
        CHECK_FALSE(directoryWithin("/w/P/s", root));
        CHECK_FALSE(directoryWithin("\\w\\P\\s", root));
    }
}

TEST_CASE("scene_containment: the leading separator run discriminates relative, absolute and UNC (CN4, D2)") {
    CHECK_FALSE(directoryWithin("/a/b", "a"));              // absolute vs relative
    CHECK_FALSE(directoryWithin("a/b", "/a"));              // relative vs absolute
    CHECK(directoryWithin("//srv/sh/p", "//srv/sh"));       // UNC vs UNC
    CHECK_FALSE(directoryWithin("//srv/sh/p", "/srv/sh"));  // UNC vs POSIX-absolute
    CHECK_FALSE(directoryWithin("/srv/sh/p", "//srv/sh"));  // and the reverse
    CHECK(directoryWithin("a/b/c", "a/b"));                 // relative vs relative still works
}

TEST_CASE("scene_containment: a '..' on either side, in any position, is refused (CN5, D3)") {
    CHECK_FALSE(directoryWithin("/w/P/../Q", "/w/P"));  // inside the root's own span
    CHECK_FALSE(directoryWithin("/w/P/s", "/w/P/.."));  // on the ROOT side
    CHECK_FALSE(directoryWithin("/w/P/s/..", "/w/P"));  // PAST the root's segments -- the trailing
                                                        // walk is the only thing that catches this
    CHECK_FALSE(directoryWithin("../w/P", "../w"));     // both sides
    // ANTI-VACUITY: a "." in the same positions is DROPPED, not refused.
    CHECK(directoryWithin("/w/P/./s", "/w/P"));
    CHECK(directoryWithin("/w/P/s", "/w/./P"));
}

TEST_CASE("scene_containment: comparison is BYTE-EXACT and never case-folded (CN6, D4/F8)") {
    CHECK_FALSE(directoryWithin("/w/proja/s", "/w/ProjA"));
    CHECK_FALSE(directoryWithin("/w/PROJA/s", "/w/ProjA"));
    CHECK(directoryWithin("/w/ProjA/s", "/w/ProjA"));  // anti-vacuity
    // The rescue is what makes a case-differing REAL path work, and CN15 is where that is proven.
}

TEST_CASE("scene_containment: a scene path that IS the root is Outside (CN7)") {
    CHECK((lexicalContainment("/w/P", "/w/P") == SceneContainment::Outside));
    CHECK((lexicalContainment("/w/P/x.scene.json", "/w/P") == SceneContainment::Contained));
    CHECK((lexicalContainment("/w/P/scenes/x.scene.json", "/w/P") == SceneContainment::Contained));
    CHECK((lexicalContainment("/w/Q/x.scene.json", "/w/P") == SceneContainment::Outside));
}

TEST_CASE("scene_containment: an empty root is NoProject, before every other test (CN8, D5)") {
    CHECK((lexicalContainment("/w/P/x.scene.json", "") == SceneContainment::NoProject));
    CHECK((lexicalContainment("", "") == SceneContainment::NoProject));                      // the root wins, FIRST
    CHECK((lexicalContainment("/w/P/../x.scene.json", "") == SceneContainment::NoProject));  // and over D3
    CHECK(containmentPermits(SceneContainment::NoProject));
}

TEST_CASE("scene_containment: Unresolvable, and all four enumerators through containmentPermits (CN9, D3)") {
    CHECK((lexicalContainment("", "/w/P") == SceneContainment::Unresolvable));
    CHECK((lexicalContainment("/w/P/../x.scene.json", "/w/P") == SceneContainment::Unresolvable));
    CHECK((lexicalContainment("/w/P/x.scene.json", "/w/P/..") == SceneContainment::Unresolvable));
    // ALL FOUR enumerators, so a FIFTH added later fails HERE first rather than silently permitting.
    CHECK(containmentPermits(SceneContainment::Contained));
    CHECK(containmentPermits(SceneContainment::NoProject));
    CHECK_FALSE(containmentPermits(SceneContainment::Outside));
    CHECK_FALSE(containmentPermits(SceneContainment::Unresolvable));
}

TEST_CASE("scene_containment: a bare leaf is Outside, not Unresolvable (CN10)") {
    // directoryOf("x.scene.json") is "" (scene_session.cpp:129-135), which is a real answer, not a
    // failure to parse -- so Outside is the honest verdict and Unresolvable would be a lie.
    CHECK((lexicalContainment("x.scene.json", "/w/P") == SceneContainment::Outside));
    CHECK((lexicalContainment("x.scene.json", "") == SceneContainment::NoProject));  // still the root first
}

// ---- CN11-CN12: normalizeForContainment ---------------------------------------------------------

TEST_CASE("scene_containment: normalizeForContainment's five properties (CN11, S21/S22)") {
    // ★ THE LITERALS BELOW ARE SUFFIXES AND EQUIVALENCES, NEVER WHOLE VALUES, and that is a platform
    // fact this tree already records from the other side: on Windows "/a/b" has a root-DIRECTORY but
    // no root-NAME, so it is NOT absolute and std::filesystem::absolute prepends the current drive --
    // "C:/a/b" (project_file.cpp:56's own paragraph, and project.hpp:90-93 saying the same about
    // is_absolute("/shared")). A whole-value `== "/a/b"` here would be green on macOS and Linux and
    // RED ON WINDOWS ALONE. The `"\\a\\b"` spelling is worse still: on POSIX a backslash is an
    // ordinary filename character, so that whole string is ONE RELATIVE SEGMENT and absolute()
    // prepends the working directory -- measured here as "<cwd>//a/b", not "/a/b". It is therefore
    // pinned by the no-backslash property below and by nothing else.
    CHECK(normalizeForContainment("").empty());  // S22's target: BEFORE absolutization. PORTABLE as
                                                 // written -- "" in, "" out on every platform.
    // S21's target: the filesystem root survives as a ROOT rather than collapsing to "". Asserted as
    // non-empty and as a prefix of a path under it, because on Windows this value is "C:" and on
    // POSIX it is "/". S21 (dropping the size() > 1 guard) is caught on the two POSIX lanes, where it
    // produces ""; on Windows both forms produce "C:" and the seed is invisible, which is stated
    // rather than papered over.
    const std::string slash = normalizeForContainment("/");
    CHECK_FALSE(slash.empty());

    const std::string ab = normalizeForContainment("/a/b");
    CAPTURE(ab);
    REQUIRE(ab.size() >= 4U);
    CHECK(ab.substr(ab.size() - 4U) == "/a/b");  // the SUFFIX is exact on all three lanes
    CHECK(ab.back() != '/');                     // no trailing separator survives
    CHECK(ab.rfind(slash, 0) == 0);              // and the root normalises to a prefix of it
    CHECK(normalizeForContainment("/a/b/") == ab);
    CHECK(normalizeForContainment("/a/b///") == ab);
    CHECK(normalizeForContainment("/a/./b") == ab);
    CHECK(normalizeForContainment("/a/b/../c") == normalizeForContainment("/a/c"));
    // ANTI-VACUITY: the equivalences above are satisfied by a constant function, so two paths that
    // must differ are asserted to.
    CHECK(normalizeForContainment("/a/b/../c") != ab);
    // NO BACKSLASH SURVIVES, for ANY input -- asserted as a property, not as five literals. The
    // R"(\\srv\sh\p)" row is a UNC path on Windows and an ordinary one on POSIX, and both answers
    // are correct: the case asserts only the absence of a backslash, which holds on both. That one is
    // spelled as a RAW literal because modernize-raw-string-literal is --warnings-as-errors in CI and
    // rejects the escaped form at that length.
    for (const std::string_view in : {"\\a\\b", "/a\\b", R"(\\srv\sh\p)", "a\\b\\", "\\"}) {
        CAPTURE(in);
        CHECK(normalizeForContainment(in).find('\\') == std::string::npos);
    }
}

TEST_CASE("scene_containment: a relative path is absolutised against the CWD (CN12, P84)") {
    const std::string norm = normalizeForContainment("rel/x.scene.json");
    CHECK_FALSE(norm.empty());
    CHECK(norm != "rel/x.scene.json");  // something was prepended
    CHECK(norm.find("rel/x.scene.json") != std::string::npos);
    // It begins with the CWD, read through std::filesystem rather than through
    // normalizeForContainment's own output -- the "two values from one source" trap.
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    const std::string cwdUtf8 = genericUtf8Of(cwd);
    CAPTURE(cwdUtf8);
    CAPTURE(norm);
    CHECK(norm.rfind(cwdUtf8, 0) == 0);
}

// ---- CN21-CN24: containmentReason, the ONE sentence (D6) ---------------------------------------

TEST_CASE("scene_containment: a permitted verdict has NO reason (CN21, D6)") {
    ContainmentVerdict v;
    v.state = SceneContainment::Contained;
    CHECK(containmentReason(v, "/w/P", false).empty());
    CHECK(containmentReason(v, "/w/P", true).empty());
    v.state = SceneContainment::NoProject;
    CHECK(containmentReason(v, "", false).empty());
    CHECK(containmentReason(v, "", true).empty());
}

TEST_CASE("scene_containment: Unresolvable names neither root nor project, in both directions (CN22, D6)") {
    ContainmentVerdict v;
    v.state = SceneContainment::Unresolvable;
    const std::string r = containmentReason(v, "/w/ProjA", false);
    CHECK_FALSE(r.empty());
    CHECK(r.back() == '.');  // the period is the FUNCTION's, never a caller's
    CHECK(r.find("/w/ProjA") == std::string::npos);
    CHECK(r.find("Save Scene As") == std::string::npos);
    CHECK(containmentReason(v, "/w/ProjA", true) == r);  // forSave does not change it
}

TEST_CASE("scene_containment: a SAVE's reason never names another project (CN23, D9)") {
    ContainmentVerdict v;
    v.state = SceneContainment::Outside;
    v.owningProjectRoot = "/w/ProjB";  // set on purpose
    v.owningProjectName = "ProjB";
    const std::string s = containmentReason(v, "/w/ProjA", true);
    CHECK(s.find("/w/ProjA") != std::string::npos);
    CHECK(s.find("Save Scene As") != std::string::npos);
    CHECK(s.find("ProjB") == std::string::npos);  // the offer NEVER leaks into a save's reason
    CHECK(s.find("/w/ProjB") == std::string::npos);
    CHECK(s.back() == '.');
}

TEST_CASE("scene_containment: an OPEN's reason has three distinct arms (CN24, D6/D8)") {
    ContainmentVerdict v;
    v.state = SceneContainment::Outside;
    v.owningProjectRoot = "/w/ProjB";
    v.owningProjectName = "ProjB";
    const std::string named = containmentReason(v, "/w/ProjA", false);
    CHECK(named.find("'ProjB'") != std::string::npos);
    CHECK(named.find("/w/ProjB") != std::string::npos);
    CHECK(named.find("outside the open project") == std::string::npos);
    CHECK(named.find("Save Scene As") == std::string::npos);
    CHECK(named.back() == '.');

    v.owningProjectName.clear();  // present but unreadable (D8)
    const std::string unnamed = containmentReason(v, "/w/ProjA", false);
    CHECK(unnamed.find("another project") != std::string::npos);
    CHECK(unnamed.find("/w/ProjB") != std::string::npos);
    CHECK(unnamed.find("'ProjB'") == std::string::npos);

    v.owningProjectRoot.clear();  // nothing found anywhere
    const std::string none = containmentReason(v, "/w/ProjA", false);
    CHECK(none.find("outside the open project") != std::string::npos);
    CHECK(none.find("/w/ProjA") != std::string::npos);
    CHECK(none.find("ProjB") == std::string::npos);

    // All three are DISTINCT -- a refactor that collapsed two arms would otherwise pass every
    // containment assertion above, each of which is a `find`.
    CHECK(named != unnamed);
    CHECK(unnamed != none);
    CHECK(named != none);
}
