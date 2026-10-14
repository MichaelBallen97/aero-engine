// tests/editor/asset_browser_chrome_test.cpp -- task E.6.3 (D8): the Assets panel's chrome model, AH1-AH19. A TU of
// aero_editor_shell_test. Tier 0, every configuration, no #if. Widths are RESTATED literals: Compact needs 576, Full
// 782, the Minimal floor is 344 (the arithmetic is in each case). W's search, searchMin and kind are the shipped font
// multiples at the 16-point body size, which AH16 checks.
#include <aero/editor/asset_actions.hpp>  // countRecordsUnder -- the cache's oracle
#include <aero/editor/asset_browser_chrome.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/editor_glyphs.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <ostream>  // the 0.4.1 MSVC trap
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using engine::Vec2;

constexpr ed::AssetHeaderWidths W{110.0F, 192.0F, 96.0F, 128.0F, 90.0F, 70.0F, 120.0F, 24.0F};

struct Span {
    float min;
    float max;
};

// The inline controls' spans, LEFT TO RIGHT, as the layout placed them.
[[nodiscard]] std::vector<Span> inlineSpans(const ed::AssetHeaderLayout& l, const ed::AssetHeaderWidths& w) {
    std::vector<Span> spans{{l.newMaterialX, l.newMaterialX + w.newMaterial}, {l.searchX, l.searchX + l.searchWidth}};
    if (l.kindInline) {
        spans.push_back({l.kindX, l.kindX + w.kind});
    }
    spans.push_back({l.viewToggleX, l.viewToggleX + w.viewToggle});
    if (l.refreshInline) {
        spans.push_back({l.refreshX, l.refreshX + w.refresh});
    }
    if (l.autoRefreshInline) {
        spans.push_back({l.autoRefreshX, l.autoRefreshX + w.autoRefresh});
    }
    spans.push_back({l.overflowX, l.overflowX + w.overflow});
    return spans;
}

[[nodiscard]] bool allFinite(const ed::AssetHeaderLayout& l) {
    const std::array<float, 8> xs{
        l.newMaterialX, l.searchX, l.searchWidth, l.kindX, l.viewToggleX, l.refreshX, l.autoRefreshX, l.overflowX,
    };
    for (const float v : xs) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<ed::AssetRecord> recordsOf(std::vector<std::string> paths) {
    std::sort(paths.begin(), paths.end());  // records() is byte-sorted, and countRecordsUnder relies on it
    std::vector<ed::AssetRecord> records;
    for (std::string& path : paths) {
        ed::AssetRecord record;
        record.relativePath = std::move(path);
        records.push_back(std::move(record));
    }
    return records;
}

[[nodiscard]] float monoX8(std::string_view t) {  // the footer's measurer at advance 8, restated by code points
    float n = 0.0F;
    for (const char c : t) {
        n += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1.0F : 0.0F;
    }
    return 8.0F * n;
}

[[nodiscard]] std::size_t codePointsOf(std::string_view t) {  // the footer's Mono cells, restated
    std::size_t n = 0;
    for (const char c : t) {
        n += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1U : 0U;
    }
    return n;
}

}  // namespace

TEST_CASE("assets chrome: the header's three modes at, below and above each boundary (task E.6.3, AH1)") {
    const auto modeAt = [](float width) { return ed::assetHeaderLayout(0.0F, width, W, 1.0F).mode; };
    CHECK((modeAt(900.0F) == ed::AssetHeaderMode::Full));
    CHECK((modeAt(782.0F) == ed::AssetHeaderMode::Full));  // 576 + 70 + 120 + 2 x 8
    CHECK((modeAt(781.0F) == ed::AssetHeaderMode::Compact));
    CHECK((modeAt(576.0F) == ed::AssetHeaderMode::Compact));  // 110 + 192 + 128 + 90 + 24 + 4 x 8
    CHECK((modeAt(575.0F) == ed::AssetHeaderMode::Minimal));
    CHECK((modeAt(344.0F) == ed::AssetHeaderMode::Minimal));
    const ed::AssetHeaderLayout full = ed::assetHeaderLayout(0.0F, 900.0F, W, 1.0F);
    CHECK(full.kindInline);
    CHECK(full.refreshInline);
    CHECK(full.autoRefreshInline);
    const ed::AssetHeaderLayout compact = ed::assetHeaderLayout(0.0F, 700.0F, W, 1.0F);
    CHECK(compact.kindInline);
    CHECK_FALSE(compact.refreshInline);  // folded into the overflow
    CHECK_FALSE(compact.autoRefreshInline);
    const ed::AssetHeaderLayout minimal = ed::assetHeaderLayout(0.0F, 500.0F, W, 1.0F);
    CHECK_FALSE(minimal.kindInline);
    CHECK_FALSE(minimal.refreshInline);
    CHECK_FALSE(minimal.autoRefreshInline);
    for (const float width : {900.0F, 700.0F, 500.0F, 200.0F}) {
        CAPTURE(width);
        CHECK(ed::assetHeaderLayout(10.0F, width, W, 1.0F).newMaterialX == 10.0F);  // New Material: always, at the left
    }
    // A folded control's x is the overflow's -- finite, never drawn.
    CHECK(minimal.kindX == minimal.overflowX);
    CHECK(compact.refreshX == compact.overflowX);
    // Full right-aligns the cluster: the overflow ends on the row's max.
    CHECK(full.overflowX + W.overflow == 900.0F);
}

TEST_CASE("assets chrome: a 1-point sweep -- monotone, no overlap, inside the row from the floor (task E.6.3, AH2)") {
    const float floor = ed::assetHeaderMinimalFloor(W, 1.0F);
    REQUIRE(floor == 344.0F);
    auto previous = static_cast<int>(ed::AssetHeaderMode::Minimal);  // auto: a bare cast (modernize-use-auto)
    std::size_t belowFloor = 0;
    std::size_t atOrAbove = 0;
    for (int step = 0; step <= 900; ++step) {
        const auto width = static_cast<float>(step);
        CAPTURE(width);
        const ed::AssetHeaderLayout l = ed::assetHeaderLayout(0.0F, width, W, 1.0F);
        REQUIRE(allFinite(l));
        const auto mode = static_cast<int>(l.mode);
        CHECK(mode <= previous);  // Full < Compact < Minimal: a wider row never yields a narrower mode
        previous = mode;
        const std::vector<Span> spans = inlineSpans(l, W);
        for (std::size_t i = 1; i < spans.size(); ++i) {
            CHECK(spans[i - 1].max <= spans[i].min + 1e-3F);  // the order kept, no two inline rects overlapping
        }
        if (width >= floor) {
            ++atOrAbove;
            CHECK(spans.front().min >= -1e-3F);
            CHECK(spans.back().max <= width + 1e-3F);  // never clips at or above the floor (D8)
        } else {
            ++belowFloor;
            CHECK(spans.back().max > width);  // only the right end crosses the row's max
        }
    }
    CHECK(belowFloor == 344U);  // ANTI-VACUITY: both regimes were sampled
    CHECK(atOrAbove == 557U);
}

