// Aero Engine -- the Assets panel's chrome as values (task E.6.3, spec D8). PURE: see asset_browser_chrome.hpp.
#include <aero/editor/asset_browser_chrome.hpp>
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_MIDDLE_DOT, AERO_GLYPH_ELLIPSIS
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/status_bar.hpp>  // groupThousands

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// The ##contents child's height never goes under ImGui's own. Begin clamps a child window WITHOUT
// ImGuiChildFlags_ResizeY -- ##contents has none -- to IMGUI_WINDOW_HARD_MIN_SIZE, 4 points at every scale
// (imgui_internal.h:2813; CalcWindowMinSize, imgui.cpp:7016-7017, applied at :8219), or to the rounding term
// max(0, style.WindowRounding - 1) if that is larger (:7027 -- the WINDOW rounding, even for a child). BeginChild
// always sets NoTitleBar (imgui.cpp:6744); a child given ImGuiWindowFlags_MenuBar adds its MenuBarHeight to the
// minimum too (:8134), and ##contents has no menu bar. So the 4 holds while the scaled WindowRounding is at most 5:
// the theme's is 0, and asset_browser_panel.cpp static_asserts both facts. A ResizeY child's minimum is
// style.WindowMinSize.y (32 dp) instead. A child asked for 1 is drawn 4 tall, so 1 here made the group 3 points
// taller than a pane at its floor (I231). It is also the 4 in assetBrowserLayout's floors, for that reason.
constexpr float CONTENTS_MIN_HEIGHT = 4.0F;
constexpr float FOOTER_SEGMENT_GAP_SPACINGS = 2.0F;  // footer segment to segment: two itemSpacing.x (16 dp)

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}
// A measured length that is not a finite, positive number counts as 0 (assetBrowserLayout's usableMetric rule).
[[nodiscard]] float usableLength(float value) noexcept { return (std::isfinite(value) && value > 0.0F) ? value : 0.0F; }
[[nodiscard]] float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }
// The crumb row: a frame-height row with panelHeaderPaddingY above and below it, 30 at 1x.
[[nodiscard]] float crumbRowHeightFor(float frameHeight, float uiScale) noexcept {
    return usableLength(frameHeight) + (2.0F * EDITOR_THEME.panels.panelHeaderPaddingY * layoutScale(uiScale));
}

[[nodiscard]] AssetHeaderWidths usableWidths(const AssetHeaderWidths& in) noexcept {
    AssetHeaderWidths out;
    out.newMaterial = usableLength(in.newMaterial);
    out.search = usableLength(in.search);
    out.searchMin = std::min(usableLength(in.searchMin), out.search);  // Minimal never asks for more than Full
    out.kind = usableLength(in.kind);
    out.viewToggle = usableLength(in.viewToggle);
    out.refresh = usableLength(in.refresh);
    out.autoRefresh = usableLength(in.autoRefresh);
    out.overflow = usableLength(in.overflow);
    return out;
}

[[nodiscard]] float headerGap(float uiScale) noexcept {
    return EDITOR_THEME.metrics.itemSpacing.x * layoutScale(uiScale);
}

[[nodiscard]] bool isUtf8Continuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

[[nodiscard]] std::size_t codePointCount(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char byte : text) {
        count += isUtf8Continuation(byte) ? 0U : 1U;
    }
    return count;
}

constexpr std::string_view FOOTER_ELLIPSIS = AERO_GLYPH_ELLIPSIS;  // U+2026, one Mono cell
constexpr std::string_view FOOTER_PART_SEPARATOR = "  -  ";        // before each of a selection's details

// The parts that are present, in AssetFooterDetails' order, each after FOOTER_PART_SEPARATOR: what the panel's one
// string carried before the details had fields of their own.
[[nodiscard]] std::string composeFooterDetails(std::string_view size, std::string_view identity,
                                               std::string_view state) {
    std::string out;
    for (const std::string_view part : {size, identity, state}) {
        if (!part.empty()) {
            out += FOOTER_PART_SEPARATOR;
            out += part;
        }
    }
    return out;
}

