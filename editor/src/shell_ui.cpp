// Aero Engine — the ONE new ImGui TU this task adds besides placeholder_panel.cpp (task 2.1.3, D9):
// menu bar, dockspace, the data-driven default layout, and the balanced Begin/End around every
// Panel::onDraw(). editor_app.cpp includes this header but never ImGui itself.
#include "shell_ui.hpp"

#include <aero/editor/command_stack.hpp>
#include <aero/editor/panel.hpp>

#include <array>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder* (2.1.1 precedent)
#include <string>

// <ImGuizmo.h> deliberately follows <imgui.h> and lives in its own trailing include block: it does
// NOT include imgui.h (it forward-declares ImGuiWindow, then names ImDrawList / ImVec2 / ImU32 /
// ImGuiContext), and SortIncludes: CaseSensitive would hoist 'I' above 'i'. The ordering is held
// STRUCTURALLY by .clang-format's ImGuizmo category (Priority 5), not by this comment.
#include <ImGuizmo.h>

namespace engine::editor {

namespace {

// One disabled stub item + its "owned by task N" tooltip (D5). A disabled MenuItem can never return
// true, so there is no unimplemented handler behind it.
void menuItemStub(const char* label, const char* shortcut, const char* owningTask) {
    ImGui::MenuItem(label, shortcut, false, /*enabled=*/false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", owningTask);  // e.g. "Not implemented yet — task 2.5.1"
    }
}

// SCREAMING_SNAKE and file-scope, NOT a `constexpr` local named historyFlags: .clang-tidy's
// readability-identifier-naming.ConstexprVariableCase is UPPER_CASE and applies to LOCALS too, so the
// camelBack spelling fails --warnings-as-errors on the Linux lane (the same class of trap as 2.3.3's
// `const bool using_`, which failed VariableCase: camelBack). File scope because both chords share it.
constexpr ImGuiInputFlags HISTORY_SHORTCUT_FLAGS = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Repeat;

void drawMenuBar(PanelRegistry& panels, PanelContext& context, ShellUiState& state) {
    // Editor shortcuts go through ImGui's routing, NEVER ctx.input() (D7/E9): a focused InputText
    // must be able to swallow the chord. ImGuiMod_Ctrl is Cmd on macOS automatically (F8).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal)) {
        state.quitRequested = true;
    }