TEST_CASE("assets chrome: a NaN, negative or zero width is Minimal with finite x everywhere (task E.6.3, AH3)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float width : {nan, -5.0F, 0.0F, inf}) {
        CAPTURE(width);
        const ed::AssetHeaderLayout l = ed::assetHeaderLayout(0.0F, width, W, 1.0F);
        CHECK((l.mode == ed::AssetHeaderMode::Minimal));
        CHECK(allFinite(l));
    }
    ed::AssetHeaderWidths bad = W;
    bad.search = nan;
    bad.kind = -3.0F;
    CHECK(allFinite(ed::assetHeaderLayout(nan, 600.0F, bad, nan)));
    CHECK(std::isfinite(ed::assetHeaderMinimalFloor(bad, nan)));
}

TEST_CASE("assets chrome: scale 2 doubles every x and gap, and a scaled gap flips a mode (task E.6.3, AH4)") {
    ed::AssetHeaderWidths doubled = W;
    const std::array<float*, 8> members{
        &doubled.newMaterial, &doubled.search,  &doubled.searchMin,   &doubled.kind,
        &doubled.viewToggle,  &doubled.refresh, &doubled.autoRefresh, &doubled.overflow,
    };
    for (float* member : members) {
        *member *= 2.0F;
    }
    for (const float width : {900.0F, 700.0F, 500.0F, 344.0F, 300.0F}) {
        CAPTURE(width);
        const ed::AssetHeaderLayout one = ed::assetHeaderLayout(0.0F, width, W, 1.0F);
        const ed::AssetHeaderLayout two = ed::assetHeaderLayout(0.0F, 2.0F * width, doubled, 2.0F);
        CHECK((two.mode == one.mode));
        CHECK(two.newMaterialX == 2.0F * one.newMaterialX);
        CHECK(two.searchX == 2.0F * one.searchX);
        CHECK(two.searchWidth == 2.0F * one.searchWidth);
        CHECK(two.viewToggleX == 2.0F * one.viewToggleX);
        CHECK(two.overflowX == 2.0F * one.overflowX);
        CHECK(two.kindX == 2.0F * one.kindX);
        CHECK(two.refreshX == 2.0F * one.refreshX);
        CHECK(two.autoRefreshX == 2.0F * one.autoRefreshX);
    }
    CHECK(ed::assetHeaderMinimalFloor(doubled, 2.0F) == 2.0F * ed::assetHeaderMinimalFloor(W, 1.0F));
    // FIXED widths, scale 2: the six gaps cost 48 more, so 782 -- Full at scale 1 -- is Compact (needs 830).
    CHECK((ed::assetHeaderLayout(0.0F, 782.0F, W, 1.0F).mode == ed::AssetHeaderMode::Full));
    CHECK((ed::assetHeaderLayout(0.0F, 782.0F, W, 2.0F).mode == ed::AssetHeaderMode::Compact));
}

TEST_CASE("assets chrome: the crumbs -- the root first, segment-wise prefixes, ## verbatim (task E.6.3, AH5)") {
    std::vector<ed::AssetCrumb> crumbs{{"stale", "stale"}};
    ed::assetBreadcrumbCrumbs("assets", "", crumbs);
    REQUIRE(crumbs.size() == 1U);  // cleared first
    CHECK(crumbs[0].label == "assets");
    CHECK(crumbs[0].relative.empty());
    ed::assetBreadcrumbCrumbs("assets", "models/props", crumbs);
    REQUIRE(crumbs.size() == 3U);
    CHECK(crumbs[1].label == "models");
    CHECK(crumbs[1].relative == "models");
    CHECK(crumbs[2].label == "props");
    CHECK(crumbs[2].relative == "models/props");
    ed::assetBreadcrumbCrumbs("assets", "sprites##old/x", crumbs);
    REQUIRE(crumbs.size() == 3U);
    CHECK(crumbs[1].label == "sprites##old");  // drawn, never parsed: the crumb is an InvisibleButton
    CHECK(crumbs[2].relative == "sprites##old/x");
}

TEST_CASE("assets chrome: the items text -- grouped, singular at one, the hint after a middle dot (task E.6.3, AH6)") {
    std::string out = "stale";
    const std::string tail = " " AERO_GLYPH_MIDDLE_DOT " drag into the viewport to place";
    ed::assetItemsText(0U, out);
    CHECK(out == "0 items" + tail);
    ed::assetItemsText(1U, out);
    CHECK(out == "1 item" + tail);
    ed::assetItemsText(96U, out);
    CHECK(out == "96 items" + tail);
    ed::assetItemsText(1284U, out);
    CHECK(out == "1 284 items" + tail);
}

TEST_CASE("assets chrome: the folder glyph -- the mock's proportions, never inverted (task E.6.3, AH7)") {
    const ed::FolderGlyph g = ed::folderGlyphRects(Vec2{0.0F, 0.0F}, Vec2{85.0F, 84.0F});
    CHECK(g.tabMax.x - g.tabMin.x == doctest::Approx(22.0F).epsilon(1e-5));  // the mock: a 22 x 6 tab
    CHECK(g.tabMax.y - g.tabMin.y == doctest::Approx(6.0F).epsilon(1e-5));
    CHECK(g.bodyMax.x - g.bodyMin.x == doctest::Approx(53.0F).epsilon(1e-5));  // over a 53 x 38 body
    CHECK(g.bodyMax.y - g.bodyMin.y == doctest::Approx(38.0F).epsilon(1e-5));
    CHECK(g.tabMax.y == g.bodyMin.y);                                          // the tab directly ABOVE the body
    CHECK(g.tabMin.x == g.bodyMin.x);                                          // on its left edge
    CHECK(g.bodyMin.x + g.bodyMax.x == doctest::Approx(85.0F).epsilon(1e-5));  // the body centred in x
    CHECK(g.tabMin.y >= 0.0F);                                                 // inside the well
    CHECK(g.bodyMax.y <= 84.0F);
    // Degenerate, inverted, non-finite: every corner on one point, never an inverted rect.
    const ed::FolderGlyph flat = ed::folderGlyphRects(Vec2{5.0F, 5.0F}, Vec2{5.0F, 50.0F});
    CHECK((flat.bodyMax == Vec2{5.0F, 5.0F}));
    const ed::FolderGlyph inverted = ed::folderGlyphRects(Vec2{10.0F, 10.0F}, Vec2{0.0F, 0.0F});
    CHECK((inverted.tabMin == Vec2{10.0F, 10.0F}));
    CHECK((inverted.bodyMax == Vec2{10.0F, 10.0F}));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ed::FolderGlyph lost = ed::folderGlyphRects(Vec2{nan, 0.0F}, Vec2{85.0F, 84.0F});
    CHECK((lost.bodyMax == Vec2{0.0F, 0.0F}));
}

TEST_CASE("assets chrome: a folder count equals countRecordsUnder, segment-wise (task E.6.3, AH8)") {
    const std::vector<ed::AssetRecord> records = recordsOf({
        "a.png",
        "tex/a.png",
        "tex/b/c.png",
        "textures/a.png",
        "textures.png",
        "textures/sub/x.png",
        "z/y/x/w.png",
    });
    ed::FolderCountCache cache;
    for (const std::string_view folder :
         {"", "tex", "tex/b", "textures", "textures/sub", "z", "z/y", "z/y/x", "te", "nope", "textures.png"}) {
        CAPTURE(folder);
        CHECK(cache.countUnder(records, 1U, folder) == ed::countRecordsUnder(records, folder));
    }
    CHECK(cache.countUnder(records, 1U, "tex") == 2U);       // never textures/a.png: "tex" is a NAME, not a prefix
    CHECK(cache.countUnder(records, 1U, "textures") == 2U);  // never textures.png
    CHECK(cache.countUnder(records, 1U, "") == 7U);          // the root holds everything
}

