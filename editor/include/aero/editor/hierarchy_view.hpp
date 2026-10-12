#pragma once
// Aero Engine -- the Hierarchy's chrome as VALUES (task E.6.3, spec D6). PUBLIC and PURE: no ImGui, so tier 0 reaches
// the search, the badge, the row and header layouts and the shortcut gate that hierarchy_panel.cpp only draws.
#include <aero/core/math.hpp>
#include <aero/editor/panel_chrome_model.hpp>  // ChromeRole
#include <aero/editor/tree_walk.hpp>           // TreeWalkEntry
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// Pre-order over `roots` (RootOrder's order), children in World order -- walkForest's own walk with EVERY node open,
// so a match under a collapsed parent is found -- appending every entity whose entityLabel contains `query`
// (containsAsciiCaseInsensitive, the Console filter's function). An empty query appends nothing. Clears `out`.
void collectHierarchyMatches(const World& world, std::span<const Entity> roots, std::string_view query,
                             std::vector<Entity>& out, std::vector<TreeWalkEntry>& stack, std::vector<Entity>& arena,
                             std::string& labelScratch);
// "12", or "3 / 12" while a query filters -- each number grouped as groupThousands groups it. Clears `out`.
void hierarchyBadgeText(std::size_t total, std::optional<std::size_t> matches, std::string& out);

struct HierarchyRowLayout {
    Vec2 dotMin{};
    Vec2 dotMax{};
    float labelX = 0.0F;
    float labelMaxX = 0.0F;  // the label elides before this
    float metaX = 0.0F;      // the meta's left edge (right-aligned at rowMaxX - rowMetaInsetX)
    bool metaShown = false;
    friend constexpr bool operator==(const HierarchyRowLayout&, const HierarchyRowLayout&) = default;
};
// The dot at labelStartX, kindDotSize square, centred on rowMidY; the label kindDotGap after the dot; the meta
// right-aligned. A row too narrow for the meta drops it before the label would shrink below one body em.
[[nodiscard]] HierarchyRowLayout hierarchyRowLayout(float labelStartX, float rowMaxX, float rowMidY, float metaWidth,
                                                    bool hasMeta, float uiScale) noexcept;

struct HierarchyHeaderLayout {
    float searchWidth = 0.0F;
    float badgeX = 0.0F;
    friend constexpr bool operator==(const HierarchyHeaderLayout&, const HierarchyHeaderLayout&) = default;
};
// search + itemSpacing.x + badge == the row; the search floored at one frame height in a narrow row (the badge then
// follows it rather than overlapping it).
[[nodiscard]] HierarchyHeaderLayout hierarchyHeaderLayout(float rowMinX, float rowWidth, float badgeWidth,
                                                          float uiScale) noexcept;

// D6: Delete, Backspace, Cmd/Ctrl+D and F2 are live only when neither a rename nor the search field owns the keys --
// active, or holding the keyboard focus after Enter or Escape deactivated it (the code-review round's finding 20). An
// active InputText owns only the arrows, Delete, Backspace, Home and End (imgui_widgets.cpp:4923-4925).
[[nodiscard]] bool hierarchyShortcutsLive(bool renaming, bool searchOwnsKeys) noexcept;

// What the Hierarchy DREW this frame (D14) -- its FIRST draw seam.
struct HierarchyRowRole {
    Entity entity{};
    ChromeRole role = ChromeRole::TextFaint;  // the role the row's dot was handed
};
struct HierarchyRowMeta {
    Entity entity{};
    std::string text;  // the meta drawn: the DIRECT child count
};
struct HierarchyDrawRecord {
    std::uint64_t framesRecorded = 0;
    std::uint32_t rowsDrawn = 0;
    std::uint32_t dotsDrawn = 0;
    std::uint32_t metasDrawn = 0;
    std::vector<HierarchyRowRole> rowRoles;  // this frame, one per drawn row: look a role up BY HANDLE
    std::vector<HierarchyRowMeta> rowMetas;  // this frame, one per drawn meta
    std::string badgeText;
    bool searchActive = false;   // the field's IsItemActive(): it is being edited
    bool searchFocused = false;  // the field's IsItemFocused(): it holds NavId, whoever gave it
    bool shortcutsLive = false;  // hierarchyShortcutsLive's verdict this frame -- the gate the shortcuts ran behind
    bool flatMode = false;
    bool headerRuleDrawn = false;  // the rule's AddRectFilled added vertices to the list
    Vec2 searchFieldMin{};
    Vec2 searchFieldMax{};
    Vec2 treeChildMin{};
    Vec2 treeChildMax{};
    // The panel window's ClipRect bottom as the ##tree child begins: BeginChild clips the child's rect with it
    // (imgui.cpp:8434-8440), so a child whose top is at or below it is SKIPPED (:8733-8738). Written every frame.
    float panelClipMaxY = 0.0F;
};
// Opens one recorded frame: + 1 frame, the counts zeroed, the two lists cleared (capacity kept).
void beginHierarchyRecordFrame(HierarchyDrawRecord& record) noexcept;

}  // namespace engine::editor
