// Aero Engine — the ONE new ImGui TU this task adds besides placeholder_panel.cpp (task 2.1.3, D9):
// menu bar, dockspace, the data-driven default layout, and the balanced Begin/End around every
// Panel::onDraw(). editor_app.cpp includes this header but never ImGui itself.
#include "shell_ui.hpp"

#include <aero/editor/command_stack.hpp>
#include <aero/editor/panel.hpp>
#include <aero/editor/scene_session.hpp>

#include "project_ui.hpp"  // task 2.6.1: drawWelcomeWindow / drawNewProjectModal

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

// SCREAMING_SNAKE and file-scope, NOT a `constexpr` local named historyFlags: .clang-tidy's
// readability-identifier-naming.ConstexprVariableCase is UPPER_CASE and applies to LOCALS too, so the
// camelBack spelling fails --warnings-as-errors on the Linux lane (the same class of trap as 2.3.3's
// `const bool using_`, which failed VariableCase: camelBack). File scope because both chords share it.
constexpr ImGuiInputFlags HISTORY_SHORTCUT_FLAGS = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_Repeat;

// The File menu's own shortcut flags. RouteGlobal, exactly like Ctrl+Q/Ctrl+Z above -- a focused
// InputText must be able to win them back. NO Repeat, UNLIKE the history chords (AC-3): holding
// Ctrl+S must write ONCE, not once per frame. ImGuiMod_Ctrl already maps to Cmd on macOS
// automatically (F8) -- no platform-conditional compilation appears anywhere in this file.
constexpr ImGuiInputFlags FILE_SHORTCUT_FLAGS = ImGuiInputFlags_RouteGlobal;

// D18/AC-6. Called after EACH of the three tools-gated items -- a single trailing call would only
// ever tooltip the LAST one, because IsItemHovered answers about the PREVIOUS item only. That is the
// shape the now-deleted disabled-stub helper already got right, and the same trap
// viewport_panel.cpp:539 records.
void ioTooltip(bool available) {
    if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Scene I/O needs AERO_REFLECT_TOOLS");
    }
}

