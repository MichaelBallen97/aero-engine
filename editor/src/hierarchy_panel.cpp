#include "hierarchy_panel.hpp"

#include <aero/editor/asset_view.hpp>  // elideCaptionRight
#include <aero/editor/command_stack.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_presentation.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/shortcut_hint.hpp>  // task E.6.2: Duplicate's hint
#include <aero/scene/mesh_renderer.hpp>   // task 3.1.5: the row arm asks the LIVE World, never the payload
#include <aero/scene/world.hpp>

#include "create_menu_ui.hpp"   // task E.5.2: drawCreateMenuItems -- the ONE drawing helper
#include "editor_theme_ui.hpp"  // currentUiScale
#include "panel_chrome_ui.hpp"
#include "text_input.hpp"

#include <algorithm>
#include <cfloat>  // FLT_MIN -- the "align to the right edge" idiom
#include <cstddef>
#include <cstring>  // std::memcpy -- the payload read (C6: the ImGui payload is alignas(1))
#include <imgui.h>
#include <imgui_internal.h>  // GetTopMostPopupModal -- the request seam's noModal (I337)
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::editor {

namespace {

constexpr const char* ENTITY_PAYLOAD_TYPE = "AERO_ENTITY";  // <= 32 chars (ImGui's own limit)

// Every moved row must be legally reparentable, or the drop is not offered at all (E14/AC-15).
// `moved` is ALREADY topMost()-filtered by the caller (engine::editor::reparentTargets) -- the same
// set applyPending's Reparent arm will actually move (D19's consistency extension; review round 2,
// Gap 4), so legality is never checked against a set different from the one that gets applied.
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

// task 3.1.5: the SAME shape, one payload type over. It PEEKS and it does not decode by hand -- every
// byte goes to decodeAssetDragPayload, which is the tree's only reader of ImGuiPayload::Data.
[[nodiscard]] std::optional<AssetDragPayload> peekAssetPayload() {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(ASSET_PAYLOAD_TYPE)) {
        return std::nullopt;
    }
    return decodeAssetDragPayload(payload->Data, payload->DataSize);
}

}  // namespace

