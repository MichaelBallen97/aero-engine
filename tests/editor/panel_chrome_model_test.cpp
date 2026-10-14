// Aero Engine -- task E.6.3: the panels' shared chrome as VALUES (PC1-PC13; PC12 is the default layout's Bottom
// split, PC13 the hatched well's clip). Tier 0: no ImGui, no GPU, no #if.
// Every expected value is a RESTATED literal (the mock's bytes, the theme's dp), never recomputed by the function
// under test.
#include <aero/editor/editor_app.hpp>  // PC12: resolveBottomDockFraction, pure -- no app is constructed here
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/material_inspector_model.hpp>
#include <aero/editor/panel_chrome_model.hpp>
#include <aero/editor/shell_chrome_record.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>  // PC12: std::bit_cast, the pass-through arm's bit-for-bit claim
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <span>     // PC13: the polygons, as clipConvexPolygon takes them
#include <string_view>
#include <type_traits>
#include <vector>  // PC13: the polygons

namespace {

namespace ed = engine::editor;
using ed::ChromeRole;
using ed::Srgb8;

// From quiet_NaN(), never a signalling one: MSVC quiets an sNaN that passes through a float lvalue.
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

void checkBytes(const Srgb8& actual, const Srgb8& expected) {
    CHECK(static_cast<int>(actual.r) == static_cast<int>(expected.r));
    CHECK(static_cast<int>(actual.g) == static_cast<int>(expected.g));
    CHECK(static_cast<int>(actual.b) == static_cast<int>(expected.b));
    CHECK(static_cast<int>(actual.a) == static_cast<int>(expected.a));
}

struct RoleRow {
    ChromeRole role;
    std::string_view name;
    Srgb8 bytes;
};

// RESTATED: the mock's bytes, TH1's literals -- never read back from the palette the map itself reads. In
// ChromeRole's declaration order (PC3 checks the order against a switch the test owns).
constexpr std::array<RoleRow, 26> ROLE_ROWS{{
    {ChromeRole::Canvas, "Canvas", {13U, 15U, 18U, 255U}},
    {ChromeRole::Inset, "Inset", {18U, 21U, 26U, 255U}},
    {ChromeRole::Chrome, "Chrome", {20U, 23U, 27U, 255U}},
    {ChromeRole::Panel, "Panel", {24U, 27U, 32U, 255U}},
    {ChromeRole::Tile, "Tile", {25U, 29U, 35U, 255U}},
    {ChromeRole::Raised, "Raised", {28U, 32U, 39U, 255U}},
    {ChromeRole::RaisedHeader, "RaisedHeader", {32U, 37U, 45U, 255U}},
    {ChromeRole::Hover, "Hover", {36U, 41U, 50U, 255U}},
    {ChromeRole::Active, "Active", {43U, 49U, 58U, 255U}},
    {ChromeRole::Selection, "Selection", {31U, 43U, 51U, 255U}},
    {ChromeRole::Border, "Border", {38U, 43U, 51U, 255U}},
    {ChromeRole::Divider, "Divider", {35U, 39U, 46U, 255U}},
    {ChromeRole::AccentBorder, "AccentBorder", {58U, 111U, 133U, 255U}},
    {ChromeRole::TextBright, "TextBright", {232U, 234U, 237U, 255U}},
    {ChromeRole::Text, "Text", {215U, 219U, 224U, 255U}},
    {ChromeRole::TextSecondary, "TextSecondary", {169U, 177U, 187U, 255U}},
    {ChromeRole::TextLabel, "TextLabel", {139U, 147U, 158U, 255U}},
    {ChromeRole::TextMuted, "TextMuted", {111U, 120U, 131U, 255U}},
    {ChromeRole::TextFaint, "TextFaint", {91U, 99U, 110U, 255U}},
    {ChromeRole::OnAccent, "OnAccent", {11U, 12U, 14U, 255U}},
    {ChromeRole::Accent, "Accent", {95U, 184U, 220U, 255U}},
    {ChromeRole::Warning, "Warning", {216U, 162U, 75U, 255U}},
    {ChromeRole::Error, "Error", {216U, 106U, 106U, 255U}},
    {ChromeRole::Success, "Success", {134U, 191U, 106U, 255U}},
    {ChromeRole::Info, "Info", {106U, 159U, 216U, 255U}},
    {ChromeRole::Critical, "Critical", {255U, 64U, 64U, 255U}},
}};

// A switch the TEST owns, with no default: -- a 27th ChromeRole reddens the Linux lint lane HERE too, and the walk in
// PC3 visits every role exactly once, in declaration order.
[[nodiscard]] std::size_t roleIndex(ChromeRole role) {
    switch (role) {
        case ChromeRole::Canvas:
            return 0U;
        case ChromeRole::Inset:
            return 1U;
        case ChromeRole::Chrome:
            return 2U;
        case ChromeRole::Panel:
            return 3U;
        case ChromeRole::Tile:
            return 4U;
        case ChromeRole::Raised:
            return 5U;
        case ChromeRole::RaisedHeader:
            return 6U;
        case ChromeRole::Hover:
            return 7U;
        case ChromeRole::Active:
            return 8U;
        case ChromeRole::Selection:
            return 9U;
        case ChromeRole::Border:
            return 10U;
        case ChromeRole::Divider:
            return 11U;
        case ChromeRole::AccentBorder:
            return 12U;
        case ChromeRole::TextBright:
            return 13U;
        case ChromeRole::Text:
            return 14U;
        case ChromeRole::TextSecondary:
            return 15U;
        case ChromeRole::TextLabel:
            return 16U;
        case ChromeRole::TextMuted:
            return 17U;
        case ChromeRole::TextFaint:
            return 18U;
        case ChromeRole::OnAccent:
            return 19U;
        case ChromeRole::Accent:
            return 20U;
        case ChromeRole::Warning:
            return 21U;
        case ChromeRole::Error:
            return 22U;
        case ChromeRole::Success:
            return 23U;
        case ChromeRole::Info:
            return 24U;
        case ChromeRole::Critical:
            return 25U;
    }
    return 26U;
}

[[nodiscard]] bool inRoster(std::string_view glyph) {
    return std::any_of(ed::EDITOR_ICONS.begin(), ed::EDITOR_ICONS.end(),
                       [glyph](const ed::EditorIcon& icon) { return icon.utf8 == glyph; });
}

}  // namespace

