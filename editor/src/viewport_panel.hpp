#pragma once
// Aero Engine — src-private: the Viewport panel (task 2.2.3). TWO-PHASE by design (D3):
//   onDraw()      — inside the ImGui frame: measure, resize, ImGui::Image, record a request.
//   renderScene() — OUTSIDE the ImGui frame, called by EditorApp::tick() after the draw walk and
//                   before ImGuiLayer::endFrame(): records + submits the offscreen scene pass.
// Never call renderScene() from inside a draw walk, and never touch ImGui inside it.
#include <aero/core/vfs.hpp>
#include <aero/editor/asset_drag.hpp>  // task 3.1.5: AssetDragPayload, ViewportAssetDrop
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/gizmo.hpp>  // task 2.3.3: GizmoMode, for the latched mode member
#include <aero/editor/panel.hpp>
#include <aero/editor/scene_bounds.hpp>       // task 3.1.5: MeshBoundsLookup, borrowed by the three consumers
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2: OverlaySegment, for the scratch member
#include <aero/render/debug_draw.hpp>         // task E.1.1: the panel's own world-space line renderer
#include <aero/render/post_process.hpp>       // task 3.6.3: the owned HDR target + the fullscreen resolve
#include <aero/render/render_target.hpp>
#include <aero/render/selection_outline.hpp>  // task E.1.4: the composite + SelectionOutlineParams
#include <aero/scene_render/scene_renderer.hpp>

#include <cstdint>
#include <optional>
#include <utility>  // std::exchange -- the one-shot taker's own idiom
#include <vector>

namespace engine::rhi {
class Device;
}  // namespace engine::rhi

namespace engine::editor {

class ViewportPanel final : public Panel {
public:
    explicit ViewportPanel(rhi::Device& device) noexcept;

    [[nodiscard]] const char* id() const noexcept override;            // "Viewport" — D16, FROZEN
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override;  // Center
    [[nodiscard]] PanelOptions options() const noexcept override;      // {noScrollbar, noPadding}
    void onDraw(PanelContext& context) override;

    // Phase 2. No-op unless onDraw() recorded a request THIS frame (which it does not when the panel
    // is hidden, tabbed away, collapsed, zero-area, or unavailable) — that is AC-7 and AC-10.
    // E12: the panel holds NO scene state and takes the World& fresh every frame, so 2.5.1's
    // wholesale World replacement needs zero change here.
    void renderScene(World& world);

    // Task 2.3.1 / D6: EditorApp::viewportCamera() forwards here. Exposed so "the Viewport renders
    // through the EDITOR camera, not the scene Camera" has a black-box signature at all -- the same
    // reason logRecordCount() exists (2.2.5 D16). 2.3.3's gizmo will read viewMatrix()/
    // projectionMatrix() through this handle.
    [[nodiscard]] EditorCamera& camera() noexcept;
    [[nodiscard]] const EditorCamera& camera() const noexcept;

    // ---- task 3.1.5 ---------------------------------------------------------------------------
    // The aspect the LAST drawn frame used, so tick() can build the same drop ray this panel would.
    [[nodiscard]] float aspect() const noexcept { return lastAspect; }

    // This panel's own ForwardRenderer and its binding table. NULL when the panel is Unavailable (the
    // SceneRenderer is a std::optional and initialization is one-shot and latched). A MeshHandle and a
    // MaterialHandle are PER-ForwardRenderer, so the scene-asset ledger MUST mint its handles on the
    // renderer that draws them -- these two accessors are what make that possible without moving
    // SceneRenderer ownership out of this panel.
    [[nodiscard]] render::ForwardRenderer* sceneForwardRenderer() noexcept;
    [[nodiscard]] scene_render::AssetBindingTable* sceneAssetBindings() noexcept;

