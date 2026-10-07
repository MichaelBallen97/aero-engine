// tests/editor/status_bar_test.cpp -- task E.6.2: the status bar's pure half (FT1-FT15). A TU of
// aero_editor_shell_test. Tier 0, every configuration, no #if. Expected strings are INDEPENDENT literals with every
// non-ASCII byte spelled as a hex escape; the shell metrics (padX 14, zoneGap 20) are restated literals.
#include <aero/editor/asset_watcher.hpp>
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/status_bar.hpp>
#include <aero/editor/text_file.hpp>  // readTextFile (FT12, FT15)

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>  // the 0.4.1 MSVC trap
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using ed::WatchState;

[[nodiscard]] float bytesX8(std::string_view t) { return 8.0F * static_cast<float>(t.size()); }
[[nodiscard]] float bytesX16(std::string_view t) { return 16.0F * static_cast<float>(t.size()); }
[[nodiscard]] bool fitsBytes(std::string_view t, std::size_t budget) { return t.size() <= budget; }

// The comment-stripped lines of a file -- editorSourceCodeLines' rule, copied file-locally (the TH10 precedent).
[[nodiscard]] std::vector<std::string> codeLinesIn(std::string_view path) {
    const ed::FileReadResult read = ed::readTextFile(path);
    REQUIRE(read.text.has_value());
    std::vector<std::string> code;
    std::string_view text = *read.text;
    while (true) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
        const std::size_t comment = line.find("//");
        code.emplace_back(comment == std::string_view::npos ? line : line.substr(0, comment));
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1U);
    }
    return code;
}

[[nodiscard]] std::size_t linesWith(const std::vector<std::string>& code, std::string_view needle) {
    std::size_t n = 0;
    for (const std::string& line : code) {
        n += line.find(needle) != std::string::npos ? 1U : 0U;
    }
    return n;
}

}  // namespace

TEST_CASE("status bar: the watcher's precedence, each arm with every LOWER flag set (task E.6.2, FT1)") {
    ed::WatchStatus all{};
    all.enabled = false;
    all.rootUnreadable = true;
    all.truncated = true;
    all.deferredSweeps = 3;
    all.unreadableDirs = 7;
    CHECK((ed::classifyWatchState(true, all) == WatchState::Off));
    all.enabled = true;
    CHECK((ed::classifyWatchState(true, all) == WatchState::Unreadable));
    all.rootUnreadable = false;
    CHECK((ed::classifyWatchState(true, all) == WatchState::Partial));
    all.truncated = false;
    CHECK((ed::classifyWatchState(true, all) == WatchState::Settling));  // seed S11 swaps this with Partial
    all.deferredSweeps = 0;
    CHECK((ed::classifyWatchState(true, all) == WatchState::SomeUnreadable));
    all.unreadableDirs = 0;
    CHECK((ed::classifyWatchState(true, all) == WatchState::Watching));
}

TEST_CASE("status bar: no project, auto-refresh off and on are three states (task E.6.2, FT2)") {
    ed::WatchStatus st{};  // enabled == true, phase == Disabled: what a no-project watcher reports
    REQUIRE(st.enabled);
    REQUIRE((st.phase == ed::WatchPhase::Disabled));
    CHECK((ed::classifyWatchState(false, st) == WatchState::NoProject));
    st.enabled = false;
    CHECK((ed::classifyWatchState(false, st) == WatchState::NoProject));  // seed S10 reads Off here
    CHECK((ed::classifyWatchState(true, st) == WatchState::Off));
    st.enabled = true;
    CHECK((ed::classifyWatchState(true, st) == WatchState::Watching));
}

TEST_CASE("status bar: the watcher's words, every state, with the count (task E.6.2, FT3)") {
    CHECK(ed::watchStateText(WatchState::NoProject, 0U, 5U).empty());
    CHECK(ed::watchStateText(WatchState::Off, 0U, 1284U) == "auto-refresh off \xC2\xB7 1 284 assets");
    CHECK(ed::watchStateText(WatchState::Unreadable, 0U, 2U) == "watch paused \xC2\xB7 2 assets");
    CHECK(ed::watchStateText(WatchState::Partial, 0U, 2U) == "watching (partial) \xC2\xB7 2 assets");
    CHECK(ed::watchStateText(WatchState::Settling, 0U, 2U) == "settling \xC2\xB7 2 assets");
    CHECK(ed::watchStateText(WatchState::SomeUnreadable, 2U, 3U) == "watching (2 unreadable) \xC2\xB7 3 assets");
    CHECK(ed::watchStateText(WatchState::Watching, 0U, 2U) == "watching \xC2\xB7 2 assets");
    CHECK(ed::watchStateText(WatchState::Watching, 0U, 1U) == "watching \xC2\xB7 1 asset");  // the singular
}

