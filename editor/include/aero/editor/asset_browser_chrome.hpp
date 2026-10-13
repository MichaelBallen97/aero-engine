#pragma once
// Aero Engine -- the Assets panel's chrome as VALUES (task E.6.3, spec D8). PUBLIC and PURE: no ImGui, no GPU, no
// <filesystem>, no logging, so tier 0 walks every width, scale and count (AH1-AH17) that asset_browser_panel.cpp only
// measures and draws. The record at the bottom is what the panel DREW, for the GPU tier.
#include <aero/core/math.hpp>                  // Vec2
#include <aero/editor/asset_meta.hpp>          // AssetRecord
#include <aero/editor/panel_chrome_model.hpp>  // ChromeRole

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>  // std::less<>: the count map's transparent comparator
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// Moved from asset_browser_panel.cpp:52-53 (D1): first-run widths in FONT units, so a DPI change moves them with the
// font. The tree pane's child is ResizeX, so the user's own width persists in the ini. AH16 pins the values, I324 where
// the panel reads them.
inline constexpr float ASSET_TREE_PANE_FONT_MULTIPLE = 14.0F;
inline constexpr float ASSET_SIZE_COLUMN_FONT_MULTIPLE = 6.0F;

// ---- the header row (D8) ----------------------------------------------------------------------------------------
// Moved from asset_browser_panel.cpp (D1, the code-review round): the header's three field widths in FONT units, so a
// DPI change moves them with the font. The panel sets AssetHeaderWidths' search, searchMin and kind to GetFontSize()
// times each; AH16 pins the values and I323 ties the widths the panel recorded to them.
inline constexpr float ASSET_HEADER_SEARCH_FONT_MULTIPLE = 12.0F;     // the search field's preferred width
inline constexpr float ASSET_HEADER_SEARCH_MIN_FONT_MULTIPLE = 6.0F;  // the narrowest Minimal shrinks it to
inline constexpr float ASSET_HEADER_KIND_FONT_MULTIPLE = 8.0F;        // the kind combo

// Declared widest first, so "a narrower row never yields a wider mode" reads as an ordering (I323(e)).
enum class AssetHeaderMode : std::uint8_t { Full, Compact, Minimal };

// The controls' widths in POINTS, MEASURED by the panel at the live font (a frame's padding included).
struct AssetHeaderWidths {
    float newMaterial = 0.0F;
    float search = 0.0F;      // the search field's preferred width: ASSET_HEADER_SEARCH_FONT_MULTIPLE em
    float searchMin = 0.0F;   // the narrowest Minimal may shrink it to: ASSET_HEADER_SEARCH_MIN_FONT_MULTIPLE em
    float kind = 0.0F;        // ASSET_HEADER_KIND_FONT_MULTIPLE em
    float viewToggle = 0.0F;  // Grid | List, both segments
    float refresh = 0.0F;
    float autoRefresh = 0.0F;
    float overflow = 0.0F;
    friend constexpr bool operator==(const AssetHeaderWidths&, const AssetHeaderWidths&) = default;
};

// ABSOLUTE x's, rowMinX included. A folded control's x is the overflow's: finite, and never drawn.
struct AssetHeaderLayout {
    AssetHeaderMode mode = AssetHeaderMode::Minimal;
    float newMaterialX = 0.0F;
    float searchX = 0.0F;
    float searchWidth = 0.0F;
    float kindX = 0.0F;
    float viewToggleX = 0.0F;
    float refreshX = 0.0F;
    float autoRefreshX = 0.0F;
    float overflowX = 0.0F;
    bool kindInline = false;
    bool refreshInline = false;
    bool autoRefreshInline = false;
    friend constexpr bool operator==(const AssetHeaderLayout&, const AssetHeaderLayout&) = default;
};

