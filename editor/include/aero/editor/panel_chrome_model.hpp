#pragma once
// Aero Engine -- the panels' shared chrome as VALUES (task E.6.3, spec D1-D4, D10, D14). PUBLIC and PURE: no ImGui,
// no entt, no SDL, no logging, so tier 0 reaches every decision the shared ImGui TU (panel_chrome_ui.cpp) only draws.
//
// THE ROLE MAP (D2): a panel names a palette colour as a ChromeRole, and chromeRoleColor reads the palette member of
// the same name, each ONCE (I287); D2's retained direct palette reads stay, pinned per file by I287, and a colour
// outside the palette is its own theme member. A TRANSPARENT fill is chromeRoleColor(role, 0), the role naming the
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
#include <span>
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
// The whole-point pen x that centres a chevron's INK in its column: round(chevronX + (columnWidth - inkWidth) / 2 -
// inkLeft), with the ink as the painter reads it at the drawn size (inkLeft relative to the pen). RenderText draws
// from IM_TRUNC(pen x) (imgui_draw.cpp:5791), so a whole pen draws exactly here and the centre misses the column's by
// at most half a point. That rounding is why the centring is witnessed HERE (PC11): at a whole pen a centred and a
// left-aligned ink often land on the same point, so no pixel reading can be relied on. TOTAL: a non-finite input
// counts as 0 -- the answer the painter's ink reading gives when it measures nothing -- and a sum past the float range
// answers 0, so the pen is always a finite whole point.
[[nodiscard]] float cardChevronPenX(float chevronX, float columnWidth, float inkLeft, float inkWidth) noexcept;

// ---- the hatched well's stripes (D8), as GEOMETRY ---------------------------------------------------------------
// The intersection of the convex `subject` with the convex `clipper` (Sutherland-Hodgman), so a hatch stripe stays
// inside the well's ROUNDED outline -- a square scissor let every stripe crossing a corner paint the square corner
// outside the curve. Each polygon is its vertices in order, closed implicitly. The result keeps the SUBJECT's winding
// and its vertex order (a subject wholly inside comes back vertex for vertex). The clipper may wind EITHER way:
// ImGui's PathRect winds clockwise on a y-down screen (a positive shoelace sum), and a counter-clockwise clipper is
// read from its own sign and clips the same. Computed in double, returned in float; consecutive result vertices
// within CLIP_MERGE_EPSILON of each other are merged, so ImGui never receives a near-zero edge (its normal would be
// noise, and AddConvexPolyFilled's fringe miter with it). TOTAL, and refused rather than guessed: EMPTY when a
// coordinate of either input is not finite (tested first), when either has under three vertices, when the clipper
// has no area or a reflex corner, and when what is left has under three vertices or no more area than
// CLIP_MERGE_EPSILON squared. Units are the caller's: points, here.
// In points, whatever the UI scale, so framebufferScale / 64 of a device pixel (one device pixel is
// 1 / framebufferScale points, hairline's reading): 1/64 at a framebuffer scale of 1, 1/32 on a Retina Mac, 1/16 at 4.
inline constexpr float CLIP_MERGE_EPSILON = 1.0F / 64.0F;
[[nodiscard]] std::vector<Vec2> clipConvexPolygon(std::span<const Vec2> subject, std::span<const Vec2> clipper);
// TRUE when no corner of the convex `piece` is sharper than a hatch stripe's own 45-degree corner (its cosine at
// most cos 45 + 1e-3). ImGui's fill fringe moves each vertex AA_SIZE / 2 / sin(corner / 2) along its miter, out for
// the fringe and in for the fill (imgui_draw.cpp:1121-1134), so a sharper corner -- the sliver a stripe's edge cuts
// from a top-left or bottom-right corner arc, whose segments run nearly parallel to the stripes -- pushes both past
// the outline; hatchedWell draws such a piece without the fringe. FALSE for under three vertices, a non-finite
// coordinate or a zero-length edge: a piece that cannot be measured takes no fringe.
[[nodiscard]] bool hatchPieceTakesFringe(std::span<const Vec2> piece) noexcept;

