#pragma once
// Aero Engine — src-private: the Viewport panel (task 2.2.3). TWO-PHASE by design (D3):
//   onDraw()      — inside the ImGui frame: measure, resize, ImGui::Image, record a request.
//   renderScene() — OUTSIDE the ImGui frame, called by EditorApp::tick() after the draw walk and
//                   before ImGuiLayer::endFrame(): records + submits the offscreen scene pass.
// Never call renderScene() from inside a draw walk, and never touch ImGui inside it.
#include <aero/core/vfs.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/panel.hpp>
#include <aero/render/render_target.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <cstdint>
#include <optional>

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
};

}  // namespace engine::editor
