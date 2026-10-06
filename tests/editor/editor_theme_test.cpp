// tests/editor/editor_theme_test.cpp -- task E.6.1: the editor's one theme (TH1-TH10), and the Inspector's
// axis row at its metrics (TH11).
// TIER 0, EVERY CONFIGURATION: editor_theme.hpp is pure, so nothing here needs a device, a window or
// generated meta. NO #if of any kind (the 3.6.3 rule). Every table below is an INDEPENDENTLY RESTATED
// literal (the IX3 / I230(g) posture): a value changed in the theme is changed here in the same commit,
// which is the point. Colour comparisons are PER CHANNEL, because CHECK((a == b)) over a struct prints
// CHECK( true ) on a failure.
#include <aero/core/math.hpp>
#include <aero/editor/asset_view.hpp>    // iconColorFor, IconColor, AssetKind (TH9)
#include <aero/editor/axis_palette.hpp>  // the AXIS_* aliases (TH3)
#include <aero/editor/editor_app.hpp>    // EditorAppConfig's clear colour (TH3)
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/gizmo_style.hpp>      // the GIZMO_* aliases (TH3)
#include <aero/editor/inspector_model.hpp>  // inspectorAxisBoxWidth, inspectorLabelColumnWidth (TH11)
#include <aero/editor/material_card.hpp>    // materialSwatchWantsDarkLabel (TH8)
#include <aero/editor/text_file.hpp>        // readTextFile (TH10)
#include <aero/editor/viewport_gizmos.hpp>  // ViewportGizmoTints (TH3)

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>  // MSVC: a CAPTURE over std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using engine::Vec4;

// Per channel, as integers, so a failure prints numbers rather than a character or `true`.
void checkSrgb8(const ed::Srgb8& actual, const ed::Srgb8& expected) {
    CHECK(static_cast<int>(actual.r) == static_cast<int>(expected.r));
    CHECK(static_cast<int>(actual.g) == static_cast<int>(expected.g));
    CHECK(static_cast<int>(actual.b) == static_cast<int>(expected.b));
    CHECK(static_cast<int>(actual.a) == static_cast<int>(expected.a));
}

// EXACT, never Approx: a moved value is the same literal digits, so the same float.
void checkVec4(const Vec4& actual, const Vec4& expected) {
    CHECK(actual.x == expected.x);
    CHECK(actual.y == expected.y);
    CHECK(actual.z == expected.z);
    CHECK(actual.w == expected.w);
}

void checkRgbaf(const ed::Rgbaf& actual, const ed::Rgbaf& expected) {
    CHECK(actual.r == expected.r);
    CHECK(actual.g == expected.g);
    CHECK(actual.b == expected.b);
    CHECK(actual.a == expected.a);
}

template <std::size_t N>
void checkBytes(const std::array<std::uint8_t, N>& actual, const std::array<std::uint8_t, N>& expected) {
    for (std::size_t i = 0; i < N; ++i) {
        CAPTURE(i);
        CHECK(static_cast<int>(actual[i]) == static_cast<int>(expected[i]));
    }
}

struct PaletteRow {
    std::string_view name;
    ed::Srgb8 expected;
    ed::Srgb8 actual;
};

void checkPairwiseDistinct(std::string_view group, const std::vector<ed::Srgb8>& tokens) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        for (std::size_t j = i + 1U; j < tokens.size(); ++j) {
            CAPTURE(group);
            CAPTURE(i);
            CAPTURE(j);
            CHECK_FALSE(tokens[i] == tokens[j]);
        }
    }
}

// WCAG 2's relative luminance, in double, from the bytes -- computed HERE rather than read from anywhere.
[[nodiscard]] double channelLinear(std::uint8_t byte) {
    const double c = static_cast<double>(byte) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double relativeLuminance(const ed::Srgb8& c) {
    return (0.2126 * channelLinear(c.r)) + (0.7152 * channelLinear(c.g)) + (0.0722 * channelLinear(c.b));
}

[[nodiscard]] double contrastRatio(const ed::Srgb8& a, const ed::Srgb8& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

struct FloorRow {
    std::string_view name;
    ed::Srgb8 foreground;
    ed::Srgb8 background;
    double floor = 0.0;
};

// The comment-stripped lines of a text -- the editorSourceCodeLines rule (imgui_layer_test.cpp), copied
// file-locally: a citation in PROSE must never satisfy or break a gate about CODE.
[[nodiscard]] std::vector<std::string> codeLinesOf(std::string_view text) {
    std::vector<std::string> code;
    while (true) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1U);
    }
    return code;
}

}  // namespace

