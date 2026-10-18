// Aero Engine — the read-only Project Settings panel's ONE ImGui TU (task 2.6.2). It includes no
// <filesystem>, performs no I/O, logs nothing and mutates nothing: every value arrives through
// PanelContext::project, which is CONST (D1/INV-S4).
//
// THREE ImGui rules this file lives or dies by:
//   * BeginTable/EndTable are ASYMMETRIC -- EndTable() ONLY when BeginTable() returned true
//     (imgui.h:913). An unbalanced call is an IM_ASSERT ABORT in the Debug build.
//   * PushID/PopID are 1:1, and there is NO continue, break or return between the pair below. Two
//     tables sharing one literal id inside one window would MERGE.
//   * A dynamic string is NEVER a format argument -- every draw goes through the three helpers: drawLabel and
//     drawValue pass the value as a "%s" argument, and drawEmptyState goes through mutedTextWrapped, which draws it
//     unformatted. `100% Cotton` is a legal project name.
#include "project_settings_panel.hpp"

#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_settings.hpp>

#include "panel_chrome_ui.hpp"  // task E.6.3: cards, roles

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
    ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextLabel));  // task E.6.3 (D10): labels textLabel
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void drawEmptyState() { mutedTextWrapped(NO_PROJECT_TEXT); }  // task E.6.3 (D11): wraps at the content edge

// ONE pass over every row of every group, so BOTH tables get the SAME column-0 width and the two
// groups align as if they were one table (D10). Computed from the LIVE font rather than a hardcoded
// font multiple, so the width is right for whatever font and DPI are in effect when a table first
// lays itself out.
//
// WHAT THIS DOES *NOT* DO, verified in the pinned 1.92.8 source rather than assumed: it does not
// re-widen the column when the font scale changes at runtime. TableSetupColumn's init width reaches
// column->WidthRequest only via TableInitColumnDefaults, which is called under `if
// (table->IsInitializing)` (imgui_tables.cpp:1693-1698), and IsInitializing is true only on the
// frame a table id first allocates (`RawData == NULL`, :574-577). The one per-frame path that would
// re-apply it, `column->WidthAuto = column->InitStretchWeightOrWidth` (:937-938), is gated on
// `!column_is_resizable` -- and TABLE_FLAGS sets Resizable deliberately, so that path is excluded
// here. The recompute therefore still costs nothing worth saving (eight CalcTextSize calls) and
// still covers a table that re-initializes, but a live font-scale change leaves the column where it
// was: labels WRAP rather than clip, and the user can drag the divider. Do not describe this as
// mitigating a runtime font-scale change.
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
    : buildVersion(std::move(buildEngineVersion)), cardPainter(std::make_unique<CardPainter>()) {}
ProjectSettingsPanel::~ProjectSettingsPanel() = default;

void ProjectSettingsPanel::onDraw(PanelContext& context) {
    beginCardRecordFrame(cardRecordValue);  // task E.6.3 (D14)
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
        ImGui::PushID(static_cast<int>(g));
        {
            // task E.6.3 (D10): each group a read-only CARD -- no overflow, no buttons; ImGui-owned open state.
            const PanelCard card(*cardPainter, "##group",
                                 CardSpec{.title = group.title, .icon = projectSettingsGroupIcon(g)}, nullptr,
                                 &cardRecordValue);
            if (card.header().open && ImGui::BeginTable("##rows", 2, TABLE_FLAGS)) {  // F9: End ONLY if true
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
                cardRecordValue.lastBodyTableMaxX = ImGui::GetItemRectMax().x;
            }
        }
        ImGui::PopID();
    }
}

}  // namespace engine::editor
