// Aero Engine -- the Assets panel's chrome as values (task E.6.3, spec D8). PURE: see asset_browser_chrome.hpp.
#include <aero/editor/asset_browser_chrome.hpp>
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_MIDDLE_DOT
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/status_bar.hpp>  // elidePathLeft, groupThousands

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::editor {

namespace {

constexpr float CONTENTS_MIN_HEIGHT = 1.0F;          // assetBrowserLayout's paneHeight floor, for the same reason
constexpr float FOOTER_SEGMENT_GAP_SPACINGS = 2.0F;  // footer segment to segment: two itemSpacing.x (16 dp)

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}
// A measured length that is not a finite, positive number counts as 0 (assetBrowserLayout's usableMetric rule).
[[nodiscard]] float usableLength(float value) noexcept { return (std::isfinite(value) && value > 0.0F) ? value : 0.0F; }
[[nodiscard]] float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

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

[[nodiscard]] std::size_t codePointCount(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char byte : text) {
        count += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U ? 1U : 0U;
    }
    return count;
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
                                                float itemSpacingY, float uiScale) noexcept {
    AssetContentsPaneLayout out;
    out.crumbRowHeight =
        usableLength(frameHeight) + (2.0F * EDITOR_THEME.panels.panelHeaderPaddingY * layoutScale(uiScale));
    const float rest =
        usableLength(paneHeight) - out.crumbRowHeight - usableLength(dividerHeight) - usableLength(itemSpacingY);
    out.contentsHeight = std::max(rest, CONTENTS_MIN_HEIGHT);
    return out;
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
    const float maxX = minX + usableLength(rowWidth);
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
    if (text.thumbnailsBudget > 0U) {
        std::string thumbnails = "thumbnails " + std::to_string(text.thumbnailsReady);
        thumbnails += "/" + std::to_string(text.thumbnailsBudget);
        out.right.push_back(AssetFooterSegment{std::move(thumbnails), ChromeRole::TextFaint, 0.0F});
    }
    if (!text.watcher.empty()) {
        out.right.push_back(AssetFooterSegment{text.watcher, ChromeRole::TextFaint, 0.0F});
    }
    float total = 0.0F;
    for (std::size_t i = 0; i < out.right.size(); ++i) {
        total += widthOf(out.right[i].text) + (i > 0U ? gap : 0.0F);
    }
    float x = maxX - total;  // ALWAYS whole: a row too narrow for it lets it cross rowMinX, never cut it
    for (AssetFooterSegment& segment : out.right) {
        segment.x = x;
        x += widthOf(segment.text) + gap;
    }
    const float leftMaxX = out.right.empty() ? maxX : out.right.front().x - gap;
    const float budget = leftMaxX - minX;
    // elidePathLeft's own rule: a budget below the ellipsis (one Mono cell) returns an ellipsis that does not fit.
    if (!text.selection.empty() && advance > 0.0F && budget >= advance) {
        const auto fits = [&widthOf, budget](std::string_view t) { return widthOf(t) <= budget; };
        out.left = elidePathLeft(text.selection, fits);
    } else if (!text.selection.empty() && advance == 0.0F) {
        out.left = text.selection;  // nothing measurable: every width is 0 and everything fits
    }
    return out;
}

void beginAssetChromeRecordFrame(AssetChromeRecord& record) noexcept {
    ++record.framesRecorded;
    record.headerRects = {};
    record.overflowDrawn.clear();
    record.headerRuleDrawn = false;
    record.crumbsDrawn = 0;
    record.crumbDropTargets = 0;
    record.itemsText.clear();
    record.treeRowsDrawn = 0;
    record.tilesRinged = 0;
    record.foldersCounted = 0;
    record.folderTileCounts.clear();
    record.hatchedWells = 0;
    record.footerLeftText.clear();
    record.footerRightTexts.clear();
}

}  // namespace engine::editor
