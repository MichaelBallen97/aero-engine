#pragma once
// Aero Engine -- the panels' shared chrome as VALUES (task E.6.3, spec D1-D4, D10, D14). PUBLIC and PURE: no ImGui,
// no entt, no SDL, no logging, so tier 0 reaches every decision the shared ImGui TU (panel_chrome_ui.cpp) only draws.
//
// THE ROLE MAP (D2): a panel never names a colour -- it names a ChromeRole, and chromeRoleColor reads the palette
// member of the same name, each ONCE (I287). A TRANSPARENT fill is chromeRoleColor(role, 0), the role naming the
// surface the item sits on (D1 rule 1) -- never a literal, never IM_COL32_BLACK_TRANS, never a copy of the theme
// builder's file-local withAlpha.
//
// EVERY LENGTH is a ThemePanels / ThemeMetrics dp value times a SANITISED scale: a non-finite or non-positive uiScale
// is 1 (US10's rule, US11 and PC5 pin it). Every function taking a scale takes it NON-DEFAULTED (US9).
#include <aero/core/math.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/material_inspector_model.hpp>  // MaterialSectionId

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// Every palette role the chrome draws. NO TextDisabled (D11: a disabled widget greys through BeginDisabled, a
// placeholder through ImGui's own hint slot) and NO Dialog (E.6.4's): no chrome draw can reach either.
enum class ChromeRole : std::uint8_t {
    Canvas,
    Inset,
    Chrome,
    Panel,
    Tile,
    Raised,
    RaisedHeader,
    Hover,
    Active,
    Selection,
    Border,
    Divider,
    AccentBorder,
    TextBright,
    Text,
    TextSecondary,
    TextLabel,
    TextMuted,
    TextFaint,
    OnAccent,
    Accent,
    Warning,
    Error,
    Success,
    Info,
    Critical,
};
inline constexpr std::size_t CHROME_ROLE_COUNT = 26;

// The palette member of the same name: one switch, no default (clang-diagnostic-switch), each role read ONCE.
[[nodiscard]] Srgb8 chromeRoleColor(ChromeRole role) noexcept;
// The same bytes with `a` replaced; reads nothing more. THE one spelling of a transparent or translucent fill.
[[nodiscard]] Srgb8 chromeRoleColor(ChromeRole role, std::uint8_t alpha) noexcept;

// ---- the card (D4), in POINTS -------------------------------------------------------------------------------
struct CardGeometry {
    float headerHeight = 0.0F;    // cardHeaderHeightDp x s: 32 at 1x
    float headerPaddingX = 0.0F;  // the strip's inner inset, left and right: 10
    float headerPaddingY = 0.0F;  // the FramePadding.y the header item is submitted with: 8
    float paddingX = 0.0F;        // the body's inset on both sides: 12
    float paddingY = 0.0F;        // the body's DRAWN top and bottom padding: 10
    float gap = 0.0F;             // card to card, drawn: 12
    float rounding = 0.0F;        // childRounding: 6
    float chevronWidth = 0.0F;    // the chevron's column, the OPEN chevron's INK width: 3.5 (closed: centred)
    float iconGap = 0.0F;         // chevron -> icon and icon -> title: 8
    float overflowWidth = 0.0F;   // the square overflow button, one frame height (body + 2 x framePadding.y): 24
    friend constexpr bool operator==(const CardGeometry&, const CardGeometry&) = default;
};
[[nodiscard]] CardGeometry cardGeometry(float uiScale) noexcept;

// Every x on the header strip, from the span the painter RECORDED -- never from ImGui's FontSize-sized arrow box,
// which no parameter here can reach (PC10).
struct CardHeaderLayout {
    float chevronX = 0.0F;
    float iconX = 0.0F;
    float titleX = 0.0F;
    float overflowX = 0.0F;
    friend constexpr bool operator==(const CardHeaderLayout&, const CardHeaderLayout&) = default;
};
[[nodiscard]] CardHeaderLayout cardHeaderLayout(float spanMinX, float spanMaxX, float iconAdvance, float overflowWidth,
                                                float uiScale) noexcept;

[[nodiscard]] float chipHeight(float uiScale) noexcept;  // chipHeightDp x s: 24 at 1x
// A rule `thicknessDp` thick, never thinner than one device pixel: max(t x s, 1 / max(fb, 1)). A non-finite or
// non-positive t is 1 and a non-finite fb is 1, so a NaN anywhere still gives a finite rule >= one device pixel.
// Every caller hands it a NAMED token: shell.ruleThickness (every rule and outline) or panels.tileRing.
[[nodiscard]] float hairline(float thicknessDp, float uiScale, float framebufferScale) noexcept;
// A type size in points, ROUNDED as ImGui rounds a pushed size (GetRoundedFontSize = IM_ROUND, imgui.cpp:9760), so
// AddText(font, faceSizePoints(dp, s), ...) binds exactly the face PushFont(font, dp) would. Never below 1.
[[nodiscard]] float faceSizePoints(float sizeDp, float uiScale) noexcept;
// The face size whose chevron INK is cardChevronWidth x s wide, scaled from the glyph's ink at the body size (spec
// section 15 item 13). Falls back to bodyPoints when either measurement is not a finite positive number. The painter
// hands it the OPEN chevron's ink in both states, so the closed chevron is the same face rotated, never re-normalised.
[[nodiscard]] float chevronFaceSize(float bodyPoints, float inkAtBody, float uiScale) noexcept;

