#pragma once
// Aero Engine — src-private: the read-only Project Settings panel (task 2.6.2). This HEADER is
// ImGui-free (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// project_settings_panel.cpp, which includes NO <filesystem> and performs NO I/O -- all project data
// arrives through PanelContext::project, which is CONST (D1).
//
// FRAME SHAPE (D3) -- onDraw is exactly two phases and has NO apply step, because there is nothing to
// apply: this panel records no action, owns no selection, and mutates nothing at all.
//   1. build     projectSettingsGroups(context.project, buildEngineVersion) -- ONE pure call
//   2. draw      the empty-state line, or SeparatorText + a table per group. Strictly READ-ONLY.
//
// READ-ONLY BY CONTRACT (D5), one notch tighter than AssetBrowserPanel's: no buttons, no clipboard, no
// reload, no navigation, no per-frame state. The ONLY member is the build engine version, captured
// once at construction.
#include <aero/editor/panel.hpp>             // the base class
#include <aero/editor/project_settings.hpp>  // PROJECT_SETTINGS_PANEL_ID

#include <string>

namespace engine::editor {

class ProjectSettingsPanel final : public Panel {
public:
    // `buildEngineVersion` is AERO_ENGINE_VERSION, read at editor_app.cpp's ONE site (F11/D8).
    explicit ProjectSettingsPanel(std::string buildEngineVersion);

    // FROZEN (D12/INV-S1) -- see PROJECT_SETTINGS_PANEL_ID's comment in project_settings.hpp.
    [[nodiscard]] const char* id() const noexcept override { return PROJECT_SETTINGS_PANEL_ID; }
    // Right, shared as a TAB with "Inspector". Registered AFTER it, so Inspector stays the selected
    // tab in a fresh layout -- the Console-before-Assets property (console_panel.hpp:36-37).
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    // Reads context.project and NOTHING else (AC-21). options() is deliberately NOT overridden.
    void onDraw(PanelContext& context) override;

private:
    std::string buildVersion;  // the ONLY member; never changes after construction
};

}  // namespace engine::editor