TEST_CASE("status bar: groupThousands counts from the right (task E.6.2, FT4)") {
    CHECK(ed::groupThousands(0U) == "0");
    CHECK(ed::groupThousands(7U) == "7");
    CHECK(ed::groupThousands(999U) == "999");
    CHECK(ed::groupThousands(1000U) == "1 000");
    CHECK(ed::groupThousands(1284U) == "1 284");
    CHECK(ed::groupThousands(50000U) == "50 000");
    CHECK(ed::groupThousands(1234567U) == "1 234 567");  // seed S12 reads "123 456 7"
    CHECK(ed::groupThousands(std::numeric_limits<std::uint64_t>::max()) == "18 446 744 073 709 551 615");
}

TEST_CASE("status bar: assetCountText (task E.6.2, FT5)") {
    CHECK(ed::assetCountText(0U) == "0 assets");
    CHECK(ed::assetCountText(1U) == "1 asset");
    CHECK(ed::assetCountText(2U) == "2 assets");
    CHECK(ed::assetCountText(1284U) == "1 284 assets");
}

TEST_CASE("status bar: home becomes ~ by a SEGMENT-WISE prefix (task E.6.2, FT6)") {
    CHECK(ed::abbreviateHome("/Users/michael", "/Users/michael") == "~");
    CHECK(ed::abbreviateHome("/Users/michael/dev/Game", "/Users/michael") == "~/dev/Game");
    CHECK(ed::abbreviateHome("/Users/michael/dev/Game", "/Users/michael/") == "~/dev/Game");  // trailing separator
    CHECK(ed::abbreviateHome("/Users/michael/x", "/Users/mi") == "/Users/michael/x");         // seed S13's sibling
    CHECK(ed::abbreviateHome("C:\\Users\\me\\x", "C:\\Users\\me\\") == "~\\x");               // the backslash form
    CHECK(ed::abbreviateHome("/Users/michael/x", "") == "/Users/michael/x");                  // no home: no rule
    CHECK(ed::abbreviateHome("C:\\Users\\Me\\x", "c:\\users\\me") == "C:\\Users\\Me\\x");     // case-sensitive
    CHECK(ed::abbreviateHome("/opt/x", "/Users/michael") == "/opt/x");
}

TEST_CASE("status bar: elidePathLeft keeps the last segment whole and never splits a code point (task E.6.2, FT7)") {
    const auto within = [](std::size_t budget) {
        return [budget](std::string_view t) { return fitsBytes(t, budget); };
    };
    CHECK(ed::elidePathLeft("/a/b/Game", within(64)) == "/a/b/Game");  // fits: verbatim
    // "/Users/jos" U+00E9 "/" + six Cyrillic letters (12 bytes): the leaf; ellipsis is 3 bytes.
    const std::string root = "/Users/jos\xC3\xA9/\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82";
    CHECK(ed::elidePathLeft(root, within(20)) == AERO_GLYPH_ELLIPSIS
          "os\xC3\xA9/\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82");
    // Budget 17: a 14-byte suffix would START on U+00E9's continuation byte -> the longest VALID one is 13.
    const std::string at17 = ed::elidePathLeft(root, within(17));
    CHECK(at17 == AERO_GLYPH_ELLIPSIS "/\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82");
    REQUIRE(at17.size() > 3U);
    CHECK((static_cast<unsigned char>(at17[3]) & 0xC0U) != 0x80U);  // the byte after the ellipsis leads (seed S53)
    // Below the floor (ellipsis + leaf, 15 bytes): the leaf right-elided.
    const std::string low = ed::elidePathLeft(root, within(9));
    CHECK(low.size() <= 9U);
    CHECK(low.ends_with(AERO_GLYPH_ELLIPSIS));
    // A budget BELOW the ellipsis (spec FT7): the ellipsis alone, which does not fit -- the header's stated
    // contract; statusBarLayout never asks for it (its `budget >= w(ellipsis)` guards).
    CHECK(ed::elidePathLeft(root, within(2)) == std::string(AERO_GLYPH_ELLIPSIS));
}

