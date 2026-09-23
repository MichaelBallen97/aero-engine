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
#include <vector>

using engine::editor::containmentPermits;
using engine::editor::containmentReason;
using engine::editor::ContainmentVerdict;
using engine::editor::CreateProblem;
using engine::editor::directoryWithin;
using engine::editor::lexicalContainment;
using engine::editor::normalizeForContainment;
using engine::editor::ProjectCreateOutcome;
using engine::editor::resolveSceneContainment;
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

// Everything after the first `//` removed. CN19 and CN20 both scan comment-stripped text, because the
// sentences explaining each rule necessarily name the very pattern the rule forbids.
[[nodiscard]] std::string_view stripLineComment(std::string_view line) {
    const std::size_t at = line.find("//");
    return at == std::string_view::npos ? line : line.substr(0, at);
}

// PU1's instrument (project_test.cpp:1596-1603): read a file through AERO_EDITOR_SRC_DIR and split on
// '\n'. A missing file is a REQUIRE failure, never a silent skip.
[[nodiscard]] std::vector<std::string_view> splitLines(const std::string& text) {
    std::vector<std::string_view> lines;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back(remaining);
            break;
        }
        lines.push_back(remaining.substr(0, newline));
        remaining.remove_prefix(newline + 1U);
    }
    return lines;
}

[[nodiscard]] bool isIdentifierByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':';
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

    // ★ THE RESOLVER'S OWN D5 ARM, and not merely lexicalContainment's. Every other case in this file
    //   hands resolveSceneContainment a NON-EMPTY root, so without this arm the resolver's own empty-root
    //   branch has no tier-0 witness at all: changing it to Outside leaves every CN case green and is
    //   caught only indirectly, by the scene_io and scene_golden cases that happen to run with no project
    //   open. findOwningProject is TRUE on purpose -- D5 returns before the walk can run, so both offer
    //   fields must still be empty.
    const ContainmentVerdict noProject = resolveSceneContainment("/w/P/x.scene.json", "", /*findOwningProject=*/true);
    CHECK((noProject.state == SceneContainment::NoProject));
    CHECK(containmentPermits(noProject.state));
    CHECK(noProject.owningProjectRoot.empty());
    CHECK(noProject.owningProjectName.empty());
    // ANTI-VACUITY: the SAME scene path against a NON-EMPTY root that does not contain it is Outside, so
    // the verdict above is a statement about the empty root rather than about this function.
    CHECK((resolveSceneContainment("/w/P/x.scene.json", "/w/Q", /*findOwningProject=*/false).state ==
           SceneContainment::Outside));
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

    // CN12(b): the RESOLVER on the same shape -- a relative scene path against an absolute root is
    // Outside and never a crash. The absolutization happens inside resolveSceneContainment, so the
    // comparison is between two absolute paths whatever the caller handed in.
    const ContainmentVerdict v = resolveSceneContainment("rel/x.scene.json", "/definitely/not/here",
                                                         /*findOwningProject=*/false);
    CHECK((v.state == SceneContainment::Outside));  // never a crash, never Unresolvable
    CHECK(v.owningProjectRoot.empty());             // findOwningProject == false
}

// ---- CN13-CN18: the resolver, the canonical rescue and the upward walk --------------------------