TEST_CASE("theme: the palette is the mock's (task E.6.1, TH1)") {
    // A token added to ThemePalette without a row here is a build error, not a silent gap.
    static_assert(sizeof(ed::ThemePalette) == 28U * sizeof(ed::Srgb8));
    const ed::ThemePalette& p = ed::EDITOR_THEME.palette;
    const std::vector<PaletteRow> rows{
        {"canvas", {13U, 15U, 18U, 255U}, p.canvas},
        {"dialog", {16U, 18U, 21U, 255U}, p.dialog},
        {"inset", {18U, 21U, 26U, 255U}, p.inset},
        {"chrome", {20U, 23U, 27U, 255U}, p.chrome},
        {"panel", {24U, 27U, 32U, 255U}, p.panel},
        {"tile", {25U, 29U, 35U, 255U}, p.tile},
        {"raised", {28U, 32U, 39U, 255U}, p.raised},
        {"raisedHeader", {32U, 37U, 45U, 255U}, p.raisedHeader},
        {"hover", {36U, 41U, 50U, 255U}, p.hover},
        {"active", {43U, 49U, 58U, 255U}, p.active},
        {"selection", {31U, 43U, 51U, 255U}, p.selection},
        {"border", {38U, 43U, 51U, 255U}, p.border},
        {"divider", {35U, 39U, 46U, 255U}, p.divider},
        {"accentBorder", {58U, 111U, 133U, 255U}, p.accentBorder},
        {"textBright", {232U, 234U, 237U, 255U}, p.textBright},
        {"text", {215U, 219U, 224U, 255U}, p.text},
        {"textSecondary", {169U, 177U, 187U, 255U}, p.textSecondary},
        {"textLabel", {139U, 147U, 158U, 255U}, p.textLabel},
        {"textMuted", {111U, 120U, 131U, 255U}, p.textMuted},
        {"textFaint", {91U, 99U, 110U, 255U}, p.textFaint},
        {"textDisabled", {77U, 84U, 94U, 255U}, p.textDisabled},
        {"onAccent", {11U, 12U, 14U, 255U}, p.onAccent},
        {"accent", {95U, 184U, 220U, 255U}, p.accent},
        {"warning", {216U, 162U, 75U, 255U}, p.warning},
        {"error", {216U, 106U, 106U, 255U}, p.error},
        {"success", {134U, 191U, 106U, 255U}, p.success},
        {"info", {106U, 159U, 216U, 255U}, p.info},
        {"critical", {255U, 64U, 64U, 255U}, p.critical},
    };
    REQUIRE(rows.size() == 28U);
    for (const PaletteRow& row : rows) {
        CAPTURE(row.name);
        checkSrgb8(row.actual, row.expected);
        CHECK(static_cast<int>(row.actual.a) == 255);  // every Category-A token is opaque
    }
}

TEST_CASE("theme: Category B is byte-identical to the branch point (task E.6.1, TH2)") {
    const ed::EditorTheme& t = ed::EDITOR_THEME;

    // The axis trio and the Inspector's neutral (axis_palette.hpp:37-44, inspector_model.cpp:309).
    checkVec4(t.axis.xLinear, Vec4{0.7605F, 0.0529F, 0.0666F, 1.0F});
    checkVec4(t.axis.yLinear, Vec4{0.2051F, 0.5711F, 0.0467F, 1.0F});
    checkVec4(t.axis.zLinear, Vec4{0.0395F, 0.2346F, 0.7605F, 1.0F});
    checkBytes(t.axis.xSrgb, std::array<std::uint8_t, 3>{226U, 65U, 73U});
    checkBytes(t.axis.ySrgb, std::array<std::uint8_t, 3>{125U, 199U, 61U});
    checkBytes(t.axis.zSrgb, std::array<std::uint8_t, 3>{56U, 133U, 226U});
    checkBytes(t.axis.neutralSrgb, std::array<std::uint8_t, 3>{140U, 140U, 140U});

    // The gizmo style (gizmo_style.hpp:76-100, :149-150): 9 lengths, 2 alphas, 7 colours, length, fraction.
    CHECK(t.gizmo.translationLineThicknessPoints == 4.0F);
    CHECK(t.gizmo.translationArrowSizePoints == 10.0F);
    CHECK(t.gizmo.rotationLineThicknessPoints == 3.0F);
    CHECK(t.gizmo.rotationScreenRingThicknessPoints == 2.0F);
    CHECK(t.gizmo.scaleLineThicknessPoints == 4.0F);
    CHECK(t.gizmo.scaleDiscRadiusPoints == 7.0F);
    CHECK(t.gizmo.hatchedAxisThicknessPoints == 0.0F);
    CHECK(t.gizmo.centerDiscRadiusPoints == 7.0F);
    CHECK(t.gizmo.centerHitHalfExtentPoints == 10.0F);
    CHECK(static_cast<int>(t.gizmo.planeFillAlpha) == 115);
    CHECK(static_cast<int>(t.gizmo.rotationFillAlpha) == 96);
    checkBytes(t.gizmo.highlightSrgb, std::array<std::uint8_t, 4>{255U, 232U, 64U, 255U});
    checkBytes(t.gizmo.inactiveSrgb, std::array<std::uint8_t, 4>{153U, 153U, 153U, 153U});
    checkBytes(t.gizmo.translationLineSrgb, std::array<std::uint8_t, 4>{220U, 220U, 220U, 200U});
    checkBytes(t.gizmo.scaleLineSrgb, std::array<std::uint8_t, 4>{150U, 150U, 150U, 255U});
    checkBytes(t.gizmo.hatchedAxisSrgb, std::array<std::uint8_t, 4>{0U, 0U, 0U, 128U});
    checkBytes(t.gizmo.textSrgb, std::array<std::uint8_t, 4>{255U, 255U, 255U, 255U});
    checkBytes(t.gizmo.textShadowSrgb, std::array<std::uint8_t, 4>{0U, 0U, 0U, 255U});
    CHECK(t.gizmo.axisLengthPoints == 90.0F);
    CHECK(t.gizmo.axisMaxViewportFraction == 0.15F);

    // The four light-gizmo tints (viewport_gizmos.hpp:52-55). 0.4342 is sRGB 176 decoded, not log10(e):
    // the theme's own NOLINT reason, restated with the literal.
    // NOLINTNEXTLINE(modernize-use-std-numbers)
    checkVec4(t.viewportTints.primarySelected, Vec4{1.0F, 0.4342F, 0.0513F, 1.0F});
    checkVec4(t.viewportTints.secondarySelected, Vec4{1.0F, 0.2961F, 0.0144F, 0.7451F});
    checkVec4(t.viewportTints.unselected, Vec4{0.62F, 0.64F, 0.68F, 0.90F});
    checkVec4(t.viewportTints.mutedDirectional, Vec4{0.30F, 0.31F, 0.33F, 0.35F});

    // The seven asset-kind colours, as the hex bytes asset_view.cpp wrote (:175-187).
    checkSrgb8(t.assetKind.folder, {0xE0U, 0xB0U, 0x30U, 255U});
    checkSrgb8(t.assetKind.texture, {0x40U, 0x90U, 0xD0U, 255U});
    checkSrgb8(t.assetKind.model, {0x60U, 0xB0U, 0x60U, 255U});
    checkSrgb8(t.assetKind.audio, {0xA0U, 0x60U, 0xC0U, 255U});
    checkSrgb8(t.assetKind.text, {0x90U, 0x90U, 0x90U, 255U});
    checkSrgb8(t.assetKind.material, {0xE0U, 0x70U, 0x45U, 255U});
    checkSrgb8(t.assetKind.unknown, {0x50U, 0x50U, 0x50U, 255U});

    // The swatch label pair and its rule (asset_tile.cpp:24/:84, material_card.cpp:43-46).
    checkSrgb8(t.swatchLabel.onLight, {24U, 24U, 24U, 255U});
    checkSrgb8(t.swatchLabel.onDark, {255U, 255U, 255U, 255U});
    CHECK(t.swatchLabel.lumaR == 2126U);
    CHECK(t.swatchLabel.lumaG == 7152U);
    CHECK(t.swatchLabel.lumaB == 722U);
    CHECK(t.swatchLabel.darkLabelThreshold == 1280000U);

    // The viewport's overlay and view-axis colours (viewport_panel.cpp:55-56, :135-138 at the branch point).
    checkRgbaf(t.viewport.overlayText, {0.7F, 0.7F, 0.75F, 0.8F});
    checkSrgb8(t.viewport.viewAxisLabel, {16U, 16U, 20U, 255U});
    checkSrgb8(t.viewport.viewAxisHoverOutline, {255U, 255U, 255U, 255U});
    checkSrgb8(t.viewport.viewAxisCenterFill, {210U, 210U, 215U, 230U});
    CHECK(t.viewport.viewAxisNegativeFillAlphaScale == 0.22F);

    // The three render-target clears (editor_app.hpp:111, viewport_panel.cpp:55, material_preview.cpp:41 and
    // material_thumbnail.cpp:34 at the branch point -- the last two already equal).
    checkRgbaf(t.clear.shell, {0.10F, 0.10F, 0.12F, 1.0F});
    checkRgbaf(t.clear.viewport, {0.06F, 0.06F, 0.07F, 1.0F});
    checkRgbaf(t.clear.preview, {0.05F, 0.05F, 0.06F, 1.0F});
}

