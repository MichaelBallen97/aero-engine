#pragma once
// Aero Engine — the editor's ONE theme (task E.6.1). PURE: no ImGui, no SDL, no GPU, no logging, so
// every pure model and tier 0 can read it. Category A (the ImGui chrome and the UI roles) adopts the
// mock; Category B (the viewport's 3D/overlay colours, identity colours and clears) moved here
// byte-identical. A NEW colour is a token here, never a file-local constant (.claude/rules/editor.md,
// "Fonts, icons and the theme").
//
// EVERY STRUCT IS AT NAMESPACE SCOPE, never nested in EditorTheme: a nested struct's default member
// initialisers are parsed only once the enclosing class is complete, which libstdc++ refuses in places
// libc++ accepts (E.5.2's Linux-tidy trap). Every number is a default member initialiser below, each
// with its source: a mock sample (spec D12), "1.92.8 default, restated", or "moved from <file:line> at
// E.6.1, value unchanged".
#include <aero/core/math.hpp>

#include <array>
#include <cstdint>

namespace engine::editor {

// A colour SPECIFIED as sRGB bytes: what ImGui draws on this tree's UNORM swapchain (SDR composition is
// B8G8R8A8_UNORM on Metal and D3D12 and B8G8R8A8/R8G8B8A8_UNORM on Vulkan -- no conversion on the way
// out).
struct Srgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    friend constexpr bool operator==(const Srgb8&, const Srgb8&) = default;
};

// A colour SPECIFIED as floats -- a Category-B value its source wrote that way, kept in floats so
// "moved byte-identical" means value-identical in its own form.
struct Rgbaf {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
    friend constexpr bool operator==(const Rgbaf&, const Rgbaf&) = default;
};

struct Dp2 {  // a length pair in dp (ImGui units at uiScale 1)
    float x = 0.0F;
    float y = 0.0F;
    friend constexpr bool operator==(const Dp2&, const Dp2&) = default;
};

// spec D12: each a MEASURED sRGB triple from the mock PNGs, opaque -- not a design guess. Each comment
// names where the mock shows it. `critical` is the one token the mock does not show.
struct ThemePalette {
    Srgb8 canvas{13U, 15U, 18U, 255U};            // viewport background, the active Viewport tab
    Srgb8 dialog{16U, 18U, 21U, 255U};            // the New Project dialog body
    Srgb8 inset{18U, 21U, 26U, 255U};             // every text field, pick field, filter
    Srgb8 chrome{20U, 23U, 27U, 255U};            // toolbar, tab bars, status bar, Console header
    Srgb8 panel{24U, 27U, 32U, 255U};             // panel bodies, menu bar, the selected tab
    Srgb8 tile{25U, 29U, 35U, 255U};              // asset tiles, the dialog's info box
    Srgb8 raised{28U, 32U, 39U, 255U};            // cards, segmented groups, popups
    Srgb8 raisedHeader{32U, 37U, 45U, 255U};      // a component card's header strip
    Srgb8 hover{36U, 41U, 50U, 255U};             // the hovered menu-bar item
    Srgb8 active{43U, 49U, 58U, 255U};            // the selected segment
    Srgb8 selection{31U, 43U, 51U, 255U};         // the selected tree row
    Srgb8 border{38U, 43U, 51U, 255U};            // the 1-dp outline of fields, cards, groups
    Srgb8 divider{35U, 39U, 46U, 255U};           // panel dividers, the toolbar's bottom rule
    Srgb8 accentBorder{58U, 111U, 133U, 255U};    // the focused field's outline
    Srgb8 textBright{232U, 234U, 237U, 255U};     // the active tab label
    Srgb8 text{215U, 219U, 224U, 255U};           // primary text
    Srgb8 textSecondary{169U, 177U, 187U, 255U};  // menu-bar items
    Srgb8 textLabel{139U, 147U, 158U, 255U};      // Inspector labels
    Srgb8 textMuted{111U, 120U, 131U, 255U};      // de-emphasised ENABLED text (spec D16)
    Srgb8 textFaint{91U, 99U, 110U, 255U};        // counts -- decorative, no legibility floor (TH7)
    Srgb8 textDisabled{77U, 84U, 94U, 255U};      // ImGui's disabled widgets -- no floor (TH7)
    Srgb8 onAccent{11U, 12U, 14U, 255U};          // text on an accent fill
    Srgb8 accent{95U, 184U, 220U, 255U};          // the active tool, a checked box, the tab overline
    Srgb8 warning{216U, 162U, 75U, 255U};         // WARN, the dirty dot
    Srgb8 error{216U, 106U, 106U, 255U};          // ERROR
    Srgb8 success{134U, 191U, 106U, 255U};        // a valid path
    Srgb8 info{106U, 159U, 216U, 255U};           // an informational dot
    // NOT in the mock: console_panel.cpp:68's (1.00, 0.25, 0.25) in bytes (0.25 x 255 = 63.75 -> 64),
    // so a Critical record stays distinct from an Error one.
    Srgb8 critical{255U, 64U, 64U, 255U};
};

