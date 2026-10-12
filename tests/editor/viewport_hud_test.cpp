// tests/editor/viewport_hud_test.cpp -- task E.6.3 (D9): the Viewport HUD model, VO1-VO14. A TU of
// aero_editor_shell_test. Tier 0, every configuration, no #if. Metrics are RESTATED: inset 14, gap 8, stats 209 wide
// and 129 high at six rows (110 at five).
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/view_axis_gizmo.hpp>
#include <aero/editor/viewport_hud.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <ostream>  // the 0.4.1 MSVC trap
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using engine::Vec2;

constexpr Vec2 IMAGE_MIN{37.0F, 91.0F};
constexpr Vec2 IMAGE_MAX{837.0F, 691.0F};  // 800 x 600

[[nodiscard]] ed::ViewportHudInputs inputs() {
    ed::ViewportHudInputs in;
    in.chipSize = Vec2{140.0F, 24.0F};
    in.statsContentSize = Vec2{180.0F, 0.0F};
    in.hintSizes = {Vec2{112.0F, 24.0F}, Vec2{80.0F, 24.0F}};
    in.flyChipSize = Vec2{90.0F, 24.0F};
    in.statsVisible = true;
    in.flying = false;
    in.viewAxisVisible = true;
    in.statsRows = 6;
    return in;
}

[[nodiscard]] Vec2 imageMaxOf(float width, float height) { return Vec2{IMAGE_MIN.x + width, IMAGE_MIN.y + height}; }

[[nodiscard]] bool overlap(const ed::HudRect& a, const ed::HudRect& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

}  // namespace

TEST_CASE("viewport hud: an 800 x 600 image -- chip, stats and hints in their corners, 14 in (task E.6.3, VO1)") {
    const ed::ViewportHudLayout l = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, inputs(), 1.0F);
    REQUIRE(l.chip.shown);
    REQUIRE(l.stats.shown);
    REQUIRE(l.hints[0].shown);
    REQUIRE(l.hints[1].shown);
    CHECK((l.chip.min == Vec2{51.0F, 105.0F}));  // top-left, 14 in
    CHECK((l.chip.max == Vec2{191.0F, 129.0F}));
    CHECK(l.stats.max.x == 837.0F - 14.0F);  // top-right, 14 in
    CHECK(l.stats.min.y == 91.0F + 14.0F);
    CHECK(l.stats.max.x - l.stats.min.x == 209.0F);  // the content (180) is narrower: the card's own width
    CHECK(l.stats.max.y - l.stats.min.y == 129.0F);  // six rows: 2 x 10 + 5 x 19 + 14
    CHECK(l.hints[0].min.x == 51.0F);                // bottom-left, 14 in
    CHECK(l.hints[0].max.y == 691.0F - 14.0F);
    CHECK(l.hints[1].min.x == l.hints[0].max.x + 8.0F);  // hudGap apart
    CHECK(l.hints[1].max.y == l.hints[0].max.y);
    CHECK_FALSE(l.flyChip.shown);
}

TEST_CASE("viewport hud: flying swaps the hints for the fly chip; Stats off hides the card (task E.6.3, VO2)") {
    ed::ViewportHudInputs in = inputs();
    in.flying = true;
    const ed::ViewportHudLayout flying = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK(flying.flyChip.shown);
    CHECK_FALSE(flying.hints[0].shown);
    CHECK_FALSE(flying.hints[1].shown);
    CHECK(flying.flyChip.min.x == 51.0F);
    CHECK(flying.flyChip.max.y == 677.0F);
    in.flying = false;
    in.statsVisible = false;
    const ed::ViewportHudLayout quiet = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK_FALSE(quiet.stats.shown);
    CHECK(quiet.chip.shown);
}