TEST_CASE("assets chrome: a moved generation recomputes, and never answers a stale count (task E.6.3, AH9)") {
    const std::vector<ed::AssetRecord> before = recordsOf({"tex/a.png"});
    const std::vector<ed::AssetRecord> after = recordsOf({"tex/a.png", "tex/b.png", "tex/c/d.png"});
    ed::FolderCountCache cache;
    CHECK(cache.countUnder(before, 7U, "tex") == 1U);
    CHECK(cache.recomputations() == 1U);
    CHECK(cache.countUnder(after, 8U, "tex") == 3U);  // seed S23: a cache keyed on nothing would still answer 1
    CHECK(cache.recomputations() == 2U);
    CHECK(cache.countUnder(after, 8U, "tex/c") == 1U);
    CHECK(cache.recomputations() == 2U);
}

TEST_CASE("assets chrome: asking twice at one generation computes once (task E.6.3, AH10)") {
    const std::vector<ed::AssetRecord> records = recordsOf({"a/b.png", "a/c/d.png"});
    ed::FolderCountCache cache;
    CHECK(cache.recomputations() == 0U);
    CHECK(cache.countUnder(records, 3U, "a") == 2U);
    CHECK(cache.countUnder(records, 3U, "a") == 2U);
    CHECK(cache.countUnder(records, 3U, "a/c") == 1U);
    CHECK(cache.recomputations() == 1U);  // three asks, one pass
}

TEST_CASE("assets chrome: the footer's right zone is whole, the left elided from the LEFT (task E.6.3, AH11)") {
    ed::AssetFooterText text;
    text.selection = "models/props/crate_a.gltf";  // 25 code points: 200 at advance 8
    text.thumbnailsReady = 96U;
    text.thumbnailsBudget = 256U;
    text.watcher = "watching";
    // Right zone: "thumbnails 96/256" (17 x 8 = 136) + 16 + "watching" (64) = 216, so in a 392-wide row it starts at
    // 176, and the left zone has 176 - 16 = 160 = 20 code points: the ellipsis and "/props/crate_a.gltf" exactly.
    const ed::AssetFooterLayout l = ed::assetFooterLayout(0.0F, 392.0F, text, 8.0F, 1.0F);
    REQUIRE(l.right.size() == 2U);
    CHECK(l.right[0].text == "thumbnails 96/256");
    CHECK(l.right[0].x == 176.0F);
    CHECK(l.right[1].text == "watching");
    CHECK(l.right[1].x + monoX8(l.right[1].text) == 392.0F);  // right-aligned, whole
    CHECK(l.left == AERO_GLYPH_ELLIPSIS "/props/crate_a.gltf");
    CHECK(l.leftX == 0.0F);
    // A row too narrow for the right zone (task E.6.3's code-review round; it used to cross rowMinX whole). Round 2:
    // with a selection a segment stays only where the row also holds one gap and the selection's cells for it, up to
    // ASSET_FOOTER_SELECTION_MIN_CELLS -- all 16 here, as this one is 25 (16 + 128 = 144) -- so at 100 the thumbnails
    // AND the watcher go (64 + 144 = 208 > 100) -- before, the watcher stayed and left the selection two cells -- and
    // the selection takes the row: the name beside the ellipsis is 13 cells (104), so the name alone (96) is drawn.
    const ed::AssetFooterLayout narrow = ed::assetFooterLayout(0.0F, 100.0F, text, 8.0F, 1.0F);
    CHECK(narrow.right.empty());
    CHECK(narrow.left == "crate_a.gltf");
    // At 240 the thumbnails go first (216 + 144 = 360 > 240) and the watcher stays (208 fits), right-aligned, so the
    // selection keeps 240 - 64 - 16 = 160: twenty cells, as in the 392 row.
    const ed::AssetFooterLayout watched = ed::assetFooterLayout(0.0F, 240.0F, text, 8.0F, 1.0F);
    REQUIRE(watched.right.size() == 1U);
    CHECK(watched.right[0].text == "watching");
    CHECK(watched.right[0].x + monoX8(watched.right[0].text) == 240.0F);
    CHECK(watched.left == AERO_GLYPH_ELLIPSIS "/props/crate_a.gltf");
    // With the listing's notice, 360 alone and wider than the row: the watcher goes too, the notice is anchored AT
    // rowMinX and never left of it, and what follows it overflows the row's RIGHT end, where the window clips it.
    ed::AssetFooterText capped = text;
    capped.notice = "truncated (caps: 10000 listed, 20000 scanned)";  // 45 code points: 360 at advance 8
    capped.matches = "3 matches";                                     // never shed
    const ed::AssetFooterLayout clipped = ed::assetFooterLayout(40.0F, 100.0F, capped, 8.0F, 1.0F);
    REQUIRE(clipped.right.size() == 2U);
    CHECK(clipped.right[0].text == capped.notice);
    CHECK(clipped.right[0].x == 40.0F);
    CHECK(clipped.right[1].text == "3 matches");
    CHECK(clipped.right[1].x == 40.0F + 360.0F + 16.0F);  // whole, in order, after one gap
    for (const ed::AssetFooterSegment& segment : clipped.right) {
        CHECK(segment.x >= 40.0F);
    }
    CHECK(clipped.left.empty());
    // Room for everything: the selection verbatim.
    CHECK(ed::assetFooterLayout(0.0F, 1000.0F, text, 8.0F, 1.0F).left == "models/props/crate_a.gltf");
}

TEST_CASE("assets chrome: the footer's segments, their roles and a bad scale (task E.6.3, AH12)") {
    ed::AssetFooterText text;
    text.notice = "3 skipped";
    text.matches = "12 matches";
    text.issues = 0U;
    text.thumbnailsReady = 96U;
    text.thumbnailsBudget = 256U;
    text.watcher = "Watching";
    ed::AssetFooterLayout l = ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, 1.0F);
    REQUIRE(l.right.size() == 4U);  // "0 issues" omitted
    CHECK(l.right[0].text == "3 skipped");
    CHECK((l.right[0].role == ed::ChromeRole::Warning));
    CHECK(l.right[1].text == "12 matches");
    CHECK((l.right[1].role == ed::ChromeRole::TextFaint));
    CHECK(l.right[2].text == "thumbnails 96/256");
    CHECK(l.right[3].text == "Watching");  // verbatim
    text.issues = 2U;
    l = ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, 1.0F);
    REQUIRE(l.right.size() == 5U);
    CHECK(l.right[2].text == "2 issues");
    CHECK((l.right[2].role == ed::ChromeRole::Warning));
    text.issues = 1U;
    CHECK(ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, 1.0F).right[2].text == "1 issue");
    text.thumbnailsBudget = 0U;
    CHECK(ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, 1.0F).right.size() == 4U);  // no budget, no segment
    // A bad scale is 1: the gaps, and so every x, equal scale 1's.
    const ed::AssetFooterLayout one = ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, 1.0F);
    const ed::AssetFooterLayout bad =
        ed::assetFooterLayout(0.0F, 2000.0F, text, 8.0F, std::numeric_limits<float>::quiet_NaN());
    REQUIRE(bad.right.size() == one.right.size());
    for (std::size_t i = 0; i < one.right.size(); ++i) {
        CHECK(bad.right[i].x == one.right[i].x);
    }
}