// spec D6, in ImGui SIZE units (= ascender - descender = 1.3 x the CSS em for Plex). Named `...Size`
// rather than `small` & co.: `small` is a macro on Windows (rpcndr.h), and this header is included
// everywhere. Strong and Mono equal Body ON PURPOSE: a heading never changes a row height, and a Console
// row mixes Mono and Body.
struct ThemeTypeScale {
    float bodySize = 16.0F;               // the default font; a body line is exactly 16 dp at 1x
    float smallSize = 14.0F;              // first consumer E.6.3
    float strongSize = 16.0F;             // SemiBold
    float monoSize = 16.0F;               // Plex Mono
    float iconSizeRatio = 11.0F / 16.0F;  // spec D7: Lucide merged at 11/16 of its face; exactly 0.6875F
};

// spec D14: EVERY ImGuiStyle size, alignment and behaviour member, in dp at uiScale 1 -- one member per
// ImGuiStyle member, named after it in camelBack, in the constructor's order (imgui.cpp:1512-1590 at
// the pinned 1.92.8). An unmarked member restates 1.92.8's own default, so the theme is the stated source
// of every number rather than an inheritor of whatever the port ships; a ★ marks a value the mock
// decides, with its evidence. The enum and flag members (WindowMenuButtonPosition, ColorButtonPosition,
// TreeLinesFlags, HoverFlagsForTooltip*) are not visual tokens and are not here.
struct ThemeMetrics {
    float alpha = 1.0F;
    float disabledAlpha = 0.60F;
    Dp2 windowPadding{8.0F, 8.0F};
    float windowRounding = 0.0F;  // docked panels are square in the mock
    float windowBorderSize = 1.0F;
    float windowBorderHoverPadding = 4.0F;
    Dp2 windowMinSize{32.0F, 32.0F};
    Dp2 windowTitleAlign{0.0F, 0.5F};
    float childRounding = 6.0F;  // ★ cards and groups ~6.8 dp
    float childBorderSize = 1.0F;
    float popupRounding = 6.0F;  // ★ cards and groups ~6.8 dp
    float popupBorderSize = 1.0F;
    Dp2 framePadding{8.0F, 4.0F};      // ★ a frame is 24 dp at body 16; fields 23-27 dp, text inset ~9
    float frameRounding = 4.0F;        // ★ buttons ~3.4 dp, fields ~6.8 dp
    float frameBorderSize = 1.0F;      // ★ every field and group has a 1-dp `border` outline
    Dp2 itemSpacing{8.0F, 6.0F};       // ★ rows 26-34 dp apart around 16-25-dp items
    Dp2 itemInnerSpacing{6.0F, 4.0F};  // ★ checkbox-to-label ~8 dp
    Dp2 cellPadding{6.0F, 3.0F};       // ★ the Inspector's label column breathes
    Dp2 touchExtraPadding{0.0F, 0.0F};
    float indentSpacing = 14.0F;  // ★ tree indent 13.7 dp
    float columnsMinSpacing = 6.0F;
    float scrollbarSize = 10.0F;     // ★ no scrollbar is drawn in the mock; thin by intent
    float scrollbarRounding = 5.0F;  // ★ thin by intent, like the bar
    float scrollbarPadding = 2.0F;
    float grabMinSize = 10.0F;  // ★ spec D14
    float grabRounding = 3.0F;  // ★ spec D14
    float logSliderDeadzone = 4.0F;
    float imageRounding = 0.0F;
    float imageBorderSize = 0.0F;
    float tabRounding = 4.0F;  // ★ spec D14
    float tabBorderSize = 0.0F;
    float tabMinWidthBase = 1.0F;
    float tabMinWidthShrink = 80.0F;
    float tabCloseButtonMinWidthSelected = -1.0F;   // a sentinel: ScaleAllSizes scales only a positive value
    float tabCloseButtonMinWidthUnselected = 0.0F;  // a sentinel
    float tabBarBorderSize = 1.0F;
    float tabBarOverlineSize = 2.0F;  // ★ the active tab's 2-dp accent overline
    // Degrees here; the builder converts with ImGui's own 35 * (IM_PI / 180) spelling.
    float tableAngledHeadersAngleDegrees = 35.0F;
    Dp2 tableAngledHeadersTextAlign{0.5F, 0.0F};
    float treeLinesSize = 1.0F;
    float treeLinesRounding = 0.0F;
    float dragDropTargetRounding = 4.0F;  // ★ matches the frame
    float dragDropTargetBorderSize = 2.0F;
    float dragDropTargetPadding = 3.0F;
    float colorMarkerSize = 3.0F;
    Dp2 buttonTextAlign{0.5F, 0.5F};
    Dp2 selectableTextAlign{0.0F, 0.0F};
    float separatorSize = 1.0F;
    float separatorTextBorderSize = 3.0F;
    Dp2 separatorTextAlign{0.0F, 0.5F};
    Dp2 separatorTextPadding{20.0F, 3.0F};
    Dp2 displayWindowPadding{19.0F, 19.0F};
    Dp2 displaySafeAreaPadding{3.0F, 3.0F};
    bool dockingNodeHasCloseButton = true;
    float dockingSeparatorSize = 2.0F;
    float mouseCursorScale = 1.0F;
    bool antiAliasedLines = true;
    bool antiAliasedLinesUseTex = true;
    bool antiAliasedFill = true;
    float curveTessellationTol = 1.25F;
    float circleTessellationMaxError = 0.30F;
    float hoverStationaryDelay = 0.15F;
    float hoverDelayShort = 0.15F;
    float hoverDelayNormal = 0.40F;
};

