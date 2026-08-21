#pragma once
// Aero Engine -- the scene-asset loader (task 3.1.5, §D-9). Pair 22, SRC-PRIVATE: it lives beside its
// .cpp and never under include/, because it names a ForwardRenderer and a scene_render::MeshBinding
// and a PUBLIC editor header may name neither.
//
// It executes ONE ledger directive. Everything it needs arrives as an argument; it holds no database,
// no World and no renderer -- the renderer is passed PER CALL because it belongs to the ViewportPanel
// and may not exist yet (that panel's SceneRenderer is a std::optional latched at first init), and
// because a MeshHandle and a MaterialHandle are PER-ForwardRenderer.
//
// NEVER CALLED FROM onDraw(). Every GPU create this task performs happens inside these functions, and
// they run only from EditorApp::tick()'s post-draw slot (the fifth occupant). No texture created here
// is ever handed to ImGui, so the 3.4.2 "reallocate where the handle is read" exception does NOT
// apply -- this TU has no draw-walk half at all.
//
// IT OWNS NO GPU OBJECT, AND THAT IS THE DESIGN (§0.26). Every handle it mints is handed straight to
// the caller, adopted by the scene-asset ledger, and released one whole service pass after the
// binding table stopped naming it. So this class can be destroyed at any point in EditorApp's
// teardown without reaching a renderer or a device that is already gone.
#include <aero/editor/asset_database.hpp>        // AssetRecord, AssetDatabase
#include <aero/editor/model_import.hpp>          // ImportedModel
#include <aero/editor/scene_asset_ledger.hpp>    // LedgerHandles, MaterialRuntimeState, TextureRequest
#include <aero/render/material.hpp>              // render::MaterialHandle/Params/TextureSlots
#include <aero/rhi/handles.hpp>                  // rhi::TextureHandle
#include <aero/scene_render/asset_bindings.hpp>  // scene_render::MeshBinding

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- texture_load.hpp's own shape
}  // namespace engine::rhi

namespace engine::render {
class ForwardRenderer;  // forward-declared: the .cpp includes forward_renderer.hpp
}  // namespace engine::render

namespace engine::editor {

// The ONE sentence an unconverted .blend produces, wherever it is produced. Both consumers of the
// cache-hit read below hand it to the user verbatim, so the instruction cannot drift between them.
inline constexpr std::string_view BLEND_UNCONVERTED_MESSAGE =
    "this .blend has not been converted on this machine yet -- select it in the Asset Browser and use "
    "Import Details to convert it";

// The .blend cache-HIT read, shared by the ledger's directive and the drop's own import (§D-19 step 2)
// rather than restated in either -- two copies of a cache-validity rule is how a cache silently stops
// invalidating (§0.9). It reads <projectRoot>/Library/BlenderExports/<guid>.json, compares it against
// the record's own hash, this build's BLENDER_SCRIPT_VERSION and blendExportSettingsFingerprint, and
// on a hit reads <guid>.glb.
//
// `expected.blenderVersion` IS LEFT EMPTY ON PURPOSE and provenanceMatches' conditional arm documents
// why: comparing a version nothing probed would require probing, and a probe is a process. So this
// function SPAWNS NOTHING, EVER -- AC-24's "no second Blender service" is held by construction here.
// The mechanical check is a grep for that service's type name over editor/src, whose count must not
// move for this task; the name is therefore deliberately NOT spelled anywhere in this pair, because
// that grep does not strip comments and a prose mention would turn it red for no violation (the
// currentHostOs / platform-macro lesson, one subsystem over).
struct BlendArtifactResult {
    bool ok = false;
    std::string message;       // "" iff ok; BLEND_UNCONVERTED_MESSAGE on every miss
    std::string bytes;         // the <guid>.glb bytes; empty unless ok. std::string is the BYTE container.
    std::string artifactLeaf;  // "<guid>.glb" -- a real .glb NAME, so importModel's dispatch routes it
                               // to the glTF backend exactly as any other GLB
};
[[nodiscard]] BlendArtifactResult readBlendCacheArtifact(const AssetRecord& record, std::string_view projectRootAbs);

class SceneAssetLoader {
public:
    explicit SceneAssetLoader(rhi::Device& device) noexcept;