TEST_CASE("scene_containment: the permitted path performs no filesystem call at all (CN13, AC-9)") {
    // THE INSTRUMENT, and why it is this one: there is no portable syscall counter in this tree
    // (LD_PRELOAD is a proven dead end, CLAUDE.md), a timing bound is below the run-to-run spread,
    // and a source-text pin would assert the call site's POSITION -- an INTENTION -- rather than that
    // the function returned before reaching it. This asserts a CONSEQUENCE: a resolver that touched
    // the disk on the happy path returns a DIFFERENT ANSWER once the disk is gone. Seed S7 (run the
    // rescue unconditionally) reddens this case and NOTHING ELSE in the tree. Do not "simplify" it
    // into a source-text pin.
    const TempDir tmp;
    const std::string root = tmp.join("ProjA");
    const std::string scene = tmp.join("ProjA/scenes/x.scene.json");
    std::error_code ec;
    std::filesystem::create_directories(tmp.pathOf("ProjA/scenes"), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    REQUIRE(engine::editor::writeTextFileAtomic(scene, "{}").empty());

    REQUIRE((resolveSceneContainment(scene, root, /*findOwningProject=*/false).state == SceneContainment::Contained));

    // DELETE THE WHOLE TREE and re-query with the IDENTICAL strings. The removal COUNT is not
    // asserted: on a case-insensitive volume a stray .DS_Store makes it unstable. Non-existence is.
    std::filesystem::remove_all(tmp.pathOf("ProjA"), ec);
    REQUIRE_FALSE(std::filesystem::exists(tmp.pathOf("ProjA")));
    CHECK((resolveSceneContainment(scene, root, /*findOwningProject=*/false).state == SceneContainment::Contained));
    CHECK((resolveSceneContainment(scene, root, /*findOwningProject=*/true).state == SceneContainment::Contained));
    // THE ANTI-VACUITY ARM IS CN14, which flips Contained -> Outside under the same deletion --
    // proving the deletion is observable at all.

    // ★ THE SECOND INSTRUMENT, and it is NOT redundant with the deleted-tree arm above. Deleting the
    //   `if (containmentPermits(out.state)) return out;` early return outright leaves the rescue
    //   running on EVERY permitted open and every permitted save -- two filesystem calls the permitted
    //   path is specified never to make -- while changing NO ANSWER anywhere, because the rescue only
    //   ever assigns Contained and can never narrow one. Measured: that mutation passes every case in
    //   this binary, this one included. A COST with no observable consequence cannot be asserted as
    //   a consequence, so the ordering is pinned as source text -- and as an ORDERING, never a mere
    //   membership (3.4.2's I96 lesson: a pin must encode the property that matters). It also reddens
    //   the other direction, a rescue MOVED above the lexical decision.
    const std::filesystem::path srcDir(AERO_EDITOR_SRC_DIR);
    const engine::editor::FileReadResult src =
        engine::editor::readTextFile(genericUtf8Of(srcDir / "scene_containment.cpp"));
    REQUIRE(src.text.has_value());
    const std::vector<std::string_view> lines = splitLines(*src.text);
    std::size_t start = lines.size();
    std::size_t end = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = stripLineComment(lines[i]);
        if (start == lines.size()) {
            if (line.find("ContainmentVerdict resolveSceneContainment(") != std::string_view::npos) {
                start = i;
            }
            continue;
        }
        if (line == "}") {  // the definition's own closing brace, the only one at column 0
            end = i;
            break;
        }
    }
    REQUIRE(start < lines.size());
    REQUIRE(end < lines.size());
    REQUIRE(end - start > 10U);  // anti-vacuity: a real body was scanned, not an empty range

    std::size_t permitsAt = lines.size();
    std::size_t firstCanonAt = lines.size();
    std::size_t canonCalls = 0;
    for (std::size_t i = start; i < end; ++i) {
        const std::string_view line = stripLineComment(lines[i]);
        if (permitsAt == lines.size() && line.find("containmentPermits(") != std::string_view::npos) {
            permitsAt = i;
        }
        if (line.find("canonicalDirectory(") != std::string_view::npos) {
            if (firstCanonAt == lines.size()) {
                firstCanonAt = i;
            }
            ++canonCalls;
        }
    }
    CAPTURE(permitsAt);
    CAPTURE(firstCanonAt);
    REQUIRE(permitsAt < lines.size());     // the permitted-path gate EXISTS...
    REQUIRE(firstCanonAt < lines.size());  // ...and so does the rescue it gates
    CHECK(canonCalls == 2U);               // both rescue calls found -- the scan is not half-blind
    CHECK(permitsAt < firstCanonAt);       // ...and the gate is ABOVE both of them
    // ...and it is an early RETURN rather than a mention: on that line, or on the one below it.
    const bool returnsThere =
        stripLineComment(lines[permitsAt]).find("return") != std::string_view::npos ||
        (permitsAt + 1U < end && stripLineComment(lines[permitsAt + 1U]).find("return") != std::string_view::npos);
    CHECK(returnsThere);
}