// ---- the section icon tables (D10): roster glyphs only, as views of the AERO_ICON_* literals -----------------
[[nodiscard]] std::string_view materialSectionIcon(MaterialSectionId id) noexcept;
enum class ImportDetailsSection : std::uint8_t {
    Overview,
    ImportSettings,
    Hierarchy,
    Meshes,
    Materials,
    SkeletonAnimation,
    Blender,
};
inline constexpr std::size_t IMPORT_DETAILS_SECTION_COUNT = 7;
[[nodiscard]] std::string_view importDetailsSectionTitle(ImportDetailsSection section) noexcept;  // today's labels
[[nodiscard]] std::string_view importDetailsSectionIcon(ImportDetailsSection section) noexcept;
// 0 = the manifest group, 1 = the location group; any other index answers the fallback glyph.
[[nodiscard]] std::string_view projectSettingsGroupIcon(std::size_t groupIndex) noexcept;
// D1: the Blender "Searched:" list's height in text rows (was import_details_panel.cpp's `* 6.0F`).
inline constexpr float IMPORT_DETAILS_SEARCHED_VISIBLE_ROWS = 6.0F;

// ---- records: what ImGui DREW, for the GPU tier (D14) -- never the predicate a panel computed ----------------
// E.6.2's ToolbarButtonRecord, MOVED here (shell_chrome_record.hpp keeps the old name as an alias, PC8).
struct ItemRectRecord {
    Vec2 min{};
    Vec2 max{};
    bool drawnActive = false;  // drawn with the active push set
    bool enabled = false;      // ImGuiItemFlags_Disabled was CLEAR on the item, read back with GetItemFlags()
};

// One card as drawn this frame. Every x/y is SCREEN space, read off ImGui at the draw.
struct CardEntryRecord {
    Vec2 outerMin{};  // the recorded span x [top, bottom] -- the outline's rect
    Vec2 outerMax{};
    float stripMinX = 0.0F;  // the header strip as drawn
    float stripMaxX = 0.0F;
    float stripBottomY = 0.0F;  // the strip's bottom edge (the rule sits on it when open)
    Vec2 headerItemMin{};       // ImGui's item rect: the span plus the framed overhang on each side
    Vec2 headerItemMax{};
    Vec2 bodyMin{};  // the narrowed body: [span min + paddingX, span max - paddingX]
    Vec2 bodyMax{};
    float chevronX = 0.0F;  // as drawn
    float iconX = 0.0F;
    float titleX = 0.0F;
    float iconAdvance = 0.0F;    // what cardHeaderLayout was handed
    float overflowWidth = 0.0F;  // likewise
    float chevronSize = 0.0F;    // the face size the chevron was drawn at: ONE for both states (I336)
    // The chevron's INK and BOX as RenderText draws them: the bake at IM_ROUND(chevronSize) scaled by chevronSize /
    // that bake's Size, from the whole-point pen the painter hands AddText.
    float chevronInkMinX = 0.0F;   // the ink's left edge
    float chevronInkWidth = 0.0F;  // its ink width -- never the glyph's X1 - X0 box (I336)
    float chevronBoxMinX = 0.0F;   // that glyph's X0 edge, where this record places it
    float chevronBoxWidth = 0.0F;  // that glyph's X1 - X0 box: the ink must be narrower (I336)
    float chevronQuadMinX = 0.0F;  // the glyph's quad as the list RECEIVED it, off AddText's vertices: the box must
    float chevronQuadMaxX = 0.0F;  // be this quad, or the ink above is not what was drawn (I336); 0 when clipped
    float topPadding = 0.0F;       // the y handed to SetCursorScreenPos for the body - stripBottomY
    float bottomPadding = 0.0F;    // the bottomY handed to the fill and the outline - the last body item's bottom
    float gapAfter = -1.0F;        // the next card's top - this card's bottom; -1 when no card follows this frame
    bool headerRuleDrawn = false;  // the rule's AddRectFilled added vertices (it adds none at alpha 0)
    bool open = false;
    bool overflowDrawn = false;
};

// Every card host's record (Inspector, Material, Import Details, Project Settings).
struct CardRecord {
    std::uint64_t framesRecorded = 0;   // + 1 per beginCardRecordFrame: every read below is vacuous while this is 0
    std::uint32_t cardsDrawn = 0;       // this frame
    std::uint32_t bodiesSubmitted = 0;  // this frame: cards drawn OPEN
    std::uint32_t primaryButtonsDrawn = 0;
    std::string titleFontName;  // the face a title was drawn in (the ImFont* handed to AddText)
    float titleFontSize = 0.0F;
    float scaleAtDraw = 0.0F;            // the UI scale the painter laid out at (named apart from US9's token)
    std::vector<CardEntryRecord> cards;  // this frame, in draw order
    float lastBodyTableMaxX = 0.0F;      // the outer max x of the last table a body submitted (its OuterRect)
    float lastBodyTableMinY = 0.0F;      // ... and its outer min y, read off ImGui's item after EndTable
    float lastBodyTableMaxY = 0.0F;      // ... and its outer max y
    float lastSlotClearMaxX = 0.0F;      // the Material slot row's trailing Clear, max x
    float lastSpanFullWidthMinX = 0.0F;  // the min x of the last SpanFullWidth row a body submitted
    // This frame: the cards were submitted inside the Material panel's ##body child -- set in its FixedRegions arm,
    // beside the BeginChild, so it reports the host ImGui drew in. False in the Scrolling mode and for every other
    // host (I331 asserts each host's overhang from it, never from the overhang itself).
    bool bodyInChild = false;
};
// Opens one recorded frame: + 1 frame, the per-frame counts zeroed and `cards` cleared (capacity kept).
void beginCardRecordFrame(CardRecord& record) noexcept;

}  // namespace engine::editor
