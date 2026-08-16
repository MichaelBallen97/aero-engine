#pragma once
// Aero Engine — src-private: the Material panel (task 3.4.2, D2). This HEADER is ImGui-free
// (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// material_panel.cpp, the only new ImGui TU this task adds.
//
// FRAME SHAPE -- onDraw READS the reconciled session and RECORDS requests; it writes nothing (INV-3).
// No file, no GPU object, no database lookup and no session mutation happens inside the draw walk;
// EditorApp::tick() drains everything, exactly as it already does for Import Details. This is the
// house rule's first application to a panel that WRITES FILES, which is why it is restated here.
#include <aero/editor/material_session.hpp>
#include <aero/editor/panel.hpp>

#include <string>

namespace engine::editor {

class MaterialPanel final : public Panel {
public:
    // FROZEN FROM THE DAY IT SHIPS (D2; the "Import Details" / "Project Settings" rule restated
    // because it applies identically): this string is the ImGui WINDOW NAME **and** the imgui.ini
    // SETTINGS KEY. RENAMING IT ORPHANS EVERY USER'S SAVED LAYOUT FOR THIS PANEL. Treat it as a
    // persisted format -- imgui_layer_test.cpp's frozenPanelIds array is the pin.
    [[nodiscard]] const char* id() const noexcept override { return "Material"; }
    // DockSlot::Right is the Inspector's and Import Details' slot, so all three TAB TOGETHER: they
    // answer the same question -- "what is the selected thing?" -- for entities, for imports and for
    // materials. Registered LAST in create(), after ImportDetailsPanel, so the Inspector keeps the
    // selected tab by default and no existing panel's registration index shifts.
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    // options() is DELIBERATELY not overridden: the panel scrolls (Project Settings' recorded posture),
    // because its sections carry no ScrollY of their own.
    void onDraw(PanelContext& context) override;

    void setSession(const MaterialSession* s) noexcept { sessionPtr = s; }  // reconciled, NEVER owned

private:
    const MaterialSession* sessionPtr = nullptr;  // non-owning; ALWAYS null-check
    std::string labelScratch;                     // per-frame scratch, NOT model state (the 2.2.1 idiom)
};

}  // namespace engine::editor