    // ---- task 3.6.3 ---------------------------------------------------------------------------
    // The tonemap settings this panel OWNS, because it owns the UI that mutates them. EditorApp::tick
    // reads this and forwards it into the Material panel's preview, so the viewport and the preview
    // can never grade the same material differently. Valid and SANITIZED even when the panel is
    // Unavailable: the member is default-constructed ({1.0F, AcesApprox}) and no failure path touches
    // it.
    [[nodiscard]] const render::TonemapParams& tonemapParams() const noexcept { return tonemapParamsValue; }

    // Records EXACTLY what drawViewOptions' combo and slider record: a candidate value, SANITIZED on
    // store. It exists because no tier in this tree can move an ImGui slider, so the clamp would
    // otherwise be undrivable -- the requestViewMode / requestSearchQuery / requestKindFilter /
    // requestSelectEntry family's fifth application. It calls the SAME sanitize the UI does, which is
    // what makes it a real witness rather than a second policy.
    void requestTonemapParams(const render::TonemapParams& params) noexcept {
        tonemapParamsValue = render::sanitizeTonemapParams(params);
    }

    // This panel's PostProcess, joining sceneForwardRenderer() / sceneAssetBindings() as a test seam.
    // NULL when the panel is Unavailable.
    [[nodiscard]] const render::PostProcess* postProcess() const noexcept;

    // ---- task E.1.1 -----------------------------------------------------------------------------
    // This panel's DebugDraw, and the seam E.1.2 (the grid) and E.2.3 (light gizmos and icons) write
    // into: `debugDraw()->batch().line(...)` from onDraw or from tick() before renderScene, which
    // DRAINS it. A push that lands AFTER renderScene in a tick draws NEXT frame -- stated rather
    // than defended: it is how every immediate-mode debug-draw API behaves. A CPU vector push is not
    // a GPU create and not a World mutation, so the draw-walk rule does not forbid it.
    // NULL when the panel is Unavailable, joining sceneForwardRenderer() / sceneAssetBindings() /
    // postProcess() / outputTarget(). THIS TASK PUSHES NOTHING INTO IT FROM THE EDITOR.
    [[nodiscard]] render::DebugDraw* debugDraw() noexcept;
    [[nodiscard]] const render::DebugDraw* debugDraw() const noexcept;

    // ---- task E.1.4 -----------------------------------------------------------------------------
    // This panel's SelectionOutline, joining the other five as a test seam. NULL when the panel is
    // Unavailable -- which is what I120/I121 read in the shader-tools-OFF configuration, where they
    // ASSERT that arm rather than skipping it.
    [[nodiscard]] const render::SelectionOutline* selectionOutlinePass() const noexcept;

    // ---- task E.1.2 -------------------------------------------------------------------------------
    // The grid toggle. SESSION STATE, default ON, and PERSISTED NOWHERE -- not to project.json, not
    // to imgui.ini, not anywhere. E.4.1 owns per-project editor state and its scope is the last
    // scene; a viewport DISPLAY preference is not project state, and inventing a second store for
    // one boolean is the abstraction this project refuses. Handed off in docs/10 to whichever task
    // introduces a per-user preferences file.
    [[nodiscard]] bool gridEnabled() const noexcept { return gridEnabledValue; }

    // Records exactly what drawViewOptions' checkbox records. It exists because NO TIER IN THIS TREE
    // CAN CLICK AN ImGui CHECKBOX, so without it the toggle is undrivable and its effect
    // unassertable -- the requestViewMode / requestSearchQuery / requestKindFilter /
    // requestSelectEntry / requestTonemapParams family's sixth application. I112 is what it buys.
    void requestGridEnabled(bool enabled) noexcept { gridEnabledValue = enabled; }