TEST_CASE("status bar: backendDisplayName (task E.6.2, FT8)") {
    CHECK(ed::backendDisplayName("metal") == "Metal");
    CHECK(ed::backendDisplayName("vulkan") == "Vulkan");
    CHECK(ed::backendDisplayName("direct3d12") == "Direct3D 12");
    CHECK(ed::backendDisplayName("gnm") == "gnm");  // unknown: verbatim
    CHECK(ed::backendDisplayName("").empty());
}

TEST_CASE("status bar: the frame readout is a windowed average of the raw delta (task E.6.2, FT9)") {
    SUBCASE("the asymmetric window: 8 x 1/64 then 12 x 1/32 is exactly 0.5 s over 20 frames") {
        ed::FrameTimeReadout r;
        for (int i = 0; i < 8; ++i) {
            r.add(1.0F / 64.0F);
        }
        for (int i = 0; i < 11; ++i) {
            r.add(1.0F / 32.0F);
        }
        CHECK_FALSE(r.published());  // 19 frames, 0.46875 s
        r.add(1.0F / 32.0F);
        REQUIRE(r.published());
        CHECK(r.fps() == 40.0F);           // 1/lastDelta reads 32, a mean of per-frame fps 44.8 (seed S15)
        CHECK(r.milliseconds() == 25.0F);  // 1000 x lastDelta reads 31.25
    }
    SUBCASE("a steady 60 Hz") {
        ed::FrameTimeReadout r;
        for (int i = 0; i < 31 && !r.published(); ++i) {
            r.add(1.0F / 60.0F);
        }
        REQUIRE(r.published());
        CHECK(r.fps() == doctest::Approx(60.0F).epsilon(1e-4));
        CHECK(r.milliseconds() == doctest::Approx(1000.0F / 60.0F).epsilon(1e-4));
    }
    SUBCASE("non-finite and negative deltas are ignored, never frames") {
        ed::FrameTimeReadout r;
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(), -0.25F}) {
            r.add(bad);
        }
        CHECK_FALSE(r.published());
        for (int i = 0; i < 8; ++i) {
            r.add(1.0F / 64.0F);
        }
        for (int i = 0; i < 12; ++i) {
            r.add(1.0F / 32.0F);
        }
        REQUIRE(r.published());
        CHECK(r.fps() == 40.0F);  // the ignored four counted as nothing
    }
    SUBCASE("one 2-s stall publishes at once; the next window starts clean") {
        ed::FrameTimeReadout r;
        r.add(2.0F);
        REQUIRE(r.published());
        CHECK(r.fps() == 0.5F);
        CHECK(r.milliseconds() == 2000.0F);
        for (int i = 0; i < 8; ++i) {
            r.add(1.0F / 64.0F);
        }
        for (int i = 0; i < 12; ++i) {
            r.add(1.0F / 32.0F);
        }
        CHECK(r.fps() == 40.0F);
        CHECK(r.milliseconds() == 25.0F);
    }
}

TEST_CASE("status bar: the readout's bytes (task E.6.2, FT10)") {
    ed::FrameTimeReadout r;
    CHECK(ed::frameReadoutText(r) == "-- fps \xC2\xB7 -- ms");
    for (int i = 0; i < 8; ++i) {
        r.add(1.0F / 64.0F);
    }
    for (int i = 0; i < 12; ++i) {
        r.add(1.0F / 32.0F);
    }
    CHECK(ed::frameReadoutText(r) == "40 fps \xC2\xB7 25.0 ms");
}

TEST_CASE("status bar: the composed text, with and without a project (task E.6.2, FT11)") {
    const ed::FrameTimeReadout fresh;
    const ed::StatusBarText open =
        ed::statusBarText(true, "~/dev/Game", "/Users/m/dev/Game", WatchState::Watching, 0U, 3U, fresh, "Metal");
    CHECK(open.projectOpen);
    CHECK(open.root == "~/dev/Game");
    CHECK(open.rootTooltip == "/Users/m/dev/Game");
    CHECK(open.watch == "watching \xC2\xB7 3 assets");
    CHECK(open.frame == "-- fps \xC2\xB7 -- ms");
    CHECK(open.backend == "Metal");
    const ed::StatusBarText closed =
        ed::statusBarText(false, "~/x", "/x", WatchState::Watching, 0U, 3U, fresh, "Metal");
    CHECK(closed.root.empty());
    CHECK(closed.watch == "No project open");
    CHECK(ed::NO_PROJECT_STATUS_TEXT == "No project open");
    // The watcher right-elided at a budget whose byte cut falls between \xC2 and \xB7 of the middle dot: with a
    // project and a 1-byte root, the left zone's watcher budget under bytesX8 is chosen so "watching \xC2" + "..."
    // would fit and "watching \xC2\xB7" would not -- the result steps back to "watching " + the ellipsis.
    const ed::StatusBarText mid{.projectOpen = true, .root = "~", .watch = "watching \xC2\xB7 3 assets"};
    // bar = padX 14 + root 8 + gap 20 + watch budget 13 bytes x 8 (104) + gap 20 + padX 14, empty right zone.
    const ed::StatusBarLayout l = ed::statusBarLayout(180.0F, mid, bytesX8, 1.0F);
    CHECK(l.watch == "watching " AERO_GLYPH_ELLIPSIS);
}

