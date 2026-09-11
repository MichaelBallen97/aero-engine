#pragma once
// Aero Engine — src-private: the Viewport panel (task 2.2.3). TWO-PHASE by design (D3):
//   onDraw()      — inside the ImGui frame: measure, resize, ImGui::Image, record a request.
//   renderScene() — OUTSIDE the ImGui frame, called by EditorApp::tick() after the draw walk and
//                   before ImGuiLayer::endFrame(): records + submits the offscreen scene pass.
// Never call renderScene() from inside a draw walk, and never touch ImGui inside it.
#include <aero/core/vfs.hpp>
#include <aero/editor/asset_drag.hpp>  // task 3.1.5: AssetDragPayload, ViewportAssetDrop
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/gizmo.hpp>        // task 2.3.3: GizmoMode, for the latched mode member
#include <aero/editor/gizmo_style.hpp>  // task E.1.5: GizmoStyle, for the read-back seam
#include <aero/editor/panel.hpp>
#include <aero/editor/scene_bounds.hpp>       // task 3.1.5: MeshBoundsLookup, borrowed by the three consumers
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2: OverlaySegment, for the scratch member
#include <aero/editor/view_axis_gizmo.hpp>    // task E.1.3: the corner widget's layout, poses and snap
#include <aero/editor/viewport_gizmos.hpp>    // task E.2.3: the icon/gizmo walk (carries viewport_icons.hpp)
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
    // task E.2.3: the panel's FIRST user-declared destructor, and it exists for the icon atlas alone
    // -- rhi::TextureHandle and rhi::SamplerHandle are not RAII types. Safe because Panel declares
    // `virtual ~Panel() = default` (panel.hpp) so PanelRegistry's unique_ptr<Panel> destroys this
    // correctly, and because the Device outlives the panel: ~ViewportPanel runs inside ~PanelRegistry
    // inside ~EditorApp, which precedes ~Device (this file's own banner, viewport_panel.cpp's).
    // It suppresses nothing: Panel DELETES both copy and both move operations, so this class has
    // never been copyable or movable and there is no implicit operation to lose.
    ~ViewportPanel() override;

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

    // ---- task E.1.5 -----------------------------------------------------------------------------
    // A TEST SEAM, and the first STATIC one, because it reads a PROCESS-WIDE GLOBAL rather than a
    // panel member: ImGuizmo's live Style, converted back to a GizmoStyle through ImGui's OWN
    // ColorConvertFloat4ToU32 (the exact packing the draw list uses), so I124 can assert what the
    // library HOLDS rather than what the panel SENT. That is what keeps aero_editor_imgui_test
    // ImGui-free at source: the alternative is #include <ImGuizmo.h> in a test TU, which ends the
    // property for one assertion this provides equally well.
    // Cheap and allocation-free; meant for the test, after tick(), on the thread that owns the ImGui
    // context -- which is the test's main thread.
    [[nodiscard]] static GizmoStyle imGuizmoStyleReadback() noexcept;

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

    // ---- task E.2.3 -------------------------------------------------------------------------------
    // The gizmo toggle: ONE checkbox covering the light/camera ICONS and the selected light's GIZMO,
    // exactly as Grid covers the grid AND the axes. Session state, default ON, persisted NOWHERE --
    // the grid toggle's own rule, handed off to whichever task introduces a per-user preferences file.
    [[nodiscard]] bool gizmosEnabled() const noexcept { return gizmosEnabledValue; }

    // Records exactly what drawViewOptions' checkbox records. It exists because NO TIER IN THIS TREE
    // CAN CLICK AN ImGui CHECKBOX -- the requestGridEnabled family's tenth application. I128 is what
    // it buys, and it is ALSO what keeps I108/I109/I112's magnitudes unrestated.
    void requestGizmosEnabled(bool enabled) noexcept { gizmosEnabledValue = enabled; }

    // ---- task E.2.4 -------------------------------------------------------------------------------
    // The view-options popover: ONE button on the strip, `View`, opening a popup that holds every
    // control the row used to carry (Grid, Gizmos, the tonemap combo, the exposure slider) plus the two
    // this task adds (Projection, View axis). `viewOptionsOpen()` reads the latch drawViewOptions
    // writes from BeginPopup's OWN answer -- so it reports the LAST DRAWN frame, exactly as
    // overlayRowMin/Max do. updateGizmo reads the same latch one step EARLIER next frame, which is the
    // RIGHT frame: ImGui closes a popup at EndFrame, AFTER the draw walk, so on the dismissing click's
    // frame the latch still says open -- which is exactly when ImGuizmo must be told.
    // `requestViewOptionsOpen` opens (OpenPopup at the button site) or closes (CloseCurrentPopup inside
    // the body) on the NEXT draw walk. It exists because NO TIER IN THIS TREE CAN CLICK A BUTTON --
    // the requestTonemapParams / requestGridEnabled / requestGizmosEnabled family's ELEVENTH
    // application. I138 is what it buys.
    [[nodiscard]] bool viewOptionsOpen() const noexcept { return viewOptionsOpenValue; }
    void requestViewOptionsOpen(bool open) noexcept { pendingViewOptionsOpen = open; }

    // The view-axis widget's visibility toggle -- E.1.3's own handoff, by name. Session state, default
    // ON, persisted NOWHERE: the grid toggle's rule, handed off unchanged to whichever task introduces
    // a per-user preferences file.
    // OFF means, in ONE fact rather than four: the layout is never computed, so nothing draws, no snap
    // can begin, viewAxisOwnsPoint answers false and viewAxisRectMin/Max answer the DEGENERATE rect --
    // which is exactly what viewAxisRect already returns when the widget hides for being too small
    // (E.1.3's D16). A snap ALREADY IN FLIGHT is the camera's animation, not the widget's, and
    // continues. The family's TWELFTH application; I139 is what it buys.
    [[nodiscard]] bool viewAxisEnabled() const noexcept { return viewAxisEnabledValue; }
    void requestViewAxisEnabled(bool enabled) noexcept { viewAxisEnabledValue = enabled; }

    // ---- task E.1.3: the view-axis gizmo's seams --------------------------------------------------
    // The requestTonemapParams / requestGridEnabled family's eighth and ninth applications. No tier in
    // this tree can click an ImDrawList circle, so without these the widget's whole behaviour is
    // undrivable. requestViewSnap routes through beginViewSnap -- the SAME three-line member the real
    // click uses -- so the seam and a click are indistinguishable downstream (the pickAt / drop-drain
    // precedent above).
    void requestViewSnap(ViewAxis axis) noexcept { beginViewSnap(axis); }
    void requestProjectionMode(ProjectionMode mode) noexcept { editorCamera.setProjectionMode(mode); }
    [[nodiscard]] bool viewSnapActive() const noexcept { return viewSnap.active(); }

    // The widget's screen rect, as THIS frame's image rect implies it -- a PURE function, unlike
    // overlayRowMin/Max below, which report what the LAST DRAWN frame recorded. Exposed so a test can
    // check it is a real, non-degenerate square inside the image rather than trusting it was computed.
    [[nodiscard]] Vec2 viewAxisRectMin() const noexcept;
    [[nodiscard]] Vec2 viewAxisRectMax() const noexcept;

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
    //
    // task E.1.3: it now ORs a SECOND rect -- the view-axis widget's, through viewAxisOwnsPoint below
    // -- and takes the image rect to compute it. That rect is PURE (viewAxisRect), so unlike the
    // row's it describes THIS frame and needs no staleness argument at all. Both arms go through one
    // containsHalfOpen helper, which is what stops the second rect acquiring a subtly different
    // containment rule. Its one production caller is updatePick, which already has both values.
    [[nodiscard]] bool overlayOwnsPress(Vec2 pressPoints, Vec2 imageOrigin, Vec2 imageSize) const noexcept;

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
    void destroyIconAtlas() noexcept;                   // task E.2.3. IDEMPOTENT: safe twice, safe on invalid handles
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

    // Task E.1.3, mirroring the updateGizmo / drawGizmoBar pairing. updateViewAxisGizmo computes the
    // layout ONCE per frame at step 8b'''' and routes the click; drawViewAxisGizmo reads that same
    // layout at step 9x, so the picture and the hit test cannot disagree (AC-16). beginViewSnap is
    // the one path both the click and requestViewSnap take.
    void beginViewSnap(ViewAxis axis) noexcept;
    void updateViewAxisGizmo(Vec2 imageOrigin, Vec2 avail, bool inputHovered);
    void drawViewAxisGizmo() const;

    // Task E.1.3, code-review round: ONE definition of "the view-axis widget owns this point", with
    // TWO consumers -- overlayOwnsPress (which keeps the press off the scene pick) and updateGizmo
    // (which keeps the same press out of ImGuizmo). They must never disagree about the rect, which is
    // exactly what a second hand-written comparison would eventually do.
    [[nodiscard]] bool viewAxisOwnsPoint(Vec2 pointPoints, Vec2 imageOrigin, Vec2 imageSize) const noexcept;

    // task E.2.4: the `View` button and its popup. The BUTTON's rect max is what step 9b records; the
    // popup's contents are D10's two groups -- Display (Projection, Tonemap, Exposure) above Overlays
    // (Grid, Gizmos, View axis). Called on the SAME LINE as drawGizmoBar() but OUTSIDE its
    // BeginDisabled(!gizmoHasTarget) scope, so every control stays live with nothing selected.
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
    // task E.2.3: the icon atlas, OWNED. 256x64 RGBA8Unorm, built once in ensureInitialized and handed
    // to debugDrawer->setBillboardTexture, which BORROWS both.
    //
    // IT IS RELEASED BEFORE ~DebugDraw, NOT AFTER, and the declaration order below has nothing to do
    // with it: rhi::TextureHandle and rhi::SamplerHandle are not RAII types, so their member
    // destructors release nothing, and ~ViewportPanel's BODY -- which calls destroyIconAtlas() -- runs
    // before any member destructor at all. Safe for two reasons that are about the borrow, not the
    // order. (1) Nothing flushes in between: the destructor's body is that one call, and the only
    // other call site (ensureInitialized's failure path) is followed immediately by
    // debugDrawer.reset(). (2) DebugDraw neither destroys nor reads a borrowed handle at teardown --
    // setBillboardTexture stores it verbatim ("BORROWED, both: never destroyed here, never adopted",
    // debug_draw.cpp) and flush() is its only reader, falling back to DebugDraw's own 1x1 white texel
    // whenever the handle is invalid.
    rhi::TextureHandle iconAtlasTexture{};
    // OWNED. Linear/Linear, MipmapMode::Nearest, ClampToEdge on U, V AND W -- SamplerDesc DEFAULTS TO
    // Repeat, which would wrap a boundary sample to the FAR SIDE of the atlas and put the camera glyph
    // on the sun's left edge. This mirrors DebugDraw's own default sampler field for field, so the
    // atlas path and the fallback path filter alike.
    rhi::SamplerHandle iconAtlasSampler{};
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
    // task E.2.3: written in drawSelectionOverlay (the only place with a PanelContext) and read in
    // renderScene, which takes a World& and CANNOT see the Selection. Both are cleared on EVERY
    // renderScene exit past the guard chain -- E.1.4's D12 discipline, three exits and three clears --
    // because a frame that returned early would otherwise draw last tick's selection's gizmos.
    std::vector<Entity> selectionSnapshot;
    Entity selectionPrimary{};
    // task E.2.3: withoutGeometry, minus the entities that now draw an icon (D10). Filtered through
    // the SAME viewportIconFor the emitter and the picker read, so all three agree by construction.
    // It needs no clear in renderScene: it is filled and consumed entirely inside drawSelectionOverlay
    // and is cleared at that function's own top, which is the asymmetry with the two above.
    std::vector<Entity> markerScratch;
    ViewportGizmoScratch gizmoScratch;
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
    bool gizmosEnabledValue = true;     // task E.2.3, the same session-state rule
    bool viewAxisEnabledValue = true;   // task E.2.4, the grid toggle's session-state rule
    bool viewOptionsOpenValue = false;  // LAST DRAWN frame's BeginPopup answer
    // The seam's channel: THREE states -- no request / open / close -- in one member, consumed
    // unconditionally after the popup so a stale request cannot fire on a later frame. Two bools would
    // make "both set" representable and would need a documented precedence.
    std::optional<bool> pendingViewOptionsOpen;
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
    // task E.2.4: the `View` BUTTON's screen rect max, captured BEFORE the popup. Step 9b records the
    // interactive row from THIS rather than from ImGui's last-item rect, so the recorded row cannot
    // move when the popover opens -- and it does not rest on ImGui restoring the parent's last item at
    // EndPopup, which the pinned tree does do (imgui.cpp:8848). Step 9b's comment carries the
    // measurement; I138 pins the rect equal open and closed as a regression guard.
    Vec2 viewOptionsButtonMax{};

    // Task E.1.3. UNLIKE overlayRowTopLeft/BottomRight above, these two are written at step 8b'''' and
    // read at step 9x IN THE SAME FRAME -- there is no staleness argument to make, because the layout
    // the click was tested against IS the layout that draws. `viewSnap` is the only state here that
    // spans frames, and a camera gesture, an F-focus or a retarget all cancel it.
    ViewSnapAnimation viewSnap;
    ViewAxisLayout axisLayout{};
    ViewAxisPick axisHover{};

    // Task 3.1.5.
    const MeshBoundsLookup* meshBounds = nullptr;       // published by EditorApp, borrowed, never owned
    Vec2 lastImageSizePoints{};                         // the last drawn image rect's size, POINTS -- what
                                                        // pickAt needs for the screen-space disc radius
    std::optional<ViewportAssetDrop> pendingAssetDrop;  // drained by tick(), never here
};

}  // namespace engine::editor
