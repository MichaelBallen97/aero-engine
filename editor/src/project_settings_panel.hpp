#pragma once
// Aero Engine — src-private: the read-only Project Settings panel (task 2.6.2). This HEADER is
// ImGui-free (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// project_settings_panel.cpp, which includes NO <filesystem> and performs NO I/O -- all project data
// arrives through PanelContext::project, which is CONST (D1).
//
// FRAME SHAPE (D3) -- onDraw is exactly two phases and has NO apply step, because there is nothing to
// apply: this panel records no action, owns no selection, and mutates nothing at all.
//   1. build     projectSettingsGroups(context.project, buildEngineVersion) -- ONE pure call
//   2. draw      the empty-state line, or one read-only card (CardPainter) over a table per group. Strictly READ-ONLY.
//
// READ-ONLY BY CONTRACT (D5), one notch tighter than AssetBrowserPanel's: no buttons, no clipboard, no
// reload, no navigation, and no per-frame state beyond the card painter and its draw record (task E.6.3); the build
// engine version is captured once at construction.
#include <aero/editor/panel.hpp>               // the base class
#include <aero/editor/panel_chrome_model.hpp>  // task E.6.3: CardRecord (pure)
#include <aero/editor/project_settings.hpp>    // PROJECT_SETTINGS_PANEL_ID

#include <memory>
#include <string>

namespace engine::editor {

class CardPainter;  // task E.6.3: panel_chrome_ui.hpp's, held by pointer so this header stays ImGui-free

class ProjectSettingsPanel final : public Panel {
public:
    // `buildEngineVersion` is AERO_ENGINE_VERSION, read at editor_app.cpp's ONE site (F11/D8).
    explicit ProjectSettingsPanel(std::string buildEngineVersion);
    ~ProjectSettingsPanel() override;  // out of line: CardPainter is incomplete here
    [[nodiscard]] const CardRecord& cardRecord() const noexcept { return cardRecordValue; }

    // FROZEN (D12/INV-S1) -- see PROJECT_SETTINGS_PANEL_ID's comment in project_settings.hpp.
    [[nodiscard]] const char* id() const noexcept override { return PROJECT_SETTINGS_PANEL_ID; }
    // Right, shared as a TAB with "Inspector". Registered AFTER it, so Inspector stays the selected
    // tab in a fresh layout -- the Console-before-Assets property (console_panel.hpp:36-37).
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    // Reads context.project and NOTHING else (AC-21). options() is deliberately NOT overridden.
    void onDraw(PanelContext& context) override;

private:
    std::string buildVersion;                  // never changes after construction
    std::unique_ptr<CardPainter> cardPainter;  // task E.6.3 -- the two group cards
    CardRecord cardRecordValue;
};

}  // namespace engine::editor