TEST_CASE("scene_containment: the canonical rescue resolves a symlinked route (CN14, D4, symlink-capable hosts only)") {
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_directories(tmp.pathOf("real/scenes"), ec);
    std::filesystem::create_directory_symlink(tmp.pathOf("real"), tmp.pathOf("link"), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
    } else {
        const std::string sceneViaTarget = tmp.join("real/scenes/x.scene.json");
        const std::string rootViaLink = tmp.join("link");
        REQUIRE(engine::editor::writeTextFileAtomic(sceneViaTarget, "{}").empty());
        // LEXICALLY it is outside: "real" is not the segment "link".
        REQUIRE((lexicalContainment(normalizeForContainment(sceneViaTarget), normalizeForContainment(rootViaLink)) ==
                 SceneContainment::Outside));
        // AND THE RESCUE SAYS CONTAINED.
        CHECK((resolveSceneContainment(sceneViaTarget, rootViaLink, /*findOwningProject=*/false).state ==
               SceneContainment::Contained));
        // THE REVERSE ROUTE TOO -- root through the target, scene through the link.
        CHECK((resolveSceneContainment(tmp.join("link/scenes/x.scene.json"), tmp.join("real"),
                                       /*findOwningProject=*/false)
                   .state == SceneContainment::Contained));
        // ★ CN13's ANTI-VACUITY ARM: delete the tree and the SAME query flips to Outside, which proves
        //   the rescue really does touch the disk and therefore that CN13's green is meaningful.
        std::filesystem::remove(tmp.pathOf("link"), ec);
        std::filesystem::remove_all(tmp.pathOf("real"), ec);
        CHECK((resolveSceneContainment(sceneViaTarget, rootViaLink, /*findOwningProject=*/false).state ==
               SceneContainment::Outside));
    }
}

TEST_CASE("scene_containment: the canonical rescue answers the CASE question the predicate refuses (CN15, D4/F8)") {
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_directories(tmp.pathOf("ProjA/scenes"), ec);
    REQUIRE(std::filesystem::is_directory(tmp.pathOf("ProjA"), ec));
    // PROBE, never a platform assumption: does the volume resolve "proja" to the "ProjA" just created?
    const bool caseInsensitive = std::filesystem::is_directory(tmp.pathOf("proja"), ec);
    CAPTURE(caseInsensitive);
    const std::string scene = tmp.join("proja/scenes/x.scene.json");
    const std::string root = tmp.join("ProjA");
    if (caseInsensitive) {
        REQUIRE(engine::editor::writeTextFileAtomic(tmp.join("ProjA/scenes/x.scene.json"), "{}").empty());
    }
    // LEXICALLY it is Outside either way -- CN6's rule, restated at the resolver.
    REQUIRE((lexicalContainment(normalizeForContainment(scene), normalizeForContainment(root)) ==
             SceneContainment::Outside));
    // BOTH ANSWERS ARE CORRECT and the case asserts whichever the volume gives: on a case-INSENSITIVE
    // volume canonicalDirectory("<tmp>/proja/scenes") case-corrects to ".../ProjA/scenes" (measured on
    // this machine) and the rescue succeeds; on a case-SENSITIVE one that directory does not exist,
    // canonicalDirectory returns "" and Outside is right.
    const SceneContainment expected = caseInsensitive ? SceneContainment::Contained : SceneContainment::Outside;
    CHECK((resolveSceneContainment(scene, root, /*findOwningProject=*/false).state == expected));
}

TEST_CASE("scene_containment: the rescue WIDENS and never narrows (CN16, D13.1, symlink-capable hosts only)") {
    // A scene inside the project whose own directory is a symlink pointing OUT. Lexically CONTAINED;
    // canonically it is not. D13.1 says it stays PERMITTED, and this case is what makes that a
    // deliberate documented gap rather than a drift. S9 (let the rescue run on a permitted verdict and
    // take its answer) reddens exactly this.
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_directories(tmp.pathOf("outside/real"), ec);
    std::filesystem::create_directories(tmp.pathOf("ProjA"), ec);
    std::filesystem::create_directory_symlink(tmp.pathOf("outside/real"), tmp.pathOf("ProjA/escape"), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
    } else {
        const std::string scene = tmp.join("ProjA/escape/x.scene.json");
        REQUIRE((lexicalContainment(normalizeForContainment(scene), normalizeForContainment(tmp.join("ProjA"))) ==
                 SceneContainment::Contained));
        CHECK((resolveSceneContainment(scene, tmp.join("ProjA"), /*findOwningProject=*/false).state ==
               SceneContainment::Contained));
    }
}

