// Aero Engine — the read-only Project Settings panel's ONE ImGui TU (task 2.6.2). It includes no
// <filesystem>, performs no I/O, logs nothing and mutates nothing: every value arrives through
// PanelContext::project, which is CONST (D1/INV-S4).
//
// FOUR ImGui rules this file lives or dies by:
//   * BeginTable/EndTable are ASYMMETRIC -- EndTable() ONLY when BeginTable() returned true
//     (imgui.h:913). An unbalanced call is an IM_ASSERT ABORT in the Debug build.
//   * PushID/PopID are 1:1, and there is NO continue, break or return between the pair below. Two
//     tables sharing one literal id inside one window would MERGE.
//   * A dynamic string is NEVER a format argument -- every draw goes through the three helpers, and
//     each of them passes the value as a "%s" argument. `100% Cotton` is a legal project name.
//   * SeparatorText takes a plain LABEL, not a format string (no IM_FMTARGS) -- the MenuItem
//     exception. Do not "fix" it; there is no such overload.
#include "project_settings_panel.hpp"

#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_settings.hpp>

#include <algorithm>
#include <cstddef>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// SizingFixedFit + Resizable so a user can widen the label column; NoSavedSettings so two tables in a
// panel with no user-visible column story never accumulate entries in the ini. NO ScrollY -- the
// panel's own window scrolls, and ScrollY with a default outer_size silently kills scrolling.
constexpr ImGuiTableFlags TABLE_FLAGS =
    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;

// Names BOTH ways out, and matches the File menu's own labels byte-for-byte (three ASCII dots).
constexpr const char* NO_PROJECT_TEXT = "No project is open. Use File > New Project... or File > Open Project...";

void drawValue(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

void drawLabel(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// There is no TextDisabledWrapped, so the wrap position is pushed by hand: 0.0f means "wrap at
// WorkRect.Max.x", which outside a table is the window's content edge.
void drawEmptyState() {
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextDisabled("%s", NO_PROJECT_TEXT);
    ImGui::PopTextWrapPos();
}

// ONE pass over every row of every group, so BOTH tables get the SAME column-0 width and the two
// groups align as if they were one table (D10). Computed from the LIVE font, so it is correct on a
// HiDPI display and after a font-scale change -- a hardcoded font multiple would not be.
[[nodiscard]] float widestLabel(const std::vector<ProjectSettingsGroup>& groups) {
    float widest = 0.0F;
    for (const ProjectSettingsGroup& group : groups) {
        for (const ProjectSettingsRow& row : group.rows) {
            widest = std::max(widest, ImGui::CalcTextSize(row.label.c_str()).x);
        }
    }
    return widest + (ImGui::GetStyle().CellPadding.x * 2.0F);
}

}  // namespace

ProjectSettingsPanel::ProjectSettingsPanel(std::string buildEngineVersion)
    : buildVersion(std::move(buildEngineVersion)) {}

void ProjectSettingsPanel::onDraw(PanelContext& context) {
    // Phase 1: ONE pure call. Rebuilt every frame from scratch -- there is no cache to invalidate (E2).
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(context.project, buildVersion);

    // Phase 2: draw. There is no apply step, because this panel records no action and mutates nothing.
    if (groups.empty()) {
        drawEmptyState();
        return;
    }
    const float labelWidth = widestLabel(groups);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const ProjectSettingsGroup& group = groups[g];
        ImGui::SeparatorText(group.title.c_str());  // OUTSIDE the table (F2/D10)
        ImGui::PushID(static_cast<int>(g));
        if (ImGui::BeginTable("##rows", 2, TABLE_FLAGS)) {  // F9: End ONLY if true
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            for (const ProjectSettingsRow& row : group.rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                drawLabel(row.label);
                ImGui::TableNextColumn();
                drawValue(row.value);  // wraps at the CELL edge (F3)
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

}  // namespace engine::editor