// The ellipsis, the longest tail of `name` that starts at or before byte `lastStart` and fits with `after` beside it,
// then `after`; nullopt when none fits. Every start is a UTF-8 boundary -- the name's first byte included -- and a
// later start is never harder to fit, so the first that fits is found by bisection over the boundaries.
[[nodiscard]] std::optional<std::string> longestFooterTail(std::string_view name, std::size_t lastStart,
                                                           std::string_view after, const CaptionLineFits& fits) {
    std::vector<std::size_t> starts;
    for (std::size_t at = 0; at <= lastStart && at <= name.size(); ++at) {
        if (at == name.size() || !isUtf8Continuation(name[at])) {
            starts.push_back(at);
        }
    }
    std::string candidate;
    const auto build = [&](std::size_t start) {
        candidate.assign(FOOTER_ELLIPSIS);
        candidate += name.substr(start);
        candidate += after;
    };
    std::size_t lo = 0;
    std::size_t hi = starts.size();  // starts.size() == no start fits
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) / 2);
        build(starts[mid]);
        if (fits(candidate)) {
            hi = mid;
        } else {
            lo = mid + 1U;
        }
    }
    if (lo == starts.size()) {
        return std::nullopt;
    }
    build(starts[lo]);
    return candidate;
}

}  // namespace

float assetHeaderMinimalFloor(const AssetHeaderWidths& widths, float uiScale) noexcept {
    const AssetHeaderWidths w = usableWidths(widths);
    return w.newMaterial + w.searchMin + w.viewToggle + w.overflow + (3.0F * headerGap(uiScale));
}

AssetHeaderLayout assetHeaderLayout(float rowMinX, float rowWidth, const AssetHeaderWidths& widths,
                                    float uiScale) noexcept {
    const AssetHeaderWidths w = usableWidths(widths);
    const float gap = headerGap(uiScale);
    const float minX = finiteOr(rowMinX, 0.0F);
    const float width = usableLength(rowWidth);
    const float compactNeed = w.newMaterial + w.search + w.kind + w.viewToggle + w.overflow + (4.0F * gap);
    const float fullNeed = compactNeed + w.refresh + w.autoRefresh + (2.0F * gap);
    AssetHeaderLayout out;
    out.newMaterialX = minX;
    if (width >= fullNeed) {
        out.mode = AssetHeaderMode::Full;
    } else if (width >= compactNeed) {
        out.mode = AssetHeaderMode::Compact;
    } else {
        out.mode = AssetHeaderMode::Minimal;
    }
    out.kindInline = out.mode != AssetHeaderMode::Minimal;
    out.refreshInline = out.mode == AssetHeaderMode::Full;
    out.autoRefreshInline = out.mode == AssetHeaderMode::Full;
    if (out.mode == AssetHeaderMode::Minimal && width < assetHeaderMinimalFloor(widths, uiScale)) {
        // BELOW THE FLOOR: left-anchored at the controls' own widths -- the order kept, only the right end crossing.
        out.searchWidth = w.searchMin;
        out.searchX = minX + w.newMaterial + gap;
        out.viewToggleX = out.searchX + out.searchWidth + gap;
        out.overflowX = out.viewToggleX + w.viewToggle + gap;
    } else {
        out.overflowX = minX + width - w.overflow;
        float x = out.overflowX;
        if (out.autoRefreshInline) {
            out.autoRefreshX = x - gap - w.autoRefresh;
            x = out.autoRefreshX;
        }
        if (out.refreshInline) {
            out.refreshX = x - gap - w.refresh;
            x = out.refreshX;
        }
        out.viewToggleX = x - gap - w.viewToggle;
        x = out.viewToggleX;
        if (out.kindInline) {
            out.kindX = x - gap - w.kind;
            x = out.kindX;
            out.searchWidth = w.search;
        } else {
            // Minimal at or above the floor: the search takes the room left, between searchMin and its own width.
            const float room = x - gap - (minX + w.newMaterial + gap);
            out.searchWidth = std::max(w.searchMin, std::min(w.search, room));
        }
        out.searchX = x - gap - out.searchWidth;
    }
    // A folded control's x is the overflow's: finite, never drawn.
    if (!out.kindInline) {
        out.kindX = out.overflowX;
    }
    if (!out.refreshInline) {
        out.refreshX = out.overflowX;
    }
    if (!out.autoRefreshInline) {
        out.autoRefreshX = out.overflowX;
    }
    return out;
}