TEST_CASE("viewport hud: shrinking drops the stats, then the hints, then the chip; no overlap (task E.6.3, VO3)") {
    const ed::ViewportHudInputs in = inputs();
    int statsGoneAt = -1;
    int hintsGoneAt = -1;
    int chipGoneAt = -1;
    for (int side = 800; side >= 0; side -= 2) {
        CAPTURE(side);
        const auto w = static_cast<float>(side);  // auto: a bare cast (modernize-use-auto)
        const float h = 0.75F * w;
        const Vec2 max = imageMaxOf(w, h);
        const ed::ViewportHudLayout l = ed::viewportHudLayout(IMAGE_MIN, max, in, 1.0F);
        Vec2 axisMin{};
        Vec2 axisMax{};
        // The view-axis CIRCLE's box (the code-review round): the circle reaches five past viewAxisRect and draws last.
        ed::viewAxisCircleRect(IMAGE_MIN, Vec2{w, h}, 1.0F, axisMin, axisMax);
        const ed::HudRect axis{axisMin, axisMax, axisMax.x > axisMin.x};
        const std::array<const ed::HudRect*, 4> kept{&l.chip, &l.stats, &l.hints[0], &l.hints[1]};
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (!kept[i]->shown) {
                continue;
            }
            CHECK(kept[i]->min.x >= IMAGE_MIN.x + 14.0F);  // inside the inset image
            CHECK(kept[i]->max.y <= max.y - 14.0F + 1e-3F);
            if (axis.shown) {
                CHECK_FALSE(overlap(*kept[i], axis));  // never under the view-axis widget's circle
            }
            for (std::size_t j = i + 1; j < kept.size(); ++j) {
                if (kept[j]->shown) {
                    CHECK_FALSE(overlap(*kept[i], *kept[j]));  // never over another kept rect
                }
            }
        }
        if (statsGoneAt < 0 && !l.stats.shown) {
            statsGoneAt = side;
        }
        if (hintsGoneAt < 0 && !l.hints[0].shown && !l.hints[1].shown) {
            hintsGoneAt = side;
        }
        if (chipGoneAt < 0 && !l.chip.shown) {
            chipGoneAt = side;
        }
    }
    REQUIRE(statsGoneAt > 0);  // ANTI-VACUITY: every rect really dropped somewhere in the sweep
    REQUIRE(hintsGoneAt > 0);
    REQUIRE(chipGoneAt >= 0);
    CHECK(statsGoneAt > hintsGoneAt);  // the stats card first (at the widest image)...
    CHECK(hintsGoneAt > chipGoneAt);   // ...the hints next, the chip last
}

TEST_CASE("viewport hud: scale 2 doubles every inset and gap; a bad scale is 1 (task E.6.3, VO4)") {
    const ed::ViewportHudInputs one = inputs();
    ed::ViewportHudInputs two = inputs();
    two.chipSize = Vec2{280.0F, 48.0F};
    two.statsContentSize = Vec2{360.0F, 0.0F};
    two.hintSizes = {Vec2{224.0F, 48.0F}, Vec2{160.0F, 48.0F}};
    const Vec2 origin{0.0F, 0.0F};
    const ed::ViewportHudLayout a = ed::viewportHudLayout(origin, Vec2{800.0F, 600.0F}, one, 1.0F);
    const ed::ViewportHudLayout b = ed::viewportHudLayout(origin, Vec2{1600.0F, 1200.0F}, two, 2.0F);
    REQUIRE(a.stats.shown);
    REQUIRE(b.stats.shown);
    CHECK((b.chip.min == Vec2{2.0F * a.chip.min.x, 2.0F * a.chip.min.y}));  // 14 -> 28
    CHECK(b.stats.min.x == 2.0F * a.stats.min.x);                           // 209 -> 418 wide, 14 -> 28 in
    CHECK(b.stats.max.y == 2.0F * a.stats.max.y);                           // 129 -> 258 high
    CHECK(b.hints[1].min.x - b.hints[0].max.x == 2.0F * (a.hints[1].min.x - a.hints[0].max.x));  // 8 -> 16
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), 0.0F, -1.0F}) {
        CAPTURE(bad);
        CHECK((ed::viewportHudLayout(origin, Vec2{800.0F, 600.0F}, one, bad) == a));
    }
}