TEST_CASE("assets chrome: the pane, the crumb row and the tree row -- the three geometries (task E.6.3, AH13)") {
    // The row SHOWN -- assetBrowserLayout's crumbRowShown; AH18 is the folded arm.
    constexpr bool SHOWN = true;
    const ed::AssetContentsPaneLayout pane = ed::assetContentsPaneLayout(300.0F, 24.0F, 1.0F, 6.0F, 1.0F, SHOWN);
    CHECK(pane.crumbRowHeight == 30.0F);                                       // 24 + 2 x 3
    CHECK(pane.crumbRowHeight + 1.0F + 6.0F + pane.contentsHeight == 300.0F);  // the group IS the pane
    // Never under ImGui's 4 (IMGUI_WINDOW_HARD_MIN_SIZE: it draws no child shorter, and reads <= 0 as "fill"); 1
    // until task E.6.3's code-review round, which ImGui drew 4 tall.
    CHECK(ed::assetContentsPaneLayout(20.0F, 24.0F, 1.0F, 6.0F, 1.0F, SHOWN).contentsHeight == 4.0F);
    CHECK(ed::assetContentsPaneLayout(600.0F, 48.0F, 2.0F, 12.0F, 2.0F, SHOWN).crumbRowHeight == 60.0F);  // doubles
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // A bad scale is 1.
    CHECK(ed::assetContentsPaneLayout(300.0F, 24.0F, 1.0F, 6.0F, nan, SHOWN).crumbRowHeight == 30.0F);
    CHECK(ed::assetContentsPaneLayout(nan, nan, nan, nan, 1.0F, SHOWN).contentsHeight == 4.0F);
    // task E.6.3's code-review round: the pane's FIXED chrome, which the panel records for assetBrowserLayout to
    // reserve -- the crumb row + the rule + the spacing, so pane - chrome is the child for any pane above it (AV66).
    CHECK(ed::assetContentsPaneChromeHeight(24.0F, 1.0F, 6.0F, 1.0F) == 37.0F);   // 30 + 1 + 6
    CHECK(ed::assetContentsPaneChromeHeight(48.0F, 2.0F, 12.0F, 2.0F) == 74.0F);  // 60 + 2 + 12: doubles at 2
    CHECK(ed::assetContentsPaneChromeHeight(24.0F, 1.0F, 6.0F, 1.0F) + pane.contentsHeight == 300.0F);
    CHECK(ed::assetContentsPaneChromeHeight(nan, -1.0F, nan, nan) == 6.0F);  // bad lengths count 0, a bad scale 1
    // The crumb row: crumbs from framePadding.x, inkWidth + 4 gaps of itemInnerSpacing.x; the items text right-aligned,
    // never left of the crumbs' end + itemSpacing.x.
    const ed::AssetCrumbRowLayout row = ed::assetCrumbRowLayout(100.0F, 500.0F, 150.0F, 4U, 120.0F, 1.0F);
    CHECK(row.firstCrumbX == 108.0F);
    CHECK(row.crumbGap == 6.0F);
    CHECK(row.labelOffsetY == 3.0F);
    CHECK(row.crumbsEndX == 108.0F + 150.0F + 24.0F);
    CHECK(row.itemsX == 600.0F - 8.0F - 120.0F);
    CHECK(row.itemsClipMaxX == 600.0F);
    const ed::AssetCrumbRowLayout tight = ed::assetCrumbRowLayout(100.0F, 300.0F, 150.0F, 4U, 120.0F, 1.0F);
    CHECK(tight.itemsX == tight.crumbsEndX + 8.0F);  // pushed right, then clipped -- never over a crumb
    // The tree row: the count right-aligned 6 in, the label eliding 8 before it -- dropped before the label would
    // shrink below one body em (16).
    const ed::AssetTreeRowLayout wide = ed::assetTreeRowLayout(20.0F, 220.0F, 30.0F, 1.0F);
    CHECK(wide.countShown);
    CHECK(wide.countX == 220.0F - 6.0F - 30.0F);
    CHECK(wide.labelMaxX == wide.countX - 8.0F);
    const ed::AssetTreeRowLayout narrow = ed::assetTreeRowLayout(20.0F, 79.0F, 30.0F, 1.0F);  // 43 - 8 - 20 = 15 < 16
    CHECK_FALSE(narrow.countShown);
    CHECK(narrow.labelMaxX == 79.0F - 6.0F);
    CHECK(ed::assetTreeRowLayout(20.0F, 80.0F, 30.0F, 1.0F).countShown);  // 44 - 8 - 20 = 16: exactly one em
}

TEST_CASE("assets chrome: the Minimal floor restated; the right end at it and 1 point below (task E.6.3, AH14)") {
    CHECK(ed::assetHeaderMinimalFloor(W, 1.0F) == 110.0F + 96.0F + 90.0F + 24.0F + (3.0F * 8.0F));
    CHECK(ed::assetHeaderMinimalFloor(W, 2.0F) == 110.0F + 96.0F + 90.0F + 24.0F + (3.0F * 16.0F));
    const ed::AssetHeaderLayout at = ed::assetHeaderLayout(0.0F, 344.0F, W, 1.0F);
    CHECK((at.mode == ed::AssetHeaderMode::Minimal));
    CHECK(at.overflowX + W.overflow == doctest::Approx(344.0F).epsilon(0.5 / 344.0));
    CHECK(at.searchWidth == 96.0F);
    const ed::AssetHeaderLayout below = ed::assetHeaderLayout(0.0F, 343.0F, W, 1.0F);
    CHECK(below.overflowX + W.overflow > 343.0F);   // the right end crosses
    CHECK(below.overflowX + W.overflow == 344.0F);  // left-anchored at the controls' own widths
}