TEST_CASE("panel chrome: every role reads its palette member's bytes (task E.6.3, PC1)") {
    REQUIRE(ROLE_ROWS.size() == ed::CHROME_ROLE_COUNT);
    for (const RoleRow& row : ROLE_ROWS) {
        CAPTURE(row.name);
        checkBytes(ed::chromeRoleColor(row.role), row.bytes);
    }
}

TEST_CASE("panel chrome: no role reaches the disabled-text or dialog bytes (task E.6.3, PC2)") {
    const Srgb8 disabled{77U, 84U, 94U, 255U};
    const Srgb8 dialog{16U, 18U, 21U, 255U};
    // ANTI-VACUITY: the two tokens ARE those bytes, so a role mapped to either would be caught below.
    CHECK((ed::EDITOR_THEME.palette.textDisabled == disabled));
    CHECK((ed::EDITOR_THEME.palette.dialog == dialog));
    for (const RoleRow& row : ROLE_ROWS) {
        CAPTURE(row.name);
        const Srgb8 color = ed::chromeRoleColor(row.role);
        CHECK_FALSE((color == disabled));
        CHECK_FALSE((color == dialog));
    }
}

TEST_CASE("panel chrome: the 26 roles are distinct values, walked exhaustively (task E.6.3, PC3)") {
    CHECK(ed::CHROME_ROLE_COUNT == 26U);
    for (std::size_t i = 0; i < ROLE_ROWS.size(); ++i) {
        CAPTURE(ROLE_ROWS[i].name);
        CHECK(roleIndex(ROLE_ROWS[i].role) == i);  // the rows are the enum, in order, each once
        for (std::size_t j = i + 1U; j < ROLE_ROWS.size(); ++j) {
            CAPTURE(ROLE_ROWS[j].name);
            CHECK_FALSE((ed::chromeRoleColor(ROLE_ROWS[i].role) == ed::chromeRoleColor(ROLE_ROWS[j].role)));
        }
    }
}

TEST_CASE("panel chrome: the card geometry is the theme's, exactly scaled (task E.6.3, PC4)") {
    const ed::CardGeometry one = ed::cardGeometry(1.0F);
    CHECK(one.headerHeight == 32.0F);  // the mock's header: a 31-dp strip plus its rule
    CHECK(one.headerPaddingX == 10.0F);
    CHECK(one.headerPaddingY == 8.0F);
    CHECK(one.paddingX == 12.0F);
    CHECK(one.paddingY == 10.0F);
    CHECK(one.gap == 12.0F);
    CHECK(one.rounding == 6.0F);  // childRounding, reused -- never restated as a new token
    CHECK(one.chevronWidth == 3.5F);
    CHECK(one.iconGap == 8.0F);
    CHECK(one.overflowWidth == 24.0F);  // one frame height: body 16 + 2 x framePadding.y 4
    for (const float s : {1.5F, 2.0F}) {
        CAPTURE(s);
        const ed::CardGeometry g = ed::cardGeometry(s);
        CHECK(g.headerHeight == 32.0F * s);
        CHECK(g.headerPaddingX == 10.0F * s);
        CHECK(g.headerPaddingY == 8.0F * s);
        CHECK(g.paddingX == 12.0F * s);
        CHECK(g.paddingY == 10.0F * s);
        CHECK(g.gap == 12.0F * s);
        CHECK(g.rounding == 6.0F * s);
        CHECK(g.chevronWidth == 3.5F * s);
        CHECK(g.iconGap == 8.0F * s);
        CHECK(g.overflowWidth == 24.0F * s);
    }
}