TEST_CASE("viewport hud: hudOwnsPoint -- shown rects, half-open, NaN owned by nothing (task E.6.3, VO5)") {
    ed::ViewportHudInputs in = inputs();
    const ed::ViewportHudLayout l = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    for (const ed::HudRect* r : {&l.chip, &l.stats, &l.hints[0], &l.hints[1]}) {
        REQUIRE(r->shown);
        CHECK(ed::hudOwnsPoint(l, Vec2{(r->min.x + r->max.x) * 0.5F, (r->min.y + r->max.y) * 0.5F}));
        CHECK(ed::hudOwnsPoint(l, r->min));        // min included
        CHECK_FALSE(ed::hudOwnsPoint(l, r->max));  // max excluded
    }
    in.statsVisible = false;
    const ed::ViewportHudLayout hidden = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    REQUIRE_FALSE(hidden.stats.shown);
    const Vec2 statsCentre{(hidden.stats.min.x + hidden.stats.max.x) * 0.5F,
                           (hidden.stats.min.y + hidden.stats.max.y) * 0.5F};
    CHECK_FALSE(ed::hudOwnsPoint(hidden, statsCentre));  // a HIDDEN rect owns nothing (its geometry is kept)
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(ed::hudOwnsPoint(l, Vec2{nan, nan}));
    CHECK_FALSE(ed::hudOwnsPoint(l, Vec2{l.chip.min.x + 1.0F, nan}));
}

TEST_CASE("viewport hud: the draw-call sum, saturating at SIZE_MAX (task E.6.3, VO6)") {
    const ed::ViewportDrawCounts counts{
        .forward = 5, .shadow = 4, .selectionMask = 1, .debug = 3, .sky = 1, .resolve = 1, .composite = 1};
    CHECK(ed::viewportDrawCalls(counts) == 16U);
    ed::ViewportDrawCounts poisoned = counts;
    poisoned.shadow = std::numeric_limits<std::size_t>::max();
    CHECK(ed::viewportDrawCalls(poisoned) == std::numeric_limits<std::size_t>::max());  // never wraps to small
    CHECK(ed::viewportDrawCalls(ed::ViewportDrawCounts{}) == 0U);
}

TEST_CASE("viewport hud: abbreviateCount, every literal of the contract (task E.6.3, VO7)") {
    struct Row {
        std::uint64_t value;
        std::string_view text;
    };
    std::string out = "stale";
    for (const Row& row :
         {Row{0U, "0"}, Row{999U, "999"}, Row{9999U, "9999"}, Row{10000U, "10 k"}, Row{184000U, "184 k"},
          Row{999499U, "999 k"}, Row{999500U, "999 k"}, Row{999999U, "999 k"}, Row{1000000U, "1.0 M"},
          Row{1200000U, "1.2 M"}, Row{9999999U, "9.9 M"}, Row{999950000U, "999 M"}, Row{999999999U, "999 M"},
          Row{1000000000U, "1.0 G"}, Row{std::numeric_limits<std::uint64_t>::max(), "18446744073 G"}}) {
        CAPTURE(row.value);
        ed::abbreviateCount(row.value, out);
        CHECK(out == row.text);
    }
}

TEST_CASE("viewport hud: the view chip names the projection and the rounded fov (task E.6.3, VO8)") {
    std::string out;
    const float pi = std::numbers::pi_v<float>;
    ed::viewChipLabel(ed::ProjectionMode::Perspective, pi / 3.0F, out);
    CHECK(out == "Perspective 60" AERO_GLYPH_DEGREE);
    ed::viewChipLabel(ed::ProjectionMode::Perspective, 59.6F * pi / 180.0F, out);
    CHECK(out == "Perspective 60" AERO_GLYPH_DEGREE);
    ed::viewChipLabel(ed::ProjectionMode::Perspective, pi / 2.0F, out);
    CHECK(out == "Perspective 90" AERO_GLYPH_DEGREE);
    ed::viewChipLabel(ed::ProjectionMode::Orthographic, pi / 3.0F, out);
    CHECK(out == "Orthographic");
    ed::viewChipLabel(ed::ProjectionMode::Perspective, std::numeric_limits<float>::quiet_NaN(), out);
    CHECK(out == "Perspective");
}

