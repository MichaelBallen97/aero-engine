// Aero Engine — the Material panel's ONE ImGui TU (task 3.4.2). Draws the reconciled material edit
// session and WRITES NOTHING (INV-3): EditorApp::tick() is the only place a request becomes a file
// write or a session mutation, exactly the "record a pending action, apply it after the walk" rule
// every panel in this tree follows.
//
// A dynamic string is NEVER a format argument (project_settings_panel.cpp's own rule, applied here a
// third time): every draw call goes through a named local built with std::format, then passed as a
// "%s" argument.
//
// ASCII ONLY in every literal (3.1.3's post-merge lesson): the editor loads no font of its own, and
// ImGui's ProggyClean covers Basic + Extended Latin only.
#include "material_panel.hpp"

#include <aero/editor/panel_context.hpp>

#include <format>
#include <imgui.h>
#include <string>

namespace engine::editor {

namespace {

constexpr ImVec4 WARNING_COLOR{1.0F, 0.4F, 0.4F, 1.0F};  // project_ui.cpp's own error-text colour
constexpr ImVec4 NOTICE_COLOR{1.0F, 0.8F, 0.4F, 1.0F};   // a warm amber for the non-fatal notices

}  // namespace

void MaterialPanel::onDraw(PanelContext& /*context*/) {  // no World/Selection/Project read (the
                                                         // ImportDetailsPanel "context is ignored"
                                                         // precedent)
    if (sessionPtr == nullptr) {
        // The very first frame of a session's life: EditorApp::tick() reconciles BEFORE drawShellUi,
        // so in practice this is reached only with no panel registration at all -- but a null pointer
        // is always checked here rather than assumed away.
        ImGui::TextDisabled("Select a material in the Assets panel.");
        return;
    }
    switch (sessionPtr->state()) {
        case MaterialSessionState::Untargeted:
            ImGui::TextDisabled("Select a material in the Assets panel.");
            return;
        case MaterialSessionState::Error: {
            labelScratch = std::string(sessionPtr->targetPath());
            ImGui::TextUnformatted(labelScratch.c_str());
            const MaterialError* error = sessionPtr->error();
            if (error != nullptr) {
                // line > 0 <=> the failure happened at the JSON stage and carries a position;
                // material-stage failures put their context (the key path) in the message instead.
                labelScratch = error->line > 0 ? std::format("{} ({}:{})", error->message, error->line, error->column)
                                               : error->message;
                ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
                ImGui::TextWrapped("%s", labelScratch.c_str());
                ImGui::PopStyleColor();
            }
            // Nothing editable, and no Apply or Revert drawn at all (AC-9): the file may hold a
            // hand-recoverable value one `git checkout` away, and this editor never "repairs" one.
            ImGui::TextDisabled("This file cannot be edited until it parses.");
            return;
        }
        case MaterialSessionState::Ready:
            break;
    }
    // The status strip. The editable rows and the preview arrive with the panel's own UI step; what is
    // here is what the reconcile can already prove, and it is what makes a targeted session visible.
    labelScratch = std::string(sessionPtr->targetPath());
    if (sessionPtr->dirty()) {
        labelScratch += " *";
    }
    ImGui::TextUnformatted(labelScratch.c_str());
    if (sessionPtr->externalChangeNoticed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", "This file changed on disk; Apply will overwrite it.");
        ImGui::PopStyleColor();
    }
    for (const std::string& warning : sessionPtr->warnings()) {
        labelScratch = warning;
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", labelScratch.c_str());
        ImGui::PopStyleColor();
    }
    if (!sessionPtr->lastMessage().empty()) {
        labelScratch = std::string(sessionPtr->lastMessage());
        ImGui::TextDisabled("%s", labelScratch.c_str());
    }
}

}  // namespace engine::editor
