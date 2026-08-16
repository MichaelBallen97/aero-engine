#pragma once
// Aero Engine — src-private: the Material panel's live preview (task 3.4.2, D6/D-4). The ONLY new GPU
// TU this task adds, and the first place in this tree where a SECOND RenderTarget and a SECOND
// ForwardRenderer are alive in the same editor frame as the viewport's.
//
// THE LIFETIME RULE (INV-5) IS STATED FIRST, BECAUSE IT IS THE ONE DEFECT CLASS THIS PLATFORM CANNOT
// SEE. Every GPU create and every GPU destroy happens inside service(), which EditorApp::tick() calls
// from its post-draw slot; onDraw() only ever RECORDS a request and READS nativeColorTexture().
// SDL_ReleaseGPUTexture frees SYNCHRONOUSLY on Vulkan and D3D12 and defers only on Metal, so a destroy
// moved into the draw walk is deterministic corruption on Windows and Linux and INVISIBLE on the one
// platform with a completed validation pass -- 3.1.3's BLOCKING-1, which shipped once already. Its
// only witnesses are I96's source-text pin and validation row 9; no runtime tier here can see it.
//
// ITS OWN ForwardRenderer IS NOT A PREFERENCE: a MaterialHandle is per-ForwardRenderer, and the
// viewport's renderer is private to its SceneRenderer, so there is no handle the two could share.
//
// LAZY AND LATCHED (A-9/R2): nothing is created until the panel has actually DRAWN a frame with a
// material targeted, so a user who never opens a material pays nothing at all. The one creation
// attempt is ViewportPanel::ensureInitialized's rule verbatim, including the
// `#if defined(AERO_EDITOR_SHADERS)` spelling -- never `#if !AERO_EDITOR_SHADERS`, which is a -Wundef
// trap for an undefined macro.
#include <aero/core/vfs.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/forward_renderer.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/render_target.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- viewport_panel.hpp's own shape
}  // namespace engine::rhi

namespace engine::editor {

class AssetDatabase;  // task 3.4.2 step 7 resolves slot GUIDs through it, BY PARAMETER (INV-4)

// The framing, COPIED from samples/phase-3-materials/main.cpp rather than re-derived (§0.5):
// validation row 3 judges this preview against the sample's known-good look, and two different
// framings would make that comparison meaningless.
inline constexpr float PREVIEW_ORBIT_RADIUS = 3.0F;  // one unit sphere, comfortably framed
inline constexpr float PREVIEW_ORBIT_HEIGHT = 1.2F;
inline constexpr float PREVIEW_ORBIT_SPEED = 0.35F;          // rad/s -- the sample's own value and its recorded
                                                             // reason: slow enough that GGX highlights are
                                                             // judgeable by eye
inline constexpr std::uint32_t PREVIEW_EXTENT_QUANTUM = 64;  // the viewport's own quantum posture
inline constexpr std::uint32_t PREVIEW_MAX_EXTENT = 512;     // §6.4's cap on the larger axis

class MaterialPreview {
public:
    MaterialPreview() noexcept = default;
    explicit MaterialPreview(rhi::Device* device) noexcept;  // nullptr == permanently unavailable
    ~MaterialPreview();
    MaterialPreview(const MaterialPreview&) = delete;
    MaterialPreview& operator=(const MaterialPreview&) = delete;
    // DELIBERATELY IMMOVABLE. The panel owns this by value and PanelRegistry owns the panel through a
    // unique_ptr, so nothing ever moves one; declaring the moves deleted is what makes a future
    // accidental move a compile error rather than a double release of a GPU handle.
    MaterialPreview(MaterialPreview&&) = delete;
    MaterialPreview& operator=(MaterialPreview&&) = delete;

    // ---- called from onDraw(): RECORDS, and does nothing else ------------------------------------
    // No GPU call, no allocation, no destroy. `pixels` is the region the panel wants rendered, already
    // converted from logical points; service() is what acts on it.
    void requestFrame(rhi::Extent2D pixels) noexcept;

    // ---- called from EditorApp::tick()'s post-draw slot, and NOWHERE ELSE (INV-5) ------------------
    // `document` is the SESSION copy (null when untargeted or unparseable); `documentChanged` is the
    // session's drained one-shot. Renders only on frames the panel actually drew, so a tabbed-away
    // Material panel costs one early return.
    void service(const MaterialDocument* document, bool documentChanged, const AssetDatabase* database,
                 std::string_view assetsRootAbs, float deltaSeconds);

    // ---- reads -------------------------------------------------------------------------------------
    [[nodiscard]] bool available() const noexcept;                 // status == Ready
    [[nodiscard]] const char* unavailableReason() const noexcept;  // "" while Ready; never nullptr
    [[nodiscard]] void* nativeColorTexture() const noexcept;       // nullptr when not renderable
    [[nodiscard]] rhi::Extent2D drawExtent() const noexcept;       // {0,0} when not renderable
    [[nodiscard]] rhi::Extent2D textureExtent() const noexcept;    // the UV sub-rect's denominator
    [[nodiscard]] std::size_t frameCount() const noexcept;         // completed endFrame submissions
    [[nodiscard]] bool blendDrawnOpaque() const noexcept;          // the renderer's latched WARN

private:
    enum class Status : std::uint8_t { Uninitialized, Ready, Unavailable };

    void ensureInitialized(rhi::Extent2D firstExtent);  // ONE attempt, latched (A-9)
    void pushMaterial(const MaterialDocument& document);
    void renderFrame(float deltaSeconds);

    rhi::Device* device = nullptr;  // non-owning; outlives the preview (EditorApp owns both)
    VirtualFileSystem shaderVfs;    // the viewport's own mount, in a second instance
    // DECLARATION ORDER IS THE REVERSE OF THE TEARDOWN ORDER, and the destructor spells that order out
    // explicitly rather than relying on it: the texture cache and the material are released first, then
    // the renderer, then the target -- all before ~Device (AC-31's clean-shutdown clause).
    std::optional<render::RenderTarget> target;
    std::optional<render::ForwardRenderer> renderer;
    render::MaterialHandle material{};            // created on first push, then updated in place
    std::vector<render::MeshInstance> instances;  // a MEMBER: RenderView BORROWS the span (F6)
    rhi::Extent2D requestedExtent{};              // last recorded by requestFrame
    float orbitAngle = 0.0F;
    std::size_t frames = 0;  // materialPreviewFrameCount()'s source
    Status status = Status::Uninitialized;
    // A string LITERAL, shown by the panel whenever !available(). Its INITIAL value is the honest
    // answer for a preview that has not been created yet: creation is lazy, so an error document or an
    // empty selection legitimately never reaches Ready and must say why rather than say "starting"
    // forever.
    const char* reason = "Preview starts when a material is loaded.";
    bool drewLastFrame = false;  // set by requestFrame, consumed UNCONDITIONALLY by service
    bool pushPending = false;    // the session's documentChanged, held until a renderer exists
};

}  // namespace engine::editor