TEST_CASE("panel chrome: a bad scale lays the card and the chip out at 1 (task E.6.3, PC5)") {
    const ed::CardGeometry one = ed::cardGeometry(1.0F);
    for (const float bad : {NAN_F, INF_F, -INF_F, 0.0F, -1.0F}) {
        CAPTURE(bad);
        const ed::CardGeometry g = ed::cardGeometry(bad);
        CHECK((g == one));
        CHECK(std::isfinite(g.headerHeight));
        CHECK(ed::chipHeight(bad) == 24.0F);
    }
    CHECK(ed::chipHeight(1.0F) == 24.0F);  // small 14 + 2 x chipPaddingY 5, the mock's chip
    CHECK(ed::chipHeight(2.0F) == 48.0F);
}

TEST_CASE("panel chrome: the hairline is t x s, never under one device pixel (task E.6.3, PC6)") {
    CHECK(ed::hairline(1.0F, 1.0F, 2.0F) == 1.0F);
    CHECK(ed::hairline(1.0F, 0.5F, 2.0F) == 0.5F);
    CHECK(ed::hairline(1.0F, 0.5F, 4.0F) == 0.5F);
    CHECK(ed::hairline(1.0F, 0.25F, 2.0F) == 0.5F);  // the pixel floor at a 2x framebuffer
    CHECK(ed::hairline(2.0F, 1.0F, 2.0F) == 2.0F);   // a thickness other than 1: a version ignoring t fails here
    CHECK(ed::hairline(2.0F, 0.5F, 1.0F) == 1.0F);
    CHECK(ed::hairline(NAN_F, 1.0F, 1.0F) == 1.0F);  // a bad thickness is 1
    CHECK(ed::hairline(0.0F, 1.0F, 2.0F) == 1.0F);
    CHECK(ed::hairline(-3.0F, 1.0F, 2.0F) == 1.0F);
    CHECK(ed::hairline(1.0F, NAN_F, 2.0F) == 1.0F);   // a bad scale is 1
    CHECK(ed::hairline(1.0F, 0.25F, NAN_F) == 1.0F);  // a bad framebuffer scale is 1: the floor is one point
    for (const float fb : {1.0F, 2.0F, 3.0F, 4.0F}) {
        CAPTURE(fb);
        const float rule = ed::hairline(0.01F, 1.0F, fb);
        CHECK(std::isfinite(rule));
        CHECK(rule >= 1.0F / fb);
    }
}

TEST_CASE("panel chrome: the section icon tables are total and roster-only (task E.6.3, PC7)") {
    struct MaterialIconRow {
        ed::MaterialSectionId id;
        std::string_view icon;
    };
    const std::array<MaterialIconRow, 8> material{{
        {ed::MaterialSectionId::Material, AERO_ICON_PALETTE},
        {ed::MaterialSectionId::BaseColor, AERO_ICON_CIRCLE},
        {ed::MaterialSectionId::MetallicRoughness, AERO_ICON_SLIDERS_HORIZONTAL},
        {ed::MaterialSectionId::Normal, AERO_ICON_IMAGE},
        {ed::MaterialSectionId::Occlusion, AERO_ICON_CIRCLE_DASHED},
        {ed::MaterialSectionId::Emission, AERO_ICON_LIGHTBULB},
        {ed::MaterialSectionId::Rendering, AERO_ICON_EYE},
        {ed::MaterialSectionId::File, AERO_ICON_FILE_TEXT},
    }};
    REQUIRE(material.size() == ed::MATERIAL_SECTION_COUNT);
    for (const MaterialIconRow& row : material) {
        CAPTURE(static_cast<int>(row.id));
        CHECK(ed::materialSectionIcon(row.id) == row.icon);
        CHECK(inRoster(ed::materialSectionIcon(row.id)));
    }
    struct ImportRow {
        ed::ImportDetailsSection section;
        std::string_view title;
        std::string_view icon;
    };
    const std::array<ImportRow, 7> import{{
        {ed::ImportDetailsSection::Overview, "Overview", AERO_ICON_INFO},
        {ed::ImportDetailsSection::ImportSettings, "Import Settings", AERO_ICON_SETTINGS},
        {ed::ImportDetailsSection::Hierarchy, "Hierarchy", AERO_ICON_LIST_TREE},
        {ed::ImportDetailsSection::Meshes, "Meshes", AERO_ICON_BOX},
        {ed::ImportDetailsSection::Materials, "Materials", AERO_ICON_PALETTE},
        {ed::ImportDetailsSection::SkeletonAnimation, "Skeleton & Animation", AERO_ICON_FILM},
        {ed::ImportDetailsSection::Blender, "Blender", AERO_ICON_EXTERNAL_LINK},
    }};
    REQUIRE(import.size() == ed::IMPORT_DETAILS_SECTION_COUNT);
    for (const ImportRow& row : import) {
        CAPTURE(row.title);
        CHECK(ed::importDetailsSectionTitle(row.section) == row.title);  // today's seven literals, in order
        CHECK(ed::importDetailsSectionIcon(row.section) == row.icon);
        CHECK(inRoster(ed::importDetailsSectionIcon(row.section)));
    }
    CHECK(ed::projectSettingsGroupIcon(0U) == std::string_view(AERO_ICON_FILE_TEXT));
    CHECK(ed::projectSettingsGroupIcon(1U) == std::string_view(AERO_ICON_FOLDER));
    CHECK(ed::projectSettingsGroupIcon(7U) == std::string_view(AERO_ICON_SETTINGS));  // total: a fallback
    for (const std::size_t i : {0U, 1U, 7U}) {
        CHECK(inRoster(ed::projectSettingsGroupIcon(i)));
    }
    CHECK(ed::IMPORT_DETAILS_SEARCHED_VISIBLE_ROWS == 6.0F);
    CHECK_FALSE(inRoster("x"));  // ANTI-VACUITY: the roster check refuses a non-glyph
}

