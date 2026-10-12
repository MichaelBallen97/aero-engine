// Aero Engine -- the Hierarchy's chrome as values (task E.6.3). PURE: see hierarchy_view.hpp.
#include <aero/editor/console_model.hpp>  // containsAsciiCaseInsensitive -- the Console filter's function
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/entity_ops.hpp>  // entityLabel
#include <aero/editor/hierarchy_view.hpp>
#include <aero/editor/status_bar.hpp>  // groupThousands

#include <algorithm>
#include <cmath>

namespace engine::editor {

namespace {

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

[[nodiscard]] float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

}  // namespace

void collectHierarchyMatches(const World& world, std::span<const Entity> roots, std::string_view query,
                             std::vector<Entity>& out, std::vector<TreeWalkEntry>& stack, std::vector<Entity>& arena,
                             std::string& labelScratch) {
    out.clear();
    if (query.empty()) {
        return;  // the flat mode is off
    }
    const auto enter = [&](Entity entity) {
        entityLabel(world, entity, labelScratch);
        if (containsAsciiCaseInsensitive(labelScratch, query)) {
            out.push_back(entity);
        }
        return true;  // EVERY node open: the walk is total, whatever the tree shows
    };
    const auto unwind = [](Entity /*entity*/, bool /*open*/) {};
    walkForest(world, roots, stack, arena, enter, unwind);
}

void hierarchyBadgeText(std::size_t total, std::optional<std::size_t> matches, std::string& out) {
    out.clear();
    if (matches.has_value()) {
        out += groupThousands(*matches);
        out += " / ";
    }
    out += groupThousands(total);
}

HierarchyRowLayout hierarchyRowLayout(float labelStartX, float rowMaxX, float rowMidY, float metaWidth, bool hasMeta,
                                      float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const ThemePanels& p = EDITOR_THEME.panels;
    const float dot = p.kindDotSize * s;
    const float gap = p.kindDotGap * s;
    const float startX = finiteOr(labelStartX, 0.0F);
    const float maxX = finiteOr(rowMaxX, startX);
    const float midY = finiteOr(rowMidY, 0.0F);
    HierarchyRowLayout out{};
    out.dotMin = Vec2{startX, midY - (dot * 0.5F)};
    out.dotMax = Vec2{startX + dot, midY + (dot * 0.5F)};
    out.labelX = startX + dot + gap;
    out.metaX = maxX - (p.rowMetaInsetX * s) - std::max(finiteOr(metaWidth, 0.0F), 0.0F);
    const float minimumLabel = EDITOR_THEME.type.bodySize * s;  // one body em
    out.metaShown = hasMeta && (out.metaX - gap >= out.labelX + minimumLabel);
    out.labelMaxX = out.metaShown ? out.metaX - gap : maxX;
    return out;
}

HierarchyHeaderLayout hierarchyHeaderLayout(float rowMinX, float rowWidth, float badgeWidth, float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const ThemeMetrics& m = EDITOR_THEME.metrics;
    const float gap = m.itemSpacing.x * s;
    const float frame = (EDITOR_THEME.type.bodySize + (2.0F * m.framePadding.y)) * s;
    const float width = std::max(finiteOr(rowWidth, 0.0F), 0.0F);
    const float badge = std::max(finiteOr(badgeWidth, 0.0F), 0.0F);
    HierarchyHeaderLayout out{};
    out.searchWidth = std::max(frame, width - badge - gap);
    out.badgeX = finiteOr(rowMinX, 0.0F) + out.searchWidth + gap;
    return out;
}

bool hierarchyShortcutsLive(bool renaming, bool searchOwnsKeys) noexcept { return !renaming && !searchOwnsKeys; }

void beginHierarchyRecordFrame(HierarchyDrawRecord& record) noexcept {
    ++record.framesRecorded;
    record.rowsDrawn = 0;
    record.dotsDrawn = 0;
    record.metasDrawn = 0;
    record.rowRoles.clear();
    record.rowMetas.clear();
    record.searchActive = false;
    record.searchFocused = false;
    record.shortcutsLive = false;
    record.flatMode = false;
    record.headerRuleDrawn = false;
}

}  // namespace engine::editor