// Moved from axis_palette.hpp:37-44 at E.6.1, value unchanged. axis_palette.hpp's names are aliases of
// these, and its comment says how the linear values were derived from the bytes (AX1 pins the trip).
struct ThemeAxisColors {
    Vec4 xLinear{0.7605F, 0.0529F, 0.0666F, 1.0F};  // sRGB 226, 65, 73
    Vec4 yLinear{0.2051F, 0.5711F, 0.0467F, 1.0F};  // sRGB 125, 199, 61
    Vec4 zLinear{0.0395F, 0.2346F, 0.7605F, 1.0F};  // sRGB  56, 133, 226
    std::array<std::uint8_t, 3> xSrgb{226U, 65U, 73U};
    std::array<std::uint8_t, 3> ySrgb{125U, 199U, 61U};
    std::array<std::uint8_t, 3> zSrgb{56U, 133U, 226U};
    // Moved from inspector_model.cpp:304-309 at E.6.1, value unchanged: the Inspector's out-of-range
    // axis row (axisRowColor).
    // ImGui's OWN fourth default colour-channel marker, IM_COL32(140,140,140,255)
    // (imgui_widgets.cpp:2257-2260 at the pinned 1.92.8) -- so the out-of-range answer is borrowed rather
    // than invented. IT HAS NO CALLER TODAY: AXIS_ROW_COMPONENTS is 3 and every loop in the panel stops
    // there. It exists because axisRowColor is TOTAL, and a total function needs an answer for every
    // index; VF3 is what keeps that answer from silently becoming X's red.
    std::array<std::uint8_t, 3> neutralSrgb{140U, 140U, 140U};
};

// Moved from gizmo_style.hpp:76-100 and :149-150 at E.6.1, unchanged, types included. gizmo_style.hpp's
// GIZMO_* names are aliases of these and keep every comment saying why each value is what it is.
struct ThemeGizmo {
    float translationLineThicknessPoints = 4.0F;
    float translationArrowSizePoints = 10.0F;
    float rotationLineThicknessPoints = 3.0F;
    float rotationScreenRingThicknessPoints = 2.0F;
    float scaleLineThicknessPoints = 4.0F;
    float scaleDiscRadiusPoints = 7.0F;
    float hatchedAxisThicknessPoints = 0.0F;
    float centerDiscRadiusPoints = 7.0F;
    float centerHitHalfExtentPoints = 10.0F;
    std::uint8_t planeFillAlpha = 115;
    std::array<std::uint8_t, 4> highlightSrgb{255U, 232U, 64U, 255U};
    std::uint8_t rotationFillAlpha = 96;
    std::array<std::uint8_t, 4> inactiveSrgb{153U, 153U, 153U, 153U};
    std::array<std::uint8_t, 4> translationLineSrgb{220U, 220U, 220U, 200U};
    std::array<std::uint8_t, 4> scaleLineSrgb{150U, 150U, 150U, 255U};
    std::array<std::uint8_t, 4> hatchedAxisSrgb{0U, 0U, 0U, 128U};
    std::array<std::uint8_t, 4> textSrgb{255U, 255U, 255U, 255U};
    std::array<std::uint8_t, 4> textShadowSrgb{0U, 0U, 0U, 255U};
    float axisLengthPoints = 90.0F;
    float axisMaxViewportFraction = 0.15F;
};