TEST_CASE("theme: every alias is the theme (task E.6.1, TH3)") {
    // Compile time first: every public constant that used to state a value now reads the theme.
    static_assert(ed::AXIS_X_SRGB == ed::EDITOR_THEME.axis.xSrgb);
    static_assert(ed::AXIS_Y_SRGB == ed::EDITOR_THEME.axis.ySrgb);
    static_assert(ed::AXIS_Z_SRGB == ed::EDITOR_THEME.axis.zSrgb);
    static_assert(ed::AXIS_X_LINEAR == ed::EDITOR_THEME.axis.xLinear);
    static_assert(ed::AXIS_Y_LINEAR == ed::EDITOR_THEME.axis.yLinear);
    static_assert(ed::AXIS_Z_LINEAR == ed::EDITOR_THEME.axis.zLinear);

    constexpr const ed::ThemeGizmo& GIZMO = ed::EDITOR_THEME.gizmo;
    static_assert(ed::GIZMO_TRANSLATION_LINE_THICKNESS_POINTS == GIZMO.translationLineThicknessPoints);
    static_assert(ed::GIZMO_TRANSLATION_ARROW_SIZE_POINTS == GIZMO.translationArrowSizePoints);
    static_assert(ed::GIZMO_ROTATION_LINE_THICKNESS_POINTS == GIZMO.rotationLineThicknessPoints);
    static_assert(ed::GIZMO_ROTATION_SCREEN_RING_THICKNESS_POINTS == GIZMO.rotationScreenRingThicknessPoints);
    static_assert(ed::GIZMO_SCALE_LINE_THICKNESS_POINTS == GIZMO.scaleLineThicknessPoints);
    static_assert(ed::GIZMO_SCALE_DISC_RADIUS_POINTS == GIZMO.scaleDiscRadiusPoints);
    static_assert(ed::GIZMO_HATCHED_AXIS_THICKNESS_POINTS == GIZMO.hatchedAxisThicknessPoints);
    static_assert(ed::GIZMO_CENTER_DISC_RADIUS_POINTS == GIZMO.centerDiscRadiusPoints);
    static_assert(ed::GIZMO_CENTER_HIT_HALF_EXTENT_POINTS == GIZMO.centerHitHalfExtentPoints);
    static_assert(ed::GIZMO_PLANE_FILL_ALPHA == GIZMO.planeFillAlpha);
    static_assert(ed::GIZMO_ROTATION_FILL_ALPHA == GIZMO.rotationFillAlpha);
    static_assert(ed::GIZMO_HIGHLIGHT_SRGB == GIZMO.highlightSrgb);
    static_assert(ed::GIZMO_INACTIVE_SRGB == GIZMO.inactiveSrgb);
    static_assert(ed::GIZMO_TRANSLATION_LINE_SRGB == GIZMO.translationLineSrgb);
    static_assert(ed::GIZMO_SCALE_LINE_SRGB == GIZMO.scaleLineSrgb);
    static_assert(ed::GIZMO_HATCHED_AXIS_SRGB == GIZMO.hatchedAxisSrgb);
    static_assert(ed::GIZMO_TEXT_SRGB == GIZMO.textSrgb);
    static_assert(ed::GIZMO_TEXT_SHADOW_SRGB == GIZMO.textShadowSrgb);
    static_assert(ed::GIZMO_AXIS_LENGTH_POINTS == GIZMO.axisLengthPoints);
    static_assert(ed::GIZMO_AXIS_MAX_VIEWPORT_FRACTION == GIZMO.axisMaxViewportFraction);

    // Run time: ViewportGizmoTints' member initialisers read the theme.
    const ed::ViewportGizmoTints tints{};
    const ed::ThemeViewportTints& theme = ed::EDITOR_THEME.viewportTints;
    checkVec4(tints.primarySelected, theme.primarySelected);
    checkVec4(tints.secondarySelected, theme.secondarySelected);
    checkVec4(tints.unselected, theme.unselected);
    checkVec4(tints.mutedDirectional, theme.mutedDirectional);

    // The shell's clear: EditorAppConfig's default reads the theme.
    const ed::EditorAppConfig config{};
    CHECK(config.clearColor.r == ed::EDITOR_THEME.clear.shell.r);
    CHECK(config.clearColor.g == ed::EDITOR_THEME.clear.shell.g);
    CHECK(config.clearColor.b == ed::EDITOR_THEME.clear.shell.b);
    CHECK(config.clearColor.a == ed::EDITOR_THEME.clear.shell.a);
}