TEST_CASE("assets chrome: a narrow footer sheds thumbnails, then the watcher -- never the notice (task E.6.3, AH15)") {
    // The code-review round's shape: the default layout's ~701-point Assets row at the Mono-small advance (6.462), a
    // truncated listing, and the Partial watcher word a truncated listing also produces (the watcher's sweep is cut by
    // the same caps). Widths: the notice 45 cells (290.79), "thumbnails 0/256" 16 (103.392), the watcher 49
    // (316.638), and a 16-point gap between two segments.
    constexpr float ADVANCE = 6.462F;
    constexpr float MIN_X = 8.0F;
    const auto cells = [](float n) { return n * ADVANCE; };
    ed::AssetFooterText text;
    text.selection = "tex/a.png";                                   // 9 cells: 58.158
    text.notice = "truncated (caps: 10000 listed, 20000 scanned)";  // drawFooter's sentence at today's caps
    text.thumbnailsReady = 0U;
    text.thumbnailsBudget = 256U;
    text.watcher = "Watching (partial -- tree exceeds the scan limit)";  // assetFooterWatchText(Partial), verbatim
    // (a) 701: all three are 110 cells + 2 gaps = 742.82, wider than the row, so the thumbnails go; the notice and the
    // watcher are 94 cells + 1 gap = 623.428. Round 2: with a selection each check also counts one gap and the
    // selection's cells, up to ASSET_FOOTER_SELECTION_MIN_CELLS -- this one's nine, 16 + 58.158 = 74.158 -- and
    // 623.428 + 74.158 = 697.586 fits 701, so the watcher stays and the zone is right-aligned. The left zone keeps
    // 701 - 623.428 - 16 = 61.572 -- nine cells, so the short selection is drawn whole, as before round 2.
    const ed::AssetFooterLayout l = ed::assetFooterLayout(MIN_X, 701.0F, text, ADVANCE, 1.0F);
    REQUIRE(l.right.size() == 2U);
    CHECK(l.right[0].text == text.notice);
    CHECK(l.right[1].text == text.watcher);
    CHECK(l.right[0].x >= MIN_X);
    CHECK(l.right[0].x == doctest::Approx(MIN_X + 701.0F - (cells(94.0F) + 16.0F)).epsilon(1e-5));
    CHECK(l.right[1].x + cells(49.0F) == doctest::Approx(MIN_X + 701.0F).epsilon(1e-5));  // right-aligned
    CHECK(l.left == "tex/a.png");
    CHECK(l.leftX == MIN_X);
    // With no selection there is no reserve at all: the same two segments, right-aligned the same way, nothing left.
    ed::AssetFooterText unselected = text;
    unselected.selection.clear();
    const ed::AssetFooterLayout bare701 = ed::assetFooterLayout(MIN_X, 701.0F, unselected, ADVANCE, 1.0F);
    REQUIRE(bare701.right.size() == 2U);
    CHECK(bare701.right[0].text == text.notice);
    CHECK(bare701.right[1].text == text.watcher);
    CHECK(bare701.right[0].x == doctest::Approx(MIN_X + 701.0F - (cells(94.0F) + 16.0F)).epsilon(1e-5));
    CHECK(bare701.right[1].x + cells(49.0F) == doctest::Approx(MIN_X + 701.0F).epsilon(1e-5));  // right-aligned
    CHECK(bare701.left.empty());
    // (b) The thumbnails' boundary with this selection, 742.82 + 74.158 = 816.978 (round 2; 742.82 before, and still
    // without a selection -- AH19 (b); 862.212 with one of 16 cells or more): at 817 nothing is shed, at 816 the
    // thumbnails are.
    const ed::AssetFooterLayout fits = ed::assetFooterLayout(MIN_X, 817.0F, text, ADVANCE, 1.0F);
    REQUIRE(fits.right.size() == 3U);
    CHECK(fits.right[1].text == "thumbnails 0/256");
    CHECK(ed::assetFooterLayout(MIN_X, 816.0F, text, ADVANCE, 1.0F).right.size() == 2U);
    // (c) The watcher's boundary with this selection, 623.428 + 74.158 = 697.586 (round 2; 623.428 before, and still
    // without one): at 698 it stays, at 697 it goes; at 623 the notice is alone, right-aligned.
    CHECK(ed::assetFooterLayout(MIN_X, 698.0F, text, ADVANCE, 1.0F).right.size() == 2U);
    CHECK(ed::assetFooterLayout(MIN_X, 697.0F, text, ADVANCE, 1.0F).right.size() == 1U);
    const ed::AssetFooterLayout alone = ed::assetFooterLayout(MIN_X, 623.0F, text, ADVANCE, 1.0F);
    REQUIRE(alone.right.size() == 1U);
    CHECK(alone.right[0].text == text.notice);
    CHECK(alone.right[0].x == doctest::Approx(MIN_X + 623.0F - cells(45.0F)).epsilon(1e-5));
    CHECK(alone.left == "tex/a.png");
    // (d) Narrower than the notice alone: nothing else to shed, so it is anchored AT rowMinX and the window clips what
    // overflows the row's right end -- never pushed off the left edge, where the sentence's head would vanish first.
    const ed::AssetFooterLayout cut = ed::assetFooterLayout(MIN_X, 250.0F, text, ADVANCE, 1.0F);
    REQUIRE(cut.right.size() == 1U);
    CHECK(cut.right[0].text == text.notice);
    CHECK(cut.right[0].x == MIN_X);
    CHECK(cut.left.empty());
    // (e) The matches and the issues are never shed: they follow the notice whole, in order, right of rowMinX.
    ed::AssetFooterText all = text;
    all.matches = "12 matches";
    all.issues = 2U;
    const ed::AssetFooterLayout kept = ed::assetFooterLayout(MIN_X, 250.0F, all, ADVANCE, 1.0F);
    REQUIRE(kept.right.size() == 3U);
    CHECK(kept.right[0].text == all.notice);
    CHECK(kept.right[0].x == MIN_X);
    CHECK(kept.right[1].text == "12 matches");
    CHECK(kept.right[2].text == "2 issues");
    for (const ed::AssetFooterSegment& segment : kept.right) {
        CHECK(segment.x >= MIN_X);
    }
    // (f) No notice, and neither shed segment fits (16 cells + 1 gap + 49 cells = 436.03, then 316.638, against 300):
    // the right zone is empty and the selection takes the whole row.
    ed::AssetFooterText quiet = text;
    quiet.notice.clear();
    const ed::AssetFooterLayout bare = ed::assetFooterLayout(MIN_X, 300.0F, quiet, ADVANCE, 1.0F);
    CHECK(bare.right.empty());
    CHECK(bare.left == "tex/a.png");
    // (g) No watcher word (a host with no reconciled watcher omits it): the thumbnails are then the LAST segment, and
    // the one shed. The notice, "2 issues" and the thumbnails are 45 + 8 + 16 cells + 2 gaps = 477.878, wider than
    // 400; without the thumbnails 358.486, which fits, so exactly one segment goes and the rest is right-aligned.
    ed::AssetFooterText unwatched = text;
    unwatched.watcher.clear();
    unwatched.issues = 2U;
    REQUIRE(ed::assetFooterLayout(MIN_X, 2000.0F, unwatched, ADVANCE, 1.0F).right.size() == 3U);
    const ed::AssetFooterLayout shed = ed::assetFooterLayout(MIN_X, 400.0F, unwatched, ADVANCE, 1.0F);
    REQUIRE(shed.right.size() == 2U);
    CHECK(shed.right[0].text == unwatched.notice);
    CHECK(shed.right[1].text == "2 issues");
    for (const ed::AssetFooterSegment& segment : shed.right) {
        CHECK(segment.text.find("thumbnails") == std::string::npos);
    }
    CHECK(shed.right[1].x + cells(8.0F) == doctest::Approx(MIN_X + 400.0F).epsilon(1e-5));  // right-aligned
    // (h) The thumbnails alone, wider than the row (16 cells, 103.392, against 80): they go, the right zone is empty,
    // and the selection takes the row whole.
    ed::AssetFooterText thumbnailsOnly;
    thumbnailsOnly.selection = "tex/a.png";
    thumbnailsOnly.thumbnailsBudget = 256U;
    REQUIRE(ed::assetFooterLayout(MIN_X, 2000.0F, thumbnailsOnly, ADVANCE, 1.0F).right.size() == 1U);
    const ed::AssetFooterLayout emptied = ed::assetFooterLayout(MIN_X, 80.0F, thumbnailsOnly, ADVANCE, 1.0F);
    CHECK(emptied.right.empty());
    CHECK(emptied.left == "tex/a.png");
    // (i) The cost the code-review round recorded here, closed in its round 2. 749 (about the default layout's Assets
    // row in a window 1366 wide) used to keep all three, since 742.82 fits, and left the selection 749 - 742.82 - 16 =
    // -9.82, under one cell: not drawn, while "thumbnails 0/256" stayed. With the selection's reserve (its nine cells)
    // the thumbnails go (816.978 > 749) and the watcher stays (697.586 fits), so the selection keeps 749 - 623.428 - 16
    // = 109.572 and is drawn whole -- as at 766, the old band's upper edge. AH19 (a) re-derives all three bands.
    const ed::AssetFooterLayout band = ed::assetFooterLayout(MIN_X, 749.0F, text, ADVANCE, 1.0F);
    REQUIRE(band.right.size() == 2U);
    CHECK(band.right[1].text == text.watcher);
    CHECK(band.left == "tex/a.png");
    CHECK_FALSE(ed::assetFooterLayout(MIN_X, 766.0F, text, ADVANCE, 1.0F).left.empty());
}

