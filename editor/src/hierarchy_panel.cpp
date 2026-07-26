#include "hierarchy_panel.hpp"

#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <cfloat>  // FLT_MIN -- the "align to the right edge" idiom
#include <cstddef>
#include <cstring>  // std::memcpy -- the payload read (C6: the ImGui payload is alignas(1))
#include <imgui.h>
#include <imgui_stdlib.h>  // ImGui::InputText(const char*, std::string*, ...) -- flat, NOT misc/cpp/
#include <span>
#include <string>
#include <vector>

namespace engine::editor {

namespace {

constexpr const char* ENTITY_PAYLOAD_TYPE = "AERO_ENTITY";  // <= 32 chars (ImGui's own limit)

// The rows a drop would move: the whole selection when the dragged row is IN it, otherwise just that
// row (E16). Dragging an unselected row moves only it -- the least surprising behaviour, and the only
// one that stays correct given the selection cannot be updated mid-walk (D12).
[[nodiscard]] std::span<const Entity> movedSet(const Selection& selection, Entity dragged,
                                               std::vector<Entity>& scratch) {
    if (selection.contains(dragged)) {
        return selection.entities();
    }
    scratch.clear();
    scratch.push_back(dragged);
    return std::span<const Entity>{scratch};
}

// Every moved row must be legally reparentable, or the drop is not offered at all (E14/AC-15).
[[nodiscard]] bool dropLegal(const World& world, std::span<const Entity> moved, Entity target) {
    if (moved.empty()) {
        return false;
    }
    for (const Entity e : moved) {
        if (!canReparent(world, e, target)) {
            return false;
        }
    }
    return true;
}

// Reads the drag payload WITHOUT registering an accept -- which is what keeps ImGui from drawing the
// highlight rect for a drop we are about to refuse (C7). std::memcpy, never a cast: ImGui stores a
// <=16-byte payload in an `unsigned char[16]` member, so a cast is a potentially-misaligned load and
// the Debug lanes run UBSan (C6).
[[nodiscard]] bool peekDraggedEntity(Entity& out) {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(ENTITY_PAYLOAD_TYPE) ||
        payload->DataSize != static_cast<int>(sizeof(Entity))) {
        return false;
    }
    std::memcpy(&out, payload->Data, sizeof(Entity));
    return true;
}

}  // namespace

void HierarchyPanel::onDraw(PanelContext& context) {
    const World& world = context.world;

    // -- phase 1: reconcile (FIRST, so the panel is correct when something ELSE changed the World --
    //    2.5.1's load, 2.4.2's undo, a future script). prune() only knows about Selection, which is
    //    why the two handle checks below are separate: they close E19 and E24.
    context.selection.prune(world);
    roots.reconcile(world);
    if (renaming.valid() && !world.alive(renaming)) {
        renaming = {};  // E24: the renamed entity was deleted mid-rename
        renameFocusPending = false;
    }
    if (rangeAnchor.valid() && !world.alive(rangeAnchor)) {
        rangeAnchor = {};  // E19
    }
    rows.clear();
    childArena.clear();
    stack.clear();
    pending = PendingAction{};

    // -- phase 2: shortcuts. DEFAULT (focus-scoped) routing, never RouteGlobal: an active InputText
    //    owns the keys it consumes (E25) and an unfocused panel sees nothing (E26). Contrast 2.1.3's
    //    deliberately global Ctrl/Cmd+Q.
    if (!renaming.valid()) {
        if (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)) {
            pending = PendingAction{.kind = ActionKind::Delete};
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) {
            pending = PendingAction{.kind = ActionKind::Duplicate};
        }
        if (const Entity primary = context.selection.primary(); primary.valid() && ImGui::Shortcut(ImGuiKey_F2)) {
            pending = PendingAction{.kind = ActionKind::BeginRename, .target = primary};
        }
    }

    // -- phase 3 --
    drawTree(context);
    // -- phase 4 --
    drawVoidTarget(context);
    // -- phase 5 --
    applyPending(context);
}