AssetContentsPaneLayout assetContentsPaneLayout(float paneHeight, float frameHeight, float dividerHeight,
                                                float itemSpacingY, float uiScale, bool crumbRowShown) noexcept {
    AssetContentsPaneLayout out;
    if (!crumbRowShown) {
        // FOLDED (assetBrowserLayout's crumbRowShown): no row, rule or spacing above the child, so it is the pane.
        out.crumbRowHeight = 0.0F;
        out.contentsHeight = std::max(usableLength(paneHeight), CONTENTS_MIN_HEIGHT);
        return out;
    }
    out.crumbRowHeight = crumbRowHeightFor(frameHeight, uiScale);
    // ONE chrome for the group and the budget: the child is the pane less what the panel records as paneChromeHeight.
    const float chrome = assetContentsPaneChromeHeight(frameHeight, dividerHeight, itemSpacingY, uiScale);
    out.contentsHeight = std::max(usableLength(paneHeight) - chrome, CONTENTS_MIN_HEIGHT);
    return out;
}

float assetContentsPaneChromeHeight(float frameHeight, float dividerHeight, float itemSpacingY,
                                    float uiScale) noexcept {
    return crumbRowHeightFor(frameHeight, uiScale) + usableLength(dividerHeight) + usableLength(itemSpacingY);
}

void assetBreadcrumbCrumbs(std::string_view rootLabel, std::string_view currentDir, std::vector<AssetCrumb>& out) {
    out.clear();
    out.push_back(AssetCrumb{std::string(rootLabel), std::string{}});
    std::size_t start = 0;
    while (start < currentDir.size()) {
        std::size_t slash = currentDir.find('/', start);
        if (slash == std::string_view::npos) {
            slash = currentDir.size();
        }
        if (slash > start) {
            out.push_back(AssetCrumb{std::string(currentDir.substr(start, slash - start)),
                                     std::string(currentDir.substr(0, slash))});
        }
        start = slash + 1U;
    }
}

void assetItemsText(std::size_t items, std::string& out) {
    out = groupThousands(items);
    out += items == 1U ? " item " : " items ";
    out += AERO_GLYPH_MIDDLE_DOT " drag into the viewport to place";
}

AssetCrumbRowLayout assetCrumbRowLayout(float rowMinX, float rowWidth, float inkWidth, std::size_t gapCount,
                                        float itemsWidth, float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const float minX = finiteOr(rowMinX, 0.0F);
    const float maxX = minX + usableLength(rowWidth);
    const float inset = EDITOR_THEME.metrics.framePadding.x * s;
    AssetCrumbRowLayout out;
    out.firstCrumbX = minX + inset;
    out.crumbGap = EDITOR_THEME.metrics.itemInnerSpacing.x * s;
    out.labelOffsetY = EDITOR_THEME.panels.panelHeaderPaddingY * s;
    out.crumbsEndX = out.firstCrumbX + usableLength(inkWidth) + (static_cast<float>(gapCount) * out.crumbGap);
    const float rightAligned = maxX - inset - usableLength(itemsWidth);
    out.itemsX = std::max(rightAligned, out.crumbsEndX + (EDITOR_THEME.metrics.itemSpacing.x * s));
    out.itemsClipMaxX = maxX;
    return out;
}

AssetTreeRowLayout assetTreeRowLayout(float labelX, float rowMaxX, float countWidth, float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const float x = finiteOr(labelX, 0.0F);
    const float maxX = std::max(finiteOr(rowMaxX, x), x);
    const float count = usableLength(countWidth);
    const float inset = EDITOR_THEME.panels.rowMetaInsetX * s;
    const float gap = EDITOR_THEME.metrics.itemSpacing.x * s;
    AssetTreeRowLayout out;
    out.countX = maxX - inset - count;
    out.countShown = count > 0.0F && (out.countX - gap - x) >= (EDITOR_THEME.type.bodySize * s);
    out.labelMaxX = out.countShown ? out.countX - gap : maxX - inset;
    return out;
}