TEST_CASE("panel chrome: ItemRectRecord IS ToolbarButtonRecord -- the hoist moved no member (task E.6.3, PC8)") {
    static_assert(std::is_same_v<ed::ToolbarButtonRecord, ed::ItemRectRecord>);
    const ed::ToolbarButtonRecord record{};
    CHECK_FALSE(record.enabled);
    CHECK_FALSE(record.drawnActive);
    CHECK(record.max.x == 0.0F);
}

TEST_CASE("panel chrome: the alpha overload replaces a only -- the one transparent spelling (task E.6.3, PC9)") {
    for (const RoleRow& row : ROLE_ROWS) {
        CAPTURE(row.name);
        const Srgb8 opaque = ed::chromeRoleColor(row.role);
        for (const std::uint8_t alpha : {std::uint8_t{0U}, std::uint8_t{128U}, std::uint8_t{255U}}) {
            CAPTURE(static_cast<int>(alpha));
            const Srgb8 c = ed::chromeRoleColor(row.role, alpha);
            CHECK(static_cast<int>(c.r) == static_cast<int>(opaque.r));
            CHECK(static_cast<int>(c.g) == static_cast<int>(opaque.g));
            CHECK(static_cast<int>(c.b) == static_cast<int>(opaque.b));
            CHECK(static_cast<int>(c.a) == static_cast<int>(alpha));
        }
    }
}

TEST_CASE("panel chrome: the header's x positions and face sizes are arithmetic on the span (task E.6.3, PC10)") {
    // No parameter is a font size, so ImGui's FontSize-sized arrow box can move no x.
    const ed::CardHeaderLayout one = ed::cardHeaderLayout(100.0F, 400.0F, 11.0F, 24.0F, 1.0F);
    CHECK(one.chevronX == 110.0F);   // spanMinX + 10
    CHECK(one.iconX == 121.5F);      // + 3.5 + 8
    CHECK(one.titleX == 140.5F);     // iconX + 11 + 8
    CHECK(one.overflowX == 366.0F);  // spanMaxX - 10 - 24
    const ed::CardHeaderLayout two = ed::cardHeaderLayout(100.0F, 400.0F, 22.0F, 48.0F, 2.0F);
    CHECK(two.chevronX - 100.0F == 2.0F * (one.chevronX - 100.0F));
    CHECK(two.iconX - 100.0F == 2.0F * (one.iconX - 100.0F));
    CHECK(two.titleX == 143.0F + 22.0F + 16.0F);
    CHECK(two.overflowX == 400.0F - 20.0F - 48.0F);
    for (const float bad : {NAN_F, INF_F, 0.0F, -2.0F}) {
        CAPTURE(bad);
        CHECK((ed::cardHeaderLayout(100.0F, 400.0F, 11.0F, 24.0F, bad) == one));
    }
    // The face size ImGui would bind: IM_ROUND(dp x s), never below 1.
    CHECK(ed::faceSizePoints(14.0F, 1.0F) == 14.0F);
    CHECK(ed::faceSizePoints(14.0F, 1.25F) == 18.0F);  // 17.5 rounds half up, as IM_ROUND does
    CHECK(ed::faceSizePoints(16.0F, 1.5F) == 24.0F);
    CHECK(ed::faceSizePoints(14.0F, 0.5F) == 7.0F);
    CHECK(ed::faceSizePoints(NAN_F, 1.0F) == 1.0F);
    CHECK(ed::faceSizePoints(14.0F, NAN_F) == 14.0F);
    // The chevron's face size: body x (3.5 x s) / the ink at body.
    CHECK(ed::chevronFaceSize(16.0F, 7.0F, 1.0F) == 8.0F);
    CHECK(ed::chevronFaceSize(16.0F, 7.0F, 2.0F) == 16.0F);
    CHECK(ed::chevronFaceSize(16.0F, 0.0F, 1.0F) == 16.0F);  // no ink measured: the body size
    CHECK(ed::chevronFaceSize(16.0F, NAN_F, 1.0F) == 16.0F);
    CHECK(ed::chevronFaceSize(NAN_F, 7.0F, 1.0F) == 8.0F);  // a bad body is the theme's body, 16
}