// New Material at rowMinX; the cluster -- search, kind, Grid | List, Refresh, Auto-refresh, the overflow -- right-
// aligned, itemSpacing.x x uiScale apart. Full holds every control inline; Compact folds Refresh and Auto-refresh into
// the overflow; Minimal folds the kind filter too and lets the search shrink to searchMin. At or above
// assetHeaderMinimalFloor every inline rect lies inside [rowMinX, rowMinX + rowWidth]; below it the controls are laid
// out left-anchored at their own widths, so the order is kept, every x stays finite and only the right end crosses.
// A non-finite or negative width counts as 0 and a bad scale as 1, so a NaN row is Minimal with finite x's (AH3).
[[nodiscard]] AssetHeaderLayout assetHeaderLayout(float rowMinX, float rowWidth, const AssetHeaderWidths& widths,
                                                  float uiScale) noexcept;
// D8's floor: New Material + searchMin + Grid | List + the overflow + three gaps -- the narrowest row Minimal lays
// out whole. The tests and D8's "never clips" claim share this one definition.
[[nodiscard]] float assetHeaderMinimalFloor(const AssetHeaderWidths& widths, float uiScale) noexcept;

// ---- the grouped contents pane (D8) -----------------------------------------------------------------------------
struct AssetContentsPaneLayout {
    float crumbRowHeight = 0.0F;  // frameHeight + 2 x panelHeaderPaddingY x uiScale: 30 at 1x
    float contentsHeight = 4.0F;  // the ##contents child, >= 4: ImGui draws no child shorter (and 0 is "fill")
    friend constexpr bool operator==(const AssetContentsPaneLayout&, const AssetContentsPaneLayout&) = default;
};
// crumbRowHeight + dividerHeight + itemSpacingY + contentsHeight == paneHeight for every pane of at least that chrome
// + 4 -- and assetBrowserLayout hands out no shorter pane, since the panel records assetContentsPaneChromeHeight as
// its paneChromeHeight -- so the group is exactly the tree pane's height (AV66's identity; drawn, I324 at a tall pane
// and I231's 640-point subcase at the floor).
[[nodiscard]] AssetContentsPaneLayout assetContentsPaneLayout(float paneHeight, float frameHeight, float dividerHeight,
                                                              float itemSpacingY, float uiScale) noexcept;
// task E.6.3 (the code-review round): the pane's FIXED chrome above the child -- crumbRowHeight + dividerHeight +
// itemSpacingY, from assetContentsPaneLayout's own inputs (37 at 1x). The panel records it before the budget is sized,
// so the crumb row is reserved rather than carved out of a pane too short to hold it.
[[nodiscard]] float assetContentsPaneChromeHeight(float frameHeight, float dividerHeight, float itemSpacingY,
                                                  float uiScale) noexcept;

// ---- the crumb row (D8) -----------------------------------------------------------------------------------------
struct AssetCrumb {
    std::string label;     // the segment verbatim -- a "##" is drawn, never parsed (the crumb is an InvisibleButton)
    std::string relative;  // the segment-wise prefix; "" for the root
};
// Root first ({rootLabel, ""}), then one crumb per non-empty segment of `currentDir`. Clears `out`.
void assetBreadcrumbCrumbs(std::string_view rootLabel, std::string_view currentDir, std::vector<AssetCrumb>& out);
// "N items " AERO_GLYPH_MIDDLE_DOT " drag into the viewport to place"; "1 item ..." for one; N grouped as
// groupThousands groups it. The caller passes the RECURSIVE count under the current folder (D8). Overwrites `out`.
void assetItemsText(std::size_t items, std::string& out);

struct AssetCrumbRowLayout {
    float firstCrumbX = 0.0F;    // rowMinX + framePadding.x x uiScale
    float crumbGap = 0.0F;       // crumb -> separator and separator -> crumb: itemInnerSpacing.x x uiScale
    float labelOffsetY = 0.0F;   // the crumbs' frame top below the row's: panelHeaderPaddingY x uiScale
    float crumbsEndX = 0.0F;     // the last crumb's right edge
    float itemsX = 0.0F;         // right-aligned at the row's max less the inset; never left of crumbsEndX + a gap
    float itemsClipMaxX = 0.0F;  // the row's max: a too-long items text clips here, never over a crumb
    friend constexpr bool operator==(const AssetCrumbRowLayout&, const AssetCrumbRowLayout&) = default;
};
// `inkWidth` is every crumb label and separator measured; `gapCount` the crumbGaps between them (2 x (n - 1)).
[[nodiscard]] AssetCrumbRowLayout assetCrumbRowLayout(float rowMinX, float rowWidth, float inkWidth,
                                                      std::size_t gapCount, float itemsWidth, float uiScale) noexcept;