TEST_CASE("viewport hud: the fly-speed text, and a non-finite speed (task E.6.3, VO9)") {
    std::string out;
    ed::flySpeedText(4.0F, out);
    CHECK(out == "fly 4.0 u/s");
    ed::flySpeedText(12.5F, out);
    CHECK(out == "fly 12.5 u/s");
    ed::flySpeedText(std::numeric_limits<float>::infinity(), out);
    CHECK(out == "fly -- u/s");
}

TEST_CASE("viewport hud: the six stats rows, in order, with their values (task E.6.3, VO10)") {
    std::vector<ed::ViewportStatsRow> rows{{"stale", "x"}};
    const ed::FrameTimeReadout unpublished{};
    ed::viewportStatsRows(unpublished, 16U, 184000U, 4U, 1280U, 720U, rows);
    REQUIRE(rows.size() == ed::VIEWPORT_STATS_ROW_COUNT);
    CHECK(rows[0].label == "Frame");
    CHECK(rows[0].value == "-- ms");  // frameReadoutText's ms half before publication
    CHECK(rows[1].label == "Draw calls");
    CHECK(rows[1].value == "16");
    CHECK(rows[2].label == "Triangles");
    CHECK(rows[2].value == "184 k");
    CHECK(rows[3].label == "Entities");
    CHECK(rows[3].value == "4");
    CHECK(rows[4].label == "GPU");
    CHECK(rows[4].value == AERO_GLYPH_EM_DASH);
    CHECK(rows[5].label == "Extent");
    CHECK(rows[5].value == "1280 " AERO_GLYPH_MULTIPLY " 720");
    ed::FrameTimeReadout published;
    published.add(0.6F);  // one delta longer than the window publishes at once (status_bar.hpp)
    REQUIRE(published.published());
    ed::viewportStatsRows(published, 0U, 0U, 0U, 1U, 1U, rows);
    CHECK(rows[0].value == "600.0 ms");
}

TEST_CASE("viewport hud: the hint and tooltip constants, restated, 7-bit (task E.6.3, VO11)") {
    CHECK(ed::HINT_FLY == "RMB + WASD to fly");
    CHECK(ed::HINT_FRAME == "F to frame");
    CHECK(ed::GPU_TIME_UNAVAILABLE_TOOLTIP == "GPU time is not measured: SDL_GPU exposes no timestamp queries");
    CHECK(ed::STATS_STALENESS_TOOLTIP == "Counted on the previous frame");
    for (const std::string_view text :
         {ed::HINT_FLY, ed::HINT_FRAME, ed::GPU_TIME_UNAVAILABLE_TOOLTIP, ed::STATS_STALENESS_TOOLTIP}) {
        for (const char c : text) {
            CHECK(static_cast<unsigned char>(c) < 0x80U);  // GL6's rule: an editor literal stays 7-bit
        }
    }
    CHECK(ed::VIEW_OPTIONS_COMBO_FONT_MULTIPLE == 7.0F);
    CHECK(ed::VIEW_OPTIONS_SLIDER_FONT_MULTIPLE == 10.0F);
}

TEST_CASE("viewport hud: a default record is vacuous -- the guard every GPU case refuses (task E.6.3, VO12)") {
    const ed::ViewportHudRecord r{};
    CHECK(r.framesRecorded == 0U);
    CHECK_FALSE(r.layout.chip.shown);
    CHECK_FALSE(r.layout.stats.shown);
    CHECK_FALSE(r.layout.hints[0].shown);
    CHECK_FALSE(r.layout.flyChip.shown);
    CHECK(r.chipLabel.empty());
    CHECK(r.statsRows.empty());
    CHECK(r.hintTexts.empty());
    CHECK((r.drawCounts == ed::ViewportDrawCounts{}));
    CHECK(r.viewAxisCircleRadius == 0.0F);
    ed::ViewportHudRecord opened;
    opened.chipLabel = "x";
    opened.statsRows.push_back(ed::ViewportStatsRow{"Frame", "1 ms"});
    opened.viewAxisCircleRadius = 43.0F;  // a frame the widget drew, before one it does not
    ed::beginViewportHudRecordFrame(opened);
    CHECK(opened.framesRecorded == 1U);
    CHECK(opened.chipLabel.empty());
    CHECK(opened.statsRows.empty());
    CHECK(opened.viewAxisCircleRadius == 0.0F);  // so I329's anti-vacuity read is THIS frame's
}

