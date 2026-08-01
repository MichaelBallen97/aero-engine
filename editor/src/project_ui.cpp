// Aero Engine — the Welcome window and the New Project modal (task 2.6.1). Its own TU because
// shell_ui.cpp is already 394 lines and would pass 550. Every control here sets exactly ONE request
// field and nothing else (AC-46) -- neither window ever mutates the World, the ProjectSession or the
// recents list directly; both route through FileAction / the form's *Requested fields, consumed
// OUTSIDE the draw walk in applyFileRequests. That is what gets the Welcome window the unsaved-
// changes guard for free and leaves this TU holding no second copy of any policy.
#include "project_ui.hpp"

#include <aero/editor/project.hpp>

#include "text_input.hpp"  // 2.2.2's inputTextString helper

#include <imgui.h>
#include <string>

namespace engine::editor {

namespace {

constexpr const char* WELCOME_WINDOW_ID = "Welcome to Aero Editor###aero_welcome";
// NoSavedSettings keeps it out of aero_editor.ini (AC-29) -- 2.3.3's AC-15 precedent for ImGuizmo's
// own window. NoDocking because it is not a panel and must never become one. Drawn IFF no project is
// open, so it needs no close button and no visibility state at all.
constexpr ImGuiWindowFlags WELCOME_FLAGS = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoFocusOnAppearing;

constexpr const char* NEW_PROJECT_MODAL_ID = "New Project###aero_new_project";
// The ### form makes the ID stable even though the visible label never changes -- ImHashStr restarts
// the hash at "###", so IsPopupOpen and BeginPopupModal agree by construction (the UNSAVED_MODAL_ID
// precedent).

}  // namespace

void drawWelcomeWindow(FileMenuContext& fileMenu) {
    if (fileMenu.project.session.isOpen()) {
        return;  // AC-29: shown IFF no project is open
    }
    // GetWorkCenter() (a METHOD in this pinned ImGui version, not a `WorkCenter` member -- measured
    // against the vendored imgui.h) == WorkPos + WorkSize * 0.5, i.e. centred on the work area.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    // Begin/End are 1:1 -- End() is UNCONDITIONAL; Begin's return value gates only the BODY
    // (.claude/rules/editor.md). This is the OPPOSITE of the BeginMenu family below.
    if (ImGui::Begin(WELCOME_WINDOW_ID, nullptr, WELCOME_FLAGS)) {
        ImGui::TextUnformatted("No project is open.");
        ImGui::TextDisabled("Create a new project or open an existing one to get started.");
        ImGui::Separator();
        if (ImGui::Button("New Project...")) {
            fileMenu.flow.requested = FileAction::NewProject;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Project...")) {
            fileMenu.flow.requested = FileAction::OpenProject;
        }
        ImGui::Separator();
        if (fileMenu.project.recents.paths.empty()) {
            ImGui::TextDisabled("No recent projects");
        } else {
            for (std::size_t i = 0; i < fileMenu.project.recents.paths.size(); ++i) {
                const std::string& path = fileMenu.project.recents.paths[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(path.c_str())) {
                    fileMenu.project.flow.requestedPath = path;
                    fileMenu.flow.requested = FileAction::OpenProject;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

void drawNewProjectModal(FileMenuContext& fileMenu) {
    NewProjectForm& form = fileMenu.project.flow.form;
    if (form.open && !ImGui::IsPopupOpen(NEW_PROJECT_MODAL_ID)) {
        ImGui::OpenPopup(NEW_PROJECT_MODAL_ID);
    }
    if (!form.open) {
        return;
    }
    // EndPopup ONLY when BeginPopupModal returned true -- the BeginMenu family, not the Begin one.
    if (ImGui::BeginPopupModal(NEW_PROJECT_MODAL_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        inputTextString("Name", form.name, ImGuiInputTextFlags_None);  // 2.2.2's helper
        const NameProblem problem = validateProjectName(form.name);    // LIVE, every frame
        if (problem != NameProblem::Ok && !form.name.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", std::string(nameProblemMessage(problem)).c_str());
        }
        ImGui::TextDisabled("Location");
        ImGui::SameLine();
        ImGui::TextUnformatted(form.location.empty() ? "(none selected)" : form.location.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            form.browseRequested = true;  // and NOTHING else -- the dialog is launched from
                                          // applyFileRequests, NEVER from inside the draw walk
        }
        if (!form.location.empty() && problem == NameProblem::Ok) {
            const std::string preview = form.location + "/" + form.name;
            ImGui::TextDisabled("Will create: %s", preview.c_str());  // a read-only preview, so there
        }  // is never a question about where
        if (!form.error.empty()) {  // the folder lands
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", form.error.c_str());
        }
        ImGui::Separator();
        const bool canCreate = problem == NameProblem::Ok && !form.location.empty();
        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create")) {
            form.createRequested = true;
            // BLOCKING-1 (code review): clearing `form.open` only stops THIS TU from re-submitting the
            // popup next frame -- ImGui itself owns `g.OpenPopupStack` and never GCs an entry for a
            // popup that simply stops being submitted (GetTopMostPopupModal() checks only the Modal
            // flag, never Active). Left uncalled, imgui.cpp's own per-frame bookkeeping sets
            // g.HoveredWindow = NULL forever after, and EVERY menu/panel/button/dock tab becomes
            // unhoverable and unclickable until a stray click happens to trim the stack. Closed
            // UNCONDITIONALLY here: a FAILED create leaves `form.open == true` (see the createRequested
            // consumer below), and this file's own OpenPopup call at the top re-opens the popup next
            // frame from that state, exactly like a fresh NewProject request would.
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            form.cancelRequested = true;
            ImGui::CloseCurrentPopup();  // BLOCKING-1: the same closure, on the path that discards
        }
        // ImGui CANNOT dismiss a MODAL with Escape: NavUpdateCancelRequest's popup branch excludes
        // ImGuiWindowFlags_Modal (imgui.cpp:15032) and BeginPopupModal always sets it -- and the
        // editor never enables ImGuiConfigFlags_NavEnableKeyboard (imgui_layer.cpp:79), so that path
        // is doubly dead. Esc is the universal DISMISS key, so we bind it OURSELVES, HERE, in the
        // body -- repeat=false, one press = one Cancel. NOT redundant with ImGui's own handling; a
        // future ImGui upgrade is the only thing that could make it so, and removing this on that
        // assumption without re-verifying against the vendored source would silently break the
        // behaviour with NO test able to catch it. Human row 10 is its only proof anywhere.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            form.cancelRequested = true;
            ImGui::CloseCurrentPopup();  // BLOCKING-1: the same closure, on the Esc path
        }
        ImGui::EndPopup();
    } else {
        // A SAFETY NET, not the Esc mechanism above: the only thing that can reach here is a
        // PROGRAMMATIC close, because a modal also swallows outside clicks. Treating it as Cancel
        // keeps the flow from wedging with form.open stuck true (the drawUnsavedChangesModal
        // precedent, shell_ui.cpp).
        form.cancelRequested = true;
    }
}

}  // namespace engine::editor