TEST_CASE("scene_containment: the rescue cannot rescue what does not exist, and a save target need not (CN17)") {
    const TempDir tmp;
    std::error_code ec;
    // (a) OUTSIDE and missing: still Outside. The rescue's two "" guards are what make it so, not a
    //     crash.
    CHECK((resolveSceneContainment(tmp.join("nowhere/x.scene.json"), tmp.join("ProjA"), /*findOwningProject=*/false)
               .state == SceneContainment::Outside));
    // (b) ★ INSIDE and missing -- the SAVE case, and the reason the rescue takes the PARENT DIRECTORY.
    //     The lexical arm answers first, so the missing directory never matters. S8 (canonicalise the
    //     scene FILE) reddens this arm.
    std::filesystem::create_directories(tmp.pathOf("ProjA"), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
    CHECK((resolveSceneContainment(tmp.join("ProjA/brand/new/dir/x.scene.json"), tmp.join("ProjA"),
                                   /*findOwningProject=*/false)
               .state == SceneContainment::Contained));
    // (c) a save target directly in the root, also missing.
    CHECK((resolveSceneContainment(tmp.join("ProjA/Untitled.scene.json"), tmp.join("ProjA"),
                                   /*findOwningProject=*/false)
               .state == SceneContainment::Contained));
}

TEST_CASE("scene_containment: findEnclosingProject, all five arms (CN18, D8/D9)") {
    const TempDir tmp;
    std::error_code ec;
    // Built with createProject(), never a hand-written manifest -- the name must come from the same
    // writer loadProjectFrom() reads, or the case asserts two spellings of one thing agreeing.
    const ProjectCreateOutcome created = engine::editor::createProject(tmp.utf8(), "ProjB", "0.1.0");
    REQUIRE(created.problem == CreateProblem::Ok);
    const std::string sceneInProjB = created.root + "/scenes/x.scene.json";
    REQUIRE(engine::editor::writeTextFileAtomic(sceneInProjB, "{}").empty());
    // The OPEN project's root for every arm below: a sibling that does not exist, so the verdict is
    // Outside lexically and the rescue's "" guard keeps it there.
    const std::string openRoot = tmp.join("ProjA");

    SUBCASE("a real project above the scene: root and name both found") {
        const ContainmentVerdict v = resolveSceneContainment(sceneInProjB, openRoot, /*findOwningProject=*/true);
        REQUIRE((v.state == SceneContainment::Outside));
        CHECK(v.owningProjectRoot == normalizeForContainment(created.root));
        CHECK(v.owningProjectName == "ProjB");
    }

    SUBCASE("a corrupt project.json STOPS the walk with a blank name") {
        // The root is STILL set, the name is EMPTY, and the walk does NOT climb to the grandparent
        // (which is `created`, a perfectly readable project). S20 is the seed.
        const std::string brokenRoot = created.root + "/vendor/Broken";
        std::filesystem::create_directories(tmp.pathOf("ProjB/vendor/Broken/scenes"), ec);
        REQUIRE_FALSE(static_cast<bool>(ec));
        REQUIRE(engine::editor::writeTextFileAtomic(brokenRoot + "/project.json", "not json at all {{{").empty());
        const std::string scene = brokenRoot + "/scenes/x.scene.json";
        const ContainmentVerdict v = resolveSceneContainment(scene, openRoot, /*findOwningProject=*/true);
        REQUIRE((v.state == SceneContainment::Outside));
        CHECK(v.owningProjectRoot == normalizeForContainment(brokenRoot));
        CHECK(v.owningProjectName.empty());
        // ★ THE ANTI-VACUITY HALF: it reported the BROKEN one, not the readable grandparent.
        CHECK(v.owningProjectRoot != normalizeForContainment(created.root));
    }

    SUBCASE("a DIRECTORY named project.json is walked THROUGH, not stopped at") {
        // This is the is_regular_file discrimination; fileExists (std::filesystem::exists) is true for
        // a directory and would stop here with a blank name. S19 is the seed.
        std::filesystem::create_directories(tmp.pathOf("ProjB/vendor/DirManifest/project.json"), ec);
        std::filesystem::create_directories(tmp.pathOf("ProjB/vendor/DirManifest/scenes"), ec);
        REQUIRE_FALSE(static_cast<bool>(ec));
        REQUIRE(std::filesystem::is_directory(tmp.pathOf("ProjB/vendor/DirManifest/project.json"), ec));
        const std::string scene = created.root + "/vendor/DirManifest/scenes/x.scene.json";
        const ContainmentVerdict v = resolveSceneContainment(scene, openRoot, /*findOwningProject=*/true);
        REQUIRE((v.state == SceneContainment::Outside));
        CHECK(v.owningProjectRoot == normalizeForContainment(created.root));
        CHECK(v.owningProjectName == "ProjB");
    }

    SUBCASE("no project anywhere above: both fields empty") {
        // This arm can only assert "empty" if nothing ABOVE the OS temp directory holds a
        // project.json -- true on this machine and on every CI lane, and said here rather than
        // pretended to be structural. `tmp` itself carries none: ProjB is a CHILD of it, not a parent.
        std::filesystem::create_directories(tmp.pathOf("empty"), ec);
        REQUIRE_FALSE(static_cast<bool>(ec));
        const ContainmentVerdict v =
            resolveSceneContainment(tmp.join("empty/x.scene.json"), openRoot, /*findOwningProject=*/true);
        REQUIRE((v.state == SceneContainment::Outside));
        CHECK(v.owningProjectRoot.empty());
        CHECK(v.owningProjectName.empty());
    }

    SUBCASE("findOwningProject == false with a project RIGHT THERE: BOTH fields empty") {
        // D9 at the value level, and the only place the save arm's "no walk is even performed" is
        // asserted at tier 0. Without it, a findEnclosingProject that never ran at all would pass the
        // "no project anywhere" arm and could be argued to pass the others.
        const ContainmentVerdict save = resolveSceneContainment(sceneInProjB, openRoot, /*findOwningProject=*/false);
        REQUIRE((save.state == SceneContainment::Outside));
        CHECK(save.owningProjectRoot.empty());
        CHECK(save.owningProjectName.empty());
    }
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

// ---- CN19-CN20: the two source-text pins (commit 3) ---------------------------------------------

TEST_CASE("scene_containment: no editor/src file hoists a .root() view into a named local (CN19, D1)") {
    // project.session.root() returns a VIEW into the live session, and adoptProject replaces `rootPath`
    // (scene_session.cpp's adoptProject) from INSIDE performAction, so a hoisted local would dangle on
    // exactly the frame a project swaps -- and ASan would only catch it on such a frame, which is why
    // this is a TEXT pin rather than a runtime case. S14 is the seed.
    //
    // ★ THE PREDICATE IS NARROWED, because the obvious "both tokens on one line" form FALSE-POSITIVES
    //   ON A CLEAN TREE: editor_app.cpp's `std::string_view EditorApp::projectRoot() const noexcept
    //   { return project.root(); }` contains both and is entirely legal. A line is a violation iff,
    //   after its // comment is stripped, it contains `.root()` AND spells `std::string_view` followed
    //   by an identifier CONTAINING NO `::` and then `=` or `(`. It does NOT ban `.root()` -- there
    //   are legitimate by-value and by-argument uses all over editor/src, including every one of this
    //   task's own production call sites.
    const std::filesystem::path srcDir(AERO_EDITOR_SRC_DIR);
    REQUIRE(std::filesystem::is_directory(srcDir));

    std::size_t filesScanned = 0;
    std::size_t linesScanned = 0;
    std::size_t rootMentions = 0;
    std::size_t hoistedViewCount = 0;
    bool sawViewReturningFunction = false;  // POSITIVE CONTROL 1 -- scanned AND classified legal
    bool sawStringCopyOfRoot = false;       // POSITIVE CONTROL 2 -- the model answer D1 asks for

    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(srcDir, ec)) {
        if (entry.path().extension() != ".cpp") {
            continue;
        }
        const std::u8string bytes = entry.path().u8string();
        const std::string pathUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const engine::editor::FileReadResult read = engine::editor::readTextFile(pathUtf8);
        REQUIRE(read.text.has_value());
        ++filesScanned;
        for (const std::string_view raw : splitLines(*read.text)) {
            ++linesScanned;
            const std::string_view line = stripLineComment(raw);
            if (line.find(".root()") == std::string_view::npos) {
                continue;
            }
            ++rootMentions;
            if (line.find("std::string_view EditorApp::projectRoot") != std::string_view::npos) {
                sawViewReturningFunction = true;
            }
            if (line.find("const std::string root(projectSession.root());") != std::string_view::npos) {
                sawStringCopyOfRoot = true;
            }
            const std::size_t viewAt = line.find("std::string_view");
            if (viewAt == std::string_view::npos) {
                continue;  // a by-value or by-argument use: legal, and the common case
            }
            std::size_t i = viewAt + std::string_view("std::string_view").size();
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                ++i;
            }
            const std::size_t tokenStart = i;
            while (i < line.size() && isIdentifierByte(line[i])) {
                ++i;
            }
            const std::string_view token = line.substr(tokenStart, i - tokenStart);
            if (token.empty() || token.find("::") != std::string_view::npos) {
                continue;  // a qualified name -- a FUNCTION returning a view, not a local hoisting one
            }
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                ++i;
            }
            if (i < line.size() && (line[i] == '=' || line[i] == '(')) {
                CAPTURE(line);
                ++hoistedViewCount;
            }
        }
    }

    // THE ANTI-VACUITY ARMS. Without them a mis-narrowed predicate is vacuously true for every tree in
    // the language (HE17's shape): the scan really read files, `.root()` really does appear, and BOTH
    // named controls were reached and classified legal.
    REQUIRE(filesScanned > 20U);
    REQUIRE(linesScanned > 100U);
    REQUIRE(rootMentions > 0U);
    CHECK(sawViewReturningFunction);
    CHECK(sawStringCopyOfRoot);
    CHECK(hoistedViewCount == 0U);
}

