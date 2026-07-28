#pragma once
// Aero Engine — src-private: the Viewport panel (task 2.2.3). TWO-PHASE by design (D3):
//   onDraw()      — inside the ImGui frame: measure, resize, ImGui::Image, record a request.
//   renderScene() — OUTSIDE the ImGui frame, called by EditorApp::tick() after the draw walk and
//                   before ImGuiLayer::endFrame(): records + submits the offscreen scene pass.
// Never call renderScene() from inside a draw walk, and never touch ImGui inside it.
#include <aero/core/vfs.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/panel.hpp>
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2: OverlaySegment, for the scratch member
#include <aero/render/render_target.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <cstdint>
#include <optional>
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

private:
    enum class Status : std::uint8_t { Uninitialized, Ready, Unavailable };

    void ensureInitialized(rhi::Extent2D firstExtent);  // D11: one attempt, latched
    void focusSelection(PanelContext& context);         // F: frame the selection, or the scene, or reset

    // Task 2.3.2. Both take POINTS (D18) as engine Vec2, never ImVec2: this header is deliberately
    // ImGui-free -- every ImGui value is converted at the ONE call site in onDraw. Both are members
    // rather than free functions because both need lastAspect and the latched `gesture`.
    void updatePick(PanelContext& context, Vec2 imageOrigin, Vec2 avail, bool hovered);
    void drawSelectionOverlay(PanelContext& context, Vec2 imageOrigin, Vec2 avail);

    rhi::Device* device = nullptr;  // non-owning; outlives the panel (EditorApp owns both)
    VirtualFileSystem shaderVfs;    // mounted once at init (AERO_SHADERS_DIR, D-user-1)
    std::optional<render::RenderTarget> target;
    std::optional<scene_render::SceneRenderer> sceneRenderer;
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
};

}  // namespace engine::editor