void HierarchyPanel::onDraw(PanelContext& context) {
    const World& world = context.world;

    // -- phase 1: reconcile (FIRST, so the panel is correct when something ELSE changed the World --
    //    2.5.1's load, 2.4.2's undo, a future script). prune() only knows about Selection, which is
    //    why the two handle checks below are separate: they close E19 and E24. The root order
    //    (RootOrder, D10) now lives on EditorApp; it is still reconciled HERE, deliberately: this must
    //    run AFTER this frame's undo (applied in drawShellUi, 2.4.1 D19) and BEFORE the tree walk,
    //    which is exactly where it is.
    context.selection.prune(world);
    context.roots.reconcile(world);
    if (renaming.valid() && !world.alive(renaming)) {
        renaming = {};  // E24: the renamed entity was deleted mid-rename
        renameFocusPending = false;
    }
    // A rename whose ##rename field was NOT SUBMITTED on the previous frame ends here, as the field's own
    // deactivation would: committed when the buffer differs from the name the rename began with (an edit,
    // IsItemDeactivatedAfterEdit's case), cancelled otherwise -- through the two helpers phase 5's arms call (task
    // E.6.3's code-review round, round 2; I344). It runs before `searchEnabled` and the shortcut verdict read
    // `renaming`, so both are live on this same frame. Never re-armed: a fresh SetKeyboardFocusHere would steal the
    // keyboard back from wherever it went meanwhile.
    // NEVER against the World's CURRENT name: something else can rename the entity while the field is gone -- an
    // Edit > Undo of an earlier rename -- and an unedited buffer would then differ and commit, re-applying the name
    // the undo removed and dropping the redo branch (I344 (d)). The one departure from the field's own path: a buffer
    // edited and typed back to the original cancels here, where IsItemDeactivatedAfterEdit commits it (an edit at any
    // point of the activation, HasBeenEditedBefore -- imgui.cpp:4808, :4898, :6561-6565). The outcome is the same,
    // since commitRename pushes nothing for an unchanged name -- unless the World's name moved meanwhile, where
    // keeping the other writer's name is the safe direction.
    // Why: the field lives in the ##tree child, and once it is not submitted ImGui drops its ActiveId at the next
    // NewFrame (imgui.cpp:5795-5799); the deactivation recorded then lapses two frames on (SetActiveID's ElapseFrame,
    // :4807, cleared at :5818-5819) unless the field is submitted again (:12063-12064). So a field gone for longer
    // never reports IsItemDeactivated, and `renaming` stayed set with no active field -- the search field disabled,
    // and Delete / Backspace / Ctrl+D / F2 dead until the field was clicked and left.
    // The frame arithmetic. renameFieldFrame is written beside the field's inputTextString call, and by the
    // BeginRename arm with the frame that arm runs in: a rename begun in phase 5 of frame N reads N here on N + 1 --
    // its first walk, which submits the field -- and passes, and a field drawn on every frame reads the frame before
    // this one every time. A ##tree child skipped on frame K (a dock cut to its header row), or a renamed row not
    // walked on K (a collapsed ancestor, a query), leaves K - 1, so the rename ends on K + 1. A panel that did not
    // draw at all (a hidden tab, a panel closed and re-opened) leaves at most M - 2 on its first drawn frame M, so it
    // ends there. A row merely scrolled out of the child is still submitted -- and ItemAdd keeps an active field
    // alive before its clip test (imgui.cpp:11987, :12025-12029) -- so it keeps its rename.
    if (renaming.valid() && renameFieldFrame != ImGui::GetFrameCount() - 1) {
        if (renameBuffer != renameOriginal) {
            commitRename(context, renaming);
        } else {
            cancelRename();
        }
    }
    if (rangeAnchor.valid() && !world.alive(rangeAnchor)) {
        rangeAnchor = {};  // E19
    }
    // The deferred multi-select-drag candidate (E24-style safety net): a dead entity can never be
    // released over legitimately, and a MISSED release -- the row it names stopped drawing (a
    // collapsed ancestor) before mouse-up ever reached drawRow's RELEASE check -- must not leave this
    // armed forever. `!IsMouseReleased` excludes the one frame drawRow's own RELEASE check still needs
    // it: reconcile runs before the tree walk (phase order, D12), so clearing it here on the actual
    // release frame would race the check that is about to consume it this same frame.
    if (deferredSelectTarget.valid() &&
        (!world.alive(deferredSelectTarget) ||
         (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)))) {
        deferredSelectTarget = {};
    }
    rows.clear();
    childArena.clear();
    stack.clear();
    beginHierarchyRecordFrame(drawRecordValue);  // task E.6.3: one recorded frame per onDraw
    pending = PendingAction{};

    // -- task E.6.3: the search request, drained under the field's own predicate and no modal (I337) --
    searchEnabled = !renaming.valid();  // a rename owns the keyboard: the field is disabled meanwhile
    const bool noModal = ImGui::GetTopMostPopupModal() == nullptr;
    if (pendingSearchQuery.has_value() && searchEnabled && noModal) {
        searchText = std::move(*pendingSearchQuery);
    }
    pendingSearchQuery.reset();  // a moved-from optional stays ENGAGED (E.4.3's one-shot rule)
    if (pendingSearchFocus && searchEnabled && noModal) {
        searchFocusThisFrame = true;  // the click's effect: drawHeaderRow puts the keyboard in the field
    }
    pendingSearchFocus = false;

    // -- phase 1b: the header row. The badge counts the PREVIOUS frame's matches: the list is built below, after the
    //    badge draws (stated, D6; the breadcrumb's precedent).
    hierarchyBadgeText(world.entityCount(), searchText.empty() ? std::nullopt : lastMatchCount, badgeScratch);
    drawHeaderRow();

    // -- phase 2: shortcuts. DEFAULT (focus-scoped) routing, never RouteGlobal (E25/E26) -- and GATED while the search
    //    field owns the keys (D6). Active, it owns only the arrows, Delete, Backspace, Home and End
    //    (imgui_widgets.cpp:4923-4925), so Cmd+D and F2 would act on the selection while the user types; deactivated by
    //    Enter or Escape, it keeps the keyboard focus, and the next Backspace -- meant for the query -- would delete
    //    the selection (the code-review round's finding 20). The verdict is recorded. The child below routes these
    //    because the panel window is on its focus route.
    const bool shortcutsLive = hierarchyShortcutsLive(renaming.valid(), searchOwnsKeys);
    drawRecordValue.shortcutsLive = shortcutsLive;
    if (shortcutsLive) {
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
    // The rename request (requestBeginRename): F2's own gate and no modal, drained here and dropped either way (I337,
    // I338).
    if (pendingBeginRename.has_value() && shortcutsLive && noModal) {
        pending = PendingAction{.kind = ActionKind::BeginRename, .target = *pendingBeginRename};
    }
    pendingBeginRename.reset();

    // -- phase 3's match list, built whether or not the tree below draws, so the badge's m / n stays current while
    //    the field is typed into over a skipped child. drawTree's flat list draws these.
    if (searchText.empty()) {
        lastMatchCount.reset();
    } else {
        collectHierarchyMatches(world, context.roots.entities(), searchText, matches, stack, childArena, labelScratch);
        lastMatchCount = matches.size();
    }

    // -- phases 3 and 4, inside the ALWAYS-present ##tree child (D6): a 90-row scene scrolls under a fixed header. The
    //    reveal scroll (SetScrollHereY acts on the current window), the void target and both context menus move with
    //    the tree; keyboardFocusPanelId reads the ROOT window's name, so E.3.2's router sees no difference.
    //    Both run only while BeginChild says the child is visible -- the Assets tree's C2 (the code-review round's
    //    finding 6). BeginChild clips the child with the panel's ClipRect (imgui.cpp:8434-8440), and a child clipped
    //    away entirely -- a dock cut down to its header row -- is skipped (:8733-8738, :8764-8768): TreeNodeBehavior
    //    then returns at its first statement (imgui_widgets.cpp:6879-6880), before TreeNodeUpdateNextOpen applies
    //    the reveal's SetNextItemOpen, so a walk there would CONSUME revealTarget and open nothing. Skipping the walk
    //    leaves it pending until the child is visible, and drawTree hands it to the tree walk alone (I340).
    //    The record's clip bottom is what BeginChild clips with: ImGui::PushClipRect and PopClipRect keep the window's
    //    ClipRect equal to its draw list's top clip rect (imgui.cpp:6114-6126), so the public read is that value.
    drawRecordValue.panelClipMaxY = ImGui::GetWindowDrawList()->GetClipRectMax().y;
    const bool treeVisible = ImGui::BeginChild("##tree", ImVec2(0.0F, 0.0F), ImGuiChildFlags_None);
    const ImVec2 treePos = ImGui::GetWindowPos();
    const ImVec2 treeSize = ImGui::GetWindowSize();
    drawRecordValue.treeChildMin = Vec2{treePos.x, treePos.y};
    drawRecordValue.treeChildMax = Vec2{treePos.x + treeSize.x, treePos.y + treeSize.y};
    if (treeVisible) {
        drawTree(context);
        drawVoidTarget(context);
    }
    ImGui::EndChild();  // UNCONDITIONAL -- 1:1 with BeginChild whatever it returned
    // -- phase 5 --
    applyPending(context);
}

