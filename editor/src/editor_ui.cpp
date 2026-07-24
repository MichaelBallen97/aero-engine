// Aero Engine — the full-window dockspace + throwaway placeholder panels (task 2.1.1). ImGui lives
// only here; editor_ui.hpp exposes engine/std types only. No menu bar (task 2.1.3); no real panel
// content (task 2.2.x) — these panels exist only to prove dockable + persisted + HiDPI-correct.
#include <aero/editor/editor_ui.hpp>

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder* — first-run default layout only (D8)

namespace engine::editor {

namespace {

// Builds the first-run default split: left=Hierarchy, right=Inspector, bottom=Console,
// center=Viewport. Named after the real 2.2.x panels so the default layout is the editor skeleton.
void buildDefaultLayout(ImGuiID dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20F, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25F, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25F, nullptr, &center);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockId);
}

}  // namespace

void drawEditorUi(EditorUiState& state) {
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (state.applyDefaultLayout) {
        state.applyDefaultLayout = false;
        buildDefaultLayout(dockId);
    }

    if (state.showHierarchy && ImGui::Begin("Hierarchy", &state.showHierarchy)) {
        ImGui::TextUnformatted("Hierarchy — placeholder (task 2.2.1)");
    }
    ImGui::End();

    if (state.showInspector && ImGui::Begin("Inspector", &state.showInspector)) {
        ImGui::TextUnformatted("Inspector — placeholder (task 2.2.2)");
    }
    ImGui::End();

    if (state.showViewport && ImGui::Begin("Viewport", &state.showViewport)) {
        ImGui::TextUnformatted("Viewport — placeholder (task 2.2.3)");
    }
    ImGui::End();

    if (state.showConsole && ImGui::Begin("Console", &state.showConsole)) {
        ImGui::TextUnformatted("Console — placeholder (task 2.2.5)");
    }
    ImGui::End();
}

}  // namespace engine::editor
