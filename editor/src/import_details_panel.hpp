#pragma once
// Aero Engine — src-private: the Import Details panel (task 3.2.1). This HEADER is ImGui-free
// (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// import_details_panel.cpp, the ONLY ImGui in this task.
//
// FRAME SHAPE -- onDraw READS the reconciled session and RECORDS requests; it writes nothing (D17/
// INV-M12). The Apply/Revert/settings-edit requests are drained by EditorApp::tick(), never applied
// here -- the identical "record a pending action, apply it after the walk" rule every panel in this
// tree follows for a write.
#include <aero/editor/model_import_session.hpp>
#include <aero/editor/panel.hpp>

#include <optional>
#include <string>

namespace engine::editor {

class ImportDetailsPanel final : public Panel {
public:
    // FROZEN (D17; 2.6.2's rule restated because it applies identically): this string is the ImGui
    // WINDOW NAME **and** the imgui.ini SETTINGS KEY. RENAMING IT ORPHANS EVERY USER'S SAVED LAYOUT
    // FOR THIS PANEL. Treat it as a persisted format.
    [[nodiscard]] const char* id() const noexcept override { return "Import Details"; }
    // DockSlot::Right is the Inspector's slot, so the two TAB TOGETHER: both answer "what is the
    // selected thing?", one for entities and one for assets. Registered LAST in create(), after
    // ProjectSettingsPanel, so Inspector keeps the selected tab and no existing panel's index shifts.
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    void onDraw(PanelContext& context) override;

    void setSession(const ModelImportSession* s) noexcept { sessionPtr = s; }  // reconciled, NEVER owned

    // The one-shot channels (takeOrphanDeleteRequest's shape, a second application). Each is drained by
    // EditorApp::tick() AS ITS OWN STATEMENT, before it is inspected (F9's ||-short-circuit rule).
    [[nodiscard]] bool takeApplyRequest() noexcept {
        const bool r = applyRequested;
        applyRequested = false;
        return r;
    }
    [[nodiscard]] bool takeRevertRequest() noexcept {
        const bool r = revertRequested;
        revertRequested = false;
        return r;
    }
    [[nodiscard]] std::optional<ImportSettings> takePendingSettings() noexcept {
        std::optional<ImportSettings> r = editedSettings;
        editedSettings.reset();
        return r;
    }

    // task 3.2.4: the Blender section's four channels, the same shape a fourth time. `Import with
    // Blender` / `Re-import` / `Retry` all record convertRequested -- they are one action with three
    // labels, and giving them one channel is what stops the panel deciding which is which.
    [[nodiscard]] bool takeConvertRequest() noexcept {
        const bool r = convertRequested;
        convertRequested = false;
        return r;
    }
    [[nodiscard]] bool takeCancelRequest() noexcept {
        const bool r = cancelRequested;
        cancelRequested = false;
        return r;
    }
    [[nodiscard]] bool takeLocateRequest() noexcept {
        const bool r = locateRequested;
        locateRequested = false;
        return r;
    }
    [[nodiscard]] bool takeRedetectRequest() noexcept {
        const bool r = redetectRequested;
        redetectRequested = false;
        return r;
    }

private:
    const ModelImportSession* sessionPtr = nullptr;  // non-owning; ALWAYS null-check
    bool applyRequested = false;
    bool revertRequested = false;
    // task 3.2.4
    bool convertRequested = false;
    bool cancelRequested = false;
    bool locateRequested = false;
    bool redetectRequested = false;
    std::optional<ImportSettings> editedSettings;  // the form's own copy while the user drags
    std::string labelScratch;                      // per-frame scratch, NOT model state (the 2.2.1 idiom)
};

}  // namespace engine::editor
