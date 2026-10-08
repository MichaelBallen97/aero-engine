#pragma once
// Aero Engine — the shell's chrome: the toolbar, the status bar and the breadcrumb (task E.6.2, D2). SRC-PRIVATE
// and ImGui-FREE: shell_ui.cpp calls these and the GPU tier names nothing here. Every ImGui call of the three
// strips is in shell_chrome_ui.cpp; every decision they draw is a pure function in a public header
// (toolbar_model.hpp, breadcrumb.hpp, status_bar.hpp), tested at tier 0.
#include "shell_ui.hpp"

namespace engine::editor {

class CommandStack;
class ProjectSession;
class SceneSession;
struct ShellChromeRecord;

// The toolbar: an ImGuiDir_Up viewport side bar under the menu bar. Writes *state.tools (a click or an obeyed
// request), state.undoRequested (applied by the ONE applyHistoryRequests later in the same drawShellUi),
// state.snapCommitted, and *state.chromeRecord. `fileEnabled` is drawShellUi's hoisted value -- this TU never calls
// modalInputActive itself. Called AFTER drawMenuBar (Up bars stack in submission order) and BEFORE
// applyHistoryRequests and drawPanels, so an Undo click applies and a tool click reaches updateGizmo this frame.
void drawToolbar(ShellUiState& state, const CommandStack& commands, bool fileEnabled);

// The status bar: an ImGuiDir_Down viewport side bar, drawing *state.statusText through statusBarLayout.
void drawStatusBar(ShellUiState& state);

// The breadcrumb, INSIDE the main menu bar: called by drawMenuBar after the View menu's EndMenu and before
// EndMainMenuBar, so it never runs when BeginMainMenuBar returned false. `record` may be null.
void drawBreadcrumb(const ProjectSession& project, const SceneSession& session, const CommandStack& commands,
                    ShellChromeRecord* record);

}  // namespace engine::editor