TEST_CASE("status bar: the Assets footer adopted the classification and owns no literal (task E.6.2, FT12)") {
    const std::vector<std::string> code = codeLinesIn(AERO_EDITOR_SRC_DIR "/asset_browser_panel.cpp");
    REQUIRE(code.size() > 100U);
    CHECK(linesWith(code, "assetFooterWatchText(") == 1U);
    CHECK(linesWith(code, "classifyWatchState(") == 1U);
    // Seed S37: the footer keeps its own chain -- no condition on the watcher's flags, and none of the six strings.
    CHECK(linesWith(code, "watchStatusPtr->rootUnreadable") == 0U);
    CHECK(linesWith(code, "watchStatusPtr->truncated") == 0U);
    CHECK(linesWith(code, "watchStatusPtr->deferredSweeps > 0") == 0U);
    CHECK(linesWith(code, "!watchStatusPtr->enabled") == 0U);  // :264's checkbox reads it un-negated, legitimately
    // ...whatever the spelling (seed S37d: a chain grown against a local alias). `rootUnreadable` and `deferredSweeps
    // >` have no other reader in the file; the classification is ONE line whose answer feeds only the text.
    CHECK(linesWith(code, "rootUnreadable") == 0U);
    CHECK(linesWith(code, "deferredSweeps >") == 0U);
    CHECK(linesWith(code, "const WatchState footerState = classifyWatchState(true, watch);") == 1U);
    CHECK(linesWith(code, "footerState") == 2U);  // declared, then handed to assetFooterWatchText -- nothing else
    for (const std::string_view literal : {"Auto-refresh off", "Watch paused", "Watching (partial",
                                           "Watching -- settling", "folder(s) unreadable", "\"Watching\""}) {
        CAPTURE(literal);
        CHECK(linesWith(code, literal) == 0U);
    }
}

TEST_CASE("status bar: the footer's six strings, byte for byte (task E.6.2, FT13)") {
    CHECK(ed::assetFooterWatchText(WatchState::NoProject, 3U, 7U).empty());
    CHECK(ed::assetFooterWatchText(WatchState::Off, 3U, 7U) == "Auto-refresh off");
    CHECK(ed::assetFooterWatchText(WatchState::Unreadable, 3U, 7U) == "Watch paused -- assets folder unreadable");
    const std::string partial = ed::assetFooterWatchText(WatchState::Partial, 3U, 7U);
    CHECK(partial == "Watching (partial -- tree exceeds the scan limit)");
    CHECK(ed::assetFooterWatchText(WatchState::Settling, 3U, 7U) == "Watching -- settling (3)");  // seed S38's count
    CHECK(ed::assetFooterWatchText(WatchState::SomeUnreadable, 3U, 7U) == "Watching (7 folder(s) unreadable)");
    CHECK(ed::assetFooterWatchText(WatchState::Watching, 3U, 7U) == "Watching");
    CHECK(ed::assetFooterWatchText(WatchState::Settling, 4294967295U, 0U) == "Watching -- settling (4294967295)");
    for (const WatchState s : {WatchState::Off, WatchState::Unreadable, WatchState::Partial, WatchState::Settling,
                               WatchState::SomeUnreadable, WatchState::Watching}) {
        for (const char c : ed::assetFooterWatchText(s, 3U, 7U)) {
            CHECK(static_cast<unsigned char>(c) < 0x80U);  // 3.1.4's post-merge ASCII rule
        }
    }
}

