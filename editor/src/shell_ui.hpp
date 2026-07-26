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
};

// Draws the menu bar, the full-viewport dockspace, and every visible panel (Begin/End around
// onDraw()). `context` is forwarded to every panel unchanged (task 2.2.1). See editor_app.cpp's
// tick() for the frame this runs inside.
void drawShellUi(PanelRegistry& panels, PanelContext& context, ShellUiState& state);

}  // namespace engine::editor
