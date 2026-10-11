#pragma once
// Aero Engine — the Hierarchy panel (task 2.2.1). SRC-PRIVATE and the only new ImGui TU: this header
// itself is ImGui-free (the class is registered by editor_app.cpp, which never sees ImGui), but its
// .cpp is where every ImGui call lives.
//
// FRAME SHAPE (D12) -- onDraw is exactly five phases, in this order:
//   1. reconcile  prune the selection, reconcile the root order, drop dead rename/anchor handles
//   1b. header    task E.6.3: the search field (its IsItemActive read HERE) and the count badge
//   2. shortcuts  Delete / Backspace / Ctrl-Cmd+D / F2, focus-routed, gated by hierarchyShortcutsLive, RECORDED only
//   3. walk       the tree, strictly READ-ONLY -- no World and no Selection mutation (I1)
//                 (inside the always-present ##tree child; the flat match list while a query is set)
//   4. void       the "click empty space to clear / drop here to unparent" target
//   5. apply      one switch over `pending` -- the ONLY place anything mutates
// Phases 1 and 5 bracket the ImGui work so that mutating mid-walk (which would invalidate
// eachChild's cursor -- world.hpp:157-163 -- and unbalance TreeNodeEx/TreePop) is impossible by
// construction. EVERY walk here is an explicit stack, never a recursive function: misc-no-recursion
// is --warnings-as-errors in CI (D13/I7).
#include <aero/editor/asset_drag.hpp>   // task 3.1.5 -- AssetDragPayload, HierarchyAssetDrop
#include <aero/editor/create_menu.hpp>  // task E.5.2 -- CreateKind, by value below; PUBLIC and PURE
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/hierarchy_view.hpp>  // task E.6.3 -- HierarchyDrawRecord; PUBLIC and PURE
#include <aero/editor/panel.hpp>
#include <aero/editor/tree_walk.hpp>
#include <aero/scene/entity.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>  // std::exchange -- the one-shot taker's own idiom
#include <vector>

namespace engine::editor {

class HierarchyPanel final : public Panel {
public:
    // D23: the id is the ImGui window name AND the imgui.ini settings key. It stays "Hierarchy" --
    // renaming it orphans every existing user's saved layout for this panel.
    [[nodiscard]] const char* id() const noexcept override { return "Hierarchy"; }
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Left; }
    void onDraw(PanelContext& context) override;

    // ---- task 3.1.5: the asset drop, a ONE-SHOT that tick() drains ---------------------------------
    // NOT a PendingAction and ActionKind is NOT extended -- a DEVIATION from AC-26, recorded: this
    // panel's applyPending cannot finish the job (instantiation needs the AssetDatabase, the importer
    // and the scene-asset ledger, none of which a panel has), so routing it through the five-phase
    // apply would create a second, half-capable path. Appending an enumerator that applyPending must
    // then refuse to handle is worse than not appending one.
    [[nodiscard]] std::optional<HierarchyAssetDrop> takeAssetDropRequest() noexcept {
        return std::exchange(pendingAssetDrop, std::nullopt);
    }
    // The SEAM: it writes the SAME member the accept writes, so a test and a real gesture are
    // indistinguishable downstream (the requestAssetBrowserSelectEntry shape).
    void requestAssetDrop(AssetDragPayload payload, Entity targetRow) noexcept {
        pendingAssetDrop = HierarchyAssetDrop{.payload = payload, .targetRow = targetRow};
    }

    // ---- task E.5.2: the create one-shot, drained by tick() -- takeAssetDropRequest's shape ----------
    // The panel RECORDS a kind; EditorApp places, pushes, frames and reveals (D1), because a create needs the
    // viewport camera this panel does not have. Both "Create Empty" items and the typed entries in BOTH
    // context menus write pendingCreate. std::exchange RESETS the member (a moved-from optional stays
    // engaged -- E.4.3's rule, which I249's "no further entity" arm is the only witness of).
    [[nodiscard]] std::optional<CreateKind> takeCreateRequest() noexcept {
        return std::exchange(pendingCreate, std::nullopt);
    }
    // The SEAM: it writes the SAME member the menu items write, so a test and a click are indistinguishable
    // downstream (requestAssetDrop's shape).
    void requestCreate(CreateKind kind) noexcept { pendingCreate = kind; }
    // Called by EditorApp's create drain with the entity it just created: the Shift-range anchor moves to it
    // (what applyPending's old CreateEmpty arm did), and the next drawTree opens its ancestors and scrolls
    // its row into view when it is not visible.
    void revealEntity(Entity entity) noexcept {
        rangeAnchor = entity;
        revealTarget = entity;
        searchText.clear();  // task E.6.3 (D6): a revealed row must be VISIBLE, so a create leaves the flat list
    }