// ---- the tree row (D8) ------------------------------------------------------------------------------------------
struct AssetTreeRowLayout {
    float labelMaxX = 0.0F;  // the label elides before this
    float countX = 0.0F;     // the count's left edge, right-aligned at rowMaxX - rowMetaInsetX x uiScale
    bool countShown = false;
    friend constexpr bool operator==(const AssetTreeRowLayout&, const AssetTreeRowLayout&) = default;
};
// The Hierarchy's rule (hierarchyRowLayout): the count is dropped before the label would shrink below one body em.
[[nodiscard]] AssetTreeRowLayout assetTreeRowLayout(float labelX, float rowMaxX, float countWidth,
                                                    float uiScale) noexcept;

// ---- the folder glyph and the recursive counts (D3, D8) -----------------------------------------------------------
// The mock's folder: a 22 x 6 tab over a 53 x 38 body in an 85 x 84 well, as fractions of the well.
inline constexpr float FOLDER_TAB_WIDTH_FRACTION = 22.0F / 85.0F;
inline constexpr float FOLDER_TAB_HEIGHT_FRACTION = 6.0F / 84.0F;
inline constexpr float FOLDER_BODY_WIDTH_FRACTION = 53.0F / 85.0F;
inline constexpr float FOLDER_BODY_HEIGHT_FRACTION = 38.0F / 84.0F;
struct FolderGlyph {
    Vec2 tabMin{};
    Vec2 tabMax{};
    Vec2 bodyMin{};
    Vec2 bodyMax{};
};
// The body centred in x, the tab on its left edge directly above it, the pair centred in y. A degenerate, inverted
// or non-finite well yields every corner at wellMin (or the origin when that is non-finite) -- never an inverted rect.
[[nodiscard]] FolderGlyph folderGlyphRects(Vec2 wellMin, Vec2 wellMax) noexcept;

// One per panel. A count equals countRecordsUnder's for the same records (the segment-wise rule); the answer is
// computed for EVERY folder in one pass when the generation moves, so a draw walk asks a map, never the record list.
class FolderCountCache {
public:
    [[nodiscard]] std::size_t countUnder(std::span<const AssetRecord> records, std::uint64_t generation,
                                         std::string_view folderRelative);
    [[nodiscard]] std::uint64_t recomputations() const noexcept { return recomputationCount; }

private:
    std::optional<std::uint64_t> cachedGeneration;
    std::size_t total = 0;  // the root's count: every record
    std::map<std::string, std::size_t, std::less<>> counts;
    std::uint64_t recomputationCount = 0;
};

// ---- the footer (D8) --------------------------------------------------------------------------------------------
struct AssetFooterText {
    std::string selection;  // the selected path and its segments, as today's line composed them
    std::string notice;     // the listing's "N skipped" / truncation sentence (AC-8: never silent); "" omits it
    std::string matches;    // "" unless a query is committed
    std::size_t issues = 0;
    std::size_t thumbnailsReady = 0;
    std::size_t thumbnailsBudget = 0;  // MAX_THUMBNAILS_RESIDENT; 0 omits the segment
    std::string watcher;               // assetFooterWatchText's word, verbatim; "" omits it
};
struct AssetFooterSegment {
    std::string text;
    ChromeRole role = ChromeRole::TextFaint;
    float x = 0.0F;
};
struct AssetFooterLayout {
    std::string left;  // the selection elided from the LEFT (elidePathLeft); "" when no room is left
    float leftX = 0.0F;
    std::vector<AssetFooterSegment> right;  // each segment whole, never cut; see below for which ones are kept
};
// Mono small is monospaced, so a width is code points x `monoSmallAdvance`. Right zone, in order: the listing's
// notice (warning), the matches (textFaint), "N issues" (warning, omitted at 0), "thumbnails R/B" (textFaint), the
// watcher word (textFaint). While the zone is wider than the row it sheds "thumbnails R/B" first, then the watcher
// word (the status bar shows the same watch state) -- NEVER the notice, the matches or the issues. What is left is
// right-aligned at the row's max when it fits, else anchored AT rowMinX (never left of it), so the notice stays on
// screen and the window clips the overflow at the row's right end. The left zone takes what the right one leaves,
// and the shedding reads the right zone's width ALONE, so that can be nothing: in a row less than one gap plus one
// Mono cell wider than the kept segments, the selection is not drawn at all (AH15 records it).
[[nodiscard]] AssetFooterLayout assetFooterLayout(float rowMinX, float rowWidth, const AssetFooterText& text,
                                                  float monoSmallAdvance, float uiScale);

