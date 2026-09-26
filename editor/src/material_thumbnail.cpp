// editor/src/material_thumbnail.cpp -- task E.4.5: the rendered material thumbnail. material_thumbnail.hpp
// states the lifetime rule this file exists to hold; every GPU call below runs inside the service pass.
#include "material_thumbnail.hpp"

#include <aero/core/math.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>     // assetContentHashUsable -- named explicitly, not via the above
#include <aero/editor/material_card.hpp>  // the FIXED rig: lighting, tonemap, orbit angle
#include <aero/editor/material_edit.hpp>  // materialParamsFor, documentSlotAt, materialSamplerDescFor, ...
#include <aero/editor/material_preview_rig.hpp>
#include <aero/editor/text_file.hpp>  // readFileBytes -- the CAPPED read
#include <aero/reflect/material_format.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/internal/native_device.hpp>

#include "texture_load.hpp"  // the decode -> cook -> parse -> upload chain; this is its THIRD consumer

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace engine::editor {

namespace {

// ALPHA 1.0 IS KEPT for PREVIEW_CLEAR_COLOR's recorded reason -- ImGui's pipeline alpha-blends -- although in
// this chain it is unobservable twice over: the sky covers every texel of the scene pass, and the resolve
// writes a literal alpha over every texel of the output. Kept because a frame whose beginScene failed must
// still clear to something.
constexpr rhi::Color MATERIAL_THUMBNAIL_CLEAR{0.05F, 0.05F, 0.06F, 1.0F};

// The slot textures ONE produce() call loaded. The material registry BORROWS a slot's texture and
// destroyMaterial never touches one (material.hpp), so they are this call's to release -- on EVERY exit,
// which is what a destructor gives and a hand-written release list at each return does not.
class SlotTextures {
public:
    explicit SlotTextures(rhi::Device& deviceIn) noexcept : device(deviceIn) {}
    ~SlotTextures() {
        for (const rhi::TextureHandle handle : handles) {
            if (handle.valid()) {
                device.destroyTexture(handle);
            }
        }
    }
    SlotTextures(const SlotTextures&) = delete;
    SlotTextures& operator=(const SlotTextures&) = delete;
    SlotTextures(SlotTextures&&) = delete;
    SlotTextures& operator=(SlotTextures&&) = delete;

    std::array<rhi::TextureHandle, render::MATERIAL_TEXTURE_SLOT_COUNT> handles{};

private:
    rhi::Device& device;
};

// Re-points the ONE material handle at the default parameters and NO slot textures when the scope ends.
// DECLARED AFTER a SlotTextures in the same scope, so it is DESTROYED BEFORE it: the registry stops naming the
// textures before they are released. Skipping it would be harmless TODAY -- every produce() re-pushes before
// it draws -- which is exactly the "true only by accident" shape E.2.1's lesson deletes; here it is true by
// construction. DEFAULT_MATERIAL_PARAMS, never MaterialParams{} (E.1.4: a glTF-default metal renders black).
class MaterialRepoint {
public:
    MaterialRepoint(render::ForwardRenderer& rendererIn, render::MaterialHandle materialIn) noexcept
        : renderer(rendererIn), material(materialIn) {}
    ~MaterialRepoint() {
        if (material.valid()) {
            (void)renderer.updateMaterial(material, render::DEFAULT_MATERIAL_PARAMS, render::MaterialTextureSlots{});
        }
    }
    MaterialRepoint(const MaterialRepoint&) = delete;
    MaterialRepoint& operator=(const MaterialRepoint&) = delete;
    MaterialRepoint(MaterialRepoint&&) = delete;
    MaterialRepoint& operator=(MaterialRepoint&&) = delete;

private:
    render::ForwardRenderer& renderer;
    render::MaterialHandle material;
};

// ONE lookup for the const and the non-const callers: `Targets` deduces to the vector or to the const
// vector, so each caller gets the matching iterator. An explicit template head, the tree's own shape
// (material_panel.cpp's tokenCombo), rather than an abbreviated `auto&` parameter.
template <typename Targets>
[[nodiscard]] auto targetLowerBound(Targets& targets, const ThumbnailKey& key) noexcept {
    return std::lower_bound(targets.begin(), targets.end(), key,
                            [](const auto& entry, const ThumbnailKey& k) { return entry.first < k; });
}

}  // namespace

MaterialThumbnailRenderer::MaterialThumbnailRenderer(rhi::Device* deviceIn) noexcept : device(deviceIn) {
#if defined(AERO_EDITOR_SHADERS)
    if (device == nullptr) {
        status = Status::Unavailable;
    }
#else  // -DAERO_SHADER_TOOLS=OFF
    // LATCHED HERE, MaterialPreview's reason: the answer needs no device, no document and no drawn frame --
    // only the build configuration. No log line: the viewport already says once per session that this build
    // has no cooked shaders.
    status = Status::Unavailable;
#endif
}

MaterialThumbnailRenderer::~MaterialThumbnailRenderer() {
    // THE EXPLICIT TEARDOWN ORDER, spelled out rather than left to member-declaration order: the per-key
    // targets, then the material, the renderer, the sky and the post-process -- all before ~Device.
    targets.clear();
    if (renderer && material.valid()) {
        renderer->destroyMaterial(material);
    }
    material = render::MaterialHandle{};
    renderer.reset();
    sky.reset();
    post.reset();
}

bool MaterialThumbnailRenderer::available() const noexcept {
    return device != nullptr && status != Status::Unavailable;
}

void MaterialThumbnailRenderer::ensureInitialized() {
    if (status != Status::Uninitialized) {
        return;  // ONE attempt, latched
    }
#if defined(AERO_EDITOR_SHADERS)
    if (device == nullptr) {
        status = Status::Unavailable;  // defensive: the constructor already latched this
        return;
    }
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    const rhi::Extent2D extent{THUMBNAIL_EDGE_TEXELS, THUMBNAIL_EDGE_TEXELS};
    // `post` FIRST: it owns the HDR target the renderer and the sky are built against. quantum 1 and
    // maxExtent THUMBNAIL_EDGE_TEXELS: the extent never changes, so nextTargetExtent answers {128, 128} on its
    // first-allocation arm and textureExtent() == drawExtent() -- the resolve's 1:1 blit by construction.
    post = render::PostProcess::create(*device, shaderVfs, extent,
                                       {.outputColorFormat = rhi::TextureFormat::RGBA8Unorm,
                                        .outputDepthFormat = rhi::TextureFormat::Invalid,
                                        .quantum = 1,
                                        .maxExtent = THUMBNAIL_EDGE_TEXELS});
    if (!post) {
        status = Status::Unavailable;
        return;
    }
    // Built against the HDR target's formats, NEVER an output target's: every output is .depth = false, so its
    // depthFormat() is Invalid, which ForwardRendererConfig REFUSES outright. shadowMapResolution 0 is EXACT and
    // means OFF (3.6.2's D16): a thumbnail has no caster and no receiver, and the 2048 default would allocate
    // ~16.8 MB of dead VRAM.
    renderer = render::ForwardRenderer::create(
        *device, shaderVfs,
        {.colorFormat = post->sceneColorFormat(), .depthFormat = post->sceneDepthFormat(), .shadowMapResolution = 0});
    if (!renderer) {
        post.reset();
        status = Status::Unavailable;
        return;
    }
    // The studio's sky, drawn BEFORE the sphere: a sphere on an undefined background has no readable
    // silhouette. ALL-OR-NOTHING with the two above.
    sky = render::SkyPass::create(*device, shaderVfs,
                                  {.colorFormat = post->sceneColorFormat(), .depthFormat = post->sceneDepthFormat()});
    if (!sky) {
        renderer.reset();
        post.reset();
        status = Status::Unavailable;
        return;
    }
    status = Status::Ready;
#else
    status = Status::Unavailable;  // unreachable: the constructor latched it in this configuration
#endif
}

ThumbnailState MaterialThumbnailRenderer::produce(const ThumbnailKey& key, std::string_view absolutePathUtf8,
                                                  const AssetDatabase& database) {
    ++attempts;  // FIRST and unconditional: a sticky refusal is still one attempt -- and the only one
    // The code-review round: A KEY THAT ALREADY HOLDS A TARGET IS ANSWERED BEFORE ANY READ AND ANY GPU WORK.
    // Only a key the ledger reports Absent reaches produce(), and releaseKey destroys a key's target before the
    // ledger forgets it, so this arm is unreachable today. It is here so that a mistake in that ordering costs a
    // stale picture -- never a render into, and a replacement of, a texture ImGui may be sampling this frame.
    if (const auto held = targetLowerBound(targets, key); held != targets.end() && held->first == key) {
        return ThumbnailState::Ready;
    }
    ensureInitialized();
    if (status != Status::Ready || device == nullptr || !post || !renderer || !sky) {
        return ThumbnailState::Skipped;  // this build or device can never render one -- STICKY in the ledger
    }
    // THE READ: the decode store's own primitive and cap, so a pathological .aeromat is refused from its size
    // alone rather than materialised.
    const FileBytesResult file = readFileBytes(absolutePathUtf8, MAX_THUMBNAIL_SOURCE_BYTES);
    if (!file.bytes.has_value()) {
        return file.refusedByCap ? ThumbnailState::Skipped : ThumbnailState::Failed;
    }
    const MaterialParseResult parsed = parseMaterial(*file.bytes);
    if (!parsed.document.has_value()) {
        return ThumbnailState::Failed;  // sticky: an edit is a new content hash, and so a new key
    }
    const MaterialDocument& document = *parsed.document;

    // THE SLOTS. A slot whose record is missing, whose bytes this scan did not hash (the preview's own
    // assetContentHashUsable rule), or whose texture fails to load is LEFT UNBOUND and the render proceeds: a
    // material with a broken normal map still has a readable base colour, and the Material panel names the
    // failure. The colour space per slot is materialSlotIsSrgb -- composed, never restated (3.4.1).
    SlotTextures loaded(*device);
    render::MaterialTextureSlots slots{};
    const std::array<render::MaterialTextureSlot*, render::MATERIAL_TEXTURE_SLOT_COUNT> bound{
        &slots.baseColor, &slots.metallicRoughness, &slots.normal, &slots.occlusion, &slots.emissive};
    for (std::size_t i = 0; i < render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        const std::optional<MaterialTextureSlot>& documentSlot = documentSlotAt(document, i);
        if (!documentSlot.has_value()) {
            continue;  // unbound: the built-in default, with the desc's own defaults
        }
        bound[i]->sampler = materialSamplerDescFor(*documentSlot);
        const AssetRecord* const record = database.findByGuid(documentSlot->guid);  // nullptr for a nil guid
        if (record == nullptr || !assetContentHashUsable(*record)) {
            continue;
        }
        const LoadedTexture texture =
            loadTextureFromSourceFile(*device, database.root() + "/" + record->relativePath, materialSlotIsSrgb(i));
        if (!texture.error.empty()) {
            continue;
        }
        loaded.handles[i] = texture.texture;  // OWNED by `loaded` from here -- released on every exit
        bound[i]->texture = texture.texture;
    }

    // THE PUSH: ONE handle, created on the first render and re-pointed per key.
    const render::MaterialParams params = materialParamsFor(document);
    if (!material.valid()) {
        material = renderer->createMaterial(params, slots);
    } else {
        (void)renderer->updateMaterial(material, params, slots);
    }
    // DECLARED AFTER `loaded`, so it runs FIRST on every exit below (D-I).
    const MaterialRepoint repoint(*renderer, material);
    if (!material.valid()) {
        return ThumbnailState::Failed;
    }

    // THE OUTPUT: this key's own target, created in the same pass that renders into it, kept only on success.
    std::optional<render::RenderTarget> output =
        render::RenderTarget::create(*device, rhi::Extent2D{THUMBNAIL_EDGE_TEXELS, THUMBNAIL_EDGE_TEXELS},
                                     {.colorFormat = rhi::TextureFormat::RGBA8Unorm,
                                      .depth = false,
                                      .quantum = 1,
                                      .maxExtent = THUMBNAIL_EDGE_TEXELS});
    if (!output) {
        return ThumbnailState::Failed;
    }

    // THE SCENE PASS, MaterialPreview::renderFrame's order: the sky, then the sphere.
    std::optional<render::Frame> frame = post->beginScene(MATERIAL_THUMBNAIL_CLEAR);
    if (!frame) {
        return ThumbnailState::Failed;
    }
    instances.resize(1);
    render::MeshInstance& sphere = instances[0];
    sphere.primitive = render::PrimitiveId::Sphere;
    sphere.model = Mat4::identity();
    sphere.normalMatrix = Mat4::identity();  // transpose(inverse(I)) == I, stated rather than inherited
    sphere.color = Vec3::one();              // the scene-side tint; a thumbnail is the MATERIAL's picture
    sphere.material = material;
    // aspect is EXACTLY 1: the target is square, and a division that could only ever produce 1.0 or a bug is
    // not written. The camera and the lighting are the RIG's -- this function states neither.
    const render::CameraView camera =
        materialPreviewCamera(MATERIAL_THUMBNAIL_RIG, MATERIAL_THUMBNAIL_ORBIT_ANGLE, 1.0F);
    const render::RenderView view = materialPreviewView(camera, materialThumbnailLighting(), instances);
    sky->draw(*frame, view);
    renderer->draw(*frame, view);
    if (!post->endScene(std::move(*frame))) {  // submits command buffer A
        return ThumbnailState::Failed;
    }

    // THE RESOLVE into this key's target (command buffer B, acquired after A was submitted).
    std::optional<render::Frame> outFrame = output->beginFrame(MATERIAL_THUMBNAIL_CLEAR);
    if (!outFrame) {
        return ThumbnailState::Failed;
    }
    post->resolve(*outFrame, materialThumbnailTonemap());
    if (!output->endFrame(std::move(*outFrame))) {
        return ThumbnailState::Failed;
    }

    // KEPT under its key. `repoint` then `loaded` release everything else as this function returns -- after
    // both submits, which is the tree's existing posture: SDL frees a released texture's CONTAINER at once and
    // its device memory only once the GPU is done with it, and nothing but this call ever held these handles.
    const auto at = targetLowerBound(targets, key);
    if (at != targets.end() && at->first == key) {
        // UNREACHABLE TWICE OVER (the code-review round): releaseKey destroys a key's target before the ledger
        // forgets the key, and the early return at the top answers a key that still holds one before any GPU
        // work -- so nothing here ever replaces a texture ImGui may be sampling.
        at->second = std::move(*output);
        return ThumbnailState::Ready;
    }
    targets.insert(at, std::pair<ThumbnailKey, render::RenderTarget>{key, std::move(*output)});
    return ThumbnailState::Ready;
}

const render::RenderTarget* MaterialThumbnailRenderer::targetFor(const ThumbnailKey& key) const noexcept {
    const auto at = targetLowerBound(targets, key);
    if (at == targets.end() || !(at->first == key)) {
        return nullptr;
    }
    return &at->second;
}

void* MaterialThumbnailRenderer::nativeTextureFor(const ThumbnailKey& key) const noexcept {
    const render::RenderTarget* const target = targetFor(key);
    if (target == nullptr || device == nullptr) {
        return nullptr;
    }
    // A READ -- the only thing a draw walk ever asks of this class.
    return rhi::internal::NativeDeviceAccessor::texture(*device, target->colorTexture());
}

void MaterialThumbnailRenderer::destroy(const ThumbnailKey& key) {
    const auto at = targetLowerBound(targets, key);
    if (at == targets.end() || !(at->first == key)) {
        return;  // a decoded key, or one this store never produced -- releaseKey calls both stores
    }
    targets.erase(at);  // ~RenderTarget releases the texture, inside the service pass
}

void MaterialThumbnailRenderer::clear() { targets.clear(); }

std::size_t MaterialThumbnailRenderer::renderAttempts() const noexcept { return attempts; }
std::size_t MaterialThumbnailRenderer::residentCount() const noexcept { return targets.size(); }

}  // namespace engine::editor