// ---- the asset-reference field's optional parts (D5), as ARITHMETIC ---------------------------------------------
// What one reference field's frame holds, left to right: padding, the thumbnail and its spacing (a Material slot's
// row: always kept), the swatch or its empty well and its spacing (the Inspector's row), the name, the faint "pick"
// hint behind its spacing, padding. Decided in this order, so the hint goes first and the swatch next as the frame
// narrows -- neither is ever drawn over the other or over the name, and the name keeps one whole ellipsis inside
// the frame's inner edge for as long as either is kept (the macOS pass's row 17 found a 34-point field drawing the
// hint ON the well and the name's ellipsis cut to its first dot at the frame's right edge):
//   1. the HINT is kept only when, beside the row's swatch (when it has one) and its spacing, the name keeps at least
//      `ellipsisWidth` before the hint's spacing. A zero `hintWidth` is no hint -- never kept, and no room reserved for
//      it -- which is how the caller's own switch (AssetFieldInputs::pickHint) reaches the budget;
//   2. the SWATCH is kept only when, beside it and its spacing, the name keeps at least `ellipsisWidth` before the
//      frame's inner edge -- the hint's rule without the hint, so the frame holds 2 x framePaddingX + swatchEdge and
//      a whole ellipsis after it -- and never beside a thumbnail (a row has one or the other; the widget hands
//      swatchEdge 0 with a thumbnail). The hint's threshold is the swatch's plus the hint and its spacing, so a kept
//      hint always has its swatch;
//   3. the name's textX / textAvail follow from what is kept; textAvail is never negative. Below 2 x framePaddingX +
//      ellipsisWidth nothing optional is left and the budget is under one ellipsis: the widget draws no name whose
//      elision does not fit its budget, so a refused input (below) draws none at all.
// UI-SCALE-FREE, unlike the lengths above, because every input is already in ImGui units: the live style's
// FramePadding.x and ItemInnerSpacing.x, the text line height, and widths measured off the scaled fonts. TOTAL: a
// non-finite or negative input keeps nothing optional and answers ReferenceFieldParts{} -- no hint, no swatch, a
// zero textX and a zero textAvail.
struct ReferenceFieldParts {
    bool hint = false;       // the faint "pick" hint draws at the frame's right
    bool swatch = false;     // the swatch (or its empty well) draws after the left padding
    float textX = 0.0F;      // the name's left edge, from the frame's left edge
    float textAvail = 0.0F;  // the name's budget: the frame's inner right edge, or the hint's spacing when it draws
    friend constexpr bool operator==(const ReferenceFieldParts&, const ReferenceFieldParts&) = default;
};
[[nodiscard]] ReferenceFieldParts referenceFieldParts(float frameWidth, float framePaddingX, float innerSpacingX,
                                                      float swatchEdge, float hintWidth, float ellipsisWidth,
                                                      float thumbnailEdge) noexcept;

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

// One vertex as a draw list RECEIVED it, copied off its VtxBuffer: the position, and the colour as ImGui packed it
// (alpha in the top byte, IM_COL32_A_SHIFT 24) -- an anti-aliased fill's outer FRINGE vertex carries alpha 0.
struct DrawnVertex {
    Vec2 pos{};
    std::uint32_t color = 0;
};

// One asset-reference field as DRAWN this frame (D5): which optional part each draw call drew, and the x span it was
// handed, SCREEN space -- a part that did not draw keeps 0 / 0. Set where each draw call is made, never from
// referenceFieldParts' answer.
struct ReferenceFieldRecord {
    float frameMinX = 0.0F;  // the button's item rect
    float frameMaxX = 0.0F;
    float swatchMinX = 0.0F;  // the swatch's or the empty well's rect, whichever drew
    float swatchMaxX = 0.0F;
    float textMinX = 0.0F;  // the name: AddText's x, and that x + CalcTextSize of the bytes AddText was handed
    float textMaxX = 0.0F;
    // The hint: drawRoleText's x, and that x + the width of PICK_HINT measured AT THE DRAW in the face drawRoleText
    // was handed -- never `hintWidth`, the layout's input, so a hint measured in another face than it is drawn in
    // shows here.
    float hintMinX = 0.0F;
    float hintMaxX = 0.0F;
    float swatchEdge = 0.0F;     // handed to referenceFieldParts, drawn or not (0: a thumbnail row's)
    float hintWidth = 0.0F;      // likewise (0: no hint wanted)
    float ellipsisWidth = 0.0F;  // likewise: the one ellipsis the name keeps beside the swatch and the hint
    bool swatchDrawn = false;    // a bound material's colour swatch
    bool wellDrawn = false;      // the empty well
    bool textDrawn = false;
    bool hintDrawn = false;
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
    // The body's clip as the list RECEIVED it (V4): end() reads, before it pops that clip, the content channel's
    // commands from the one the push left current to the last, keeping those that drew (ElemCount > 0): how many, and
    // the least and greatest ClipRect x among them. Their values, never their indices -- the splitter's Merge appends
    // the content channel after the background one and renumbers it (imgui_draw.cpp:2185-2240). A closed card: 0.
    std::uint32_t bodyDrawCommands = 0;
    float bodyClipMinX = 0.0F;
    float bodyClipMaxX = 0.0F;
    float fillClipMaxX = 0.0F;     // ClipRect.z of the command that received the body's fill (an open card's)...
    float outlineClipMaxX = 0.0F;  // ...and the outline's: drawn after the pop, so each keeps the window's clip
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
    // The Material slot row's colour-space note, the LAST one ItemAdd let through this frame (IsItemVisible: on
    // screen, so it was rendered): the index in `cards` of the card it sits in, and its item rect -- TextEx's
    // position + CalcTextSize, the text's UNCLIPPED extent (imgui_widgets.cpp:177-186). -1 / 0 / 0: none drew.
    int slotNoteCard = -1;
    float slotNoteMinX = 0.0F;
    float slotNoteMaxX = 0.0F;
    float lastSpanFullWidthMinX = 0.0F;  // the min x of the last SpanFullWidth row a body submitted
    // This frame: the cards were submitted inside the Material panel's ##body child -- set in its FixedRegions arm,
    // beside the BeginChild, so it reports the host ImGui drew in. False in the Scrolling mode and for every other
    // host (I331 asserts each host's overhang from it, never from the overhang itself).
    bool bodyInChild = false;
};
// Opens one recorded frame: + 1 frame, the per-frame counts zeroed and `cards` cleared (capacity kept).
void beginCardRecordFrame(CardRecord& record) noexcept;

}  // namespace engine::editor