    // ---- the overlay strip's claim on a click -----------------------------------------------------
    // Does the interactive overlay row own a press at `pressPoints` (screen-space POINTS, the space
    // io.MousePos is in)? updatePick's ARM step asks exactly this, and so can a test -- which is the
    // whole reason it is a named member rather than two lines inline.
    //
    // WHY A RECT AND NOT AN ImGui QUESTION. The first attempt at this disarmed the pick on
    // ImGui::IsAnyItemActive() after the strip was submitted, reasoning that if a widget had taken
    // the click then ActiveId would be non-zero. IT SHIPPED AND IT DISABLED SCENE PICKING ENTIRELY.
    // ImGui sets ActiveId to the WINDOW'S MoveId on a click in window empty space (imgui.cpp:5522 ->
    // StartMouseMovingWindow at :5534 -> SetActiveID(window->MoveId, window) at :5389, with
    // IsAnyItemActive() being `g.ActiveId != 0` at :6617), and because ImGui::Image submits with
    // id 0, a click on the viewport image IS window empty space. So the guard was true on precisely
    // the frames a pick was being attempted. The old comment's "ImGui::Image never becomes Active"
    // was correct about the ITEM and irrelevant: it is the window's MoveId that goes active.
    //
    // A rect is deterministic and answers the question actually being asked -- "is this press on the
    // strip" -- rather than a global that conflates a widget with the window background.
    [[nodiscard]] bool overlayOwnsPress(Vec2 pressPoints) const noexcept;

    // The rect that decision reads, as the LAST DRAWN FRAME recorded it. Exposed so a test can check
    // it is a REAL, non-degenerate rect inside the image rather than trusting that it was recorded --
    // an empty rect would make overlayOwnsPress() answer false for everything and silently restore
    // the defect this pair exists to fix.
    [[nodiscard]] Vec2 overlayRowMin() const noexcept { return overlayRowTopLeft; }
    [[nodiscard]] Vec2 overlayRowMax() const noexcept { return overlayRowBottomRight; }

    // The ImGui-visible OUTPUT target, as a READ-ONLY seam beside postProcess(). It exists so
    // "nothing depth-tests into this target any more" is an assertable RUNTIME fact rather than a
    // source-text claim -- depthFormat() reads Invalid here and a real depth format on the scene
    // target inside `post`, and no test can otherwise tell the two apart. NULL when Unavailable.
    [[nodiscard]] const render::RenderTarget* outputTarget() const noexcept;

    // The MeshBoundsLookup the ledger publishes each service pass. BORROWED, never owned; valid until
    // the next publish. Consumed by picking, by framing and by the highlight -- ALL THREE OR NONE
    // (INV-D6), which is why it is one member read by one accessor rather than three parameters.
    void setMeshBounds(const MeshBoundsLookup* lookup) noexcept { meshBounds = lookup; }

    // The last scene pass's two unresolved counts, LATCHED inside SceneRenderer::render: buildRenderView
    // runs there and its RenderView does not outlive that call. Zero when no scene pass has run.
    [[nodiscard]] std::uint32_t lastUnresolvedMeshes() const noexcept;
    [[nodiscard]] std::uint32_t lastUnresolvedMaterials() const noexcept;

    // The entity under an NDC point, through THIS panel's camera, aspect, last image size and published
    // mesh bounds. Public so the DRAIN asks the identical question the accept-time peek asked -- which
    // is what makes the seam below and a real drop indistinguishable downstream, and is why no picked
    // entity is carried across frames in a member.
    [[nodiscard]] Entity pickAt(const World& world, Vec2 ndc) const;

    // The drop one-shot, drained by tick(). The panel RECORDS and never acts: nothing here mutates the
    // World or the Selection, which is what keeps AcceptDragDropPayload's frame semantics from
    // mattering.
    [[nodiscard]] std::optional<ViewportAssetDrop> takeAssetDropRequest() noexcept {
        return std::exchange(pendingAssetDrop, std::nullopt);
    }
    void requestAssetDrop(AssetDragPayload payload, Vec2 ndc) noexcept {
        pendingAssetDrop = ViewportAssetDrop{.payload = payload, .ndc = ndc};
    }

private:
    enum class Status : std::uint8_t { Uninitialized, Ready, Unavailable };