    // Task 2.4.1. RouteGlobal, exactly like Ctrl+Q above -- which is what makes a focused InputText
    // win these chords back: InputTextEx binds Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z on the ACTIVE ITEM's own
    // id with the default RouteFocused (imgui_widgets.cpp:5130-5131), and plain RouteGlobal is LAST in
    // the documented priority list (imgui.h:1758). Repeat: ImGui's own text undo repeats, and
    // RepeatUntilKeyModsChange is applied automatically (imgui.cpp:11457-11460), so RELEASING the
    // modifier stops the repeat instead of leaving a stuck auto-repeat on Z. ImGuiMod_Ctrl is Cmd on
    // macOS (imgui.h:1735 + the AddKeyEvent swap at imgui.cpp:1894). The two chords cannot cross-fire:
    // a chord requires an EXACT 4-bit mod match (imgui.cpp:11386, `g.IO.KeyMods != mods`), so this
    // order is documentation, not arbitration.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, HISTORY_SHORTCUT_FLAGS)) {
        state.undoRequested = true;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, HISTORY_SHORTCUT_FLAGS)) {
        state.redoRequested = true;
    }
    // Ctrl+Y is DELIBERATELY NOT bound (D13): ImGuiMod_Ctrl is Cmd on macOS, so a global Ctrl+Y would
    // bind ⌘Y, which is redo on no platform. InputText keeps its own local Ctrl+Y and is unaffected.

    if (!ImGui::BeginMainMenuBar()) {
        return;  // F7: End only when Begin returned true
    }
    if (ImGui::BeginMenu("File")) {
        menuItemStub("New Scene", "Ctrl+N", "Not implemented yet — task 2.5.1");
        menuItemStub("Open Scene...", "Ctrl+O", "Not implemented yet — task 2.5.1");
        menuItemStub("Save Scene", "Ctrl+S", "Not implemented yet — task 2.5.1");
        menuItemStub("Save Scene As...", "Ctrl+Shift+S", "Not implemented yet — task 2.5.1");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Ctrl+Q")) {
            state.quitRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        // Task 2.4.1: these are IMPLEMENTED now, so the disabled-stub helper's "owned by task N"
        // tooltip would be a lie. Built only while the menu is OPEN, so the allocation happens on the
        // frames a human is looking at it. No std::format and no snprintf: += over a string_view is
        // the whole operation.
        const CommandStack& commands = context.commands;
        std::string undoText = "Undo";
        if (const std::string_view label = commands.undoLabel(); !label.empty()) {
            undoText += ' ';
            undoText += label;
        }
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, commands.canUndo())) {
            state.undoRequested = true;
        }
        std::string redoText = "Redo";
        if (const std::string_view label = commands.redoLabel(); !label.empty()) {
            redoText += ' ';
            redoText += label;
        }
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Shift+Z", false, commands.canRedo())) {
            state.redoRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (std::size_t i = 0; i < panels.count(); ++i) {
            bool shown = panels.visibleAt(i);
            if (ImGui::MenuItem(panels.panelAt(i).title(), nullptr, &shown)) {
                panels.setVisibleAt(i, shown);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            state.applyDefaultLayout = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// The shortcut labels above are DISPLAY STRINGS ONLY -- MenuItem's `shortcut` argument draws text and
// binds nothing. The only live bindings are the THREE explicit ImGui::Shortcut calls at the top of
// drawMenuBar (Ctrl+Q, Ctrl+Z, Ctrl+Shift+Z); the File-menu stubs deliberately bind nothing, so Ctrl+S
// does nothing until 2.5.1 wires it.

// Applied HERE -- after drawMenuBar returned (EndMainMenuBar has run) and BEFORE the dockspace and the
// panel walk. That is the precondition .claude/rules/editor.md's "never mutate the World during a draw
// walk" actually protects: no ImGui tree is open and no eachChild walk is in flight
// (viewport_panel.cpp:264-266 records the same reasoning for the selection write). Applying here rather
// than back in tick() is what makes THIS frame's panels show post-undo state, with no lag (D19/AC-21).
//
// Undo before redo when both are set in one frame: the two chords cannot both fire (an exact mod match,
// imgui.cpp:11386), but requestUndo() and requestRedo() can both be called before one tick. A fixed
// order makes that deterministic and testable; the net effect is a no-op plus two DEBUG lines (E12).
void applyHistoryRequests(PanelContext& context, ShellUiState& state) {
    if (state.undoRequested) {
        state.undoRequested = false;
        context.commands.undo(context.world);
    }
    if (state.redoRequested) {
        state.redoRequested = false;
        context.commands.redo(context.world);
    }
}

// The balanced-End core (E1). Hidden panels `continue` before Begin, so they never call End either;
// a visible panel's End() is unconditional — Begin's return value only gates onDraw().
void drawPanels(PanelRegistry& panels, PanelContext& context) {
    for (std::size_t i = 0; i < panels.count(); ++i) {
        bool open = panels.visibleAt(i);
        if (!open) {
            continue;  // hidden: no Begin, therefore no End (E1)
        }
        Panel& panel = panels.panelAt(i);
        const PanelOptions opts = panel.options();

        ImGuiWindowFlags flags = 0;
        if (opts.noScrollbar) {
            flags |= ImGuiWindowFlags_NoScrollbar;
        }
        if (opts.hasMenuBar) {
            flags |= ImGuiWindowFlags_MenuBar;
        }
        if (opts.noScrollWithMouse) {
            flags |= ImGuiWindowFlags_NoScrollWithMouse;
        }

        if (opts.noPadding) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        }
        const bool contentVisible = ImGui::Begin(panel.id(), &open, flags);
        if (opts.noPadding) {
            ImGui::PopStyleVar();  // pop right after Begin: applies to the window frame, not the
                                   // panel's widgets
        }
        if (contentVisible) {
            panel.onDraw(context);
        }
        ImGui::End();                  // ALWAYS — Begin's return value never gates it
        panels.setVisibleAt(i, open);  // the close-'X' wrote through `open` (E2)
    }
}

// C4: the DockSlot enumerator count, kept in lockstep with panel.hpp by the static_assert below —
// this is what keeps a future 5th slot from silently writing out of bounds into `used`.
constexpr std::size_t DOCK_SLOT_COUNT = 4;
static_assert(static_cast<std::size_t>(DockSlot::Bottom) + 1U == DOCK_SLOT_COUNT,
              "DockSlot gained an enumerator — widen DOCK_SLOT_COUNT and add its split below");

constexpr std::size_t slotIndex(DockSlot slot) noexcept { return static_cast<std::size_t>(slot); }

// D3 + F10: the default/reset dock layout. Only splits a slot at least one registered panel asks
// for — an empty ImGui dock node is not pruned, it renders as a dead grey rectangle.
void buildDefaultLayout(ImGuiID dockId, PanelRegistry& panels) {
    std::array<bool, DOCK_SLOT_COUNT> used{};  // value-initialised: all false
    for (std::size_t i = 0; i < panels.count(); ++i) {
        used[slotIndex(panels.panelAt(i).defaultDockSlot())] = true;
    }

    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);  // F9/F11

    ImGuiID center = dockId;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    if (used[slotIndex(DockSlot::Left)]) {
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20F, nullptr, &center);
    }
    if (used[slotIndex(DockSlot::Right)]) {
        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25F, nullptr, &center);
    }
    if (used[slotIndex(DockSlot::Bottom)]) {
        bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25F, nullptr, &center);
    }

    for (std::size_t i = 0; i < panels.count(); ++i) {
        const Panel& panel = panels.panelAt(i);
        ImGuiID node = center;
        switch (panel.defaultDockSlot()) {
            case DockSlot::Left:
                node = left;
                break;
            case DockSlot::Right:
                node = right;
                break;
            case DockSlot::Bottom:
                node = bottom;
                break;
            case DockSlot::Center:
                node = center;
                break;
        }
        ImGui::DockBuilderDockWindow(panel.id(), node);  // by NAME — works for never-submitted and
                                                         // currently-hidden panels alike (F11/E13)
    }
    ImGui::DockBuilderFinish(dockId);
}

}  // namespace