void HierarchyPanel::drawTree(PanelContext& context) {
    const World& world = context.world;

    // Review round 2, Gap 2: the ancestor chain of `revealTarget` (a just-created/duplicated entity
    // set by the PREVIOUS frame's applyPending), forced open below so a collapsed parent does not
    // hide the row the user just asked for. ITERATIVE (misc-no-recursion is --warnings-as-errors on
    // the Linux lane, D13/I7). Computed here, not in applyPending, so a `revealTarget` that died
    // before this frame is simply ignored rather than dereferenced.
    revealPath.clear();
    if (revealTarget.valid() && world.alive(revealTarget)) {
        for (Entity cur = world.parent(revealTarget); cur.valid(); cur = world.parent(cur)) {
            revealPath.push_back(cur);
        }
    }
    revealTarget = {};

    // The actual walk is the pure, ImGui-free engine::editor::walkForest (review round 2, Gap 1):
    // `enter` draws the row and reports whether it is open; `unwind` owes ImGui the matching
    // TreePop/PopID pair (I3) -- TreePop only when `open`, PopID unconditionally, mirroring
    // TreeNodeEx's own contract (leaf nodes always report `open == true`, imgui.h:1360).
    const auto enter = [this, &context](Entity e) { return drawRow(context, e); };
    const auto unwind = [](Entity /*entity*/, bool open) {
        if (open) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::PopID();
    };
    walkForest(world, roots.entities(), stack, childArena, enter, unwind);

    revealPath.clear();  // consumed -- ImGui's own per-ID storage now remembers the open state
}

