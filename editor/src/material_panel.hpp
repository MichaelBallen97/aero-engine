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
#include <aero/editor/asset_drag.hpp>  // task 3.1.5: MaterialSlotTextureDrop
#include <aero/editor/material_session.hpp>
#include <aero/editor/panel.hpp>

#include "material_preview.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::rhi {
class Device;  // forward-declared: the preview holds the pointer, this header names no other rhi type
}  // namespace engine::rhi

namespace engine::editor {

// Forward-declared, never #included here: this header stays ImGui-free BY FILE PLACEMENT, and
// asset_picker.hpp is src-private. `AssetPickerState` is a STRUCT -- declared with the matching tag so
// clang does not warn under -Wmismatched-tags (task E.3.3).
struct AssetPickerState;
class ThumbnailService;

// task E.3.4: forward-declared rather than included, so this header's include set does not move. The
// model is PUBLIC and ImGui-free, so including it would be legal -- the forward declaration is simply
// the smaller edit, and drawBody takes the layout by const reference.
struct MaterialPanelLayout;

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
    // options() is DELIBERATELY not overridden, and the reason CHANGED at E.3.4. The old comment said
    // "its sections carry no ScrollY of their own", which stopped being true when the body became a
    // child -- and then only SOMETIMES, which is why this now names THREE cases rather than two:
    //
    //   1. Untargeted / Error -- the WINDOW scrolls. A long parse error wraps and must be readable;
    //      I100 drives that arm.
    //   2. Ready, MaterialPanelMode::FixedRegions -- the window's content is exactly header + a child
    //      sized to the remainder + footer, which CANNOT overflow. The child scrolls; the window does
    //      not need to.
    //   3. Ready, MaterialPanelMode::Scrolling -- there is NO child at all and the WINDOW scrolls,
    //      exactly as in case 1. This is the arm a short dock node takes, and on a Retina display at
    //      320x180 it is the arm the product actually takes (the fixed chrome is 103 points against a
    //      98-point content region).
    //
    // So noScrollbar would buy nothing in case 2 and would SILENTLY CLIP cases 1 and 3 -- the Error
    // state's wrapped message, and the whole form on a short panel.
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

    // ---- task 3.1.5: the slot texture drop ---------------------------------------------------------
    // THE ONE ASYMMETRY among the three drop surfaces, and it is deliberate: this drop does not travel
    // to tick() as an ACTION. The accept mutates the frame copy and rides the existing pendingDocument
    // -> session.edit -> dirty -> Apply river, so nothing new writes an .aeromat. The seam records a
    // pending slot drop that the panel's own NEXT onDraw folds into the frame copy; the taker exists
    // ONLY so tick() can observe the request and log a vanished-guid refusal.
    void requestSlotTextureDrop(std::size_t slot, Guid textureGuid) noexcept {
        pendingSlotDrop = MaterialSlotTextureDrop{.slot = slot, .textureGuid = textureGuid};
        observedSlotDrop = pendingSlotDrop;
    }
    [[nodiscard]] std::optional<MaterialSlotTextureDrop> takeAssetDropRequest() noexcept {
        return std::exchange(observedSlotDrop, std::nullopt);
    }

    // ---- the preview's service pass (task 3.4.2, D6/INV-5) ----------------------------------------
    // Called from EditorApp::tick()'s POST-DRAW SLOT and nowhere else -- the ViewportPanel::renderScene
    // mould, not a second path into a subsystem. It drains the session's documentChanged one-shot and
    // forwards; every GPU create, destroy and submit happens inside MaterialPreview::service.
    // task 3.6.3: `tonemap` is APPENDED LAST, so no existing argument moves. It is forwarded verbatim
    // to MaterialPreview::service; this panel neither owns nor edits it -- the VIEWPORT does.
    // task E.2.4: `lighting` is APPENDED LAST for `tonemap`'s reason. It is the bridge's resolution,
    // handed in by EditorApp::tick; this panel neither resolves nor edits it.
    void servicePreview(MaterialSession& session, const AssetDatabase& database, std::string_view assetsRootAbs,
                        float deltaSeconds, const render::TonemapParams& tonemap,
                        const MaterialPreviewLighting& lighting);

