#pragma once
// Aero Engine — the Hierarchy panel (task 2.2.1). SRC-PRIVATE and the only new ImGui TU: this header
// itself is ImGui-free (the class is registered by editor_app.cpp, which never sees ImGui), but its
// .cpp is where every ImGui call lives.
//
// FRAME SHAPE (D12) -- onDraw is exactly five phases, in this order:
//   1. reconcile  prune the selection, reconcile the root order, drop dead rename/anchor handles
//   2. shortcuts  Delete / Backspace / Ctrl-Cmd+D / F2, focus-routed, RECORDED only
//   3. walk       the tree, strictly READ-ONLY -- no World and no Selection mutation (I1)
//   4. void       the "click empty space to clear / drop here to unparent" target
//   5. apply      one switch over `pending` -- the ONLY place anything mutates
// Phases 1 and 5 bracket the ImGui work so that mutating mid-walk (which would invalidate
// eachChild's cursor -- world.hpp:157-163 -- and unbalance TreeNodeEx/TreePop) is impossible by
// construction. EVERY walk here is an explicit stack, never a recursive function: misc-no-recursion
// is --warnings-as-errors in CI (D13/I7).
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel.hpp>
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

class HierarchyPanel final : public Panel {
public:
    // D23: the id is the ImGui window name AND the imgui.ini settings key. It stays "Hierarchy" --
    // renaming it orphans every existing user's saved layout for this panel.
    [[nodiscard]] const char* id() const noexcept override { return "Hierarchy"; }
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Left; }
    void onDraw(PanelContext& context) override;

private:
    // performance-enum-size: the explicit underlying type is mandatory, like every engine enum.
    enum class ActionKind : std::uint8_t {
        None = 0,
        Select,        // target: the clicked row
        Toggle,        // target: the ctrl/cmd-clicked row
        Range,         // target: the shift-clicked row (the anchor is a member)
        ClearSelection,
        CreateEmpty,
        CreateChild,   // target: the parent (the selection primary)
        Delete,
        Duplicate,
        BeginRename,   // target: the row to edit
        CommitRename,  // target: the row; renameBuffer holds the new text
        CancelRename,
        Reparent,      // target: the moved entity, second: the new parent (Entity{} == to root)
    };

    struct PendingAction {
        ActionKind kind = ActionKind::None;
        Entity target{};
        Entity second{};
    };

    // One node of the explicit draw stack. `childBegin/childNext/childEnd` index into childArena.
    struct StackEntry {
        Entity entity{};
        std::size_t childBegin = 0;
        std::size_t childNext = 0;
        std::size_t childEnd = 0;
        bool open = false;     // TreeNodeEx returned true -> we owe exactly one TreePop (I3)
        bool entered = false;  // the row has been drawn and its two IDs pushed
    };

    void drawTree(PanelContext& context);
    [[nodiscard]] bool drawRow(PanelContext& context, Entity entity);
    void drawVoidTarget(PanelContext& context);
    void applyPending(PanelContext& context);

    RootOrder roots;
    std::vector<Entity> rows;        // this frame's visible rows, in DRAW order (the Shift-range domain)
    std::vector<Entity> childArena;  // LIFO scratch for expanded nodes' children (D14)
    std::vector<StackEntry> stack;
    std::vector<Entity> moveScratch;  // the drag-drop moved set, when it is a single unselected row
    std::string labelScratch;
    std::string renameBuffer;
    Entity renaming{};
    Entity rangeAnchor{};
    bool renameFocusPending = false;
    PendingAction pending{};
};

}  // namespace engine::editor