    void ensureInitialized(rhi::Extent2D firstExtent);  // D11: one attempt, latched
    void focusSelection(PanelContext& context);         // F: frame the selection, or the scene, or reset

    // Task 2.3.2. Both take POINTS (D18) as engine Vec2, never ImVec2: this header is deliberately
    // ImGui-free -- every ImGui value is converted at the ONE call site in onDraw. Both are members
    // rather than free functions because both need lastAspect and the latched `gesture`.
    void updatePick(PanelContext& context, Vec2 imageOrigin, Vec2 avail, bool hovered);
    void drawSelectionOverlay(PanelContext& context, Vec2 imageOrigin, Vec2 avail);

    // task E.1.4: the composite's params, derived from lastFramebufferScale. SANITIZED on the way
    // out, so no caller can hand the composite an out-of-range radius.
    [[nodiscard]] render::SelectionOutlineParams selectionOutlineParams() const noexcept;

    // Task 2.3.3. Both take POINTS (D18) as engine Vec2, never ImVec2: this header is deliberately
    // ImGui-free -- every ImGui value is converted at the ONE call site in onDraw (the 2.3.2
    // precedent). updateGizmo is a member because it needs lastAspect, editorCamera and `gesture`.
    void updateGizmo(PanelContext& context, Vec2 imageOrigin, Vec2 avail, bool hovered);
    void drawGizmoBar();  // takes nothing: everything it needs is a member (A13)

    // task 3.6.3: the operator combo + the exposure slider. Called on the SAME LINE as
    // drawGizmoBar() but OUTSIDE its BeginDisabled(!gizmoHasTarget) scope, so the tonemap
    // controls stay live with nothing selected.
    void drawViewOptions();

    // task 3.1.5: the custom drop target's whole body, a member so the ImGui glue stays in one place.
    // PEEK -> classify -> only then accept, so an illegal drop draws no highlight rect.
    void acceptViewportAssetDrop(PanelContext& context, Vec2 imageOrigin, Vec2 avail);

    rhi::Device* device = nullptr;  // non-owning; outlives the panel (EditorApp owns both)
    VirtualFileSystem shaderVfs;    // mounted once at init (AERO_SHADERS_DIR, D-user-1)
    // task 3.6.3: `post` OWNS the HDR scene target the SceneRenderer draws into; `target` below stays
    // the ImGui-visible OUTPUT and is now DEPTH-FREE, because the only thing drawn into it is a
    // depth-off fullscreen triangle.
    std::optional<render::PostProcess> post;
    std::optional<render::RenderTarget> target;
    std::optional<scene_render::SceneRenderer> sceneRenderer;
    // task E.1.1: the panel's DebugDraw, built against the SAME HDR pair the SceneRenderer was, so a
    // line records into the scene pass with matching formats. Member/accessor collision rule: the
    // MEMBER is debugDrawer, the accessor debugDraw() (the tonemapParamsValue/tonemapParams()
    // precedent one member down). DECLARED AFTER sceneRenderer and therefore DESTROYED BEFORE it and
    // before `post`, which is the order its pipelines' formats came from.
    std::optional<render::DebugDraw> debugDrawer;
    // task E.1.4: the edge-detect composite, built against the OUTPUT target's formats -- NOT the HDR
    // pair the other four GPU objects use. That asymmetry IS the point: the outline is editor chrome
    // and composites into the already-tonemapped image, so its pipeline's colour format is
    // target->colorFormat() and its depth format is Invalid. DECLARED AFTER debugDrawer and therefore
    // DESTROYED BEFORE it, `target` and `post`, which is the order its formats came from.
    std::optional<render::SelectionOutline> selectionOutline;
    // task E.1.4: built ONCE per tick at the top of drawSelectionOverlay and consumed TWICE -- for
    // the markers here and for the mask in renderScene (D12). Cleared at the end of renderScene, so a
    // second renderScene on one tick (which cannot happen today) would draw nothing rather than last
    // tick's selection.
    scene_render::SelectionMaskScratch selectionMaskScratch;
    scene_render::SelectionMaskSet selectionMaskSet;
    // task E.1.4: io.DisplayFramebufferScale.x, captured in onDraw beside the existing toPixels
    // calls, because renderScene MUST NOT call ImGui (2.2.3 INV-3, still in force).
    float lastFramebufferScale = 1.0F;
    // task 3.6.3: session state -- never written to project.aero, never to imgui.ini, never persisted
    // anywhere. Default-constructed and SANITIZED on every write, so it is valid even when the panel
    // never initialises. Member/accessor names differ by the house collision rule.
    render::TonemapParams tonemapParamsValue{};
    // Member/accessor collision rule: the MEMBER takes the distinct name (budgetValue/budget(),
    // tonemapParamsValue/tonemapParams(), the RenderTarget precedent).
    bool gridEnabledValue = true;
    Status status = Status::Uninitialized;
    const char* unavailableReason = nullptr;  // string literal; shown in-panel when Unavailable
    bool renderRequested = false;             // set by onDraw, consumed by renderScene

