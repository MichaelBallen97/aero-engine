#pragma once
// Aero Engine — engine::editor::Selection (task 2.2.1, D2). The editor's shared entity selection:
// ordered by SELECTION order, not by entity index or tree position. The PRIMARY entity is the most
// recently ADDED one -- the single entity 2.2.2's inspector edits and 2.3.3's gizmo attaches to.
//
// Holds handles, never pointers, and never keeps a World reference: an entity destroyed behind this
// object's back simply becomes a stale handle, which prune() drops.
//
// CONSUMERS: 2.2.2's inspector reads primary() only; 2.2.3's viewport reads the whole set; 2.3.2's
// picking WRITES into this same object (click-to-select in the viewport and click-to-select in the
// hierarchy must remain ONE selection, never two); 2.4.2's commands capture and restore it.
//
// ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9).

#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

class Selection {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;
    [[nodiscard]] bool contains(Entity entity) const noexcept;  // false for Entity{}
    [[nodiscard]] Entity primary() const noexcept;              // Entity{} when empty
    [[nodiscard]] std::span<const Entity> entities() const noexcept;

    void set(Entity entity);                        // replace with exactly one; Entity{} clears
    void add(Entity entity);                        // no-op if present; otherwise appended and becomes primary
    void remove(Entity entity);                     // no-op if absent
    void toggle(Entity entity);                     // add or remove
    void setAll(std::span<const Entity> entities);  // replace wholesale (Shift+click ranges)
    void clear() noexcept;

    // Drops every dead or null handle. Returns how many were dropped. The primary follows: if it
    // was dropped, the new primary is the last surviving entry (or Entity{}).
    std::size_t prune(const World& world);

private:
    std::vector<Entity> items;
    Entity primaryEntity{};
};

// ---- the click-to-select decision (bugfix, task 2.2.1: multi-select drag) ---------------------
//
// The bug this closes: the Hierarchy panel used to replace the whole selection with a single row the
// instant its mouse-DOWN edge fired (ImGui::IsItemClicked() triggers on !Down -> Down, imgui.cpp), so
// pressing down on an already-selected row to BEGIN a multi-row drag collapsed the selection to that
// one row before BeginDragDropSource ever saw it -- the drag then only ever had one entity to move.
//
// The fix defers a plain click's REPLACE onto an already-selected row to mouse-release, so a drag that
// starts from that same press still reads the full, untouched selection. This function is the pure
// decision at the heart of that sequencing -- kept ImGui-free and out of the (src-private,
// ImGui-bound) HierarchyPanel precisely so a tier-0 test can drive the whole matrix with no window, no
// GPU and no ImGui context, exactly like review round 2 extracted walkForest and
// resolveCreateChildParent out of the same panel.

// PRESS is what a plain ImGui::IsItemClicked() edge means: the mouse went !Down -> Down this frame.
// RELEASE is the deferred half a plain click on an ALREADY-selected row schedules for later; the panel
// only ever asks for it once it has already confirmed the release landed back over the SAME row that
// was pressed (a screen-space fact this function has no notion of and does not need).
enum class ClickPhase : std::uint8_t { Press, Release };

// Deliberately distinct from HierarchyPanel::ActionKind (src-private, ImGui-bound) -- keeping this
// header ImGui-free is what makes it testable outside editor/src. The panel maps this 1:1 onto its own
// ActionKind at the one call site.
enum class ClickSelectionAction : std::uint8_t { None, Select, Toggle, Range };

// `arrowToggled` mirrors ImGui::IsItemToggledOpen(): expanding/collapsing a row via its arrow must
// never touch the selection, on EITHER phase, and takes precedence over everything below.
//
// PRESS -- Ctrl/Cmd and Shift resolve FULLY here and never enter the deferred state (they only ever
// grow/shrink/extend the selection, never destructively replace it, so there is nothing for a
// same-gesture drag to lose):
//   shift            -> Range
//   ctrlOrCmd        -> Toggle
//   !alreadySelected -> Select   (a plain click that REPLACES the selection is safe to apply
//                                 immediately -- there is no existing multi-selection it could collapse)
//   otherwise        -> None     (a plain click on an ALREADY-selected row: DEFER to RELEASE, so a
//                                 drag begun from this same press can still move the whole selection,
//                                 per E16 / AC-15)
//
// RELEASE -- only ever asked for that one deferred candidate (a plain click on an already-selected
// row); `ctrlOrCmd`/`shift` play no role here (those never defer) and `alreadySelected` still reflects
// the state AT PRESS TIME, since a deferred candidate is never itself mutated before its RELEASE is
// resolved:
//   dragOccurred     -> None     (a real drag happened this gesture -- never collapse the selection,
//                                 whether the drop landed elsewhere or back on the source row itself)
//   otherwise        -> Select   (a plain press+release with no drag: commit the replace now)
[[nodiscard]] ClickSelectionAction clickSelectionAction(bool alreadySelected, bool ctrlOrCmd, bool shift,
                                                        bool arrowToggled, bool dragOccurred,
                                                        ClickPhase phase) noexcept;

}  // namespace engine::editor