bool HierarchyPanel::drawRow(PanelContext& context, Entity entity) {
    const World& world = context.world;
    const Selection& selection = context.selection;

    // D22: key the ID on index AND generation, via two PushID(int) calls -- performance-no-int-to-ptr
    // rules out the usual TreeNodeEx((void*)(intptr_t)id, ...) idiom (F17). Keying on the NAME would
    // reset expansion state on every rename; keying on the index alone would let a recycled slot
    // inherit the previous entity's expansion state.
    ImGui::PushID(static_cast<int>(entity.index));
    ImGui::PushID(static_cast<int>(entity.generation));

    const bool isRenaming = (renaming == entity);
    entityLabel(world, entity, labelScratch);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_AllowOverlap;
    if (world.childCount(entity) == 0) {
        flags |= ImGuiTreeNodeFlags_Leaf;  // no expander on a childless entity (AC-9)
    }
    if (selection.contains(entity)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Review round 2, Gap 2: force this row open if it sits on the path to `revealTarget` -- MUST run
    // before TreeNodeEx, which is the "next" widget call SetNextItemOpen affects. `ImGuiCond_Always`
    // (not `_Once`/`_FirstUseEver`) so a re-reveal genuinely re-opens a row the user manually closed
    // in between.
    if (!revealPath.empty() && std::find(revealPath.begin(), revealPath.end(), entity) != revealPath.end()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    // The label goes through the "%s" FORMAT argument, never as the format string: the str_id
    // overload is IM_FMTARGS(3), and an entity named "%s" would otherwise be a format-string bug.
    const bool open = ImGui::TreeNodeEx("##row", flags, "%s", isRenaming ? "" : labelScratch.c_str());
    rows.push_back(entity);  // visible-row order -- the Shift-range domain (E20)

    // -- drag SOURCE ------------------------------------------------------------------------------
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(ENTITY_PAYLOAD_TYPE, &entity, sizeof(Entity));  // trivially copyable (F24)
        ImGui::TextUnformatted(labelScratch.c_str());
        ImGui::EndDragDropSource();  // ONLY when BeginDragDropSource returned true (F18)
    }
    // -- drop TARGET ------------------------------------------------------------------------------
    if (ImGui::BeginDragDropTarget()) {
        Entity dragged{};
        if (peekDraggedEntity(dragged)) {
            const std::span<const Entity> moved = movedSet(selection, dragged, moveScratch);
            // AC-15/E14: AcceptDragDropPayload is NOT CALLED for an illegal drop, so no highlight is
            // drawn and no reparent is attempted -- and World::setParent never logs an ERROR during
            // ordinary dragging.
            if (dropLegal(world, moved, entity) && ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD_TYPE) != nullptr) {
                pending = PendingAction{.kind = ActionKind::Reparent, .target = dragged, .second = entity};
            }
        }
        ImGui::EndDragDropTarget();  // ONLY when BeginDragDropTarget returned true (F18)
    }

    // -- click / double-click ---------------------------------------------------------------------
    // IsItemToggledOpen() guards the arrow: with _OpenOnArrow a click on the arrow also reports
    // IsItemClicked, and expanding a node must not change the selection.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        const ImGuiIO& io = ImGui::GetIO();  // io.KeyCtrl is Cmd on macOS automatically (imgui.h:2683)
        if (io.KeyShift) {
            pending = PendingAction{.kind = ActionKind::Range, .target = entity};
        } else if (io.KeyCtrl) {
            pending = PendingAction{.kind = ActionKind::Toggle, .target = entity};
        } else {
            pending = PendingAction{.kind = ActionKind::Select, .target = entity};
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        pending = PendingAction{.kind = ActionKind::BeginRename, .target = entity};  // AC-14
    }

    // -- context menu ------------------------------------------------------------------------------
    if (ImGui::BeginPopupContextItem()) {  // keyed on the current ID scope == this row
        if (ImGui::MenuItem("Create Empty")) {
            pending = PendingAction{.kind = ActionKind::CreateEmpty};
        }
        if (ImGui::MenuItem("Create Child")) {
            pending = PendingAction{.kind = ActionKind::CreateChild, .target = entity};
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2")) {
            pending = PendingAction{.kind = ActionKind::BeginRename, .target = entity};
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            pending = PendingAction{.kind = ActionKind::Duplicate, .target = entity};
        }
        if (ImGui::MenuItem("Delete", "Del")) {
            pending = PendingAction{.kind = ActionKind::Delete, .target = entity};
        }
        ImGui::EndPopup();  // ONLY when BeginPopupContextItem returned true
    }

    // -- the inline rename field (AC-14) ------------------------------------------------------------
    if (isRenaming) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (renameFocusPending) {
            ImGui::SetKeyboardFocusHere();
            renameFocusPending = false;
        }
        // F19/C5: the std::string overload -- no fixed buffer, no truncation rule.
        const bool committed = ImGui::InputText(
            "##rename", &renameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (committed || ImGui::IsItemDeactivatedAfterEdit()) {
            pending = PendingAction{.kind = ActionKind::CommitRename, .target = entity};
        } else if (ImGui::IsItemDeactivated()) {
            // Escape reverts ImGui's own buffer and THEN deactivates, so a deactivation with no edit
            // is exactly the cancel case.
            pending = PendingAction{.kind = ActionKind::CancelRename, .target = entity};
        }
    }
    return open;
}

void HierarchyPanel::drawVoidTarget(PanelContext& context) {
    // Fill the remaining content area with an invisible button: it is both the "click empty space to
    // clear the selection" surface and the "drop here to unparent" target (E17). A zero-or-negative
    // size is illegal for InvisibleButton, so clamp.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{avail.x > 1.0F ? avail.x : 1.0F, avail.y > 1.0F ? avail.y : 1.0F};
    ImGui::InvisibleButton("##void", size);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        pending = PendingAction{.kind = ActionKind::ClearSelection};
    }
    if (ImGui::BeginDragDropTarget()) {
        Entity dragged{};
        if (peekDraggedEntity(dragged)) {
            const std::span<const Entity> moved = movedSet(context.selection, dragged, moveScratch);
            if (dropLegal(context.world, moved, Entity{}) &&
                ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD_TYPE) != nullptr) {
                pending = PendingAction{.kind = ActionKind::Reparent, .target = dragged, .second = Entity{}};
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##voidmenu")) {
        if (ImGui::MenuItem("Create Empty")) {
            pending = PendingAction{.kind = ActionKind::CreateEmpty};
        }
        ImGui::EndPopup();
    }
}