TEST_CASE("assets chrome: header, tree pane and size column widths are named font multiples (task E.6.3, AH16)") {
    // D1 (the code-review round's finding 3): the panel sets the search, its Minimal floor and the kind filter to
    // GetFontSize() times these, so a changed multiple moves every mode boundary and the floor -- pinned here, where
    // tier 0 reads them, since I323's identity holds for any value.
    CHECK(ed::ASSET_HEADER_SEARCH_FONT_MULTIPLE == 12.0F);
    CHECK(ed::ASSET_HEADER_SEARCH_MIN_FONT_MULTIPLE == 6.0F);
    CHECK(ed::ASSET_HEADER_KIND_FONT_MULTIPLE == 8.0F);
    // W's restated widths are those multiples at the 16-point body size, so the two cannot drift apart unseen.
    CHECK(W.search == 16.0F * ed::ASSET_HEADER_SEARCH_FONT_MULTIPLE);
    CHECK(W.searchMin == 16.0F * ed::ASSET_HEADER_SEARCH_MIN_FONT_MULTIPLE);
    CHECK(W.kind == 16.0F * ed::ASSET_HEADER_KIND_FONT_MULTIPLE);
    // The tree pane's first-run width and the List size column, moved by D1 from asset_browser_panel.cpp:52-53 at the
    // branch point, where they read 14 and 6. No other tier sees either value; I324 pins where the panel reads them.
    CHECK(ed::ASSET_TREE_PANE_FONT_MULTIPLE == 14.0F);
    CHECK(ed::ASSET_SIZE_COLUMN_FONT_MULTIPLE == 6.0F);
}

TEST_CASE("assets chrome: a recorded frame clears every per-frame count, list and text (task E.6.3, AH17)") {
    // The panel only APPENDS to the lists and BUMPS the counts, so a missing reset grows a list by every folded
    // control, folder tile, footer segment, drop target or crumb, every frame -- and the GPU cases' membership reads
    // stay green on the duplicates (the code-review round's finding 13). Every per-frame member is written, as a drawn
    // frame leaves it.
    ed::AssetChromeRecord record{};
    CHECK(record.framesRecorded == 0U);
    record.headerRects[static_cast<std::size_t>(ed::AssetHeaderControl::Search)] =
        ed::AssetDrawnRect{.drawn = true, .min = Vec2{1.0F, 2.0F}, .max = Vec2{3.0F, 4.0F}};
    record.overflowDrawn.push_back(ed::AssetHeaderControl::Kind);
    record.headerRuleDrawn = true;
    record.crumbsDrawn = 3U;
    record.folderDropTargetsAttached = 7U;
    record.crumbDropTargets = 3U;
    record.folderDropTargetItemIds.push_back(11U);
    record.crumbDropTargetItemIds.push_back(12U);
    record.crumbItemIds.push_back(12U);
    record.itemsText = "4 items";
    record.treeRowsDrawn = 5U;
    record.tilesRinged = 6U;
    record.foldersCounted = 2U;
    record.folderTileCounts.push_back(ed::AssetFolderTileCount{.relative = "models", .count = 4U});
    record.hatchedWells = 1U;
    record.footerLeftText = "models/a.txt";
    record.footerRightTexts.emplace_back("1 issue");
    ed::beginAssetChromeRecordFrame(record);
    CHECK(record.framesRecorded == 1U);
    for (const ed::AssetDrawnRect& rect : record.headerRects) {
        CHECK_FALSE(rect.drawn);
        CHECK(rect.min == Vec2{});
        CHECK(rect.max == Vec2{});
    }
    CHECK(record.overflowDrawn.empty());
    CHECK_FALSE(record.headerRuleDrawn);
    CHECK(record.crumbsDrawn == 0U);
    CHECK(record.folderDropTargetsAttached == 0U);
    CHECK(record.crumbDropTargets == 0U);
    CHECK(record.folderDropTargetItemIds.empty());
    CHECK(record.crumbDropTargetItemIds.empty());
    CHECK(record.crumbItemIds.empty());
    CHECK(record.itemsText.empty());
    CHECK(record.treeRowsDrawn == 0U);
    CHECK(record.tilesRinged == 0U);
    CHECK(record.foldersCounted == 0U);
    CHECK(record.folderTileCounts.empty());
    CHECK(record.hatchedWells == 0U);
    CHECK(record.footerLeftText.empty());
    CHECK(record.footerRightTexts.empty());
    ed::beginAssetChromeRecordFrame(record);
    CHECK(record.framesRecorded == 2U);  // one per frame, never reset
}

// task E.6.3's code-review round, rounds 2 and 3: THE FOLD. Where the pane the Issues list would leave open at its
// one-row floor cannot hold the crumb row's chrome over one frame of contents, assetBrowserLayout answers
// crumbRowShown false and the panel passes it on, so the child takes the WHOLE pane: no row, no rule, no spacing,
// whatever the chrome inputs say, and never under ImGui's 4.
TEST_CASE("assets chrome: a folded pane is its child alone; a shown one is unchanged (task E.6.3, AH18)") {
    constexpr bool FOLDED = false;
    constexpr bool SHOWN = true;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // I231's 640-point window with Issues closed: a 29-point pane, folded -- the open list would leave 7 of the 61 the
    // row needs at 1x (the chrome 37 + one frame 24).
    const ed::AssetContentsPaneLayout folded = ed::assetContentsPaneLayout(29.0F, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED);
    CHECK(folded.crumbRowHeight == 0.0F);
    CHECK(folded.contentsHeight == 29.0F);  // the child IS the pane
    // ... at 2x too, and with any chrome input, healthy or not: a folded pane reads none of them.
    CHECK((ed::assetContentsPaneLayout(29.0F, 48.0F, 2.0F, 12.0F, 2.0F, FOLDED) == folded));
    CHECK((ed::assetContentsPaneLayout(29.0F, nan, -1.0F, inf, nan, FOLDED) == folded));
    // A tall pane folded is still the whole pane -- the mode, not the height, decides.
    CHECK(ed::assetContentsPaneLayout(300.0F, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED).contentsHeight == 300.0F);
    // ImGui's 4 (IMGUI_WINDOW_HARD_MIN_SIZE): never shorter, and never the <= 0 BeginChild reads as "fill".
    for (const float pane : {3.0F, 1.0F, 0.0F, -5.0F, nan, -inf}) {
        CAPTURE(pane);
        const ed::AssetContentsPaneLayout tiny = ed::assetContentsPaneLayout(pane, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED);
        CHECK(tiny.crumbRowHeight == 0.0F);
        CHECK(tiny.contentsHeight == 4.0F);
    }
    CHECK(ed::assetContentsPaneLayout(4.0F, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED).contentsHeight == 4.0F);
    CHECK(ed::assetContentsPaneLayout(4.5F, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED).contentsHeight == 4.5F);
    // The SHOWN arm is AH13's, unchanged: the row, the rule and the spacing above a child of the rest.
    const ed::AssetContentsPaneLayout shown = ed::assetContentsPaneLayout(300.0F, 24.0F, 1.0F, 6.0F, 1.0F, SHOWN);
    CHECK(shown.crumbRowHeight == 30.0F);
    CHECK(shown.contentsHeight == 263.0F);  // 300 - 30 - 1 - 6
    // Anti-vacuity: the two modes really differ at one pane -- the flag is read.
    CHECK_FALSE((ed::assetContentsPaneLayout(300.0F, 24.0F, 1.0F, 6.0F, 1.0F, FOLDED) == shown));
}