void drawMenuBar(PanelRegistry& panels, PanelContext& context, ShellUiState& state, FileMenuContext& fileMenu) {
    // Editor shortcuts go through ImGui's routing, NEVER ctx.input() (D7/E9): a focused InputText
    // must be able to swallow the chord. ImGuiMod_Ctrl is Cmd on macOS automatically (F8).
    //
    // D8/AC-5, widened by BLOCKING-2 (the 2.5.1 code-review round) and again by task 2.6.1:
    // `fileEnabled` gates every File chord AND every File menu item below -- while a native dialog is
    // in flight, the unsaved-changes modal is up, OR the New Project modal is up, the whole menu
    // behaves as disabled at the input layer too, not only visually. Before BLOCKING-2 widened this,
    // the modal being up alone did not stop a chord (e.g. Ctrl+S) from reaching AskWhereToSave and
    // launching a SECOND, native dialog on top of the still-open modal -- `scene_session.cpp`'s
    // `applyFileRequests` mirrors this exact same widening for the request path itself (defence in
    // depth: the chords stay quiet AND the state machine refuses the request even if something else
    // got past this check, e.g. a raw `request*()` call). The 2.5.1 code-review round's BLOCKING-2 is
    // the standing evidence of what a disagreement between the two definitions costs.
    const bool fileEnabled =
        fileMenu.flow.dialog == DialogKind::None && !fileMenu.flow.confirmOpen && !fileMenu.project.flow.form.open;
    if (fileEnabled && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, FILE_SHORTCUT_FLAGS)) {
        fileMenu.flow.requested = FileAction::Quit;  // D1: the GUARDED quit
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

    // The three I/O chords additionally require sceneIoAvailable() (D18/AC-6) -- New Scene does not,
    // because it needs no serialization at all.
    const bool io = fileEnabled && sceneIoAvailable();
    if (fileEnabled && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, FILE_SHORTCUT_FLAGS)) {
        fileMenu.flow.requested = FileAction::NewScene;
    }
    if (io && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, FILE_SHORTCUT_FLAGS)) {
        fileMenu.flow.requested = FileAction::OpenScene;
    }
    if (io && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, FILE_SHORTCUT_FLAGS)) {
        fileMenu.flow.requested = FileAction::SaveScene;
    }
    if (io && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, FILE_SHORTCUT_FLAGS)) {
        fileMenu.flow.requested = FileAction::SaveSceneAs;
    }

    if (!ImGui::BeginMainMenuBar()) {
        return;  // F7: End only when Begin returned true
    }
    if (ImGui::BeginMenu("File")) {
        // task 2.6.1: New/Open Project, Open Recent, then a separator, before the existing scene
        // items. NONE of the four is ever disabled for sceneIoAvailable() -- the project flow is
        // gate-free (D4), so ioTooltip is not called for any of them.
        if (ImGui::MenuItem("New Project...", nullptr, false, fileEnabled)) {
            fileMenu.flow.requested = FileAction::NewProject;
        }
        if (ImGui::MenuItem("Open Project...", nullptr, false, fileEnabled)) {
            fileMenu.flow.requested = FileAction::OpenProject;
        }
        // BeginMenu obeys the OPPOSITE balance rule from Begin: EndMenu() ONLY when BeginMenu()
        // returned true (.claude/rules/editor.md). An unbalanced call is an IM_ASSERT ABORT in Debug,
        // not a glitch.
        const bool recentsEnabled = fileEnabled && !fileMenu.project.recents.paths.empty();
        if (ImGui::BeginMenu("Open Recent", recentsEnabled)) {
            for (std::size_t i = 0; i < fileMenu.project.recents.paths.size(); ++i) {
                const std::string& path = fileMenu.project.recents.paths[i];
                ImGui::PushID(static_cast<int>(i));  // two entries whose LABELS collide would
                                                     // otherwise MERGE into one item -- ImGui derives
                                                     // the ID from the label. MenuItem's `label` is
                                                     // NOT a format string, so a '%' in a path is
                                                     // safe HERE (unlike ImGui::Text).
                if (ImGui::MenuItem(path.c_str())) {
                    fileMenu.project.flow.requestedPath = path;
                    fileMenu.flow.requested = FileAction::OpenProject;
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent Projects")) {
                fileMenu.project.flow.clearRecentsRequested = true;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, fileEnabled)) {
            fileMenu.flow.requested = FileAction::NewScene;
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, io)) {
            fileMenu.flow.requested = FileAction::OpenScene;
        }
        // D18/AC-6: the tooltip names ONLY the AERO_REFLECT_TOOLS reason (finding 8 of the 2.5.1
        // code-review round -- `io` also folds in `fileEnabled`, so with reflect tools ON and a dialog
        // in flight or the modal up, `ioTooltip(io)` would falsely claim "Scene I/O needs
        // AERO_REFLECT_TOOLS". `sceneIoAvailable()` alone is the ONLY thing this tooltip is about, at
        // all three call sites below.
        ioTooltip(sceneIoAvailable());
        // AC-4: nothing to write when the document is clean AND has a path.
        const bool canSave = io && (!context.commands.isClean() || fileMenu.session.untitled());
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave)) {
            fileMenu.flow.requested = FileAction::SaveScene;
        }
        ioTooltip(sceneIoAvailable());
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, io)) {
            fileMenu.flow.requested = FileAction::SaveSceneAs;
        }
        ioTooltip(sceneIoAvailable());
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Ctrl+Q", false, fileEnabled)) {
            fileMenu.flow.requested = FileAction::Quit;  // D1: the GUARDED quit
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
// binds nothing. The live bindings are the seven explicit chord registrations at the top of
// drawMenuBar (Ctrl+Q, Ctrl+Z, Ctrl+Shift+Z, Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S) -- every File menu
// item and chord is now LIVE; no disabled stub remains anywhere in the editor (G5).

// The unsaved-changes confirmation modal. A file-local static, drawn in the SAME slot as
// applyHistoryRequests below -- after drawMenuBar returned (EndMainMenuBar has run, so the ID stack
// is clean and OpenPopup is legal, F13) and before the dockspace/panel walk.
constexpr const char* UNSAVED_MODAL_ID = "Unsaved Changes###aero_unsaved";
// The ### form makes the ID stable even though the visible label never changes -- ImHashStr restarts
// the hash at "###" (imgui.cpp:2539-2545), so IsPopupOpen and BeginPopupModal agree by construction.