TEST_CASE("scene_containment: editor/src never spells the permissive context (CN20, D12)") {
    // `SceneFileContext{}` is an easy thing to type without thinking, so the permissive value has a
    // NAME and this case asserts that no production file uses it. A production site taking the
    // permissive arm is therefore a FAILING TEST rather than something a code-review round has to
    // notice. The scan is over EVERY tracked editor/src/*.cpp, not only scene_session.cpp -- a future
    // panel calling openSceneFile is exactly the site this protects.
    constexpr std::string_view TOKEN = "NO_PROJECT_SCENE_CONTEXT";
    const std::filesystem::path srcDir(AERO_EDITOR_SRC_DIR);
    REQUIRE(std::filesystem::is_directory(srcDir));
    // DERIVED, not a second compile definition: AERO_EDITOR_SRC_DIR is <src>/editor/src, so two steps
    // up and back down is tests/editor. The case asserts the derived directory exists AND holds this
    // very file, so a repo layout change is a clear failure rather than a silent zero.
    const std::filesystem::path testsDir = srcDir.parent_path().parent_path() / "tests" / "editor";
    REQUIRE(std::filesystem::is_directory(testsDir));
    REQUIRE(std::filesystem::is_regular_file(testsDir / "scene_containment_test.cpp"));

    const auto countIn = [&TOKEN](const std::filesystem::path& dir, std::size_t& files) {
        std::size_t hits = 0;
        std::error_code ec;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".cpp") {
                continue;
            }
            const std::u8string bytes = entry.path().u8string();
            const std::string pathUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const engine::editor::FileReadResult read = engine::editor::readTextFile(pathUtf8);
            REQUIRE(read.text.has_value());
            ++files;
            for (const std::string_view raw : splitLines(*read.text)) {
                if (stripLineComment(raw).find(TOKEN) != std::string_view::npos) {
                    ++hits;
                }
            }
        }
        return hits;
    };

    std::size_t productionFiles = 0;
    std::size_t testFiles = 0;
    const std::size_t productionHits = countIn(srcDir, productionFiles);
    const std::size_t testHits = countIn(testsDir, testFiles);

    REQUIRE(productionFiles > 20U);
    REQUIRE(testFiles > 20U);
    CHECK(productionHits == 0U);
    // ★ THE ANTI-VACUITY HALF, and it is the point: without it a one-character typo in the scanned
    //   token makes `productionHits == 0` true for every tree in the language. A loose bound, so
    //   adding a test does not redden it, and tight enough that a typo cannot hide behind it.
    CHECK(testHits > 20U);
}