TEST_CASE("panel chrome: the chevron's pen centres its ink, a whole point, total (task E.6.3, PC11)") {
    // round(chevronX + (column - ink width) / 2 - ink left). The GPU tier cannot be relied on to witness the centring:
    // RenderText draws from a whole-point pen, and at I336's geometry a left-aligned ink rounds to the SAME point (on
    // a 1x display both states; on a 2x one the open state). So these inputs are chosen where the centred and the
    // left-aligned pens round to DIFFERENT points.
    CHECK(ed::cardChevronPenX(100.0F, 3.5F, 0.0F, 2.0F) == 101.0F);  // 100.75; left-aligned: 100
    CHECK(ed::cardChevronPenX(200.0F, 3.5F, 2.5F, 1.0F) == 199.0F);  // 198.75; left-aligned: 197.5 -> 198
    // The open chevron: its ink overhangs the column (3.75 in 3.5), so the centring term is NEGATIVE.
    CHECK(ed::cardChevronPenX(100.35F, 3.5F, 1.8F, 3.75F) == 98.0F);  // 98.425; left-aligned: 98.55 -> 99
    // An ink that fills the column exactly has no centring term: its ink starts at the column's start, less the
    // rounding (108 + 1.75 = 109.75 for a column at 110).
    CHECK(ed::cardChevronPenX(110.0F, 3.5F, 1.75F, 3.5F) == 108.0F);  // 108.25
    // I336's geometry at body 16, scale 1, on a 1x display (a column at 991): the open ink {1.8, 3.73} and the closed
    // {2.33, 2.67} both give 989.08 -> 989, and left-aligned 989.2 and 988.67 -> 989 too: the same pixels.
    CHECK(ed::cardChevronPenX(991.0F, 3.5F, 1.8F, 3.73333F) == 989.0F);
    CHECK(ed::cardChevronPenX(991.0F, 3.5F, 2.33333F, 2.66667F) == 989.0F);
    // TOTAL: a non-finite input counts as 0 (the ink reading's answer when it measures nothing), so every answer is a
    // finite whole point.
    for (const float bad : {NAN_F, INF_F, -INF_F}) {
        CAPTURE(bad);
        const float noChevronX = ed::cardChevronPenX(bad, 3.5F, 0.0F, 2.0F);
        const float noColumn = ed::cardChevronPenX(100.0F, bad, 0.0F, 2.0F);
        const float noInkLeft = ed::cardChevronPenX(100.0F, 3.5F, bad, 2.0F);
        const float noInkWidth = ed::cardChevronPenX(100.0F, 3.5F, 0.0F, bad);
        CHECK(noChevronX == 1.0F);    // 0 + 0.75
        CHECK(noColumn == 99.0F);     // 100 + (0 - 2) / 2
        CHECK(noInkLeft == 101.0F);   // 100 + 0.75 - 0
        CHECK(noInkWidth == 102.0F);  // 100 + 1.75 = 101.75
    }
    // ...and a sum past the float range answers 0, never an infinity.
    constexpr float MAX_F = std::numeric_limits<float>::max();
    CHECK(ed::cardChevronPenX(MAX_F, MAX_F, 0.0F, 0.0F) == 0.0F);
    CHECK(ed::cardChevronPenX(-MAX_F, 0.0F, MAX_F, 0.0F) == 0.0F);
}

TEST_CASE("panel chrome: the Bottom dock fraction passes a sane value and refuses the rest (task E.6.3, PC12)") {
    // The code-review round, round 4: the seam that sizes the Assets / Console node (EditorAppConfig's
    // bottomDockFraction), resolved once in EditorApp::create. The shipping value, restated.
    CHECK(ed::DEFAULT_BOTTOM_DOCK_FRACTION == 0.25F);
    static_assert(noexcept(ed::resolveBottomDockFraction(0.25F)));
    // A value inside [0.05, 0.95] passes through bit for bit -- the default, a test's 0.4, and both bounds.
    for (const float sane : {0.25F, 0.4F, 0.05F, 0.95F}) {
        CAPTURE(sane);
        const auto resolvedBits = std::bit_cast<std::uint32_t>(ed::resolveBottomDockFraction(sane));
        CHECK(resolvedBits == std::bit_cast<std::uint32_t>(sane));
    }
    // Anything else falls back to the shipping 0.25 -- never a clamp to the nearer bound (0.05 for 0.0 and 0.04,
    // 0.95 for 0.96 and 1.0), and never a NaN let through by a range test that a NaN fails both halves of.
    for (const float refused : {0.0F, 0.04F, 0.96F, 1.0F, -0.5F, NAN_F, INF_F, -INF_F}) {
        CAPTURE(refused);
        CHECK(ed::resolveBottomDockFraction(refused) == 0.25F);
    }
}