FolderGlyph folderGlyphRects(Vec2 wellMin, Vec2 wellMax) noexcept {
    const float width = wellMax.x - wellMin.x;
    const float height = wellMax.y - wellMin.y;
    const bool finiteWell = std::isfinite(wellMin.x) && std::isfinite(wellMin.y) && std::isfinite(width + height);
    if (!finiteWell || !(width > 0.0F) || !(height > 0.0F)) {
        const Vec2 at = (std::isfinite(wellMin.x) && std::isfinite(wellMin.y)) ? wellMin : Vec2{};
        return FolderGlyph{at, at, at, at};
    }
    const float bodyWidth = width * FOLDER_BODY_WIDTH_FRACTION;
    const float bodyHeight = height * FOLDER_BODY_HEIGHT_FRACTION;
    const float tabWidth = width * FOLDER_TAB_WIDTH_FRACTION;
    const float tabHeight = height * FOLDER_TAB_HEIGHT_FRACTION;
    const float left = wellMin.x + ((width - bodyWidth) * 0.5F);
    const float top = wellMin.y + ((height - (tabHeight + bodyHeight)) * 0.5F);
    return FolderGlyph{Vec2{left, top}, Vec2{left + tabWidth, top + tabHeight}, Vec2{left, top + tabHeight},
                       Vec2{left + bodyWidth, top + tabHeight + bodyHeight}};
}

std::size_t FolderCountCache::countUnder(std::span<const AssetRecord> records, std::uint64_t generation,
                                         std::string_view folderRelative) {
    if (!cachedGeneration.has_value() || *cachedGeneration != generation) {
        // ONE pass for every folder: each record adds one to every ancestor that ends before one of its '/'s -- the
        // segment-wise rule by construction ("tex" never counts "textures/a.png", which has no '/' after "tex").
        counts.clear();
        total = records.size();
        for (const AssetRecord& record : records) {
            const std::string_view path(record.relativePath);
            for (std::size_t slash = path.find('/'); slash != std::string_view::npos;
                 slash = path.find('/', slash + 1U)) {
                const std::string_view folder = path.substr(0, slash);
                if (const auto found = counts.find(folder); found != counts.end()) {
                    ++found->second;
                } else {
                    counts.emplace(std::string(folder), 1U);
                }
            }
        }
        cachedGeneration = generation;
        ++recomputationCount;
    }
    if (folderRelative.empty()) {
        return total;  // the assets root holds everything (countRecordsUnder's own first arm)
    }
    const auto found = counts.find(folderRelative);
    return found != counts.end() ? found->second : 0U;
}