    struct ModelLoadResult {
        bool ok = false;
        std::string message;                          // "" iff ok
        LedgerHandles handles;                        // mesh, materials, materialStates, bounds
        scene_render::MeshBinding binding;            // ready to hand to AssetBindingTable::setMesh
        std::vector<TextureRequest> textureRequests;  // (materialIndex, slot, guid, srgb) per bound slot
        // RETURNED, never printed (§D-9 step 8): this TU does not log, so the material mapping's own
        // omission/clamp warnings travel back to the caller that owns the Console.
        std::vector<std::string> warnings;
    };
    struct MaterialLoadResult {
        bool ok = false;
        std::string message;
        render::MaterialHandle material{};
        // The params/slots pair the ledger stores parallel to its `materials` vector: ForwardRenderer
        // offers no read-back, so nothing could rebind an arriving slot texture without it.
        MaterialRuntimeState state;
        std::vector<TextureRequest> textureRequests;
        std::vector<std::string> warnings;
    };
    struct TextureLoadResult {
        bool ok = false;
        std::string message;
        rhi::TextureHandle texture{};  // owned by the LEDGER entry once reported
    };

    // Read + import, then loadFromImportedModel. `projectRootAbs` is needed only for the .blend fork.
    [[nodiscard]] ModelLoadResult loadModel(const AssetRecord& record, std::string_view assetsRootAbs,
                                            std::string_view projectRootAbs, const AssetDatabase& database,
                                            render::ForwardRenderer& renderer);
    // The drop's own Full import, handed straight to the cook -> upload half, so a dropped model does
    // not import TWICE in one gesture (once to plan, once to load). The caller must already have run
    // assignImageGuids over `model.images` -- this half never sees a database.
    [[nodiscard]] ModelLoadResult loadFromImportedModel(const ImportedModel& model, const AssetRecord& record,
                                                        render::ForwardRenderer& renderer);
    [[nodiscard]] MaterialLoadResult loadMaterial(const AssetRecord& record, std::string_view assetsRootAbs,
                                                  render::ForwardRenderer& renderer);
    [[nodiscard]] TextureLoadResult loadSlotTexture(const AssetRecord& record, std::string_view assetsRootAbs,
                                                    bool srgb);
    // Rebinds one arrived texture into a live material -- ForwardRenderer::updateMaterial's SECOND
    // production call site, after the material preview's. `slots` is the ledger's own copy and is
    // written in place, so the next rebind of a sibling slot keeps this one.
    void rebindSlot(render::ForwardRenderer& renderer, render::MaterialHandle material,
                    const render::MaterialParams& params, render::MaterialTextureSlots& slots, std::size_t slot,
                    rhi::TextureHandle texture);

    // Imports this loader itself performed, and meshes it handed to createMesh. Both LIFETIME totals:
    // they exist so a retry loop reads as a climbing count rather than as a suspicion.
    [[nodiscard]] std::size_t importCount() const noexcept;
    [[nodiscard]] std::size_t meshUploadCount() const noexcept;
    [[nodiscard]] std::size_t textureFailureCount() const noexcept;

private:
    // §0.21's key, byte for byte the material preview's: the same source in two colour spaces is two
    // keys, and `hash` enters it ONLY when assetContentHashUsable(record) -- an unhashed record's
    // all-zero digest is the empty file's real value and never a sentinel (the 3.4.2 lesson), so such
    // a record is not keyed at all and simply reloads.
    struct TextureFailureKey {
        Guid guid;
        ContentHash hash;
        bool srgb = false;
        [[nodiscard]] bool operator==(const TextureFailureKey&) const noexcept = default;
    };
    // THE CACHE IS NEGATIVE, AND THIS IS A RECORDED DEVIATION FROM §0.21, taken because §0.21 and
    // §D-8 cannot both hold. §0.21 describes a refcounted cache of LIVE handles; §D-8's ledger --
    // already built -- ADOPTS every reported texture into the owning entry's LedgerHandles::textures
    // and releases it on the deferred-destroy list. Serving one handle to two directives (two
    // materials of one model naming the same image in the same colour space is ordinary) would give
    // that handle TWO owners and one premature destroy, with a live material still sampling it. So a
    // SUCCESS is never shared -- it uploads once per directive, exactly the cost 3.4.1's ORM-atlas
    // rule already accepts -- and only a FAILURE is remembered, which is the half that matters: a
    // broken image then costs one decode per key per session instead of one per service pass (the
    // ThumbnailLedger stickiness rule). It is also what makes "this loader owns no GPU object"
    // (§0.26's shutdown-ordering requirement) true rather than merely intended.
    struct TextureFailure {
        TextureFailureKey key;
        std::string message;
    };
    [[nodiscard]] const TextureFailure* findFailure(const TextureFailureKey& key) const noexcept;

    rhi::Device* device = nullptr;
    std::vector<TextureFailure> textureFailures;
    std::size_t imports = 0;
    std::size_t meshUploads = 0;
};

}  // namespace engine::editor