// task E.6.3 (D6): an UNFILLED header row (the window's own `panel` ground) over a divider rule: the search field
// with its hint filling the row, the count badge right-aligned. Whether the field owns the keys is read HERE -- before
// the shortcut phase, on the same frame (I312's order arm).
void HierarchyPanel::drawHeaderRow() {
    const float s = currentUiScale();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float padY = EDITOR_THEME.panels.panelHeaderPaddingY * s;
    const float frame = ImGui::GetFrameHeight();
    const HierarchyHeaderLayout layout =
        hierarchyHeaderLayout(rowMin.x, rowWidth, faceTextWidth(PanelFace::MonoSmall, badgeScratch), s);
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + padY));
    ImGui::SetNextItemWidth(layout.searchWidth);
    ImGui::BeginDisabled(!searchEnabled);
    if (searchFocusThisFrame) {
        ImGui::SetKeyboardFocusHere();  // the seam's click: the field activates on the next frame
        searchFocusThisFrame = false;
    }
    inputTextString("##search", "Search entities", searchText, ImGuiInputTextFlags_None);
    // The field OWNS THE KEYS while it is active, or while it keeps the keyboard focus its OWN activation gave it (the
    // code-review round's finding 20). Activating it -- a click, or the seam above -- runs SetActiveID, SetFocusID and
    // FocusWindow (imgui_widgets.cpp:4913-4915); Enter and Escape undo only the first (clear_active_id at :5180 /
    // :5212, then ClearActiveID at :5452-5453, which is SetActiveID(0, NULL) and touches no NavId,
    // imgui.cpp:4863-4866). So IsItemFocused -- NavId is this item (imgui.cpp:6568-6572) -- stays true until another
    // item takes the focus: a row's or the void target's ButtonBehavior calls SetFocusID (imgui_widgets.cpp:632, :650)
    // on the ##tree child. DECIDED: a tab click or a focus command restores the window's last NavId (FocusWindow,
    // imgui.cpp:13733), which is the field's once it was activated, so after a search the hold survives a round trip
    // and lasts until a row is clicked -- the safe direction.
    // But ImGui ALSO hands the field NavId by itself: a tab or title-bar click that restores no NavId runs
    // NavInitWindow (imgui.cpp:19750-19752), so does a docked window reappearing with no modal up (:8616-8619,
    // :14402-14409), and NavInit picks the root window's first item -- this field, since the rows live in the child.
    // That focus owns nothing, or Delete would be dead after a plain tab click until a row is clicked. Hence the
    // LATCH: set by the field's activation, released whenever the keyboard is in this panel (the child included) but
    // not on the field -- a row click, or the NavId 0 a re-initialising window starts from. A NavId restored while
    // the keyboard was elsewhere keeps it.
    if (ImGui::IsItemActive()) {
        searchHeldKeys = true;
    } else if (!ImGui::IsItemFocused() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        searchHeldKeys = false;
    }
    searchOwnsKeys = ImGui::IsItemActive() || (searchHeldKeys && ImGui::IsItemFocused());
    drawRecordValue.searchActive = ImGui::IsItemActive();
    drawRecordValue.searchFocused = ImGui::IsItemFocused();
    drawRecordValue.searchFieldMin = Vec2{ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y};
    drawRecordValue.searchFieldMax = Vec2{ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
    ImGui::EndDisabled();
    // The badge, its small Mono line centred on the field.
    const float smallPoints = panelFacePoints(PanelFace::MonoSmall);
    ImGui::SetCursorScreenPos(ImVec2(layout.badgeX, rowMin.y + padY + ((frame - smallPoints) * 0.5F)));
    countBadge(badgeScratch);
    drawRecordValue.badgeText = badgeScratch;
    const float ruleY = rowMin.y + frame + (2.0F * padY);
    // The record reads what the list RECEIVED: AddRectFilled adds no vertex at alpha 0 (imgui_draw.cpp:1532).
    ImDrawList* const headerList = ImGui::GetWindowDrawList();
    const int ruleVertices = headerList->VtxBuffer.Size;
    horizontalRule(headerList, rowMin.x, rowMin.x + rowWidth, ruleY, ChromeRole::Divider);
    drawRecordValue.headerRuleDrawn = headerList->VtxBuffer.Size > ruleVertices;
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, ruleY + ruleHeight()));  // the ##tree child is the item that follows
}