// task E.6.3's code-review round, round 2: THE SELECTION'S MINIMUM. With a selection, both shedding checks also count
// one gap and the selection's Mono cells, up to ASSET_FOOTER_SELECTION_MIN_CELLS, for it, so the thumbnails or the
// watcher stay only where the row also holds that much for the selection -- the whole of a shorter one; with none,
// the thresholds are the zone's width alone, as before. The notice, the matches and the issues are still never shed,
// so the one band left is the notice's own. AH15's text, with a long selection: the notice 45 cells (290.79),
// "thumbnails 0/256" 16 (103.392), the watcher 49 (316.638), the gap 16. Round 3 adds the reserve's two units: it
// counts code points, never bytes (e), and its gap is scaled with the zone's (f).
TEST_CASE("assets chrome: a kept footer segment leaves the selection its minimum; a bad row (task E.6.3, AH19)") {
    constexpr float ADVANCE = 6.462F;
    constexpr float MIN_X = 8.0F;
    const auto cells = [](std::size_t n) { return static_cast<float>(n) * ADVANCE; };
    CHECK(ed::ASSET_FOOTER_SELECTION_MIN_CELLS == 16U);  // the ellipsis and a fifteen-character file name
    ed::AssetFooterText text;
    text.selection = "models/props/crate_large_damaged_b.gltf";  // 39 cells, its name 26
    text.notice = "truncated (caps: 10000 listed, 20000 scanned)";
    text.thumbnailsBudget = 256U;
    text.watcher = "Watching (partial -- tree exceeds the scan limit)";
    ed::AssetFooterText unselected = text;
    unselected.selection.clear();

    // (a) The three bands the round measured at this text, where the selection was not drawn while a segment that can
    // be shed stayed: 742.82-765.28 (all three kept), 623.43-645.89 (the notice and the watcher) and 290.79-313.25
    // (the notice alone). Re-derived for the reserve (one gap and 16 cells, 119.392, as this selection is longer): the
    // thumbnails now need 862.212 and the watcher 742.82. At 749 the thumbnails go and the watcher stays, leaving the
    // selection 749 - 623.428 - 16 = 109.572: 16 cells, the name's front and the ellipsis. At 630 the watcher goes too,
    // and the notice alone leaves 630 - 290.79 - 16 = 323.21: the path whole.
    struct Band {
        float width;
        std::size_t kept;  // the notice, then the watcher where it stays
        std::string_view left;
    };
    const std::array<Band, 2> bands{
        Band{749.0F, 2U, "crate_large_dam" AERO_GLYPH_ELLIPSIS},
        Band{630.0F, 1U, text.selection},
    };
    for (const Band& band : bands) {
        CAPTURE(band.width);
        const ed::AssetFooterLayout l = ed::assetFooterLayout(MIN_X, band.width, text, ADVANCE, 1.0F);
        CHECK(codePointsOf(l.left) >= ed::ASSET_FOOTER_SELECTION_MIN_CELLS);
        CHECK(l.left == band.left);
        REQUIRE_FALSE(l.right.empty());
        CHECK(l.right.front().text == text.notice);
        CHECK(l.right.size() == band.kept);
        for (const ed::AssetFooterSegment& segment : l.right) {
            CHECK(segment.text.find("thumbnails") == std::string::npos);
        }
        // The selection's zone ends one gap before the notice, and its cells fit it.
        CHECK(cells(codePointsOf(l.left)) <= l.right.front().x - 16.0F - MIN_X);
    }
    // The third band is the notice's and stays: the notice is never shed, so a row less than one gap and one cell
    // wider than it (313.252) leaves the selection nothing -- at 300 the zone is 300 - 290.79 - 16 = -6.79. Both
    // segments that can be shed went long before.
    const ed::AssetFooterLayout noticeBand = ed::assetFooterLayout(MIN_X, 300.0F, text, ADVANCE, 1.0F);
    REQUIRE(noticeBand.right.size() == 1U);
    CHECK(noticeBand.right[0].text == text.notice);
    CHECK(noticeBand.left.empty());
    // A quarter-point sweep, 250 to 1000: wherever the watcher or the thumbnails stay, the selection draws at least the
    // minimum; they stay at the 1029 points from 743 (the watcher's 742.82) up; and the last width with no selection
    // drawn is 313.25, the notice's band -- before round 2 it was 765.25, the top of the first band.
    std::size_t keptPoints = 0;
    std::size_t shortSelections = 0;
    float lastEmpty = 0.0F;
    for (int step = 0; step <= 3000; ++step) {
        const float width = 250.0F + (0.25F * static_cast<float>(step));
        const ed::AssetFooterLayout l = ed::assetFooterLayout(MIN_X, width, text, ADVANCE, 1.0F);
        if (l.right.size() > 1U) {  // the notice leads; anything after it here is the watcher or the thumbnails
            ++keptPoints;
            shortSelections += codePointsOf(l.left) < ed::ASSET_FOOTER_SELECTION_MIN_CELLS ? 1U : 0U;
        }
        if (l.left.empty()) {
            lastEmpty = width;
        }
    }
    CHECK(keptPoints == 1029U);
    CHECK(shortSelections == 0U);
    CHECK(lastEmpty == 313.25F);

    // (b) No selection, no reserve: the thresholds are the right zone's width alone, exactly as before round 2 -- all
    // three from 742.82, the watcher from 623.428. With the selection the same widths shed one segment more, and the
    // reserve's exact size (16 + 103.392) puts the thumbnails' threshold at 862.212 and the watcher's at 742.82.
    const auto keptAt = [](float width, const ed::AssetFooterText& t) {
        return ed::assetFooterLayout(MIN_X, width, t, ADVANCE, 1.0F).right.size();
    };
    CHECK(keptAt(742.83F, unselected) == 3U);
    CHECK(keptAt(742.81F, unselected) == 2U);
    CHECK(keptAt(623.44F, unselected) == 2U);
    CHECK(keptAt(623.42F, unselected) == 1U);
    CHECK(keptAt(862.22F, text) == 3U);
    CHECK(keptAt(862.20F, text) == 2U);
    CHECK(keptAt(742.83F, text) == 2U);
    CHECK(keptAt(742.81F, text) == 1U);

    // (c) A row width that is not a finite, positive number is 0 (usableLength): both checks shed, the notice is
    // anchored AT rowMinX, every x is finite and the selection gets nothing. A check reading the raw width would keep
    // all three at NaN or +inf. A NaN rowMinX is 0.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const ed::AssetFooterText& t : {text, unselected}) {
        for (const float width : {nan, -5.0F, 0.0F, inf}) {
            CAPTURE(width);
            CAPTURE(t.selection);
            const ed::AssetFooterLayout l = ed::assetFooterLayout(MIN_X, width, t, ADVANCE, 1.0F);
            REQUIRE(l.right.size() == 1U);
            CHECK(l.right[0].text == t.notice);
            CHECK(l.right[0].x == MIN_X);
            CHECK(std::isfinite(l.right[0].x));
            CHECK(std::isfinite(l.leftX));
            CHECK(l.left.empty());
        }
    }
    const ed::AssetFooterLayout noMinX = ed::assetFooterLayout(nan, 100.0F, text, ADVANCE, 1.0F);
    REQUIRE(noMinX.right.size() == 1U);
    CHECK(noMinX.right[0].text == text.notice);
    CHECK(noMinX.right[0].x == 0.0F);
    CHECK(noMinX.leftX == 0.0F);

    // (d) A selection shorter than the minimum reserves its own cells and no more, so a segment is never shed where the
    // row holds it, one gap and the WHOLE selection -- a flat 16 cells would shed the watcher at 701 beside a
    // nine-cell selection that fits there whole (AH15 (a)). With AH15's "tex/a.png" (58.158) the watcher needs
    // 623.428 + 16 + 58.158 = 697.586: at 698 it stays and the selection is drawn whole; at 697 it goes.
    ed::AssetFooterText nine = text;
    nine.selection = "tex/a.png";
    const ed::AssetFooterLayout at698 = ed::assetFooterLayout(MIN_X, 698.0F, nine, ADVANCE, 1.0F);
    // The notice is never shed. CHECKs below it, so the sweep still runs on a miss.
    REQUIRE_FALSE(at698.right.empty());
    CHECK(at698.right.size() == 2U);
    CHECK(at698.right.back().text == text.watcher);
    CHECK(at698.left == "tex/a.png");
    CHECK(keptAt(697.0F, nine) == 1U);
    // Five selections over (a)'s quarter-point sweep. The watcher stays from 623.428 + 16 + min(cells, 16) x 6.462 up,
    // the thumbnails from 119.392 above that (their 103.392 and a gap): for one cell 645.89 and 765.282, nine 697.586
    // and 816.978, twelve 716.972 and 836.364, sixteen -- and so 39 -- 742.82 and 862.212. Each from-width below is
    // the first quarter point at or above its threshold, and every quarter point from there up keeps the segment.
    // Wherever the watcher stays, a selection of up to 16 cells is drawn whole and a longer one keeps 16 cells.
    struct Sweep {
        std::string_view selection;
        std::size_t cells;
        float watcherFrom;
        float thumbnailsFrom;
    };
    const std::array<Sweep, 5> sweeps{
        Sweep{"a", 1U, 646.0F, 765.5F},
        Sweep{"tex/a.png", 9U, 697.75F, 817.0F},
        Sweep{"props/b.gltf", 12U, 717.0F, 836.5F},
        Sweep{"props/crate.gltf", 16U, 743.0F, 862.25F},
        Sweep{text.selection, 39U, 743.0F, 862.25F},
    };
    for (const Sweep& sweep : sweeps) {
        CAPTURE(sweep.selection);
        CHECK(codePointsOf(sweep.selection) == sweep.cells);
        ed::AssetFooterText swept = text;
        swept.selection = std::string(sweep.selection);
        float firstWatcher = 0.0F;
        float firstThumbnails = 0.0F;
        std::size_t watcherPoints = 0;
        std::size_t thumbnailsPoints = 0;
        std::size_t shortPoints = 0;
        for (int step = 0; step <= 3000; ++step) {
            const float width = 250.0F + (0.25F * static_cast<float>(step));
            const ed::AssetFooterLayout l = ed::assetFooterLayout(MIN_X, width, swept, ADVANCE, 1.0F);
            if (l.right.size() > 1U) {  // the watcher stays (it is shed after the thumbnails)
                firstWatcher = watcherPoints == 0U ? width : firstWatcher;
                ++watcherPoints;
                const bool whole = l.left == swept.selection;
                const bool full = codePointsOf(l.left) >= ed::ASSET_FOOTER_SELECTION_MIN_CELLS;
                shortPoints += (sweep.cells <= ed::ASSET_FOOTER_SELECTION_MIN_CELLS ? whole : full) ? 0U : 1U;
            }
            if (l.right.size() == 3U) {
                firstThumbnails = thumbnailsPoints == 0U ? width : firstThumbnails;
                ++thumbnailsPoints;
            }
        }
        CHECK(firstWatcher == sweep.watcherFrom);
        CHECK(watcherPoints == static_cast<std::size_t>((1000.0F - sweep.watcherFrom) * 4.0F) + 1U);
        CHECK(firstThumbnails == sweep.thumbnailsFrom);
        CHECK(thumbnailsPoints == static_cast<std::size_t>((1000.0F - sweep.thumbnailsFrom) * 4.0F) + 1U);
        CHECK(shortPoints == 0U);
    }

    // (e) The reserve counts CODE POINTS -- the Mono cells drawn -- never bytes (round 3). The footer's selections
    // carry multi-byte glyphs: a file whose size is unknown appends "  -  " and UNKNOWN_SIZE, an em dash, and the
    // GUID segment splices an ellipsis. "a" with an unknown size is 7 cells in 9 bytes, so the watcher stays from
    // 623.428 + 16 + 7 x 6.462 = 684.662; a byte count would move it to 697.586 and shed the watcher at 690.
    ed::AssetFooterText glyph = text;
    glyph.selection = "a  -  " AERO_GLYPH_EM_DASH;
    REQUIRE(codePointsOf(glyph.selection) == 7U);
    REQUIRE(glyph.selection.size() == 9U);  // ANTI-VACUITY: the bytes and the cells differ
    const ed::AssetFooterLayout at690 = ed::assetFooterLayout(MIN_X, 690.0F, glyph, ADVANCE, 1.0F);
    REQUIRE_FALSE(at690.right.empty());
    CHECK(at690.right.size() == 2U);
    CHECK(at690.right.back().text == text.watcher);
    CHECK(at690.left == glyph.selection);  // drawn whole beside it
    CHECK(keptAt(684.0F, glyph) == 1U);

    // (f) The reserve's gap is the SCALED one (round 3): at uiScale 2 every gap is 32, the zone's and the reserve's
    // alike. The advance is held at 6.462, so the gap is the only length that moves: the notice and the watcher take
    // 290.79 + 32 + 316.638 = 639.428, the reserve 32 + 103.392 = 135.392, so the watcher stays from 774.82 -- a
    // literal 16 in the reserve would keep it from 758.82, at 770 beside a selection under its 16 cells. The
    // thumbnails stay from 774.82 + 32 + 103.392 = 910.212.
    const auto keptAt2x = [](float width, const ed::AssetFooterText& t) {
        return ed::assetFooterLayout(MIN_X, width, t, ADVANCE, 2.0F).right.size();
    };
    CHECK(keptAt2x(770.0F, text) == 1U);
    CHECK(keptAt2x(775.0F, text) == 2U);
    CHECK(keptAt2x(910.0F, text) == 2U);
    CHECK(keptAt2x(911.0F, text) == 3U);
    const ed::AssetFooterLayout at775 = ed::assetFooterLayout(MIN_X, 775.0F, text, ADVANCE, 2.0F);
    REQUIRE_FALSE(at775.right.empty());
    CHECK(at775.right.back().text == text.watcher);
    CHECK(codePointsOf(at775.left) == ed::ASSET_FOOTER_SELECTION_MIN_CELLS);  // 775 - 639.428 - 32 = 103.572
    CHECK(at775.right.front().x == doctest::Approx(MIN_X + 775.0F - 639.428F).epsilon(1e-5));  // right-aligned
}