TEST_CASE("theme: the type scale (task E.6.1, TH4)") {
    const ed::ThemeTypeScale& type = ed::EDITOR_THEME.type;
    CHECK(type.bodySize == 16.0F);
    CHECK(type.smallSize == 14.0F);
    CHECK(type.strongSize == 16.0F);
    CHECK(type.monoSize == 16.0F);
    // spec D6's equal-line-height rule, each as its own assertion: a heading never changes a row height,
    // and a Console row mixes Mono and Body.
    CHECK(type.monoSize == type.bodySize);
    CHECK(type.strongSize == type.bodySize);
    CHECK(type.iconSizeRatio == 0.6875F);  // 11/16 is exact in binary
}

TEST_CASE("theme: every ImGuiStyle size member is stated (task E.6.1, TH5)") {
    // The member SET is pinned at compile time: 45 float x 4 + 15 Dp2 x 8 + 4 bool, plus 4 padding bytes
    // (3 after dockingNodeHasCloseButton, 1 after antiAliasedFill), alignment 4 on every target. A member
    // added without a row below, or removed, is a build error here rather than a silent gap.
    static_assert(sizeof(ed::ThemeMetrics) == 308U);
    // THE TWO PADDING GAPS, pinned by offset: sizeof cannot see a member that lands IN padding, so each bool
    // and the float after it are pinned. Derived from the struct: the 53 members before
    // dockingNodeHasCloseButton are 38 floats and 15 Dp2s, 38 x 4 + 15 x 8 = 272; that bool is followed by 3
    // padding bytes (276), then two floats (280, 284); the three bools sit at 284, 285 and 286, followed by 1
    // padding byte (288); five floats then end the struct at 288 + 5 x 4 = 308.
    static_assert(offsetof(ed::ThemeMetrics, dockingNodeHasCloseButton) == 272U);
    static_assert(offsetof(ed::ThemeMetrics, dockingSeparatorSize) == 276U);  // 272 + 1 + 3 padding
    static_assert(offsetof(ed::ThemeMetrics, antiAliasedLines) == 284U);      // after mouseCursorScale at 280
    static_assert(offsetof(ed::ThemeMetrics, antiAliasedLinesUseTex) == 285U);
    static_assert(offsetof(ed::ThemeMetrics, antiAliasedFill) == 286U);
    static_assert(offsetof(ed::ThemeMetrics, curveTessellationTol) == 288U);  // 286 + 1 + 1 padding
    const ed::ThemeMetrics& m = ed::EDITOR_THEME.metrics;
    // ... but a member that lands INSIDE a gap moves no offset at all: a bool after antiAliasedFill takes
    // the free byte at 287 and every pin above, sizeof included, still holds (measured). So the member
    // COUNT is pinned too: this binding names exactly 64 members, and a 65th is a build error. The names
    // are the members' own, in declaration order, for reading -- the binding checks the count, and the
    // CHECK below that its LAST name is the last member (which also keeps the binding from being unused).
    const auto& [alpha, disabledAlpha, windowPadding, windowRounding, windowBorderSize,           // 5
                 windowBorderHoverPadding, windowMinSize, windowTitleAlign, childRounding,        // 9
                 childBorderSize, popupRounding, popupBorderSize, framePadding, frameRounding,    // 14
                 frameBorderSize, itemSpacing, itemInnerSpacing, cellPadding, touchExtraPadding,  // 19
                 indentSpacing, columnsMinSpacing, scrollbarSize, scrollbarRounding,              // 23
                 scrollbarPadding, grabMinSize, grabRounding, logSliderDeadzone, imageRounding,   // 28
                 imageBorderSize, tabRounding, tabBorderSize, tabMinWidthBase,                    // 32
                 tabMinWidthShrink, tabCloseButtonMinWidthSelected,                               // 34
                 tabCloseButtonMinWidthUnselected, tabBarBorderSize, tabBarOverlineSize,          // 37
                 tableAngledHeadersAngleDegrees, tableAngledHeadersTextAlign, treeLinesSize,      // 40
                 treeLinesRounding, dragDropTargetRounding, dragDropTargetBorderSize,             // 43
                 dragDropTargetPadding, colorMarkerSize, buttonTextAlign, selectableTextAlign,    // 47
                 separatorSize, separatorTextBorderSize, separatorTextAlign,                      // 50
                 separatorTextPadding, displayWindowPadding, displaySafeAreaPadding,              // 53
                 dockingNodeHasCloseButton, dockingSeparatorSize, mouseCursorScale,               // 56
                 antiAliasedLines, antiAliasedLinesUseTex, antiAliasedFill,                       // 59
                 curveTessellationTol, circleTessellationMaxError, hoverStationaryDelay,          // 62
                 hoverDelayShort, hoverDelayNormal] = m;                                          // 64
    CHECK(&hoverDelayNormal == &m.hoverDelayNormal);
    CHECK(m.alpha == 1.0F);
    CHECK(m.disabledAlpha == 0.60F);
    CHECK(m.windowPadding.x == 8.0F);
    CHECK(m.windowPadding.y == 8.0F);
    CHECK(m.windowRounding == 0.0F);
    CHECK(m.windowBorderSize == 1.0F);
    CHECK(m.windowBorderHoverPadding == 4.0F);
    CHECK(m.windowMinSize.x == 32.0F);
    CHECK(m.windowMinSize.y == 32.0F);
    CHECK(m.windowTitleAlign.x == 0.0F);
    CHECK(m.windowTitleAlign.y == 0.5F);
    CHECK(m.childRounding == 6.0F);
    CHECK(m.childBorderSize == 1.0F);
    CHECK(m.popupRounding == 6.0F);
    CHECK(m.popupBorderSize == 1.0F);
    CHECK(m.framePadding.x == 8.0F);
    CHECK(m.framePadding.y == 4.0F);
    CHECK(m.frameRounding == 4.0F);
    CHECK(m.frameBorderSize == 1.0F);
    CHECK(m.itemSpacing.x == 8.0F);
    CHECK(m.itemSpacing.y == 6.0F);
    CHECK(m.itemInnerSpacing.x == 6.0F);
    CHECK(m.itemInnerSpacing.y == 4.0F);
    CHECK(m.cellPadding.x == 6.0F);
    CHECK(m.cellPadding.y == 3.0F);
    CHECK(m.touchExtraPadding.x == 0.0F);
    CHECK(m.touchExtraPadding.y == 0.0F);
    CHECK(m.indentSpacing == 14.0F);
    CHECK(m.columnsMinSpacing == 6.0F);
    CHECK(m.scrollbarSize == 10.0F);
    CHECK(m.scrollbarRounding == 5.0F);
    CHECK(m.scrollbarPadding == 2.0F);
    CHECK(m.grabMinSize == 10.0F);
    CHECK(m.grabRounding == 3.0F);
    CHECK(m.logSliderDeadzone == 4.0F);
    CHECK(m.imageRounding == 0.0F);
    CHECK(m.imageBorderSize == 0.0F);
    CHECK(m.tabRounding == 4.0F);
    CHECK(m.tabBorderSize == 0.0F);
    CHECK(m.tabMinWidthBase == 1.0F);
    CHECK(m.tabMinWidthShrink == 80.0F);
    CHECK(m.tabCloseButtonMinWidthSelected == -1.0F);
    CHECK(m.tabCloseButtonMinWidthUnselected == 0.0F);
    CHECK(m.tabBarBorderSize == 1.0F);
    CHECK(m.tabBarOverlineSize == 2.0F);
    CHECK(m.tableAngledHeadersAngleDegrees == 35.0F);
    CHECK(m.tableAngledHeadersTextAlign.x == 0.5F);
    CHECK(m.tableAngledHeadersTextAlign.y == 0.0F);
    CHECK(m.treeLinesSize == 1.0F);
    CHECK(m.treeLinesRounding == 0.0F);
    CHECK(m.dragDropTargetRounding == 4.0F);
    CHECK(m.dragDropTargetBorderSize == 2.0F);
    CHECK(m.dragDropTargetPadding == 3.0F);
    CHECK(m.colorMarkerSize == 3.0F);
    CHECK(m.buttonTextAlign.x == 0.5F);
    CHECK(m.buttonTextAlign.y == 0.5F);
    CHECK(m.selectableTextAlign.x == 0.0F);
    CHECK(m.selectableTextAlign.y == 0.0F);
    CHECK(m.separatorSize == 1.0F);
    CHECK(m.separatorTextBorderSize == 3.0F);
    CHECK(m.separatorTextAlign.x == 0.0F);
    CHECK(m.separatorTextAlign.y == 0.5F);
    CHECK(m.separatorTextPadding.x == 20.0F);
    CHECK(m.separatorTextPadding.y == 3.0F);
    CHECK(m.displayWindowPadding.x == 19.0F);
    CHECK(m.displayWindowPadding.y == 19.0F);
    CHECK(m.displaySafeAreaPadding.x == 3.0F);
    CHECK(m.displaySafeAreaPadding.y == 3.0F);
    CHECK(m.dockingNodeHasCloseButton);
    CHECK(m.dockingSeparatorSize == 2.0F);
    CHECK(m.mouseCursorScale == 1.0F);
    CHECK(m.antiAliasedLines);
    CHECK(m.antiAliasedLinesUseTex);
    CHECK(m.antiAliasedFill);
    CHECK(m.curveTessellationTol == 1.25F);
    CHECK(m.circleTessellationMaxError == 0.30F);
    CHECK(m.hoverStationaryDelay == 0.15F);
    CHECK(m.hoverDelayShort == 0.15F);
    CHECK(m.hoverDelayNormal == 0.40F);
}

