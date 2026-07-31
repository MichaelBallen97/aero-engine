#pragma once
// Aero Engine — src-private (F14/D9): the shell UI's state struct + entry point. ImGui appears only
// in shell_ui.cpp; this header stays ImGui-free so editor_app.cpp (which includes it) never needs to
// see ImGui either.
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/scene_session.hpp>  // task 2.5.1: FileMenuContext names FileFlow/FileDialogHost

namespace engine::editor {

struct ShellUiState {
    bool applyDefaultLayout = false;  // in: build the default layout this frame; out: cleared once
                                      // consumed, or re-set to true by View > Reset Layout
    // task 2.5.1 step 6/7 (plan A10/A15): still written by the OLD, unguarded Ctrl+Q / File > Exit
    // paths through Step 6's gate -- Step 7 reroutes both through FileAction::Quit (FileMenuContext's
    // flow) and DELETES this field outright, since nothing would write it any more and a field
    // nobody writes is a trap. The confirmed OUTPUT of a GUARDED quit is FileFlow::quitConfirmed.
    bool quitRequested = false;
    // Task 2.4.1. IN: seeded by EditorApp::requestUndo()/requestRedo() (D11); also set INSIDE
    // drawShellUi by Ctrl+Z / Ctrl+Shift+Z and by the Edit menu items. Cleared the moment they are
    // applied, in the SAME drawShellUi call (D19) -- unlike applyDefaultLayout these are NEVER read
    // back by tick(), because reading them back would re-arm the request every frame.
    bool undoRequested = false;
    bool redoRequested = false;
};

// task 2.5.1 (plan A14): everything the File menu needs that PanelContext deliberately does NOT
// carry (D17). Built fresh each frame in tick(), exactly like PanelContext, and for the same reason.
struct FileMenuContext {
    SceneSession& session;
    FileFlow& flow;
    FileDialogHost dialogs;  // BY VALUE: two pointers and a string_view
};

// Draws the menu bar, the full-viewport dockspace, and every visible panel (Begin/End around
// onDraw()). `context` is forwarded to every panel unchanged (task 2.2.1). See editor_app.cpp's
// tick() for the frame this runs inside. `fileMenu` is task 2.5.1's fourth parameter (plan A14).
void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state, FileMenuContext& fileMenu);

}  // namespace engine::editor