void drawUnsavedChangesModal(FileMenuContext& fileMenu) {
    if (fileMenu.flow.confirmOpen && !ImGui::IsPopupOpen(UNSAVED_MODAL_ID)) {
        ImGui::OpenPopup(UNSAVED_MODAL_ID);
    }
    if (!fileMenu.flow.confirmOpen) {
        return;
    }
    // F13: EndPopup ONLY when BeginPopupModal returned true -- the BeginMenu family, not the Begin one.
    if (ImGui::BeginPopupModal(UNSAVED_MODAL_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string name = fileMenu.session.documentName();  // built only while the modal is up
        ImGui::Text("Save changes to \"%s\"?", name.c_str());      // %s, NEVER name.c_str() bare -- a
                                                                   // document name containing '%'
                                                                   // would otherwise be a format bug
        ImGui::TextDisabled("Your changes will be lost if you don't save them.");
        ImGui::Separator();
        if (ImGui::Button("Save")) {
            fileMenu.flow.choice = ConfirmChoice::Save;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();  // Enter == Save
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            fileMenu.flow.choice = ConfirmChoice::Discard;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            fileMenu.flow.choice = ConfirmChoice::Cancel;
            ImGui::CloseCurrentPopup();
        }
        // ImGui CANNOT dismiss a MODAL with Escape: NavUpdateCancelRequest's popup branch excludes
        // ImGuiWindowFlags_Modal (imgui.cpp:15032) and BeginPopupModal always sets it (imgui.cpp:13232)
        // -- and the editor does not enable ImGuiConfigFlags_NavEnableKeyboard at all
        // (imgui_layer.cpp:79), so that path is doubly dead. Esc is the universal DISMISS key
        // (.claude/rules/editor.md), so we bind it OURSELVES, HERE, inside the body -- repeat=false,
        // one press = one Cancel. Deliberately NOT the global-route chord mechanism used above: the
        // editor-chord rule exists so a focused InputText can win a chord back, but a modal already
        // blocks every other window, and a global Escape route would also fire on the frames the
        // modal is NOT up.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            fileMenu.flow.choice = ConfirmChoice::Cancel;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        // A SAFETY NET, not the Esc mechanism above: in 1.92.8 the only thing that can reach here is a
        // PROGRAMMATIC close, because a modal also swallows outside clicks. Treating it as Cancel keeps
        // the flow from wedging with confirmOpen stuck true.
        fileMenu.flow.choice = ConfirmChoice::Cancel;
    }
}

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
    CommandContext cmd = toCommandContext(context);
    if (state.undoRequested) {
        state.undoRequested = false;
        context.commands.undo(cmd);
    }
    if (state.redoRequested) {
        state.redoRequested = false;
        context.commands.redo(cmd);
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

void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state, FileMenuContext& fileMenu) {
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

    drawMenuBar(panels, context, state, fileMenu);  // reserves the viewport work area (F9) and is
                                                    // where Reset Layout can still affect THIS frame
                                                    // -- the first REAL window, after the
                                                    // transparent, NoInputs "gizmo" window
    drawUnsavedChangesModal(fileMenu);              // may set fileMenu.flow.choice
    drawNewProjectModal(fileMenu);                  // the SAME slot as the unsaved-changes modal: drawMenuBar has
                                                    // returned, EndMainMenuBar has run, the ID stack is clean and
                                                    // OpenPopup is legal here (2.5.1's F13)
    {
        // applyFileRequests runs BEFORE applyHistoryRequests, on purpose (plan A32/E22): if one frame
        // carries both a scene swap and an undo request, the undo must be evaluated against the stack
        // that exists AFTER the swap -- which, being freshly cleared, has nothing to undo and takes
        // the ordinary "nothing to undo" path rather than driving a command against the wrong World.
        // toCommandContext's return is a PRVALUE and push()/undo()/redo() take CommandContext&, so the
        // named local `cmd` is mandatory (2.4.2 §A10's rule).
        CommandContext cmd = toCommandContext(context);
        applyFileRequests(cmd, context.commands, fileMenu.session, fileMenu.flow, fileMenu.dialogs, fileMenu.project);
    }
    applyHistoryRequests(context, state);  // task 2.4.1, D19/AC-21
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (state.applyDefaultLayout) {
        state.applyDefaultLayout = false;
        buildDefaultLayout(dockId, panels);  // must run AFTER the dockspace exists this frame (E17)
    }
    drawPanels(panels, context);
    drawWelcomeWindow(fileMenu);  // AFTER the dockspace and the panels, so it FLOATS above them
}

}  // namespace engine::editor
