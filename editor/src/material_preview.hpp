#pragma once
// Aero Engine — src-private: the Material panel's live preview (task 3.4.2, D6/D-4). The ONLY new GPU
// TU this task adds, and the first place in this tree where a SECOND RenderTarget and a SECOND
// ForwardRenderer are alive in the same editor frame as the viewport's.
//
// THE LIFETIME RULE (INV-5) IS STATED FIRST, BECAUSE IT IS THE ONE DEFECT CLASS THIS PLATFORM CANNOT
// SEE, AND THE CODE-REVIEW ROUND CORRECTED HOW IT IS STATED. SDL_ReleaseGPUTexture frees the texture
// CONTAINER SYNCHRONOUSLY on Vulkan and D3D12 and only queues it on Metal, so a release at the wrong
// moment is deterministic corruption on Windows and Linux and INVISIBLE on the one platform with a
// completed validation pass -- 3.1.3's BLOCKING-1, which shipped once already. The rule that follows
// from that is about ORDERING AGAINST ImGui'S CONSUMPTION, not about which function a call sits in:
//   * the COLOUR TARGET, which ImGui samples, is reallocated in prepareFrame -- inside the draw walk,
//     immediately BEFORE the handle is read, so the id ImGui records is the id ImGui binds at
//     endFrame. This is ViewportPanel::onDraw's own ordering, and moving that resize into the service
//     pass (where it originally sat) puts the release BETWEEN the record and the bind;
//   * everything ImGui never sees -- the slot upload cache, the material handle, the renderer, the
//     target itself -- is created and destroyed ONLY in service(), which EditorApp::tick() calls from
//     its post-draw slot, and in the destructor.
// I96 pins both halves; no runtime tier here can see either violated, so they are held by construction.
//
// ITS OWN ForwardRenderer IS NOT A PREFERENCE: a MaterialHandle is per-ForwardRenderer, and the
// viewport's renderer is private to its SceneRenderer, so there is no handle the two could share.
//
// LAZY AND LATCHED (A-9/R2): nothing is created until the panel has actually DRAWN a frame with a
// material targeted, so a user who never opens a material pays nothing at all. The one creation
// attempt is ViewportPanel::ensureInitialized's rule verbatim, including the
// `#if defined(AERO_EDITOR_SHADERS)` spelling -- never `#if !AERO_EDITOR_SHADERS`, which is a -Wundef
// trap for an undefined macro.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/core/vfs.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/forward_renderer.hpp>
#include <aero/render/material.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/render_target.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

// What a slot's texture is doing, for the panel's own per-slot notice row (AC-21/AC-31). `None` is
// "this slot references nothing the preview can load" -- unbound, or bound to a GUID the database does
// not know, which the panel already names from the record side.
enum class PreviewTextureState : std::uint8_t { None, Loading, Ready, Failed };

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

    // ---- called from onDraw(), and the ONE GPU operation the draw walk performs -------------------
    // Records the region the panel wants (already converted from logical points to pixels) and APPLIES
    // THE RESIZE, in that order. Returns true iff nativeColorTexture() may be handed to ImGui::Image
    // this frame.
    //
    // THE RESIZE LIVES HERE, NOT IN service(), AND THE ORDERING IS THE WHOLE POINT. ImGui RECORDS an
    // ImTextureID during the draw walk and BINDS it inside ImGuiLayer::endFrame -- which runs AFTER
    // tick()'s post-draw service pass. RenderTarget::allocate destroys the previous pair FIRST, and SDL
    // frees the texture CONTAINER immediately on Vulkan and D3D12 ("Containers are just client handles,
    // so we can destroy immediately", SDL_gpu_vulkan.c / SDL_gpu_d3d12.c), queueing it only on Metal;
    // it is the device MEMORY that is deferred, never the pointer. So a reallocation between the draw
    // walk and endFrame hands the GPU a pointer that is already freed on two of the three backends and
    // still live on the one this project has validated on. Resizing HERE, before the handle is read, is
    // ViewportPanel::onDraw's own ordering (its steps 5-8) and is why the viewport has never had this
    // defect. A destroy is still forbidden everywhere else in the draw walk -- see I96, which pins the
    // ordering rather than the function name.
    [[nodiscard]] bool prepareFrame(rhi::Extent2D pixels);

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
    // Frames on which prepareFrame handed the draw walk a texture to bind. It is what makes "the error
    // state shows NO picture" observable: a stale image under an error message is otherwise
    // indistinguishable from a correct one, because the target keeps whatever was last rendered into it.
    [[nodiscard]] std::size_t imageCount() const noexcept;
    // MUST BE ZERO, ALWAYS. Frames whose handed-out colour texture was no longer the target's live one
    // by the time the next draw walk began -- i.e. frames on which ImGui bound a released texture. See
    // prepareFrame for why no sanitizer on this platform can see that happen.
    [[nodiscard]] std::size_t staleImageCount() const noexcept;
    // AC-22's latched WARN, as a count rather than a bool, so "latched" is assertable: an unlatched
    // implementation climbs past 1 as edits re-push.
    [[nodiscard]] std::size_t uvSetWarnCount() const noexcept;

    // ---- the texture cache, as the panel and the GPU tier see it (task 3.4.2 step 7, D7) ----------
    [[nodiscard]] PreviewTextureState slotTextureState(std::size_t slotIndex) const noexcept;
    // The refusal's own reason, for the ONE notice row a failed slot shows. "" unless the slot's entry
    // is Failed.
    [[nodiscard]] std::string_view slotNotice(std::size_t slotIndex) const noexcept;
    [[nodiscard]] std::size_t readyTextureCount() const noexcept;
    // Monotonic for this preview's lifetime. It is what makes STICKY FAILURE observable at all: without
    // it, "a broken image never loads" is indistinguishable from "a broken image is retried forever and
    // fails every time" (3.1.3's ThumbnailStore::loadAttempts, for the identical reason).
    [[nodiscard]] std::size_t textureLoadAttempts() const noexcept;

