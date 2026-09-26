#pragma once
// SRC-PRIVATE (task E.4.5): the RENDERED material thumbnail -- the second producer behind the ONE
// ThumbnailLedger (ThumbnailService owns both). It is MaterialPreview's chain with three differences:
//   1. THE EXTENT IS FIXED at THUMBNAIL_EDGE_TEXELS square: no resize, no quantum hysteresis and no
//      prepareFrame/service split. MaterialPreview's draw-walk ordering rule exists because ImGui samples a
//      target that RESIZES; nothing here ever resizes, and a key's output target is created in the SAME
//      service pass that renders into it and destroyed only through the shared ledger's release path.
//   2. THE LIGHTING, THE TONEMAP AND THE ORBIT ARE FIXED (material_card.hpp), because ThumbnailKey cannot
//      express any of them. produce() takes no lighting and no tonemap, so no caller can pass one.
//   3. ONE KEY IS PRODUCED ONCE: no slot cache and no per-tick state machine. The five slot textures are
//      loaded, used and released inside ONE produce() call, by RAII (the .cpp's two guards).
//
// THE LIFETIME RULE, STATED FIRST. EVERY GPU CREATE AND EVERY GPU DESTROY BELOW HAPPENS INSIDE
// ThumbnailService::service -- which EditorApp::tick calls from its post-draw slot, never from a draw walk --
// or in clear() (a project swap, in the reconcile block, before the draw walk) or in the destructor. A key's
// output texture is destroyed during a frame ONLY through ThumbnailService::releaseKey, driven by
// ThumbnailLedger::evictions / supersededBy, BOTH of which exclude a key touched at the current frame. That
// is what makes it structurally impossible to free a texture whose native pointer this frame's
// drawAssetTileFace already wrote into the ImGui draw list: SDL frees the texture CONTAINER synchronously on
// Vulkan and D3D12 and defers only on Metal, so the defect would be deterministic on the two platforms with
// no validation pass and invisible on the one with one (3.1.3's BLOCKING-1). And a render never targets a
// texture ImGui has seen: produce() runs only for keys the ledger reports Absent, which have no texture and
// drew as a swatch this frame.
//
// ITS OWN ForwardRenderer IS NOT A PREFERENCE: a MaterialHandle is per-ForwardRenderer, so this is the THIRD
// renderer alive in an editor frame (the viewport's, the preview's, this one). LAZY AND LATCHED: nothing is
// created until the first material render, so a project with no material on screen pays nothing at all.
// NOTHING HERE LOGS: a refusal is a state, and a failed create is already named by the render layer's own
// ERROR -- once, because creation is attempted once. parseMaterial's unknown-key WARNs are the parser's own
// contract and pass through unchanged.
#include <aero/core/vfs.hpp>
#include <aero/editor/thumbnail_cache.hpp>
#include <aero/render/forward_renderer.hpp>
#include <aero/render/material.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/post_process.hpp>
#include <aero/render/render_target.hpp>
#include <aero/render/sky_pass.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- thumbnail_store.hpp's shape
}  // namespace engine::rhi

namespace engine::editor {

class AssetDatabase;  // forward-declared: the .cpp includes <aero/editor/asset_database.hpp>

class MaterialThumbnailRenderer {
public:
    MaterialThumbnailRenderer() noexcept = default;
    explicit MaterialThumbnailRenderer(rhi::Device* device) noexcept;  // nullptr == permanently unavailable
    ~MaterialThumbnailRenderer();                                      // every target, then the chain
    MaterialThumbnailRenderer(const MaterialThumbnailRenderer&) = delete;
    MaterialThumbnailRenderer& operator=(const MaterialThumbnailRenderer&) = delete;
    // DELIBERATELY IMMOVABLE, MaterialPreview's posture: ThumbnailService holds this by value and is itself
    // immovable, so nothing ever moves one -- and a deleted move makes a future accidental move a COMPILE
    // ERROR rather than a double release of a GPU handle.
    MaterialThumbnailRenderer(MaterialThumbnailRenderer&&) = delete;
    MaterialThumbnailRenderer& operator=(MaterialThumbnailRenderer&&) = delete;

    // A device, cooked shaders in this build, and no latched creation failure. TRUE before the first
    // produce() (creation is lazy) and false for ever once a create has failed.
    [[nodiscard]] bool available() const noexcept;

    // THE ONE PRODUCER, called only from ThumbnailService::service's walk. Reads and parses
    // `absolutePathUtf8`, loads its slot textures, renders one sphere, KEEPS the output target under `key`,
    // releases everything else, and returns the state the LEDGER records:
    //   Ready   -- a target now exists for `key`
    //   Failed  -- unreadable or unparseable, or a per-key GPU step failed (sticky: never retried)
    //   Skipped -- this build or device can never render one, or the file exceeds the read cap (sticky)
    // renderAttempts() counts EVERY call, first and unconditionally -- ThumbnailStore::loadAttempts' twin, and
    // what makes "a key is produced once, ever" observable at all.
    [[nodiscard]] ThumbnailState produce(const ThumbnailKey& key, std::string_view absolutePathUtf8,
                                         const AssetDatabase& database);

    [[nodiscard]] void* nativeTextureFor(const ThumbnailKey& key) const noexcept;  // nullptr unless Ready
    // READ-ONLY, for the GPU tier's pixel read -- MaterialPreview::outputTarget()'s posture. nullptr unless a
    // target exists for `key`.
    [[nodiscard]] const render::RenderTarget* targetFor(const ThumbnailKey& key) const noexcept;
    void destroy(const ThumbnailKey& key);  // a no-op for a key this store never produced
    void clear();                           // every target; the chain stays (it is project-independent)

    [[nodiscard]] std::size_t renderAttempts() const noexcept;
    [[nodiscard]] std::size_t residentCount() const noexcept;

private:
    enum class Status : std::uint8_t { Uninitialized, Ready, Unavailable };
    void ensureInitialized();  // ONE attempt, latched -- MaterialPreview::ensureInitialized's rule verbatim

    rhi::Device* device = nullptr;  // non-owning; outlives this object (the device outlives the EditorApp)
    VirtualFileSystem shaderVfs;    // the viewport's own shader mount, in a third instance
    // ALL-OR-NOTHING, created post -> renderer -> sky, and released by the destructor in the order IT spells
    // out -- never left to member-declaration order (MaterialPreview's recorded correction).
    std::optional<render::PostProcess> post;  // the HDR scene target, THUMBNAIL_EDGE_TEXELS square, with depth
    std::optional<render::ForwardRenderer> renderer;
    std::optional<render::SkyPass> sky;
    render::MaterialHandle material{};            // ONE handle, pushed per key and emptied after each render
    std::vector<render::MeshInstance> instances;  // a MEMBER: RenderView BORROWS the span
    // The per-key OUTPUT targets, SORTED by ThumbnailKey -- ThumbnailStore's container, for its reason. Each is
    // {RGBA8Unorm, depth = false, quantum = 1, maxExtent = THUMBNAIL_EDGE_TEXELS}: ONE 64 KiB texture, exactly
    // a decoded thumbnail's cost, which is why MAX_THUMBNAILS_RESIDENT and its comment stay true unedited.
    std::vector<std::pair<ThumbnailKey, render::RenderTarget>> targets;
    std::size_t attempts = 0;
    Status status = Status::Uninitialized;
};

}  // namespace engine::editor