AssetFooterLayout assetFooterLayout(float rowMinX, float rowWidth, const AssetFooterText& text, float monoSmallAdvance,
                                    float uiScale) {
    const float minX = finiteOr(rowMinX, 0.0F);
    const float rowLength = usableLength(rowWidth);
    const float maxX = minX + rowLength;
    const float advance = usableLength(monoSmallAdvance);
    const float gap = FOOTER_SEGMENT_GAP_SPACINGS * EDITOR_THEME.metrics.itemSpacing.x * layoutScale(uiScale);
    const auto widthOf = [advance](std::string_view t) { return static_cast<float>(codePointCount(t)) * advance; };
    AssetFooterLayout out;
    out.leftX = minX;
    if (!text.notice.empty()) {
        out.right.push_back(AssetFooterSegment{text.notice, ChromeRole::Warning, 0.0F});
    }
    if (!text.matches.empty()) {
        out.right.push_back(AssetFooterSegment{text.matches, ChromeRole::TextFaint, 0.0F});
    }
    if (text.issues > 0U) {
        std::string issues = groupThousands(text.issues);
        issues += text.issues == 1U ? " issue" : " issues";
        out.right.push_back(AssetFooterSegment{std::move(issues), ChromeRole::Warning, 0.0F});
    }
    const bool hasThumbnails = text.thumbnailsBudget > 0U;
    if (hasThumbnails) {
        std::string thumbnails = "thumbnails " + std::to_string(text.thumbnailsReady);
        thumbnails += "/" + std::to_string(text.thumbnailsBudget);
        out.right.push_back(AssetFooterSegment{std::move(thumbnails), ChromeRole::TextFaint, 0.0F});
    }
    const bool hasWatcher = !text.watcher.empty();
    if (hasWatcher) {
        out.right.push_back(AssetFooterSegment{text.watcher, ChromeRole::TextFaint, 0.0F});
    }
    const auto zoneWidth = [&out, &widthOf, gap] {
        float total = 0.0F;
        for (std::size_t i = 0; i < out.right.size(); ++i) {
            total += widthOf(out.right[i].text) + (i > 0U ? gap : 0.0F);
        }
        return total;
    };
    // task E.6.3's code-review round: a zone wider than the row sheds the thumbnails first, then the watcher word (the
    // status bar shows the same classifyWatchState), and never the notice, the matches or the issues. The thumbnails
    // were pushed just before the watcher, which is last when present. Its round 2: with a selection both checks also
    // count one gap and the selection's cells, up to ASSET_FOOTER_SELECTION_MIN_CELLS, for it, so neither segment is
    // kept unless the row also holds that much for the selection -- reading the zone alone dropped the selection in a
    // 749-point row (a window about 1366 wide) while "thumbnails R/B" stayed. Never more than the selection's own
    // cells, so a shorter one costs no segment the row holds beside it whole. No selection, no reserve. The macOS
    // pass's V2 split the selection's path from its details; its cells are still both together, the details composed
    // with their separators, as they were when one string carried them.
    const AssetFooterDetails& details = text.selectionDetails;
    const std::string allDetails = composeFooterDetails(details.size, details.identity, details.state);
    const std::size_t selectionCells = codePointCount(text.selection) + codePointCount(allDetails);
    const std::size_t reserved = std::min(selectionCells, ASSET_FOOTER_SELECTION_MIN_CELLS);
    const float selectionReserve = text.selection.empty() ? 0.0F : gap + (static_cast<float>(reserved) * advance);
    float total = zoneWidth();
    if (hasThumbnails && total + selectionReserve > rowLength) {
        out.right.erase(out.right.end() - (hasWatcher ? 2 : 1));
        total = zoneWidth();
    }
    if (hasWatcher && total + selectionReserve > rowLength) {
        out.right.pop_back();
        total = zoneWidth();
    }
    // Right-aligned when it fits; else anchored AT rowMinX, so the notice leads and the window clips the tail past the
    // row's right end, as the footer's one left-aligned line before task E.6.3 did. Never left of rowMinX: the window
    // would clip the notice there first, and a cap is surfaced, never silent (AC-8).
    float x = std::max(maxX - total, minX);
    for (AssetFooterSegment& segment : out.right) {
        segment.x = x;
        x += widthOf(segment.text) + gap;
    }
    const float leftMaxX = out.right.empty() ? maxX : out.right.front().x - gap;
    const float budget = leftMaxX - minX;
    // A budget below the ellipsis (one Mono cell) draws nothing: elideFooterSelection's own (4), gated here as well.
    if (!text.selection.empty() && advance > 0.0F && budget >= advance) {
        const auto fits = [&widthOf, budget](std::string_view t) { return widthOf(t) <= budget; };
        out.left = elideFooterSelection(text.selection, text.selectionDetails, fits);
    } else if (!text.selection.empty() && advance == 0.0F) {
        out.left = text.selection + allDetails;  // nothing measurable: every width is 0, all fits
    }
    return out;
}