private:
    enum class Status : std::uint8_t { Uninitialized, Ready, Unavailable };

    // (guid, contentHash, srgb) -- D7's key, all three parts load-bearing. The HASH is what makes an
    // external edit re-load rather than serve a stale upload; the SRGB FLAG is what makes one source
    // referenced by baseColor AND occlusion load TWICE, correctly, because a cooked artifact's colour
    // space is its format and the two slots want different ones.
    struct TextureKey {
        Guid guid;
        ContentHash hash;
        bool srgb = false;
        bool operator==(const TextureKey&) const noexcept = default;
    };
    struct TextureEntry {
        TextureKey key;
        rhi::TextureHandle texture{};
        PreviewTextureState state = PreviewTextureState::Loading;
        std::string message;  // "" unless state == Failed
    };

    void ensureInitialized(rhi::Extent2D firstExtent);  // ONE attempt, latched (A-9)
    void pushMaterial(const MaterialDocument& document);
    void renderFrame(float deltaSeconds);
    // Recomputes the five desired keys from the document + the database, creating a Loading entry for
    // each key not already cached. Returns true when the desired set MOVED, which is what tells
    // service() to re-push. A null document clears every slot, so untargeting orphans the whole cache.
    [[nodiscard]] bool rebuildSlots(const MaterialDocument* document, const AssetDatabase* database);
    // D7's chain, for AT MOST ONE Loading entry per call. Returns true only when an entry became
    // Ready -- a failure changes no slot and needs no re-push.
    [[nodiscard]] bool loadOneTexture(const AssetDatabase* database, std::string_view assetsRootAbs);
    // THE ONLY texture destroy site (INV-5). Drops every entry whose key is no longer desired.
    void destroyOrphans();
    [[nodiscard]] const TextureEntry* findEntry(const TextureKey& key) const noexcept;

    rhi::Device* device = nullptr;  // non-owning; outlives the preview (EditorApp owns both)
    VirtualFileSystem shaderVfs;    // the viewport's own mount, in a second instance
    // DECLARATION ORDER IS THE REVERSE OF THE TEARDOWN ORDER, and the destructor spells that order out
    // explicitly rather than relying on it: the texture cache and the material are released first, then
    // the renderer, then the target -- all before ~Device (AC-31's clean-shutdown clause).
    std::optional<render::RenderTarget> target;
    std::optional<render::ForwardRenderer> renderer;
    render::MaterialHandle material{};  // created on first push, then updated in place
    // The upload cache. A LINEAR-SCANNED vector, not a map: the desired set is at most five entries
    // plus whatever one frame's edit orphaned, so a binary search would be slower than the scan and a
    // node-based container would cost this type its nothrow move for nothing.
    std::vector<TextureEntry> textures;
    std::array<std::optional<TextureKey>, render::MATERIAL_TEXTURE_SLOT_COUNT> slotKeys;
    std::vector<render::MeshInstance> instances;  // a MEMBER: RenderView BORROWS the span (F6)
    rhi::Extent2D requestedExtent{};              // last recorded by prepareFrame
    // The colour texture prepareFrame last handed the draw walk, held so the NEXT prepareFrame can
    // check it survived the frame. Invalid whenever no image was handed out.
    rhi::TextureHandle imageHandle{};
    float orbitAngle = 0.0F;
    std::size_t frames = 0;         // materialPreviewFrameCount()'s source
    std::size_t loadAttempts = 0;   // textureLoadAttempts()'s source; monotonic, never reset
    std::size_t images = 0;         // imageCount()'s source; monotonic
    std::size_t staleImages = 0;    // staleImageCount()'s source; must never leave 0
    std::size_t uvSetWarnings = 0;  // uvSetWarnCount()'s source; 0 or 1 while the latch below holds
    Status status = Status::Uninitialized;
    // A string LITERAL, shown by the panel whenever !available(). Its INITIAL value is the honest
    // answer for a preview that has not been created yet: creation is lazy, so an error document or an
    // empty selection legitimately never reaches Ready and must say why rather than say "starting"
    // forever.
    const char* reason = "Preview starts when a material is loaded.";
    bool drewLastFrame = false;  // set by prepareFrame, consumed UNCONDITIONALLY by service
    bool pushPending = false;    // the session's documentChanged, held until a renderer exists
    // AC-22: uvSet != 0 is STORED by the format and honoured by nothing -- MeshVertex carries one UV
    // set, and render::MaterialTextureSlot carries no uvSet at all, so the renderer cannot say this and
    // the preview is the only place that can. Latched once per preview lifetime, exactly as
    // samples/phase-3-materials/main.cpp latches its own: a drag would otherwise print one line a frame.
    bool warnedUvSet = false;
};

}  // namespace engine::editor
