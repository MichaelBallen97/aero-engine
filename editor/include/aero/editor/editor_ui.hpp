#pragma once
// Aero Engine — editor dockspace + throwaway placeholder panels (task 2.1.1). ImGui lives entirely
// in editor_ui.cpp; this header exposes only the tiny state struct the caller owns. No menu bar
// here (task 2.1.3); no real panel content (task 2.2.x) — these four panels exist only to prove
// dockable + persisted + HiDPI-correct.

namespace engine::editor {

struct EditorUiState {
    bool showHierarchy = true;
    bool showInspector = true;
    bool showViewport = true;
    bool showConsole = true;
    // Set true (by the caller, from ImGuiLayer::wantsDefaultLayout()) to build the first-run
    // DockBuilder default split on the next call; drawEditorUi resets it to false after building.
    bool applyDefaultLayout = false;
};

// Hosts a full-window dockspace and draws the placeholder panels (Hierarchy/Inspector/Viewport/
// Console). Builds the first-run default split exactly once, when state.applyDefaultLayout is true.
void drawEditorUi(EditorUiState& state);

}  // namespace engine::editor
