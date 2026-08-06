#pragma once
// Aero Engine — src-private (F14/D9): the shell UI's state struct + entry point. ImGui appears only
// in shell_ui.cpp; this header stays ImGui-free so editor_app.cpp (which includes it) never needs to
// see ImGui either.
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/scene_session.hpp>  // task 2.5.1: FileMenuContext names FileFlow/FileDialogHost

#include <string>  // code-review BLOCKING-1: ShellUiState::focusPanelId

namespace engine::editor {

struct ShellUiState {
    bool applyDefaultLayout = false;  // in: build the default layout this frame; out: cleared once
                                      // consumed, or re-set to true by View > Reset Layout
    // In: this frame, dock any panel the restored imgui.ini has never heard of. Out: cleared once
    // consumed, and NEVER re-armed -- unlike applyDefaultLayout there is no menu item that re-sets it.
    // Mutually exclusive with applyDefaultLayout by construction (EditorApp seeds one as the other's
    // negation): a default layout places every panel already, so there is nothing left unplaced.
    bool placeUnplacedPanels = false;
    // `quitRequested` is DELETED (task 2.5.1, plan A10/A15): after D1, both Ctrl+Q and File > Exit
    // are FileAction::Quit, routed through FileMenuContext's flow -- nothing writes this field any
    // more, and a field nobody writes is a trap. The confirmed OUTPUT of a GUARDED quit is
    // FileFlow::quitConfirmed.
    // Task 2.4.1. IN: seeded by EditorApp::requestUndo()/requestRedo() (D11); also set INSIDE
    // drawShellUi by Ctrl+Z / Ctrl+Shift+Z and by the Edit menu items. Cleared the moment they are
    // applied, in the SAME drawShellUi call (D19) -- unlike applyDefaultLayout these are NEVER read
    // back by tick(), because reading them back would re-arm the request every frame.
    bool undoRequested = false;
    bool redoRequested = false;
    // code-review BLOCKING-1 test seam (task 3.1.3): IN, consumed by drawShellUi -- selects (and
    // shows) the panel named here as the active TAB in its dock node, the "Edit > Project
    // Settings..." click's OWN two calls (panels.setVisible + ImGui::SetWindowFocus) applied
    // generically instead of to one hardcoded id. Needed because the ImGui-free-at-source GPU tier
    // cannot click a tab, and the Asset Browser shares its dock slot with Console (D3) -- ImGui keeps
    // whichever tab won the FIRST frame active forever afterward, with no further per-frame signal,
    // so a test that needs the SAME panel drawn on a LATER tick has no other way to ask for it. "" (the
    // default) does nothing. Cleared once consumed, never re-armed -- the identical
    // undoRequested/redoRequested posture above.
    std::string focusPanelId;
};

// task 2.5.1 (plan A14): everything the File menu needs that PanelContext deliberately does NOT
// carry (D17). Built fresh each frame in tick(), exactly like PanelContext, and for the same reason.
struct FileMenuContext {
    SceneSession& session;
    FileFlow& flow;
    FileDialogHost dialogs;  // BY VALUE: two pointers and a string_view
    // task 2.6.1: a REFERENCE, not a value -- ProjectContext already holds references, and copying
    // it per frame would be a second object with the same referents.
    ProjectContext& project;
};

// Draws the menu bar, the full-viewport dockspace, and every visible panel (Begin/End around
// onDraw()). `context` is forwarded to every panel unchanged (task 2.2.1). See editor_app.cpp's
// tick() for the frame this runs inside. `fileMenu` is task 2.5.1's fourth parameter (plan A14).
void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state, FileMenuContext& fileMenu);

}  // namespace engine::editor