std::string elideFooterSelection(std::string_view path, const AssetFooterDetails& details,
                                 const CaptionLineFits& fits) {
    if (path.empty()) {
        return {};  // no selection
    }
    const std::string all = composeFooterDetails(details.size, details.identity, details.state);
    std::string candidate(path);
    candidate += all;
    if (fits(candidate)) {
        return candidate;  // (1) whole
    }
    if (!fits(FOOTER_ELLIPSIS)) {
        return {};  // (7) an ellipsis that does not fit is never drawn
    }
    // The leaf: what follows the path's last '/', or the whole path when nothing does -- no '/', or a trailing one.
    const std::size_t lastSlash = path.rfind('/');
    const bool hasFolders = lastSlash != std::string_view::npos && lastSlash + 1U < path.size();
    const std::size_t leafStart = hasFolders ? lastSlash + 1U : 0U;
    const std::string_view leaf = path.substr(leafStart);
    // (2) The folders, WHOLE segments, longest first: the ellipsis and '/' stand for the folders dropped, and the tail
    // after each '/' up to the leaf's own is tried with every detail beside it. A longer tail is never easier to fit,
    // so the first that fits is the longest; the paths are short, so a walk rather than a bisection.
    if (hasFolders) {
        for (std::size_t slash = path.find('/'); slash <= lastSlash; slash = path.find('/', slash + 1U)) {
            candidate.assign(FOOTER_ELLIPSIS);
            candidate += '/';
            candidate += path.substr(slash + 1U);
            candidate += all;
            if (fits(candidate)) {
                return candidate;
            }
        }
    }
    // From here the folders stay gone: the leaf follows the ellipsis and '/', or stands bare when there were none, as
    // nothing on its left was cut. (3) the identity, then (4) the size; an absent part is no arm.
    std::string leafAlone(hasFolders ? FOOTER_ELLIPSIS : std::string_view());
    if (hasFolders) {
        leafAlone += '/';
    }
    leafAlone += leaf;
    if (!details.identity.empty()) {
        candidate = leafAlone + composeFooterDetails(details.size, {}, details.state);
        if (fits(candidate)) {
            return candidate;
        }
    }
    const std::string stateOnly = composeFooterDetails({}, {}, details.state);
    if (!details.size.empty()) {
        candidate = leafAlone + stateOnly;
        if (fits(candidate)) {
            return candidate;
        }
    }
    // (5) The name's front, beside the state, cut no further than its extension: the leaf from its last '.' after its
    // first byte, or all of the leaf when it has none -- so the import state never stands beside no name at all.
    // (6) Then the state goes, and the name alone takes the zone: the leaf as above, else the ellipsis and its longest
    // tail -- the empty one included, the ellipsis alone, which (7) has measured.
    if (!details.state.empty()) {
        const std::size_t dot = leaf.rfind('.');
        const std::size_t extensionStart = dot != std::string_view::npos && dot > 0U ? dot : 0U;
        if (std::optional<std::string> kept = longestFooterTail(leaf, extensionStart, stateOnly, fits)) {
            return *std::move(kept);
        }
    }
    if (fits(leafAlone)) {
        return leafAlone;
    }
    return longestFooterTail(leaf, leaf.size(), {}, fits).value_or(std::string(FOOTER_ELLIPSIS));
}

void beginAssetChromeRecordFrame(AssetChromeRecord& record) noexcept {
    ++record.framesRecorded;
    record.headerRects = {};
    record.overflowDrawn.clear();
    record.headerRuleDrawn = false;
    record.crumbsDrawn = 0;
    record.folderDropTargetsAttached = 0;
    record.crumbDropTargets = 0;
    record.folderDropTargetItemIds.clear();
    record.crumbDropTargetItemIds.clear();
    record.crumbItemIds.clear();
    record.itemsText.clear();
    record.treeRowsDrawn = 0;
    record.tilesRinged = 0;
    record.foldersCounted = 0;
    record.folderTileCounts.clear();
    record.hatchedWells = 0;
    record.lastHatchedWellVertices.clear();
    record.lastHatchedWellStripesBegin = 0;
    record.lastHatchedWellIndices.clear();
    record.footerLeftText.clear();
    record.footerRightTexts.clear();
}

}  // namespace engine::editor
