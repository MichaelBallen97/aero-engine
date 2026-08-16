#pragma once
// Aero Engine — src-private: the Material panel (task 3.4.2, D2). This HEADER is ImGui-free
// (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// material_panel.cpp, the only new ImGui TU this task adds. It is also render-free and rhi-free:
// material_edit.hpp's render/rhi aggregates are needed by the .cpp alone.
//
// FRAME SHAPE -- onDraw READS the reconciled session and RECORDS requests; it writes nothing (INV-3).
// No file, no GPU object, no database WRITE and no session mutation happens inside the draw walk;
// EditorApp::tick() drains everything, exactly as it already does for Import Details. This is the
// house rule's first application to a panel that WRITES FILES, which is why it is restated here.
//
// Every control writes into a per-frame COPY of the session document and, if that copy differs,
// records it as ONE pending whole-document edit -- last-writer-wins, the house's pending-action
// shape. The document is small, and one slot cannot half-apply the way a per-field channel can.
#include <aero/editor/material_session.hpp>
#include <aero/editor/panel.hpp>

#include "material_preview.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::rhi {
class Device;  // forward-declared: the preview holds the pointer, this header names no other rhi type
}  // namespace engine::rhi

namespace engine::editor {

class AssetDatabase;  // a reconciled POINTER, never a reference member (3.1.1's D13 / A-2 / INV-4):
                      // EditorApp is movable, so a reference binds to a pre-move address.

class MaterialPanel final : public Panel {
public:
    // The device arrives AT CONSTRUCTION, exactly like ViewportPanel's and AssetBrowserPanel's (3.1.3's
    // A17): unlike the project root it can never change during a session, so there is nothing to
    // reconcile. It is stored by the preview alone; this class never touches the GPU.
    explicit MaterialPanel(rhi::Device& device) noexcept;

    // render::MATERIAL_TEXTURE_SLOT_COUNT, restated so this header stays out of the render umbrella.
    // material_panel.cpp static_asserts the two equal, so a disagreement is a COMPILE ERROR rather
    // than a slot section that silently stops being drawn.
    static constexpr std::size_t SLOT_COUNT = 5;

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
    void setDatabase(const AssetDatabase* d) noexcept { databasePtr = d; }  // reconciled, NEVER owned

    // The one-shot channels (ImportDetailsPanel's shape, a second application). Each is drained by
    // EditorApp::tick() AS ITS OWN STATEMENT, before it is inspected (F9's ||-short-circuit rule).
    [[nodiscard]] std::optional<MaterialDocument> takePendingDocument() noexcept {
        std::optional<MaterialDocument> r = std::move(pendingDocument);
        pendingDocument.reset();  // a moved-from optional is still ENGAGED -- the move alone is not a drain
        return r;
    }
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

    // ---- the preview's service pass (task 3.4.2, D6/INV-5) ----------------------------------------
    // Called from EditorApp::tick()'s POST-DRAW SLOT and nowhere else -- the ViewportPanel::renderScene
    // mould, not a second path into a subsystem. It drains the session's documentChanged one-shot and
    // forwards; every GPU create, destroy and submit happens inside MaterialPreview::service.
    void servicePreview(MaterialSession& session, const AssetDatabase& database, std::string_view assetsRootAbs,
                        float deltaSeconds);

    // Black-box reads for EditorApp's accessors (the modelImportState() family's shape).
    [[nodiscard]] bool previewAvailable() const noexcept { return preview.available(); }
    [[nodiscard]] std::size_t previewFrameCount() const noexcept { return preview.frameCount(); }
    [[nodiscard]] bool previewBlendDrawnOpaque() const noexcept { return preview.blendDrawnOpaque(); }

private:
    void drawPreview();  // the preview strip: an ImGui::Image, or ONE line saying why not (AC-32)

    const MaterialSession* sessionPtr = nullptr;  // non-owning; ALWAYS null-check
    const AssetDatabase* databasePtr = nullptr;   // non-owning; null before the first scan
    std::optional<MaterialDocument> pendingDocument;
    bool applyRequested = false;
    bool revertRequested = false;
    // ---- UI-ONLY state, never model state --------------------------------------------------------
    // The name field's draft. InputText commits on deactivate-after-edit (AC-17), and on THAT frame
    // ImGui reports no per-frame change, so a form copy rebuilt from the session would already have
    // discarded what was typed. The draft persists across the gesture and re-syncs from the document
    // on every frame the widget is not active -- which is also how a retarget reaches it.
    std::string nameDraft;
    bool nameEditing = false;
    std::array<std::string, SLOT_COUNT> slotSearch;  // one picker search line per slot
    std::string labelScratch;                        // per-frame scratch, NOT model state (the 2.2.1 idiom)
    MaterialPreview preview;                         // OWNED; the only GPU state anywhere in this panel
};

}  // namespace engine::editor