TEST_CASE("viewport hud: the stats card is sized by the layout, never by its values (task E.6.3, VO13)") {
    ed::ViewportHudInputs in = inputs();
    in.statsContentSize = Vec2{150.0F, 0.0F};  // "9.9 ms" / "9999"
    const float narrowMin = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F).stats.min.x;
    in.statsContentSize = Vec2{171.0F, 0.0F};  // "10.1 ms" / "10 k"
    const ed::ViewportHudLayout wider = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK(wider.stats.min.x == narrowMin);  // IDENTICAL: the card never moves while its values fit 209
    CHECK(wider.stats.max.x - wider.stats.min.x == 209.0F);
    in.statsContentSize = Vec2{240.0F, 0.0F};  // an unexpectedly wide value: the content wins
    const ed::ViewportHudLayout widest = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK(widest.stats.max.x - widest.stats.min.x == 240.0F);
    in.statsContentSize = Vec2{150.0F, 0.0F};
    in.statsRows = 5;
    const ed::ViewportHudLayout five = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK(five.stats.max.y - five.stats.min.y == 110.0F);  // the mock's card: 2 x 10 + 4 x 19 + 14
    in.statsRows = 6;
    const ed::ViewportHudLayout six = ed::viewportHudLayout(IMAGE_MIN, IMAGE_MAX, in, 1.0F);
    CHECK(six.stats.max.y - six.stats.min.y == 129.0F);  // a whole-pitch-per-row card reads 134 -- red
    ed::ViewportHudInputs doubled = in;
    doubled.statsContentSize = Vec2{300.0F, 0.0F};
    const ed::ViewportHudLayout big = ed::viewportHudLayout(Vec2{0.0F, 0.0F}, Vec2{1600.0F, 1200.0F}, doubled, 2.0F);
    REQUIRE(big.stats.shown);
    CHECK(big.stats.max.y - big.stats.min.y == 258.0F);  // exactly doubled at 2
    CHECK(big.stats.max.x - big.stats.min.x == 418.0F);
}