TEST_CASE("theme: roles are distinct (a set claim) (task E.6.1, TH6)") {
    const ed::ThemePalette& p = ed::EDITOR_THEME.palette;
    // ALL SEVEN text tokens: spec D12's table has seven, and every role needs its own value.
    const std::vector<ed::Srgb8> text{p.textBright, p.text,      p.textSecondary, p.textLabel,
                                      p.textMuted,  p.textFaint, p.textDisabled};
    const std::vector<ed::Srgb8> status{p.accent, p.warning, p.error, p.success, p.info, p.critical};
    const std::vector<ed::Srgb8> surfaces{
        p.canvas, p.dialog, p.inset,        p.chrome, p.panel,  // the flat surfaces, darkest first
        p.tile,   p.raised, p.raisedHeader, p.hover,            // the lifted ones
    };
    REQUIRE(text.size() == 7U);
    REQUIRE(status.size() == 6U);
    REQUIRE(surfaces.size() == 9U);
    checkPairwiseDistinct("text", text);
    checkPairwiseDistinct("status", status);
    checkPairwiseDistinct("surfaces", surfaces);
}

TEST_CASE("theme: the palette meets its legibility floors (task E.6.1, TH7)") {
    // WCAG 2 contrast against `panel`, computed here from the bytes. The measured value is in each row's
    // comment. textFaint (2.84) and textDisabled (2.26) carry NO floor, on purpose: counts are decorative,
    // and disabled text is meant to recede.
    const ed::ThemePalette& p = ed::EDITOR_THEME.palette;
    const std::vector<FloorRow> rows{
        {"text", p.text, p.panel, 7.0},                     // 12.41
        {"textSecondary", p.textSecondary, p.panel, 4.5},   // 7.97
        {"textLabel", p.textLabel, p.panel, 4.5},           // 5.56
        {"textMuted", p.textMuted, p.panel, 3.0},           // 3.86
        {"accent", p.accent, p.panel, 4.5},                 // 7.71
        {"warning", p.warning, p.panel, 4.5},               // 7.55
        {"error", p.error, p.panel, 4.5},                   // 5.09
        {"success", p.success, p.panel, 4.5},               // 7.95
        {"info", p.info, p.panel, 4.5},                     // 6.22
        {"critical", p.critical, p.panel, 4.5},             // 4.98
        {"onAccent on accent", p.onAccent, p.accent, 4.5},  // 8.74
    };
    REQUIRE(rows.size() == 11U);
    for (const FloorRow& row : rows) {
        CAPTURE(row.name);
        const double ratio = contrastRatio(row.foreground, row.background);
        CAPTURE(ratio);
        CHECK(ratio >= row.floor);
    }
}