void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state) {
    // Task 2.3.3 (D17): FIRST, before anything else submits a window. This call is MANDATORY, not
    // optional: gContext.mbOverGizmoHotspot is reset in exactly ONE place in the whole library
    // (ImGuizmo.cpp:1016, inside BeginFrame), and every operation handler does
    // `type = mbOverGizmoHotspot ? MT_NONE : Get*Type(...)` followed by `mbOverGizmoHotspot |= ...`
    // (ImGuizmo.cpp:2258, :2308, :2431). Skip it and the flag latches true the first time the cursor
    // crosses a handle, after which the gizmo DRAWS but can never be GRABBED again -- a latching,
    // one-way failure no smoke test can see (human validation row 6).
    //
    // It submits a fully transparent, NoInputs, NoBringToFrontOnFocus, NoSavedSettings full-screen
    // window named "gizmo". NoSavedSettings is what keeps it out of aero_editor.ini (AC-15), and
    // NoInputs (=> NoMouseInputs) is what keeps it out of g.HoveredWindow, which is what makes
    // ImGuizmo's own IsHoveringWindow() resolve to "Viewport" (F6). It is HERE rather than in
    // editor_app.cpp (ImGui-FREE by design, 2.1.3 D1) or imgui_layer.cpp (backend lifetime,
    // byte-identical for eight tasks) because THIS is the ImGui frame-composition TU.
    ImGuizmo::BeginFrame();

    drawMenuBar(panels, context, state);   // reserves the viewport work area (F9) and is where Reset
                                           // Layout can still affect THIS frame -- the first REAL
                                           // window, after the transparent, NoInputs "gizmo" window
    applyHistoryRequests(context, state);  // task 2.4.1, D19/AC-21
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (state.applyDefaultLayout) {
        state.applyDefaultLayout = false;
        buildDefaultLayout(dockId, panels);  // must run AFTER the dockspace exists this frame (E17)
    }
    drawPanels(panels, context);
}

}  // namespace engine::editor
