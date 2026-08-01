#pragma once
// Aero Engine — src-private: the Welcome window and the New Project modal (task 2.6.1). This HEADER
// is ImGui-free (shell_ui.cpp calls both functions and never needs to see ImGui through this header);
// every ImGui call lives in project_ui.cpp. Its own TU because shell_ui.cpp is already 394 lines and
// would pass 550.
#include "shell_ui.hpp"  // FileMenuContext

namespace engine::editor {

// Shown IFF no project is open (AC-29); not dockable, not saved to imgui.ini. Drawn AFTER the
// dockspace and the panels, so it floats above them.
void drawWelcomeWindow(FileMenuContext& fileMenu);

// Opened when `fileMenu.project.flow.form.open` first goes true. Drawn in the same post-menu-bar slot
// as the unsaved-changes modal.
void drawNewProjectModal(FileMenuContext& fileMenu);

}  // namespace engine::editor