TEST_CASE("theme: the swatch label rule is unchanged (task E.6.1, TH8)") {
    const ed::ThemeSwatchLabel& s = ed::EDITOR_THEME.swatchLabel;
    CHECK(s.lumaR == 2126U);
    CHECK(s.lumaG == 7152U);
    CHECK(s.lumaB == 722U);
    CHECK(s.darkLabelThreshold == 1280000U);
    CHECK(s.lumaR + s.lumaG + s.lumaB == 10000U);
    // Through the rule itself: the stated crossing between grey 127 and 128, and E.4.5's measured
    // "Charcoal is the only white label".
    CHECK_FALSE(ed::materialSwatchWantsDarkLabel(ed::IconColor{.r = 127U, .g = 127U, .b = 127U, .a = 255U}));
    CHECK(ed::materialSwatchWantsDarkLabel(ed::IconColor{.r = 128U, .g = 128U, .b = 128U, .a = 255U}));
    CHECK_FALSE(ed::materialSwatchWantsDarkLabel(ed::IconColor{.r = 49U, .g = 49U, .b = 49U, .a = 255U}));
}

TEST_CASE("theme: iconColorFor reads the theme (task E.6.1, TH9)") {
    struct KindRow {
        ed::AssetKind kind;
        ed::Srgb8 theme;
    };
    const ed::ThemeAssetKinds& k = ed::EDITOR_THEME.assetKind;
    const std::vector<KindRow> rows{
        {ed::AssetKind::Folder, k.folder},      // a warm gold
        {ed::AssetKind::Texture, k.texture},    // a cool blue
        {ed::AssetKind::Model, k.model},        // a green
        {ed::AssetKind::Audio, k.audio},        // a violet
        {ed::AssetKind::Text, k.text},          // a neutral grey
        {ed::AssetKind::Material, k.material},  // a warm coral
        {ed::AssetKind::Unknown, k.unknown},    // a darker grey
    };
    REQUIRE(rows.size() == 7U);
    for (const KindRow& row : rows) {
        CAPTURE(static_cast<int>(row.kind));
        const ed::IconColor c = ed::iconColorFor(row.kind);
        CHECK(static_cast<int>(c.r) == static_cast<int>(row.theme.r));
        CHECK(static_cast<int>(c.g) == static_cast<int>(row.theme.g));
        CHECK(static_cast<int>(c.b) == static_cast<int>(row.theme.b));
        CHECK(static_cast<int>(c.a) == static_cast<int>(row.theme.a));
    }
    // The unreachable fallback is unchanged: IconColor{}, opaque black.
    const ed::IconColor fallback = ed::iconColorFor(static_cast<ed::AssetKind>(250));
    CHECK(static_cast<int>(fallback.r) == 0);
    CHECK(static_cast<int>(fallback.g) == 0);
    CHECK(static_cast<int>(fallback.b) == 0);
    CHECK(static_cast<int>(fallback.a) == 255);
}