    // Black-box reads for EditorApp's accessors (the modelImportState() family's shape).
    [[nodiscard]] bool previewAvailable() const noexcept { return preview.available(); }
    [[nodiscard]] std::size_t previewFrameCount() const noexcept { return preview.frameCount(); }
    [[nodiscard]] bool previewBlendDrawnOpaque() const noexcept { return preview.blendDrawnOpaque(); }
    [[nodiscard]] std::size_t previewTextureCount() const noexcept { return preview.readyTextureCount(); }
    [[nodiscard]] std::size_t previewTextureLoadAttempts() const noexcept { return preview.textureLoadAttempts(); }
    [[nodiscard]] std::size_t previewImageCount() const noexcept { return preview.imageCount(); }
    [[nodiscard]] std::size_t previewStaleImageCount() const noexcept { return preview.staleImageCount(); }
    [[nodiscard]] std::size_t previewUvSetWarnCount() const noexcept { return preview.uvSetWarnCount(); }
    [[nodiscard]] std::uint32_t previewTextureWidth() const noexcept { return preview.textureExtent().width; }
    [[nodiscard]] std::uint32_t previewTextureHeight() const noexcept { return preview.textureExtent().height; }
    [[nodiscard]] std::size_t previewSkyDrawCount() const noexcept { return preview.skyDrawCount(); }
    // The LATCH, not a live read: written by the service pass, read by the NEXT draw walk. One frame
    // late and invisible -- the lastImageSizePoints posture of reading a latched value between phases.
    [[nodiscard]] bool previewHasSun() const noexcept { return previewHasSunValue; }
    [[nodiscard]] const render::RenderTarget* previewOutputTarget() const noexcept { return preview.outputTarget(); }

    // task E.3.3: both set ONCE in EditorApp::create (the viewportPanel posture), not reconciled --
    // each points at a heap object EditorApp holds through a unique_ptr, so the address survives the
    // app's own move. NULL is a legal state: the slot then draws its bound-state block and no control.
    void setAssetPicker(AssetPickerState* s) noexcept { assetPicker = s; }
    void setThumbnails(ThumbnailService* s) noexcept { thumbnails = s; }

private:
    // task E.3.4: the height is the LAYOUT's now, not a constant. An ImGui::Image, or ONE line saying
    // why not (AC-32).
    void drawPreview(float previewHeight);
    // task E.3.4: the eight sections and the two wrapped notices, as ONE function called from BOTH
    // body paths -- so nothing inside it can drift between FixedRegions and Scrolling. A member for
    // drawSlotSection's stated reason: it reads preview, previewHasSunValue, nameDraft, nameEditing,
    // labelScratch and databasePtr off `this`.
    void drawBody(MaterialDocument& form, const MaterialPanelLayout& layout,
                  const std::optional<MaterialError>& invalid, bool& changed);
    // task E.3.4: the File section's read-only rows. A member for drawBody's reason -- it reads
    // sessionPtr, databasePtr and labelScratch off `this`, so the parameter list is one.
    void drawFileSection(float labelWidth);
    // task E.3.3: a PRIVATE MEMBER rather than a free function -- it reads databasePtr, labelScratch,
    // observedSlotDrop, assetPicker, thumbnails and keyScratch off `this`, so the parameter list is
    // four rather than the ten a free function would have needed.
    [[nodiscard]] bool drawSlotSection(std::size_t index, MaterialDocument& form, PreviewTextureState textureState,
                                       std::string_view textureNotice);

    const MaterialSession* sessionPtr = nullptr;  // non-owning; ALWAYS null-check
    const AssetDatabase* databasePtr = nullptr;   // non-owning; null before the first scan
    std::optional<MaterialDocument> pendingDocument;
    bool applyRequested = false;
    bool revertRequested = false;
    // task 3.1.5. `pendingSlotDrop` is the SEAM's channel, folded into the frame copy by the next
    // onDraw; `observedSlotDrop` is what tick() drains, written by BOTH the seam and a real accept.
    std::optional<MaterialSlotTextureDrop> pendingSlotDrop;
    std::optional<MaterialSlotTextureDrop> observedSlotDrop;
    // ---- UI-ONLY state, never model state --------------------------------------------------------
    // The name field's draft. InputText commits on deactivate-after-edit (AC-17), and on THAT frame
    // ImGui reports no per-frame change, so a form copy rebuilt from the session would already have
    // discarded what was typed. The draft persists across the gesture and re-syncs from the document
    // on every frame the widget is not active -- which is also how a retarget reaches it.
    std::string nameDraft;
    bool nameEditing = false;
    // task E.2.4: the RESOLUTION's answer, latched in the service pass. NOT `sun.intensity != 0` --
    // a sun the user set to 0 is a sun they switched off, and the notice must not claim there is none.
    bool previewHasSunValue = false;
    // task E.3.3: `slotSearch` is DELETED. The picker's search is per OPEN, not per slot (D12), so
    // there is nothing left for a per-slot line to remember.
    std::string labelScratch;  // per-frame scratch, NOT model state (the 2.2.1 idiom)
    std::string keyScratch;    // labelScratch's idiom, for the slot's seam key
    // task E.3.4: retarget detection, for the per-slot UI state ALONE. It is NOT model state and it is
    // never compared against anything the session owns -- the session's own sticky-target rule is
    // untouched by it.
    std::string lastTargetPath;
    // task E.3.3: borrowed, never owned, set once -- see setAssetPicker/setThumbnails above.
    AssetPickerState* assetPicker = nullptr;
    ThumbnailService* thumbnails = nullptr;
    MaterialPreview preview;  // OWNED; the only GPU state anywhere in this panel
};

}  // namespace engine::editor