// ---- PC13: the hatched well's stripes, clipped as geometry (the code-review round, round 6) ------------------
namespace {

using engine::Vec2;

// Twice the signed area: POSITIVE for a polygon wound clockwise on a y-down screen, as ImGui's are (imgui.h:3497).
[[nodiscard]] double twiceArea(std::span<const Vec2> polygon) {
    double sum = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[(i + 1U) % polygon.size()];
        sum += (static_cast<double>(a.x) * b.y) - (static_cast<double>(b.x) * a.y);
    }
    return sum;
}

// Point-in-convex for a CLOCKWISE (y-down) polygon: `p` is no further than `epsilon` outside any edge's line.
[[nodiscard]] bool insideConvex(Vec2 p, std::span<const Vec2> polygon, double epsilon) {
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[(i + 1U) % polygon.size()];
        const double ex = static_cast<double>(b.x) - a.x;
        const double ey = static_cast<double>(b.y) - a.y;
        const double cross = (ex * (static_cast<double>(p.y) - a.y)) - (ey * (static_cast<double>(p.x) - a.x));
        if (!(cross / std::hypot(ex, ey) >= -epsilon)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasVertexNear(std::span<const Vec2> polygon, Vec2 p, float epsilon) {
    return std::any_of(polygon.begin(), polygon.end(), [p, epsilon](Vec2 v) {
        return std::abs(v.x - p.x) <= epsilon && std::abs(v.y - p.y) <= epsilon;
    });
}

// Each expected vertex is in `polygon` (to `epsilon`), and `polygon` holds nothing else.
void checkVertexSet(std::span<const Vec2> polygon, std::span<const Vec2> expected, float epsilon) {
    CHECK(polygon.size() == expected.size());
    for (const Vec2 v : expected) {
        CAPTURE(v.x);
        CAPTURE(v.y);
        CHECK(hasVertexNear(polygon, v, epsilon));
    }
}
void checkVertexSet(std::span<const Vec2> polygon, std::initializer_list<Vec2> expected, float epsilon) {
    checkVertexSet(polygon, std::span<const Vec2>(expected.begin(), expected.size()), epsilon);
}

// A 64-point well at the origin with a 3-point rounding, as ImGui's PathRect tessellates that radius (1x): two
// segments a corner -- IM_DRAWLIST_CIRCLE_AUTO_SEGMENT_CALC(3, 0.30) is 8 a circle, so _PathArcToFastEx steps 6 of
// its 48 samples -- clockwise from the top-left arc's left end. 0.87868 is 3 - 3 cos 45, 63.12132 is 64 less that.
constexpr std::array<Vec2, 12> WELL{{{0.0F, 3.0F},
                                     {0.87868F, 0.87868F},
                                     {3.0F, 0.0F},
                                     {61.0F, 0.0F},
                                     {63.12132F, 0.87868F},
                                     {64.0F, 3.0F},
                                     {64.0F, 61.0F},
                                     {63.12132F, 63.12132F},
                                     {61.0F, 64.0F},
                                     {3.0F, 64.0F},
                                     {0.87868F, 63.12132F},
                                     {0.0F, 61.0F}}};
constexpr std::array<Vec2, 4> SQUARE{{{0.0F, 0.0F}, {10.0F, 0.0F}, {10.0F, 10.0F}, {0.0F, 10.0F}}};
// hatchedWell's band over the well's top-left corner, at stripe 10: the k = 0 quad, (x0 + h, min.y), (x0 + stripe +
// h, min.y), (x0 + stripe, max.y), (x0, max.y) with x0 = min.x - h -- its first vertex IS the square corner.
constexpr std::array<Vec2, 4> CORNER_BAND{{{0.0F, 0.0F}, {10.0F, 0.0F}, {-54.0F, 64.0F}, {-64.0F, 64.0F}}};
// The same band moved 2.5 along: its left edge, x + y = 2.5, crosses the top-left arc between its ends (x + y = 3)
// and its middle vertex (x + y = 1.757).
constexpr std::array<Vec2, 4> CROSSING_BAND{{{2.5F, 0.0F}, {12.5F, 0.0F}, {-51.5F, 64.0F}, {-61.5F, 64.0F}}};
// ...and the band ending there, x + y <= 2.5: inside the well it is only the CAP the line cuts from the arc.
constexpr std::array<Vec2, 4> CAP_BAND{{{-7.5F, 0.0F}, {2.5F, 0.0F}, {-61.5F, 64.0F}, {-71.5F, 64.0F}}};

constexpr float CLIP_EPSILON = 1.0e-4F;  // float rounding at these magnitudes (64 points: one ulp is 7.6e-6)

}  // namespace

TEST_CASE("panel chrome: a convex clip keeps a hatch stripe inside the well's rounded outline (task E.6.3, PC13)") {
    // A square scissor let every stripe crossing a corner paint the square corner outside the rounded fill (the
    // macOS manual pass, rows 11 and 12); hatchedWell clips each stripe to the fill's own outline as geometry.
    REQUIRE(twiceArea(WELL) > 0.0);         // the clockwise winding ImGui's PathRect has
    REQUIRE(twiceArea(CORNER_BAND) > 0.0);  // ...and hatchedWell's stripe quad
    // A subject wholly inside comes back vertex for vertex, in its own order.
    const std::vector<Vec2> inner{{4.0F, 1.0F}, {6.0F, 1.0F}, {3.0F, 4.0F}, {1.0F, 4.0F}};
    const std::vector<Vec2> kept = ed::clipConvexPolygon(inner, SQUARE);
    REQUIRE(kept.size() == inner.size());
    for (std::size_t i = 0; i < inner.size(); ++i) {
        CAPTURE(i);
        CHECK(kept[i] == inner[i]);
    }
    // A disjoint subject leaves nothing.
    const std::vector<Vec2> disjoint{{20.0F, 20.0F}, {30.0F, 20.0F}, {30.0F, 30.0F}, {20.0F, 30.0F}};
    CHECK(ed::clipConvexPolygon(disjoint, SQUARE).empty());
    // A rounding of 0 is a square outline, and a stripe still hatches it edge to edge: x + y in [4, 8] meets the top
    // edge at x = 4 and 8 and the left at y = 4 and 8.
    const std::vector<Vec2> squareBand{{4.0F, 0.0F}, {8.0F, 0.0F}, {-2.0F, 10.0F}, {-6.0F, 10.0F}};
    const std::vector<Vec2> squareCut = ed::clipConvexPolygon(squareBand, SQUARE);
    checkVertexSet(squareCut, {{4.0F, 0.0F}, {8.0F, 0.0F}, {0.0F, 8.0F}, {0.0F, 4.0F}}, CLIP_EPSILON);

    // THE CORNER. The band's first vertex is the square corner, outside the rounded outline...
    REQUIRE_FALSE(insideConvex(CORNER_BAND[0], WELL, CLIP_EPSILON));
    const std::vector<Vec2> corner = ed::clipConvexPolygon(CORNER_BAND, WELL);
    // ...and the clipped band holds every vertex inside it, REACHES the arc -- its three vertices are the band's --
    // and keeps the band's two edge crossings: (10, 0) on the top edge, (0, 10) on the left.
    for (const Vec2 v : corner) {
        CAPTURE(v.x);
        CAPTURE(v.y);
        CHECK(insideConvex(v, WELL, CLIP_EPSILON));
    }
    checkVertexSet(corner, {{0.0F, 3.0F}, {0.87868F, 0.87868F}, {3.0F, 0.0F}, {10.0F, 0.0F}, {0.0F, 10.0F}},
                   CLIP_EPSILON);
    CHECK(twiceArea(corner) > 0.0);  // the subject's winding, so ImGui's fringe still faces out
    // A band edge that crosses the arc cuts it where x + y = 2.5 meets its two segments: (sqrt 2 / 4, 2.5 - that).
    const std::vector<Vec2> crossing = ed::clipConvexPolygon(CROSSING_BAND, WELL);
    for (const Vec2 v : crossing) {
        CAPTURE(v.x);
        CAPTURE(v.y);
        CHECK(insideConvex(v, WELL, CLIP_EPSILON));
    }
    checkVertexSet(
        crossing,
        {{0.0F, 3.0F}, {0.35355F, 2.14645F}, {2.14645F, 0.35355F}, {3.0F, 0.0F}, {12.5F, 0.0F}, {0.0F, 12.5F}},
        CLIP_EPSILON);

    // THE WINDING. A counter-clockwise clipper is read from its own sign and clips the same; the result keeps the
    // SUBJECT's winding either way.
    const std::vector<Vec2> reversedWell(WELL.rbegin(), WELL.rend());
    REQUIRE(twiceArea(reversedWell) < 0.0);
    const std::vector<Vec2> fromReversed = ed::clipConvexPolygon(CORNER_BAND, reversedWell);
    checkVertexSet(fromReversed, corner, CLIP_EPSILON);
    CHECK(twiceArea(fromReversed) > 0.0);
    const std::vector<Vec2> reversedBand(CORNER_BAND.rbegin(), CORNER_BAND.rend());
    const std::vector<Vec2> reversedCut = ed::clipConvexPolygon(reversedBand, WELL);
    checkVertexSet(reversedCut, corner, CLIP_EPSILON);
    CHECK(twiceArea(reversedCut) < 0.0);

    // MERGED. A subject edge through a clipper vertex leaves that vertex twice, once per clip edge; ImGui would
    // receive a zero-length edge. The diamond |x - 5| + |y - 5| <= 10 holds the square, touching all four corners.
    const std::vector<Vec2> diamond{{-5.0F, 5.0F}, {5.0F, -5.0F}, {15.0F, 5.0F}, {5.0F, 15.0F}};
    checkVertexSet(ed::clipConvexPolygon(diamond, SQUARE), SQUARE, CLIP_EPSILON);

    // REFUSED, never guessed: empty.
    const std::vector<Vec2> segment{{0.0F, 0.0F}, {10.0F, 0.0F}};
    CHECK(ed::clipConvexPolygon(segment, SQUARE).empty());  // under three vertices, either input
    CHECK(ed::clipConvexPolygon(inner, segment).empty());
    const std::vector<Vec2> flat{{0.0F, 0.0F}, {5.0F, 0.0F}, {10.0F, 0.0F}};
    CHECK(ed::clipConvexPolygon(inner, flat).empty());  // a clipper with no area
    // A reflex corner at (5, 4): Sutherland-Hodgman would answer a wrong polygon, so the clipper is refused --
    // though `inner` lies inside its convex hull.
    const std::vector<Vec2> reflex{{0.0F, 0.0F}, {10.0F, 0.0F}, {10.0F, 10.0F}, {5.0F, 4.0F}, {0.0F, 10.0F}};
    REQUIRE(twiceArea(reflex) > 0.0);
    CHECK(ed::clipConvexPolygon(inner, reflex).empty());
    // What is left with no area: a square touching this one along x = 10 shares only that edge.
    const std::vector<Vec2> touching{{10.0F, 0.0F}, {20.0F, 0.0F}, {20.0F, 10.0F}, {10.0F, 10.0F}};
    CHECK(ed::clipConvexPolygon(touching, SQUARE).empty());
    // A non-finite coordinate in either input, wherever it sits.
    for (const float bad : {NAN_F, INF_F, -INF_F}) {
        CAPTURE(bad);
        for (std::size_t i = 0; i < SQUARE.size(); ++i) {
            CAPTURE(i);
            std::array<Vec2, 4> badX = SQUARE;
            badX[i].x = bad;
            std::array<Vec2, 4> badY = SQUARE;
            badY[i].y = bad;
            CHECK(ed::clipConvexPolygon(badX, SQUARE).empty());
            CHECK(ed::clipConvexPolygon(badY, SQUARE).empty());
            CHECK(ed::clipConvexPolygon(inner, badX).empty());
            CHECK(ed::clipConvexPolygon(inner, badY).empty());
        }
    }

    // THE FRINGE. A piece no sharper than the stripe's own 45-degree corner takes ImGui's fringe; the CAP a band's
    // edge cuts from the arc -- corners of 22.5 degrees, where the arc's segments run nearly parallel to the band --
    // does not: its miter, AA_SIZE / 2 / sin 11.25, would push the fringe 2.6 points out and the fill's own
    // vertices as far in, past the cap's far side and out of the outline.
    CHECK(ed::hatchPieceTakesFringe(corner));
    CHECK(ed::hatchPieceTakesFringe(crossing));
    CHECK(ed::hatchPieceTakesFringe(CORNER_BAND));  // the unclipped stripe: 45 and 135 degrees
    CHECK(ed::hatchPieceTakesFringe(SQUARE));
    const std::vector<Vec2> cap = ed::clipConvexPolygon(CAP_BAND, WELL);
    checkVertexSet(cap, {{2.14645F, 0.35355F}, {0.35355F, 2.14645F}, {0.87868F, 0.87868F}}, CLIP_EPSILON);
    CHECK_FALSE(ed::hatchPieceTakesFringe(cap));
    // The boundary: a 45-degree corner takes it, a 44-degree one (10 tan 44 = 9.65689) does not.
    const std::vector<Vec2> at45{{0.0F, 0.0F}, {10.0F, 0.0F}, {10.0F, 10.0F}};
    const std::vector<Vec2> at44{{0.0F, 0.0F}, {10.0F, 0.0F}, {10.0F, 9.65689F}};
    CHECK(ed::hatchPieceTakesFringe(at45));
    CHECK_FALSE(ed::hatchPieceTakesFringe(at44));
    // A piece that cannot be measured takes none: under three vertices, a zero-length edge, a non-finite vertex.
    const std::vector<Vec2> repeated{{0.0F, 0.0F}, {10.0F, 0.0F}, {10.0F, 0.0F}, {0.0F, 10.0F}};
    const std::vector<Vec2> notFinite{{0.0F, 0.0F}, {10.0F, 0.0F}, {NAN_F, 10.0F}};
    CHECK_FALSE(ed::hatchPieceTakesFringe(segment));
    CHECK_FALSE(ed::hatchPieceTakesFringe(repeated));
    CHECK_FALSE(ed::hatchPieceTakesFringe(notFinite));
}