TEST_CASE("status bar: the layout's fixed order over a 1-dp sweep, at scale 1 and 2 (task E.6.2, FT14)") {
    for (const float s : {1.0F, 2.0F}) {
        CAPTURE(s);
        const ed::TextWidth measure = s == 1.0F ? ed::TextWidth(bytesX8) : ed::TextWidth(bytesX16);
        const float pad = 14.0F * s;
        const float gap = 20.0F * s;
        const ed::StatusBarText text{.projectOpen = true,
                                     .root = "~/dev/SampleProject",
                                     .rootTooltip = "/Users/m/dev/SampleProject",
                                     .watch = "watching \xC2\xB7 1 284 assets",
                                     .frame = "60 fps \xC2\xB7 16.6 ms",
                                     .backend = "Metal"};
        float previousLeftEnd = std::numeric_limits<float>::infinity();
        bool sawRootElided = false;
        bool sawWatchElided = false;
        for (int width = static_cast<int>(1200.0F * s); width >= 0; --width) {
            CAPTURE(width);
            const auto bar = static_cast<float>(width);
            const ed::StatusBarLayout l = ed::statusBarLayout(bar, text, measure, s);
            // (1) the right zone: whole, anchored to the right edge, at every width.
            CHECK(l.frame == text.frame);
            CHECK(l.backend == text.backend);
            CHECK(l.backendX + measure(l.backend) == bar - pad);
            if (!l.leftDrawn) {
                continue;
            }
            // the left zone ends a zone gap before the right one, and only shrinks as the bar does.
            CHECK(l.leftEnd + gap <= l.rightStart);
            // ...and its parts sit where the layout's rule puts them: the root at the padding, the watcher one zone
            // gap after the root, the zone ending where the watcher does.
            if (!l.root.empty()) {
                CHECK(l.rootX == pad);
            }
            if (!l.root.empty() && !l.watch.empty()) {
                CHECK(l.watchX == pad + measure(l.root) + gap);
            }
            if (!l.watch.empty()) {
                CHECK(l.leftEnd == l.watchX + measure(l.watch));
            }
            CHECK(l.leftEnd <= previousLeftEnd);
            previousLeftEnd = l.leftEnd;
            // (2) before (3): the watcher stays whole until the root is at its floor (ellipsis + "SampleProject").
            if (l.watch != text.watch) {
                sawWatchElided = true;
                CHECK((l.root.empty() || measure(l.root) <= measure(AERO_GLYPH_ELLIPSIS "SampleProject")));
            }
            sawRootElided = sawRootElided || (!l.root.empty() && l.root != text.root);
        }
        CHECK(sawRootElided);  // ANTI-VACUITY: the sweep reached each stage
        CHECK(sawWatchElided);
        // INSIDE branch (2)'s band (seed S50): at 600 dp the root's floor + gap + the whole watcher fits
        // (128 + 20 + 192 = 340 <= the 356 left budget, at 8 per byte), so the ROOT is elided to its 18-byte budget
        // and the watcher stays whole. S50 (branch (3) first) drops the root straight to its floor instead.
        // The arithmetic scales exactly at s = 2 (bytesX16); re-derive it from the layout if a metric moves.
        const ed::StatusBarLayout band = ed::statusBarLayout(600.0F * s, text, measure, s);
        CHECK(band.root == AERO_GLYPH_ELLIPSIS "v/SampleProject");
        CHECK(band.watch == text.watch);
        // Degenerate widths: no left zone, finite positions.
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), -3.0F, 0.0F}) {
            const ed::StatusBarLayout l = ed::statusBarLayout(bad, text, measure, s);
            CHECK_FALSE(l.leftDrawn);
            CHECK(std::isfinite(l.frameX));
            CHECK(std::isfinite(l.backendX));
        }
        // No project: "No project open" right-elided, never a root.
        const ed::StatusBarText closed{
            .projectOpen = false,
            .watch = "No project open",
            .frame = "f",
            .backend = "",
        };
        const ed::StatusBarLayout narrow =
            ed::statusBarLayout(pad + (12.0F * 8.0F * s) + gap + (8.0F * s) + pad, closed, measure, s);
        CHECK(narrow.root.empty());
        CHECK(narrow.watch.ends_with(AERO_GLYPH_ELLIPSIS));
    }
}

TEST_CASE("status bar: the chrome draws the layouts' answers and elides nothing itself (task E.6.2, FT15)") {
    const std::vector<std::string> code = codeLinesIn(AERO_EDITOR_SRC_DIR "/shell_chrome_ui.cpp");
    REQUIRE(code.size() > 100U);
    CHECK(linesWith(code, "statusBarLayout(") == 1U);
    CHECK(linesWith(code, "breadcrumbLayout(") == 1U);
    CHECK(linesWith(code, "toolbarLayout(") == 1U);
    CHECK(linesWith(code, "elidePathLeft(") == 0U);  // seed S50's shape: a second elision in the draw code
    CHECK(linesWith(code, "elideCaptionRight(") == 0U);
}