    EditorCamera editorCamera;     // WRITTEN only in onDraw; READ in both phases (INV-3)
    CameraGestureState gesture{};  // LATCHED across frames -- D5 rule 1 needs the previous value
    float lastAspect = 1.0F;       // set in onDraw (PIXELS, D15); read by F's focusOn, same frame

    // Task 2.3.2 (D10): the pick's own press/release tracking. NOT an ImGui item state -- ImGui::Image
    // submits its item with id 0, so nothing on the image ever becomes Active and there is no item
    // state to consult (F28); and the SDL3 backend captures the mouse while a button is held (F29), so
    // a press inside followed by a release far outside is an ORDINARY sequence, not an edge case.
    bool pickArmed = false;                      // LMB went down on the image with no camera gesture
    Vec2 pickPressPos{};                         // where, in POINTS -- for the slop test
    std::vector<OverlaySegment> overlayScratch;  // caller-owned, cleared and reused every frame (D6)

    // Task 2.3.3.
    GizmoMode gizmoMode{};          // LATCHED across frames; W/E/R/X and the overlay bar both write it
    bool gizmoActive = false;       // D10: THIS frame's "the gizmo owns the cursor". Assigned on EVERY
                                    // updateGizmo entry (INV-4) -- false whenever no Manipulate was
                                    // called, because ImGuizmo::IsOver() would answer from stale
                                    // gContext state on such a frame (F8).
    bool gizmoHasTarget = false;    // A13: assigned every frame beside gizmoActive; the ONLY thing the
                                    // overlay bar's enabled state reads, so the bar can never disagree
                                    // with whether a gizmo actually drew.
    bool gizmoWasUsing = false;     // previous frame's IsUsing(), for gizmoDragEdge (D22)
    bool gizmoWarnLatched = false;  // D12: one WARN per drag, not one per frame

    // The interactive overlay row's screen rect in POINTS, written at onDraw's step 9b and read by
    // overlayOwnsPress() on the NEXT frame's step 8b. ONE FRAME OLD BY CONSTRUCTION, and that is
    // sound rather than tolerated: the strip's origin is imageOrigin + OVERLAY_INSET and its extent
    // is fixed by the widgets on it, so it only moves when the dock does. Empty until the first
    // frame that reaches step 9b, and an empty rect owns nothing.
    Vec2 overlayRowTopLeft{};
    Vec2 overlayRowBottomRight{};

    // Task 3.1.5.
    const MeshBoundsLookup* meshBounds = nullptr;       // published by EditorApp, borrowed, never owned
    Vec2 lastImageSizePoints{};                         // the last drawn image rect's size, POINTS -- what
                                                        // pickAt needs for the screen-space disc radius
    std::optional<ViewportAssetDrop> pendingAssetDrop;  // drained by tick(), never here
};

}  // namespace engine::editor