TEST_CASE("viewport hud: the HUD keeps off the view-axis CIRCLE's box (task E.6.3, VO14)") {
    // The divider circle (viewAxisCircleRadius, 43 at 1) reaches five past viewAxisRect at the middle of each side and
    // is drawn LAST, over the HUD. At scale 1 the six-row card's bottom sits 14 + 129 = 143 below the image's top and
    // the circle's top 23 + 38 + 43 = 104 above its bottom, inside the card's x range: the card must go below a
    // 247-point image. The half-extent rect (99 above the bottom) kept it down to 242, and the arc crossed it.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // 1. The box: viewAxisRect grown by the gap on every side, which IS centre +- the radius (exact at 1 and 2).
    for (const float s : {1.0F, 2.0F}) {
        CAPTURE(s);
        const Vec2 size{900.0F * s, 600.0F * s};
        Vec2 rectMin{};
        Vec2 rectMax{};
        Vec2 boxMin{};
        Vec2 boxMax{};
        ed::viewAxisRect(IMAGE_MIN, size, s, rectMin, rectMax);
        ed::viewAxisCircleRect(IMAGE_MIN, size, s, boxMin, boxMax);
        REQUIRE(rectMax.x > rectMin.x);  // ANTI-VACUITY: the widget is visible at this size
        const float gap = 5.0F * s;      // viewAxisCircleGap, restated
        CHECK((boxMin == Vec2{rectMin.x - gap, rectMin.y - gap}));
        CHECK((boxMax == Vec2{rectMax.x + gap, rectMax.y + gap}));
        const Vec2 centre{(rectMin.x + rectMax.x) * 0.5F, (rectMin.y + rectMax.y) * 0.5F};
        const float radius = ed::viewAxisCircleRadius(s);
        CHECK((boxMin == Vec2{centre.x - radius, centre.y - radius}));
        CHECK((boxMax == Vec2{centre.x + radius, centre.y + radius}));
    }

    // 2. DEGENERATE exactly when viewAxisRect is: too small, non-finite, or a bad scale -- the widget draws nothing
    // there (viewAxisRect's own rule, not US11's "a bad scale is 1"), so its circle owns nothing either.
    struct Hidden {
        Vec2 origin;
        Vec2 size;
        float scale;
    };
    const Vec2 full{900.0F, 600.0F};
    const std::initializer_list<Hidden> hiddenRows{
        Hidden{IMAGE_MIN, Vec2{139.0F, 600.0F}, 1.0F},  // under VIEW_AXIS_MIN_IMAGE_POINTS wide
        Hidden{IMAGE_MIN, Vec2{900.0F, 139.0F}, 1.0F},  // ...and tall
        Hidden{Vec2{nan, 0.0F}, full, 1.0F},
        Hidden{IMAGE_MIN, Vec2{inf, 600.0F}, 1.0F},
        Hidden{IMAGE_MIN, full, nan},
        Hidden{IMAGE_MIN, full, 0.0F},
        Hidden{IMAGE_MIN, full, -1.0F},
        Hidden{IMAGE_MIN, full, inf},
    };
    for (const Hidden& row : hiddenRows) {
        CAPTURE(row.size.x);
        CAPTURE(row.scale);
        Vec2 boxMin{1.0F, 1.0F};  // stale values the call must overwrite
        Vec2 boxMax{2.0F, 2.0F};
        ed::viewAxisCircleRect(row.origin, row.size, row.scale, boxMin, boxMax);
        CHECK((boxMin == Vec2{}));
        CHECK((boxMax == Vec2{}));
    }

    // 3. The keep test at scale 1, 900 wide, across the band. Nothing but the widget can drop the card at these
    // heights (the anti-vacuity REQUIRE: with the widget hidden it is shown), and the half-extent rect clears it from
    // 242 -- so a keep test on that rect shows the card under the arc at 242..246, and 244 and 248 sit either side.
    const ed::ViewportHudInputs in = inputs();
    ed::ViewportHudInputs noWidget = in;
    noWidget.viewAxisVisible = false;
    for (int h = 240; h <= 250; ++h) {
        CAPTURE(h);
        const auto height = static_cast<float>(h);  // auto: a bare cast (modernize-use-auto)
        const Vec2 max = imageMaxOf(900.0F, height);
        REQUIRE(ed::viewportHudLayout(IMAGE_MIN, max, noWidget, 1.0F).stats.shown);
        Vec2 rectMin{};
        Vec2 rectMax{};
        ed::viewAxisRect(IMAGE_MIN, Vec2{900.0F, height}, 1.0F, rectMin, rectMax);
        REQUIRE(rectMax.x > rectMin.x);  // the widget is visible
        const ed::ViewportHudLayout l = ed::viewportHudLayout(IMAGE_MIN, max, in, 1.0F);
        CHECK(l.stats.shown == (h >= 247));
        if (h >= 242) {
            CHECK(l.stats.max.y <= rectMin.y);  // clear of the half-extent rect: only the circle drops it
        }
    }

    // 4. Scale 2 doubles the band: the card goes below a 494-point image.
    ed::ViewportHudInputs two = in;
    two.chipSize = Vec2{280.0F, 48.0F};
    two.statsContentSize = Vec2{360.0F, 0.0F};
    two.hintSizes = {Vec2{224.0F, 48.0F}, Vec2{160.0F, 48.0F}};
    ed::ViewportHudInputs twoNoWidget = two;
    twoNoWidget.viewAxisVisible = false;
    REQUIRE(ed::viewportHudLayout(IMAGE_MIN, imageMaxOf(1800.0F, 492.0F), twoNoWidget, 2.0F).stats.shown);
    CHECK_FALSE(ed::viewportHudLayout(IMAGE_MIN, imageMaxOf(1800.0F, 492.0F), two, 2.0F).stats.shown);
    CHECK(ed::viewportHudLayout(IMAGE_MIN, imageMaxOf(1800.0F, 494.0F), two, 2.0F).stats.shown);
}
