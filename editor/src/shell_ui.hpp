#pragma once
// Aero Engine — src-private (F14/D9): the shell UI's state struct + entry point. ImGui appears only
// in shell_ui.cpp; this header stays ImGui-free so editor_app.cpp (which includes it) never needs to
// see ImGui either.
#include <aero/editor/panel_registry.hpp>

namespace engine::editor {

struct ShellUiState {
    bool applyDefaultLayout = false;  // in: build the default layout this frame; out: cleared once
                                      // consumed, or re-set to true by View > Reset Layout
    bool quitRequested = false;       // out: File > Exit or Ctrl/Cmd+Q fired
    // Task 2.4.1. IN: seeded by EditorApp::requestUndo()/requestRedo() (D11); also set INSIDE
    // drawShellUi by Ctrl+Z / Ctrl+Shift+Z and by the Edit menu items. Cleared the moment they are
    // applied, in the SAME drawShellUi call (D19) -- unlike applyDefaultLayout these are NEVER read
    // back by tick(), because reading them back would re-arm the request every frame.
    bool undoRequested = false;
    bool redoRequested = false;
};

// Draws the menu bar, the full-viewport dockspace, and every visible panel (Begin/End around
// onDraw()). `context` is forwarded to every panel unchanged (task 2.2.1). See editor_app.cpp's
// tick() for the frame this runs inside.
void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state);

}  // namespace engine::editor