void HierarchyPanel::applyPending(PanelContext& context) {
    World& world = context.world;
    Selection& selection = context.selection;

    switch (pending.kind) {
        case ActionKind::None:
            break;
        case ActionKind::Select:
            selection.set(pending.target);
            rangeAnchor = pending.target;
            break;
        case ActionKind::Toggle:
            selection.toggle(pending.target);
            rangeAnchor = pending.target;
            break;
        case ActionKind::Range: {
            // The range is over VISIBLE-ROW order (E20) -- what the user sees, not the forest.
            const auto anchorIt = std::find(rows.begin(), rows.end(), rangeAnchor);
            const auto clickedIt = std::find(rows.begin(), rows.end(), pending.target);
            if (anchorIt == rows.end() || clickedIt == rows.end()) {
                selection.set(pending.target);  // E18/E19: no (or no longer visible) anchor
                rangeAnchor = pending.target;
                break;
            }
            auto first = anchorIt;
            auto last = clickedIt;
            if (last < first) {
                std::swap(first, last);
            }
            selection.setAll(std::span<const Entity>{&*first, static_cast<std::size_t>(last - first) + 1U});
            break;
        }
        case ActionKind::ClearSelection:
            selection.clear();
            rangeAnchor = {};
            break;
        case ActionKind::CreateEmpty: {
            const Entity created = createEntity(world, {});
            if (created.valid()) {
                selection.set(created);  // AC-11
                rangeAnchor = created;
            }
            break;
        }
        case ActionKind::CreateChild: {
            const Entity parent = selection.primary().valid() ? selection.primary() : pending.target;
            const Entity created = createEntity(world, parent);
            if (created.valid()) {
                selection.set(created);
                rangeAnchor = created;
                revealTarget = created;  // Gap 2: open a collapsed parent to show it, next frame
            }
            break;
        }
        case ActionKind::Delete: {
            // A context-menu Delete on a row OUTSIDE the selection deletes that row; otherwise the
            // whole selection (the least-surprising reading, and the same rule the drag uses -- E16).
            if (pending.target.valid() && !selection.contains(pending.target)) {
                moveScratch.clear();
                moveScratch.push_back(pending.target);
                destroyEntities(world, moveScratch);
            } else {
                destroyEntities(world, selection.entities());  // AC-12/E8/E13
                selection.clear();
            }
            selection.prune(world);  // no dead handles survive the frame (I5/AC-12)
            rangeAnchor = {};
            renaming = {};
            break;
        }
        case ActionKind::Duplicate: {
            std::vector<Entity> created;
            if (pending.target.valid() && !selection.contains(pending.target)) {
                moveScratch.clear();
                moveScratch.push_back(pending.target);
                created = duplicateEntities(world, moveScratch);
            } else {
                created = duplicateEntities(world, selection.entities());  // AC-13/E9/E13
            }
            if (!created.empty()) {
                selection.setAll(created);
                rangeAnchor = selection.primary();
                revealTarget = rangeAnchor;  // Gap 2: reveal the primary copy too
            }
            break;
        }
        case ActionKind::BeginRename:
            if (world.alive(pending.target)) {
                renaming = pending.target;
                renameBuffer.assign(world.name(pending.target));  // D18: materialise the view
                renameFocusPending = true;
            }
            break;
        case ActionKind::CommitRename:
            if (world.alive(pending.target)) {
                world.setName(pending.target, renameBuffer);  // "" clears (E22/AC-14)
            }
            renaming = {};
            renameFocusPending = false;
            break;
        case ActionKind::CancelRename:
            renaming = {};  // the name is left untouched
            renameFocusPending = false;
            break;
        case ActionKind::Reparent: {
            const std::span<const Entity> moved = movedSet(selection, pending.target, moveScratch);
            // Copy before mutating: `moved` may alias Selection's own vector, and reparentEntity does
            // not touch the Selection today -- but a copy makes that independent of a future change.
            const std::vector<Entity> targets(moved.begin(), moved.end());
            for (const Entity e : targets) {
                reparentEntity(world, e, pending.second);
            }
            break;
        }
    }
    pending = PendingAction{};
}

}  // namespace engine::editor