// Moved from viewport_gizmos.hpp:48-55 at E.6.1, unchanged -- WITH its NOLINT, which belongs to the
// literal. LINEAR RGBA; the comment above ViewportGizmoTints says how each relates to its sRGB bytes.
struct ThemeViewportTints {
    // The NOLINT is not a waiver: 176/255 decoded through the sRGB EOTF is 0.43415, which happens to
    // sit 9.4e-05 from std::numbers::log10e and trips modernize-use-std-numbers. Every accurate
    // spelling of this colour does -- it is a coincidence of the constant, not a missed abstraction.
    // NOLINTNEXTLINE(modernize-use-std-numbers)
    Vec4 primarySelected{1.0F, 0.4342F, 0.0513F, 1.0F};       // sRGB 255,176,64,255 -- ..._PRIMARY_DEFAULT
    Vec4 secondarySelected{1.0F, 0.2961F, 0.0144F, 0.7451F};  // sRGB 255,148,32,190 -- ..._SECONDARY_DEFAULT
    Vec4 unselected{0.62F, 0.64F, 0.68F, 0.90F};              // chrome, brighter than the grid's lines
    Vec4 mutedDirectional{0.30F, 0.31F, 0.33F, 0.35F};        // "the bridge ignored this one"
};

// Moved from asset_view.cpp:175-187 at E.6.1, unchanged -- the hex spelling kept. iconColorFor returns
// these.
struct ThemeAssetKinds {
    Srgb8 folder{0xE0U, 0xB0U, 0x30U, 255U};    // a warm gold
    Srgb8 texture{0x40U, 0x90U, 0xD0U, 255U};   // a cool blue
    Srgb8 model{0x60U, 0xB0U, 0x60U, 255U};     // a green
    Srgb8 audio{0xA0U, 0x60U, 0xC0U, 255U};     // a violet
    Srgb8 text{0x90U, 0x90U, 0x90U, 255U};      // a neutral grey
    Srgb8 material{0xE0U, 0x70U, 0x45U, 255U};  // a warm coral
    Srgb8 unknown{0x50U, 0x50U, 0x50U, 255U};   // a darker grey
};

// Moved from asset_tile.cpp:24/:84 and material_card.cpp:41-46 at E.6.1 (spec D17), every value
// unchanged: a contrast answer measured right at E.4.5 (Charcoal 49,49,49 is the only white label across
// its seven fixtures), not re-tuned by a refactor.
struct ThemeSwatchLabel {
    Srgb8 onLight{24U, 24U, 24U, 255U};    // the kind label on a PALE swatch (was DARK_SWATCH_LABEL)
    Srgb8 onDark{255U, 255U, 255U, 255U};  // every other kind label (was IM_COL32_WHITE)
    // 0.2126 / 0.7152 / 0.0722 in ten-thousandths -- they sum to exactly 10 000, so grey g weighs
    // 10 000 g and the threshold 128 * 10 000 is crossed between 127 and 128 with no rounding anywhere.
    std::uint32_t lumaR = 2126U;
    std::uint32_t lumaG = 7152U;
    std::uint32_t lumaB = 722U;
    std::uint32_t darkLabelThreshold = 128U * 10000U;
};

struct EditorTheme {
    ThemePalette palette;
    ThemeTypeScale type;
    ThemeMetrics metrics;
    ThemeAxisColors axis;
    ThemeGizmo gizmo;
    ThemeViewportTints viewportTints;
    ThemeAssetKinds assetKind;
    ThemeSwatchLabel swatchLabel;
};

inline constexpr EditorTheme EDITOR_THEME{};  // THE one value

inline constexpr float UI_SCALE_MIN = 0.5F;
inline constexpr float UI_SCALE_MAX = 4.0F;              // "a 400 % display"; nothing is designed past it
inline constexpr float UI_SCALE_STEPS_PER_UNIT = 20.0F;  // quantum 0.05, applied by DIVISION (spec D8)

// spec D8. PURE, TOTAL, noexcept: the factor by which the UI grows in WINDOW coordinates, quantised and
// clamped. `previous` is kept when the inputs are unusable (a minimised window, an SDL error); a
// non-finite or non-positive `previous` is itself replaced by 1.0, and a kept value is clamped like a
// computed one, so the result is finite and in [UI_SCALE_MIN, UI_SCALE_MAX] for EVERY input (US6).
[[nodiscard]] float resolveUiScale(float displayScale, float pixelDensity, float previous) noexcept;

}  // namespace engine::editor