// ---- what the panel DREW (D14, §6.5) ----------------------------------------------------------------------------
enum class AssetHeaderControl : std::uint8_t { NewMaterial, Search, Kind, ViewToggle, Refresh, AutoRefresh, Overflow };
inline constexpr std::size_t ASSET_HEADER_CONTROL_COUNT = 7;
struct AssetDrawnRect {
    bool drawn = false;
    Vec2 min{};
    Vec2 max{};
};
struct AssetFolderTileCount {
    std::string relative;
    std::size_t count = 0;
};
struct AssetChromeRecord {
    std::uint64_t framesRecorded = 0;
    AssetHeaderMode headerMode = AssetHeaderMode::Full;
    AssetHeaderLayout headerLayout{};  // what assetHeaderLayout answered, as the panel used it
    float headerRowMinX = 0.0F;
    float headerRowWidth = 0.0F;
    AssetHeaderWidths headerWidths{};  // what the panel measured and passed
    float headerUiScale = 1.0F;
    std::array<AssetDrawnRect, ASSET_HEADER_CONTROL_COUNT> headerRects{};  // indexed by AssetHeaderControl, as DRAWN
    std::vector<AssetHeaderControl> overflowDrawn;                         // the controls folded into the overflow
    bool headerRuleDrawn = false;  // the rule's AddRectFilled added vertices to the list
    std::uint32_t crumbsDrawn = 0;
    // attachFolderDropTarget's calls this frame, every site, counted inside the callee; crumbDropTargets is the crumb
    // loop's share of it, never a bump beside the call (the code-review round's finding 14).
    std::uint32_t folderDropTargetsAttached = 0;
    std::uint32_t crumbDropTargets = 0;  // folder drop targets attached in the crumb row this frame
    std::string itemsText;               // the string handed to the items text's draw call
    float crumbRowHeight = 0.0F;
    Vec2 treeChildMin{};
    Vec2 treeChildMax{};
    Vec2 contentsChildMin{};
    Vec2 contentsChildMax{};
    float contentsGroupMaxY = 0.0F;
    std::uint32_t treeRowsDrawn = 0;
    std::uint32_t treeCurrentLabelColor = 0;  // the packed colour the current row's label draw received
    std::uint32_t treeArrowTextColor = 0;     // the Text colour pushed around every node
    float treeSelectionRounding = 0.0F;       // the rounding handed to the current row's fill
    std::uint32_t tilesRinged = 0;
    std::uint32_t lastRestingRingColor = 0;   // an unselected, unhovered tile's ring
    std::uint32_t lastSelectedRingColor = 0;  // the selected tile's ring
    std::uint32_t foldersCounted = 0;
    std::vector<AssetFolderTileCount> folderTileCounts;  // this frame, one per folder tile drawn
    std::uint32_t hatchedWells = 0;
    std::uint32_t lastHatchLabelColor = 0;
    std::uint32_t lastFolderTabColor = 0;
    std::string footerLeftText;
    std::vector<std::string> footerRightTexts;
    std::uint64_t folderCountRecomputations = 0;  // the cache's, copied every frame
};
// Opens one recorded frame: + 1 frame, the counts zeroed, the header rects hidden, the per-frame lists and texts
// cleared (capacity kept); AH17 pins each. The other colours, rects and layouts are overwritten by the draw that
// produces them.
void beginAssetChromeRecordFrame(AssetChromeRecord& record) noexcept;

}  // namespace engine::editor