TEST_CASE("theme: the header is pure (task E.6.1, TH10)") {
    const ed::FileReadResult read = ed::readTextFile(AERO_EDITOR_INCLUDE_DIR "/aero/editor/editor_theme.hpp");
    REQUIRE(read.text.has_value());
    const std::vector<std::string> code = codeLinesOf(*read.text);
    REQUIRE(code.size() > 100U);  // anti-vacuity: the reader read the header, not an empty file

    // A SET assertion over the bracketed names, never a count.
    std::set<std::string> includes;
    std::string includeList;
    for (const std::string& line : code) {
        if (!line.starts_with("#include")) {
            continue;
        }
        const std::size_t open = line.find_first_of("<\"");
        const std::size_t close = open == std::string::npos ? open : line.find_first_of(">\"", open + 1U);
        REQUIRE(close != std::string::npos);
        includes.insert(line.substr(open, close - open + 1U));
        includeList += line.substr(open, close - open + 1U) + " ";
    }
    CAPTURE(includeList);
    CHECK(includes == std::set<std::string>{"<aero/core/math.hpp>", "<array>", "<cstdint>"});

    // The comment-stripped CODE names no ImGui, SDL or logging token (a citation in a comment may).
    bool sawTheValue = false;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const std::string& line = code[i];
        CAPTURE(i);
        CAPTURE(line);
        CHECK(line.find("ImGui") == std::string::npos);
        CHECK(line.find("ImVec") == std::string::npos);
        CHECK(line.find("SDL") == std::string::npos);
        CHECK(line.find("AERO_LOG") == std::string::npos);
        sawTheValue = sawTheValue || line.find("EDITOR_THEME{}") != std::string::npos;
    }
    // Positive control: a reader that read nothing cannot pass.
    CHECK(sawTheValue);
}