void HierarchyPanel::drawTree(PanelContext& context) {
    const World& world = context.world;

    // Review round 2, Gap 2: the ancestor chain of `revealTarget` (a just-created/duplicated entity
    // set by the PREVIOUS frame's applyPending -- or an earlier one's, when the reveal had to wait),
    // forced open below so a collapsed parent does not hide the row the user just asked for.
    // ITERATIVE (misc-no-recursion is --warnings-as-errors on the Linux lane, D13/I7). Computed here,
    // not in applyPending, so a `revealTarget` that died before this frame is simply ignored rather
    // than dereferenced. Only the TREE walk takes it: a flat row is Leaf, and TreeNodeUpdateNextOpen
    // returns before it reads SetNextItemOpen (imgui_widgets.cpp:6808-6809), so the flat list would
    // use the reveal up and open nothing. A query typed while it waits -- over a skipped ##tree child
    // (this function runs only while the child is visible), or on the frame after a create -- defers
    // it to the first tree walk (I340).
    revealPath.clear();
    if (searchText.empty()) {
        if (revealTarget.valid() && world.alive(revealTarget)) {
            for (Entity cur = world.parent(revealTarget); cur.valid(); cur = world.parent(cur)) {
                revealPath.push_back(cur);
            }
        }
        scrollTarget = revealTarget;  // task E.5.2 -- consumed by drawRow, or dropped below
        revealTarget = {};
    }

    if (!searchText.empty()) {
        // task E.6.3 (D6): a FLAT pre-order list of every match -- onDraw collected this frame's -- with no forced
        // expansion of the user's tree. Each row is the same row widget drawn Leaf | NoTreePushOnOpen, so the unwind
        // owes ImGui its two PopIDs and no TreePop.
        drawRecordValue.flatMode = true;
        flatRow = true;
        for (const Entity entity : matches) {
            (void)drawRow(context, entity);  // a NoTreePushOnOpen leaf pushes no tree level
            ImGui::PopID();
            ImGui::PopID();
        }
        flatRow = false;
    } else {
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
        walkForest(world, context.roots.entities(), stack, childArena, enter, unwind);
    }

    revealPath.clear();  // consumed -- ImGui's own per-ID storage now remembers the open state
    scrollTarget = {};   // task E.5.2: a reveal whose row was never drawn does not scroll on a later frame
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
    if (flatRow) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;  // D6: the flat list's row
    }

    // Review round 2, Gap 2: force this row open if it sits on the path to `revealTarget` -- MUST run
    // before TreeNodeEx, which is the "next" widget call SetNextItemOpen affects. `ImGuiCond_Always`
    // (not `_Once`/`_FirstUseEver`) so a re-reveal genuinely re-opens a row the user manually closed
    // in between.
    if (!revealPath.empty() && std::find(revealPath.begin(), revealPath.end(), entity) != revealPath.end()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    // The node's own label is EMPTY: the entity's name is drawn by drawRowChrome (elideCaptionRight, then
    // drawRoleText, never formatted). "%s" stays the format, so nothing user-named can reach the format slot.
    // task E.6.3 (D6): ImGui draws the arrow (textFaint) and its own square selection fill (Header = selection, KEPT
    // for the Hierarchy alone); the label is drawn below at the x ImGui would have used, after the kind dot.
    const float rowCursorX = ImGui::GetCursorScreenPos().x;
    ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextFaint));
    const bool open = ImGui::TreeNodeEx("##row", flags, "%s", "");
    ImGui::PopStyleColor();
    drawRowChrome(world, entity, selection.contains(entity), isRenaming, rowCursorX);
    // task E.5.2 (D7): the revealed row is scrolled into view when it is NOT visible -- a root create is
    // appended at the bottom of the root order, so in a long scene the new selection would otherwise be off
    // screen. IsItemVisible and SetScrollHereY both read the item JUST submitted (SetScrollHereY targets
    // CursorPosPrevLine, imgui.cpp:12733-12743). A scroll request mutates no World and no Selection, so it is
    // legal inside this read-only walk.
    if (scrollTarget.valid() && entity == scrollTarget) {
        if (!ImGui::IsItemVisible()) {
            ImGui::SetScrollHereY(0.5F);
        }
        scrollTarget = {};
    }
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
            const std::vector<Entity> moved = reparentTargets(world, selection.entities(), dragged);
            // AC-15/E14: AcceptDragDropPayload is NOT CALLED for an illegal drop, so no highlight is
            // drawn and no reparent is attempted -- and World::setParent never logs an ERROR during
            // ordinary dragging.
            if (dropLegal(world, moved, entity) && ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD_TYPE) != nullptr) {
                pending = PendingAction{.kind = ActionKind::Reparent, .target = dragged, .second = entity};
            }
        } else if (const std::optional<AssetDragPayload> asset = peekAssetPayload(); asset.has_value()) {
            // task 3.1.5: a SECOND ARM inside the existing target, never a second target. The two
            // payload types cannot cross-fire: peekDraggedEntity's IsDataType("AERO_ENTITY") and
            // peekAssetPayload's IsDataType("AERO_ASSET") are mutually exclusive by construction.
            //
            // REFUSED AT PEEK: classifyAssetDrop runs BEFORE AcceptDragDropPayload, so ImGui draws no
            // highlight rect for a drop this row will not take. The panel RECORDS and does not act --
            // instantiation needs the database, the importer and the ledger, none of which this panel
            // has, so the drop is its own one-shot drained by tick() rather than a PendingAction.
            const auto kind = static_cast<AssetKind>(asset->kind);
            const bool hasMesh = world.has<MeshRenderer>(entity);
            if (classifyAssetDrop(kind, DropSurface::HierarchyRow, hasMesh, /*fieldKind=*/std::nullopt) !=
                    DropAction::None &&
                ImGui::AcceptDragDropPayload(ASSET_PAYLOAD_TYPE) != nullptr) {
                pendingAssetDrop = HierarchyAssetDrop{.payload = *asset, .targetRow = entity};
            }
        }
        ImGui::EndDragDropTarget();  // ONLY when BeginDragDropTarget returned true (F18)
    }

    // -- click / double-click ---------------------------------------------------------------------
    // The PRESS half: engine::editor::clickSelectionAction (selection.hpp) is the pure decision --
    // IsItemToggledOpen() (the arrow guard) and io.KeyCtrl/KeyShift are simply gathered here and
    // handed to it, so the whole matrix (incl. the arrow guard) is provable at tier-0 with no ImGui
    // context (hierarchy_test.cpp). A plain click on an ALREADY-selected row resolves to None here ON
    // PURPOSE -- it is deferred to the RELEASE check below, which is what keeps a same-press multi-drag
    // (E16/AC-15) from ever seeing its selection collapsed to one row (the bug this closes).
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();  // io.KeyCtrl is Cmd on macOS automatically (imgui.h:2683)
        const bool arrowToggled = ImGui::IsItemToggledOpen();
        const ClickSelectionAction action =
            clickSelectionAction(selection.contains(entity), io.KeyCtrl, io.KeyShift, arrowToggled,
                                 /*dragOccurred=*/false, ClickPhase::Press);
        switch (action) {
            case ClickSelectionAction::None:
                // Only the deferred candidate (a plain click on an already-selected row) arms here --
                // the arrow guard also resolves to None but must never arm it (a click that only
                // toggled the arrow open/closed is not the start of a click-or-drag gesture at all).
                if (!arrowToggled) {
                    deferredSelectTarget = entity;
                }
                break;
            case ClickSelectionAction::Select:
                pending = PendingAction{.kind = ActionKind::Select, .target = entity};
                break;
            case ClickSelectionAction::Toggle:
                pending = PendingAction{.kind = ActionKind::Toggle, .target = entity};
                break;
            case ClickSelectionAction::Range:
                pending = PendingAction{.kind = ActionKind::Range, .target = entity};
                break;
        }
    }
    // The RELEASE half: resolves the ONE candidate armed above, on whatever later frame the mouse
    // button actually comes back up. `GetDragDropPayload() != nullptr` is the PUBLIC-API drag signal
    // (ImGui::IsDragDropActive() is internal-only, declared in imgui_internal.h; this TU includes that header
    // only for GetTopMostPopupModal and keeps the public signal) -- and it is accurate here specifically because
    // this function unconditionally calls
    // BeginDragDropSource() + SetDragDropPayload() every frame for every row above: the payload is live
    // the instant this row's own press crosses ImGui's drag threshold, and per imgui.cpp's own "Elapse
    // payload" bookkeeping (EndFrame), it is NOT cleared until the frame AFTER release even when
    // delivered -- so it is still live for this exact check on the release frame itself, and stays live
    // whether the drop landed on a different row (legal) or back on this same row (illegal self-drop,
    // AC-15/E14) -- deliberately: a real drag must never collapse the selection either way, even if it
    // lands back where it started. Consumed unconditionally: one release resolves (or discards) one
    // press.
    if (deferredSelectTarget == entity && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const bool hoveredNow = ImGui::IsItemHovered();
        const bool dragOccurred = ImGui::GetDragDropPayload() != nullptr;
        if (hoveredNow && clickSelectionAction(/*alreadySelected=*/true, false, false, false, dragOccurred,
                                               ClickPhase::Release) == ClickSelectionAction::Select) {
            pending = PendingAction{.kind = ActionKind::Select, .target = entity};
        }
        deferredSelectTarget = {};
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        pending = PendingAction{.kind = ActionKind::BeginRename, .target = entity};  // AC-14
    }

    // -- context menu ------------------------------------------------------------------------------
    if (ImGui::BeginPopupContextItem()) {  // keyed on the current ID scope == this row
        // task E.5.2 (D6): Create Empty is ONE kind through the ONE pipeline -- recorded here, placed,
        // pushed, framed and revealed by EditorApp::applyCreate on the next tick.
        // task E.6.1: the item draws with its icon.
        if (drawCreateKindItem("Create Empty", CreateKind::Empty, true)) {
            pendingCreate = CreateKind::Empty;
        }
        if (ImGui::MenuItem("Create Child")) {
            pending = PendingAction{.kind = ActionKind::CreateChild, .target = entity};
        }
        // task E.5.2 (D5): the typed entries create at the ROOT, never under this row -- a world-space
        // default made local to an arbitrary parent would be a tilted, stretched Plane.
        if (const std::optional<CreateKind> typed = drawCreateMenuItems(true); typed.has_value()) {
            pendingCreate = typed;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2")) {
            pending = PendingAction{.kind = ActionKind::BeginRename, .target = entity};
        }
        if (ImGui::MenuItem("Duplicate", chordHint(currentHostOs(), {.ctrl = true}, "D").c_str())) {
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
        const bool committed = inputTextString(
            "##rename", renameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        renameFieldFrame = ImGui::GetFrameCount();  // submitted this frame -- what onDraw's stale-field check reads
        drawRecordValue.renameFieldDrawn = true;
        drawRecordValue.renameActive = ImGui::IsItemActive();
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

// task E.6.3 (D6): the row's dot, label and meta, on the draw list -- NO item, so the node stays the last item for
// the drag, drop, click and context-menu reads after it. Every x from hierarchyRowLayout.
void HierarchyPanel::drawRowChrome(const World& world, Entity entity, bool selected, bool renaming, float rowCursorX) {
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    // Where ImGui would have put an unframed node's label: the arrow box (FontSize) and two FramePadding.x
    // (imgui_widgets.cpp:6894's unframed text_offset_x).
    const float labelStartX = rowCursorX + ImGui::GetFontSize() + (2.0F * style.FramePadding.x);
    const float rowMidY = (itemMin.y + itemMax.y) * 0.5F;
    const std::size_t children = world.childCount(entity);
    metaScratch.clear();
    if (children > 0U) {
        metaScratch = std::to_string(children);  // the DIRECT child count (D6), never the descendants
    }
    const HierarchyRowLayout layout =
        hierarchyRowLayout(labelStartX, itemMax.x, rowMidY, faceTextWidth(PanelFace::MonoSmall, metaScratch),
                           children > 0U, currentUiScale());
    const ChromeRole role = entityKindRole(entityKindFor(world, entity), selected);
    // The dot is counted off the vertices the list RECEIVED, never beside the call: AddCircleFilled adds none at
    // alpha 0 or a radius under 0.5 (imgui_draw.cpp:1634), and a deleted call adds none either (I309).
    const int dotVertices = list->VtxBuffer.Size;
    const ImVec2 dotCenter((layout.dotMin.x + layout.dotMax.x) * 0.5F, rowMidY);
    roleDot(list, dotCenter, layout.dotMax.x - layout.dotMin.x, role);
    ++drawRecordValue.rowsDrawn;
    if (list->VtxBuffer.Size > dotVertices) {
        ++drawRecordValue.dotsDrawn;
    }
    drawRecordValue.rowRoles.push_back(HierarchyRowRole{.entity = entity, .role = role});
    if (!renaming) {
        const float maxWidth = layout.labelMaxX - layout.labelX;
        const std::string drawn = elideCaptionRight(labelScratch, [maxWidth](std::string_view text) {
            return faceTextWidth(PanelFace::Body, text) <= maxWidth;
        });
        const float textY = rowMidY - (panelFacePoints(PanelFace::Body) * 0.5F);
        drawRoleText(list, PanelFace::Body, ImVec2(layout.labelX, textY),
                     selected ? ChromeRole::Accent : ChromeRole::Text, drawn);
    }
    if (layout.metaShown) {
        const float smallY = rowMidY - (panelFacePoints(PanelFace::MonoSmall) * 0.5F);
        drawRoleText(list, PanelFace::MonoSmall, ImVec2(layout.metaX, smallY), ChromeRole::TextFaint, metaScratch);
        ++drawRecordValue.metasDrawn;
        drawRecordValue.rowMetas.push_back(HierarchyRowMeta{.entity = entity, .text = metaScratch});
    }
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
            const std::vector<Entity> moved = reparentTargets(context.world, context.selection.entities(), dragged);
            if (dropLegal(context.world, moved, Entity{}) &&
                ImGui::AcceptDragDropPayload(ENTITY_PAYLOAD_TYPE) != nullptr) {
                pending = PendingAction{.kind = ActionKind::Reparent, .target = dragged, .second = Entity{}};
            }
        } else if (const std::optional<AssetDragPayload> asset = peekAssetPayload(); asset.has_value()) {
            // task 3.1.5: the row arm's exact shape, with the VOID surface -- so a material dropped on
            // empty space is refused (there is no entity to assign it to) while a model instantiates
            // at the scene root. targetHasMeshRenderer is FALSE here because there is no target.
            const auto kind = static_cast<AssetKind>(asset->kind);
            if (classifyAssetDrop(kind, DropSurface::HierarchyVoid, /*targetHasMeshRenderer=*/false,
                                  /*fieldKind=*/std::nullopt) != DropAction::None &&
                ImGui::AcceptDragDropPayload(ASSET_PAYLOAD_TYPE) != nullptr) {
                pendingAssetDrop = HierarchyAssetDrop{.payload = *asset, .targetRow = Entity{}};
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##voidmenu")) {
        // task E.6.1: the item draws with its icon.
        if (drawCreateKindItem("Create Empty", CreateKind::Empty, true)) {
            pendingCreate = CreateKind::Empty;  // task E.5.2 (D6): through the one pipeline
        }
        ImGui::Separator();
        if (const std::optional<CreateKind> typed = drawCreateMenuItems(true); typed.has_value()) {
            pendingCreate = typed;
        }
        ImGui::EndPopup();
    }
}

void HierarchyPanel::commitRename(PanelContext& context, Entity target) {
    const World& world = context.world;
    // D19: compare first -- an unchanged name pushes nothing. The std::string(...) copy is MANDATORY, not
    // defensive: world.name()'s view is invalidated by the very rename commit this command is about to perform.
    if (world.alive(target) && world.name(target) != renameBuffer) {
        CommandContext cmd = toCommandContext(context);
        context.commands.push(
            cmd, std::make_unique<RenameEntityCommand>(target, std::string(world.name(target)), renameBuffer));
    }
    renaming = {};
    renameFocusPending = false;
}

void HierarchyPanel::cancelRename() noexcept {
    renaming = {};  // the name is left untouched
    renameFocusPending = false;
}

void HierarchyPanel::applyPending(PanelContext& context) {
    // Every mutation now flows through a Command's redo() (task 2.4.2) -- this function only ever
    // READS the World directly (alive()/name(), and reparentTargets()'s own const query).
    const World& world = context.world;
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
        case ActionKind::CreateChild: {
            // Section O-2's consistency extension (review round 2, Gap 5): resolveCreateChildParent
            // makes a right-click on a row OUTSIDE the selection act on that row alone -- Delete and
            // Duplicate below already follow this rule; CreateChild now does too, so right-clicking
            // an unselected row never silently creates a child under a DIFFERENT (selected) entity.
            const Entity parent = resolveCreateChildParent(selection.entities(), selection.primary(), pending.target);
            CommandContext cmd = toCommandContext(context);
            if (context.commands.push(
                    cmd, std::make_unique<CreateEntityCommand>(parent, std::string_view{}, selection.entities()))) {
                rangeAnchor = selection.primary();
                revealTarget = rangeAnchor;  // Gap 2: open a collapsed parent to show it, next frame
                searchText.clear();          // task E.6.3 (D6)
            }
            break;
        }
        case ActionKind::Delete: {
            // A context-menu Delete on a row OUTSIDE the selection deletes that row; otherwise the
            // whole selection (the least-surprising reading, and the same rule the drag uses -- E16).
            if (pending.target.valid() && !selection.contains(pending.target)) {
                moveScratch.clear();
                moveScratch.push_back(pending.target);
            } else {
                moveScratch.assign(selection.entities().begin(), selection.entities().end());
            }
            CommandContext cmd = toCommandContext(context);
            // The command's own redo prunes the selection (AC-12/E8/E13/I5): it empties exactly when
            // the destroyed set WAS the selection and leaves it untouched otherwise, expressing that
            // rule once instead of twice.
            context.commands.push(cmd, std::make_unique<DeleteEntitiesCommand>(moveScratch, selection.entities()));
            rangeAnchor = {};
            renaming = {};
            break;
        }
        case ActionKind::Duplicate: {
            if (pending.target.valid() && !selection.contains(pending.target)) {
                moveScratch.clear();
                moveScratch.push_back(pending.target);
            } else {
                moveScratch.assign(selection.entities().begin(), selection.entities().end());
            }
            CommandContext cmd = toCommandContext(context);
            if (context.commands.push(cmd,
                                      std::make_unique<DuplicateEntitiesCommand>(moveScratch, selection.entities()))) {
                // AC-13/E9/E13: the command has already selected the copies (D5).
                rangeAnchor = selection.primary();
                revealTarget = rangeAnchor;  // Gap 2: reveal the primary copy too
                searchText.clear();          // task E.6.3 (D6)
            }
            break;
        }
        case ActionKind::BeginRename:
            if (world.alive(pending.target)) {
                renaming = pending.target;
                renameBuffer.assign(world.name(pending.target));  // D18: materialise the view
                renameOriginal = renameBuffer;  // what onDraw's stale-field check compares the buffer with
                renameFocusPending = true;
                renameFieldFrame = ImGui::GetFrameCount();  // so the next frame's stale-field check passes
            }
            break;
        case ActionKind::CommitRename:
            commitRename(context, pending.target);
            break;
        case ActionKind::CancelRename:
            cancelRename();
            break;
        case ActionKind::Reparent: {
            // D19's consistency extension to the drag-drop path (review round 2, Gap 4):
            // reparentTargets() is ALREADY topMost()-filtered -- the exact set `dropLegal` validated
            // during the hover that produced this action -- so a multi-selected parent+child dragged
            // together moves as ONE subtree, never split with the child peeled off into the target
            // directly.
            const std::vector<Entity> targets = reparentTargets(world, selection.entities(), pending.target);
            CommandContext cmd = toCommandContext(context);
            context.commands.push(cmd, std::make_unique<ReparentCommand>(targets, pending.second));
            break;
        }
    }
    pending = PendingAction{};
}

}  // namespace engine::editor