    // ---- task E.6.3: the search field's seam and the draw record (D14) ----------------------------------------
    // The SEAM writes the query the field would hold; it is applied at the next draw, and only while the field is
    // enabled (no rename in progress) and no modal is up -- the field's own predicate (I337).
    void requestSearchQuery(std::string query) { pendingSearchQuery = std::move(query); }
    [[nodiscard]] const HierarchyDrawRecord& drawRecord() const noexcept { return drawRecordValue; }

private:
    // performance-enum-size: the explicit underlying type is mandatory, like every engine enum.
    enum class ActionKind : std::uint8_t {
        None = 0,
        Select,  // target: the clicked row
        Toggle,  // target: the ctrl/cmd-clicked row
        Range,   // target: the shift-clicked row (the anchor is a member)
        ClearSelection,
        CreateChild,  // target: the parent (the selection primary)
        Delete,
        Duplicate,
        BeginRename,   // target: the row to edit
        CommitRename,  // target: the row; renameBuffer holds the new text
        CancelRename,
        Reparent,  // target: the moved entity, second: the new parent (Entity{} == to root)
    };

    struct PendingAction {
        ActionKind kind = ActionKind::None;
        Entity target{};
        Entity second{};
    };

    void drawTree(PanelContext& context);
    [[nodiscard]] bool drawRow(PanelContext& context, Entity entity);
    void drawRowChrome(const World& world, Entity entity, bool selected, bool renaming, float rowCursorX);
    void drawVoidTarget(PanelContext& context);
    void applyPending(PanelContext& context);
    void drawHeaderRow();

    // Task 2.4.2, D10: the root order (RootOrder) moved to EditorApp (accessor context.roots) so a
    // structural command can restore a deleted root to the row it occupied. This panel still
    // reconciles it, in phase 1.
    std::vector<Entity> rows;        // this frame's visible rows, in DRAW order (the Shift-range domain)
    std::vector<Entity> childArena;  // LIFO scratch for expanded nodes' children (D14), owned by walkForest
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> moveScratch;  // the drag-drop moved set, when it is a single unselected row
    std::string labelScratch;
    std::string renameBuffer;
    Entity renaming{};
    Entity rangeAnchor{};
    // The bugfix (multi-select drag, task 2.2.1): a plain click's REPLACE on an ALREADY-selected row is
    // deferred here at mouse-DOWN rather than applied immediately, so a drag begun from that same press
    // still reads the untouched selection (engine::editor::clickSelectionAction, selection.hpp). Set by
    // drawRow's PRESS check; consumed (and always cleared) by drawRow's RELEASE check on a LATER frame,
    // or defensively dropped in phase 1 if that release is ever missed (E24-style: see onDraw).
    Entity deferredSelectTarget{};
    // Review round 2, Gap 2: the just-created/duplicated entity a collapsed ancestor must open to
    // reveal. `revealTarget` is set by applyPending (Create Child, Duplicate) or by revealEntity
    // (EditorApp's create drain) and consumed (turned into `revealPath`, the
    // ancestor chain, then cleared) by the VERY NEXT drawTree -- one frame after creation, since the
    // entity does not exist yet during the frame that creates it (drawTree runs before applyPending,
    // D12's phase order).
    Entity revealTarget{};
    std::vector<Entity> revealPath;
    // task E.5.2: the row to scroll into view on THIS walk -- copied from revealTarget at the top of drawTree
    // and cleared when that row is drawn, or unconditionally at the end of drawTree, so a target whose row is
    // never drawn cannot scroll on a later frame.
    Entity scrollTarget{};
    bool renameFocusPending = false;
    PendingAction pending{};
    std::optional<HierarchyAssetDrop> pendingAssetDrop;  // task 3.1.5 -- drained by tick(), never here
    std::optional<CreateKind> pendingCreate;             // task E.5.2 -- drained by tick(), never here
    // task E.6.3 (D6): the search and the header row.
    std::string searchText;                         // the field's buffer; non-empty = the flat match list
    std::optional<std::string> pendingSearchQuery;  // requestSearchQuery's one-shot, drained in onDraw
    bool searchEnabled = true;                      // !renaming, set in onDraw before the header draws
    bool searchActive = false;                      // the field's IsItemActive(), read in drawHeaderRow
    bool flatRow = false;                           // drawRow is drawing a flat-list row (Leaf | NoTreePushOnOpen)
    std::vector<Entity> matches;                    // this frame's flat list, in pre-order
    std::optional<std::size_t> lastMatchCount;      // the badge counts the PREVIOUS frame's matches (it draws first)
    std::string badgeScratch;
    std::string metaScratch;
    HierarchyDrawRecord drawRecordValue;  // drawRecord()'s value, named apart from it
};

}  // namespace engine::editor