TEST_CASE("theme: the Inspector's axis box holds a three-decimal value at these metrics (task E.6.1, TH11)") {
    // The manual validation pass found the Inspector's X/Y/Z boxes clipping "0.000" to "0.00" in a 258-wide
    // right dock where the branch point showed it whole. Two task E.3.1 tuning values met this theme: the
    // label column's floor of 5 x the font (80 at body 16, where ProggyClean 13 gave 65) and five WHOLE
    // ItemInnerSpacing gaps per row (6 here, 4 before). Every number below is a RESTATED measurement (this
    // file's posture) taken by I289's windows, so a change to any of them is re-measured here.
    //
    // The metrics the measurement was taken at, pinned to the theme: changing one invalidates the numbers.
    constexpr float BODY = 16.0F;
    constexpr float INNER = 6.0F;         // ItemInnerSpacing.x
    constexpr float CELL_PADDING = 6.0F;  // CellPadding.x
    CHECK(ed::EDITOR_THEME.type.bodySize == BODY);
    CHECK(ed::EDITOR_THEME.metrics.itemInnerSpacing.x == INNER);
    CHECK(ed::EDITOR_THEME.metrics.cellPadding.x == CELL_PADDING);

    // "0.000" in IBM Plex Sans Regular at 16: (4 x 600 + 272) units of advance x 16 / 1300 (ImGui's size is
    // ascender - descender, 1025 + 275) = 32.886 points; I289 measures the same 32.8862 off the baked font.
    constexpr float THREE_DECIMALS = 32.8862F;
    // The widest of X, Y and Z as drawAxisRow measures it: CalcTextSize rounds every width UP
    // (IM_TRUNC(w + 0.99999f)), so the 7.545 points of 'X' measure 8.
    constexpr float LETTER = 8.0F;
    // The value cell in the 258-wide dock, measured (drawAxisRow's GetContentRegionAvail in I289's window).
    // The Inspector's content is 242 (258 - 2 x 8 of window padding); the borderless two-column table spends
    // 2 x CellPadding.x = 12 between its columns; the label column is 75 -- the Cube's widest label,
    // "meshIndex" (62.535 -> 63), plus 2 x 6 -- so 242 - 12 - 75 = 155.
    constexpr float CELL = 155.0F;
    // The same cell before this fix, measured: the 80-point floor took the column, so 242 - 12 - 80 = 150.
    constexpr float CELL_BEFORE = 150.0F;
    constexpr float CONTENT = 242.0F;
    // The letter gap at scale 1: one point (inspector_model.hpp says why), and the unit gap is the inner spacing.
    constexpr float LETTER_GAP = 1.0F;
    CHECK(ed::AXIS_LETTER_GAP_DP == LETTER_GAP);

    SUBCASE("the label column's floor is 4 x the font: 64 at body 16") {
        // A Transform-only entity's widest label, "position" (44.38 -> 45), plus both cell paddings is 57 --
        // under the floor, so the floor IS the column: VF15's rule at this theme's body size.
        CHECK(ed::inspectorLabelColumnWidth(45.0F, CELL_PADDING, BODY, CONTENT) == 64.0F);
        // The Cube's widest label decides its column now; under the 5 x floor, the floor did.
        const float column = ed::inspectorLabelColumnWidth(63.0F, CELL_PADDING, BODY, CONTENT);
        CHECK(column == 75.0F);
        // The two measured cells differ by exactly the column's change: 80 under the old floor, `column` now.
        CHECK((CELL - CELL_BEFORE) == ((5.0F * BODY) - column));
    }

    SUBCASE("the box at the measured cell holds the value with a point to spare") {
        // 38.667 here; ImGui truncates an item width to a whole point, so it lays the box out at 38.
        const float box = ed::inspectorAxisBoxWidth(CELL, LETTER, LETTER_GAP, INNER);
        CAPTURE(box);
        CHECK(box >= THREE_DECIMALS + 1.0F);
        // ANTI-VACUITY -- the defect, reproduced: the budget before this fix (five WHOLE gaps), in the cell
        // the 80-point floor left, is narrower than the value. 32 is also what I289 saw ImGui lay out there.
        const float before = (CELL_BEFORE - (3.0F * LETTER) - (5.0F * INNER)) / 3.0F;
        CAPTURE(before);
        CHECK(before < THREE_DECIMALS);
    }

    SUBCASE("a five-character value fits from the content width the branch point needed: 225") {
        // DERIVED, not measured: the branch point drew ProggyClean at 13 (every character 7 wide, so "0.000"
        // is 35), with 4-point inner spacing and cell padding, and the Cube's column 63 + 2 x 4 = 71. Its box
        // was (content - 71 - 8 - 3 x 7 - 5 x 4) / 3 = (content - 120) / 3, so "0.000" was whole from content
        // 225 -- a 241-wide dock, I289's window. Here the cell at that content is 225 - 12 - 75 = 138.
        constexpr float BRANCH_POINT_CONTENT = 225.0F;
        constexpr float BRANCH_POINT_BOX = (BRANCH_POINT_CONTENT - 120.0F) / 3.0F;
        CHECK(BRANCH_POINT_BOX == 35.0F);
        constexpr float CELL_AT_THRESHOLD = BRANCH_POINT_CONTENT - (2.0F * CELL_PADDING) - 75.0F;
        // 33 exactly: every input is a whole point (CalcTextSize rounds widths up), so ImGui lays out 33.
        const float box = ed::inspectorAxisBoxWidth(CELL_AT_THRESHOLD, LETTER, LETTER_GAP, INNER);
        CAPTURE(box);
        CHECK(box == 33.0F);
        CHECK(std::trunc(box) >= THREE_DECIMALS);
        // ANTI-VACUITY: with half the inner spacing as the letter gap -- this fix's first round -- the box
        // there is 31, and "0.000" clipped in a dock the branch point showed it whole in.
        const float halfGap = ed::inspectorAxisBoxWidth(CELL_AT_THRESHOLD, LETTER, INNER * 0.5F, INNER);
        CAPTURE(halfGap);
        CHECK(std::trunc(halfGap) < THREE_DECIMALS);
    }

    SUBCASE("the budget fills the cell: three letters, three letter gaps, two unit gaps and three boxes") {
        const float box = ed::inspectorAxisBoxWidth(CELL, LETTER, LETTER_GAP, INNER);
        const float row = (3.0F * LETTER) + (3.0F * LETTER_GAP) + (2.0F * INNER) + (3.0F * box);
        CAPTURE(row);
        CHECK(row == doctest::Approx(CELL).epsilon(1e-6));
        // The two gaps are distinct parameters: swapping them moves the answer, so neither is ignored.
        const float swapped = ed::inspectorAxisBoxWidth(CELL, LETTER, INNER, LETTER_GAP);
        CHECK(swapped != box);
    }

    SUBCASE("total: NaN, the infinities, zero, negatives and an overflow all answer a finite width >= 1") {
        static_assert(noexcept(ed::inspectorAxisBoxWidth(0.0F, 0.0F, 0.0F, 0.0F)));
        // From quiet_NaN(), never a signalling one: MSVC quiets an sNaN that passes through a float lvalue.
        constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
        constexpr float INF_F = std::numeric_limits<float>::infinity();
        constexpr float MAX_F = std::numeric_limits<float>::max();
        for (const float bad : {NAN_F, INF_F, -INF_F}) {
            CAPTURE(bad);
            CHECK(ed::inspectorAxisBoxWidth(bad, LETTER, LETTER_GAP, INNER) == 1.0F);
            CHECK(ed::inspectorAxisBoxWidth(CELL, bad, LETTER_GAP, INNER) == 1.0F);
            CHECK(ed::inspectorAxisBoxWidth(CELL, LETTER, bad, INNER) == 1.0F);
            CHECK(ed::inspectorAxisBoxWidth(CELL, LETTER, LETTER_GAP, bad) == 1.0F);
        }
        CHECK(ed::inspectorAxisBoxWidth(MAX_F, -MAX_F, 0.0F, 0.0F) == 1.0F);           // 3 x -MAX overflows
        CHECK(ed::inspectorAxisBoxWidth(0.0F, 0.0F, 0.0F, 0.0F) == 1.0F);              // a zero-width cell
        CHECK(ed::inspectorAxisBoxWidth(-250.0F, LETTER, LETTER_GAP, INNER) == 1.0F);  // a negative cell
        // A negative letter or gap is finite arithmetic: it widens the box, and the answer stays finite.
        const float negativeLetter = ed::inspectorAxisBoxWidth(CELL, -LETTER, LETTER_GAP, INNER);
        const float negativeLetterGap = ed::inspectorAxisBoxWidth(CELL, LETTER, -LETTER_GAP, INNER);
        const float negativeUnitGap = ed::inspectorAxisBoxWidth(CELL, LETTER, LETTER_GAP, -INNER);
        for (const float widened : {negativeLetter, negativeLetterGap, negativeUnitGap}) {
            CAPTURE(widened);
            CHECK(std::isfinite(widened));
            CHECK(widened >= 1.0F);
        }
    }
}
