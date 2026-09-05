// Aero Engine — the Material panel's live preview (task 3.4.2, D6/D-4). See material_preview.hpp for
// the INV-5 lifetime rule this file exists to hold, stated as ordering rather than as membership: the
// COLOUR TARGET is reallocated in prepareFrame, inside the draw walk and immediately before the handle
// ImGui will bind is read; everything ImGui never sees is created and destroyed only in service(),
// which runs in EditorApp::tick()'s post-draw slot, and in the destructor.
//
// This is ForwardRenderer::updateMaterial's FIRST PRODUCTION CALL SITE. 3.4.1 built that seam for this
// task by name and it has had no caller outside tests since; every session-document change reaches the
// preview through it on the next service pass, token-only changes included.
#include "material_preview.hpp"

#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/profiler.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>  // assetContentHashUsable -- named explicitly, not via the above
#include <aero/editor/material_edit.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/internal/native_device.hpp>

#include "texture_load.hpp"  // task 3.1.5: the decode->cook->parse->upload chain, extracted (§D-14)

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace engine::editor {

namespace {

// ALPHA 1.0 IS LOAD-BEARING, exactly as it is for the viewport (2.2.3's E4): ImGui's pipeline
// alpha-blends, so a 0-alpha clear would let the panel's chrome show THROUGH the preview wherever no
// geometry drew. A shade darker than the viewport's so the two are distinguishable side by side.
constexpr rhi::Color PREVIEW_CLEAR_COLOR{0.05F, 0.05F, 0.06F, 1.0F};

// The sample's own light, copied verbatim (§0.5) so validation row 3 can compare the two pictures.
constexpr Vec3 PREVIEW_LIGHT_DIRECTION{-0.5F, -1.0F, -0.3F};
constexpr float PREVIEW_LIGHT_INTENSITY = 3.0F;
constexpr Vec3 PREVIEW_AMBIENT{0.03F, 0.03F, 0.03F};
constexpr float PREVIEW_FOV_DEGREES = 60.0F;
constexpr float PREVIEW_NEAR = 0.1F;
constexpr float PREVIEW_FAR = 100.0F;

}  // namespace

MaterialPreview::MaterialPreview(rhi::Device* deviceIn) noexcept : device(deviceIn) {
#if defined(AERO_EDITOR_SHADERS)
    if (device == nullptr) {
        status = Status::Unavailable;
        reason = "Preview unavailable -- no GPU device.";
    }
#else  // -DAERO_SHADER_TOOLS=OFF (AC-32)
    // LATCHED HERE rather than in ensureInitialized, unlike the viewport's, and for a reason: this
    // preview initialises lazily on the first TARGETED, VISIBLE frame (A-9), so a build with no cooked
    // shaders would otherwise show "starting" forever on a panel with nothing selected. Knowing the
    // answer needs no device, no document and no drawn frame -- only the build configuration -- so the
    // constructor is where it belongs. No log line: ViewportPanel::ensureInitialized already warns
    // once per session that this build has no cooked shaders, and a second sentence saying the same
    // thing is noise.
    status = Status::Unavailable;
    reason = "Preview unavailable in this build (no cooked shaders).";
#endif
}

MaterialPreview::~MaterialPreview() {
    // THE EXPLICIT TEARDOWN ORDER (INV-5/AC-31), spelled out rather than left to member declaration
    // order: the TEXTURE CACHE first, then the material, then the renderer, then the target -- all
    // before ~Device, which EditorApp destroys after the panel registry. I88 runs this whole chain
    // under ASan. The textures are OURS to release even though the material referenced them: the
    // registry BORROWS a slot's texture and destroyMaterial never touches one (material.hpp).
    if (device != nullptr) {
        for (const TextureEntry& entry : textures) {
            if (entry.texture.valid()) {
                device->destroyTexture(entry.texture);
            }
        }
    }
    textures.clear();
    if (renderer && material.valid()) {
        renderer->destroyMaterial(material);
    }
    material = render::MaterialHandle{};
    renderer.reset();
    post.reset();  // task 3.6.3: after the renderer, before the target -- it owns the HDR pair
    target.reset();
}

bool MaterialPreview::prepareFrame(rhi::Extent2D pixels) {
    // (a) THE OUTSTANDING-HANDLE CHECK, and this is the one moment it can be made. Between a draw walk
    // and the ImGuiLayer::endFrame that binds what it recorded, the ONLY thing that runs is the
    // post-draw service pass -- and by the time control reaches here, that whole window has closed for
    // the previous frame. So a colour texture that is no longer the one the draw walk handed out is a
    // texture ImGui bound after RenderTarget::allocate released it. That must be impossible by
    // construction (the resize below is the only reallocation site, and it runs BEFORE the handle is
    // read); this counter is the only tier-visible witness that it stays impossible, because Metal
    // queues the container free and no sanitizer on this platform can see the defect at all.
    if (imageHandle.valid() && (!target || target->colorTexture() != imageHandle)) {
        ++staleImages;
    }
    imageHandle = {};

    requestedExtent = pixels;
    drewLastFrame = true;
    if (status != Status::Ready || !target || !post) {
        // Nothing is allocated yet (creation is lazy and happens in the service pass, A-9) or the
        // status has latched. Either way there is no texture to hand out and no resize to apply.
        // task 3.6.3: `post` is created inside ensureInitialized BEFORE `target`, so the two are
        // engaged together or neither is -- but this guard must name both, or `prepareFrame`
        // dereferences an empty optional on the first drawn frame.
        return false;
    }
    // task 3.6.3: THE TWO RESIZES ARE ADJACENT and driven by the SAME `pixels`, which is what makes
    // the resolve's 1:1 blit true BY CONSTRUCTION rather than by two call sites that could drift.
    // Reallocating the HDR target here is NOT the ordering violation the note above is about: that
    // rule is about ImGui's CONSUMPTION, and ImGui never sees this texture -- only `target`'s, whose
    // resize is the line below and must stay there.
    //
    // BOTH RESULTS ARE CHECKED AND EITHER FAILURE LATCHES, for the reason spelled out at
    // ViewportPanel::onDraw's step 5: discarding the HDR target's answer leaves renderFrame returning
    // at its own `if (!frame)` -- ABOVE the resolve -- so PostProcess's not-renderable WARN never
    // fires, RenderTarget::resize re-runs its failed allocate() and re-emits its ERROR once per frame
    // forever, and the 4 B/texel output target can still succeed where the 8 B/texel HDR pair failed,
    // handing ImGui a texture nothing rendered into.
    //
    // A real allocation failure on EITHER target. allocate() has ALREADY destroyed the previous pair,
    // so returning false here is not merely a message: it is what stops the panel binding a texture
    // that ceased to exist inside this very call. Latched, like the viewport's -- retrying every frame
    // would spend the whole frame budget failing.
    const bool sceneResized = post->resize(pixels);
    const bool outputResized = target->resize(pixels);
    if (!sceneResized || !outputResized) {
        status = Status::Unavailable;
        reason = sceneResized ? "Preview unavailable -- render target allocation failed."
                              : "Preview unavailable -- HDR scene target allocation failed.";
        return false;
    }
    imageHandle = target->colorTexture();
    if (!imageHandle.valid()) {
        return false;
    }
    ++images;
    return true;
}

void MaterialPreview::ensureInitialized([[maybe_unused]] rhi::Extent2D firstExtent) {
    if (status != Status::Uninitialized) {
        return;  // ONE attempt, latched -- ViewportPanel::ensureInitialized's rule verbatim
    }
#if defined(AERO_EDITOR_SHADERS)
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    // task 3.6.3: `post` FIRST -- it owns the HDR target the ForwardRenderer is then built against, so
    // the two are engaged together or neither is (which is what prepareFrame's `|| !post` rests on).
    post = render::PostProcess::create(*device, shaderVfs, firstExtent,
                                       {.outputColorFormat = rhi::TextureFormat::RGBA8Unorm,
                                        .outputDepthFormat = rhi::TextureFormat::Invalid,
                                        .quantum = PREVIEW_EXTENT_QUANTUM,
                                        .maxExtent = PREVIEW_MAX_EXTENT});
    if (!post) {
        status = Status::Unavailable;
        reason =
            "Preview unavailable -- post-process creation failed (are res://fullscreen.vert / res://tonemap.frag "
            "cooked?).";
        return;
    }
    // task 3.6.3: `.depth = false` -- the scene's depth lives on the HDR target inside `post`, and the
    // only thing drawn into this one is a depth-off fullscreen triangle.
    target = render::RenderTarget::create(*device, firstExtent,
                                          {.colorFormat = rhi::TextureFormat::RGBA8Unorm,
                                           .depth = false,
                                           .quantum = PREVIEW_EXTENT_QUANTUM,
                                           .maxExtent = PREVIEW_MAX_EXTENT});
    if (!target) {
        post.reset();
        status = Status::Unavailable;
        reason = "Preview unavailable -- render target creation failed.";
        return;
    }
    // task 3.6.3: built against the HDR target's formats, NOT this preview's output target's.
    // THE TWO HALVES OF THIS CALL COME FROM DIFFERENT TASKS AND BOTH ARE LOAD-BEARING. 3.6.2's side
    // of the merge read `target->depthFormat()`, which on this branch is now Invalid -- the output
    // target is `.depth = false`, because the only thing drawn into it is a depth-off fullscreen
    // triangle -- and ForwardRendererConfig REFUSES an Invalid depthFormat outright. So the formats
    // must come from `post`, and 3.6.2's shadow field rides along unchanged.
    //
    // shadowMapResolution 0 is EXACT and means OFF (task 3.6.2's D16), and this renderer never calls
    // renderShadowMap: a material preview lights a single sphere with no caster and no receiver. At
    // the 2048 default it would allocate ~16.8 MB of dead VRAM, a comparison sampler, three extra
    // shader loads and two extra pipeline compiles per editor session, for a map nothing ever writes
    // or samples. 0 shrinks the bind placeholder to 1x1, which slot 5 still needs.
    renderer = render::ForwardRenderer::create(
        *device, shaderVfs,
        {.colorFormat = post->sceneColorFormat(), .depthFormat = post->sceneDepthFormat(), .shadowMapResolution = 0});
    if (!renderer) {
        target.reset();  // created above and now unusable: released HERE, in the service pass (INV-5)
        post.reset();
        status = Status::Unavailable;
        reason = "Preview unavailable -- renderer creation failed (are res://scene.vert/.frag cooked?).";
        return;
    }
    status = Status::Ready;
#else
    // Unreachable: the constructor already latched Unavailable in this configuration.
    status = Status::Unavailable;
#endif
}

const MaterialPreview::TextureEntry* MaterialPreview::findEntry(const TextureKey& key) const noexcept {
    for (const TextureEntry& entry : textures) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

bool MaterialPreview::rebuildSlots(const MaterialDocument* document, const AssetDatabase* database) {
    bool moved = false;
    for (std::size_t i = 0; i < render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        std::optional<TextureKey> wanted;
        if (document != nullptr && database != nullptr) {
            const std::optional<MaterialTextureSlot>& slot = documentSlotAt(*document, i);
            if (slot.has_value() && slot->guid.valid()) {
                // A GUID the database does not know resolves to NO key at all: there is nothing to
                // load, the slot draws its built-in default, and the panel already names that case
                // from the record side (AC-21). The preview does not duplicate the sentence.
                const AssetRecord* record = database->findByGuid(slot->guid);
                // assetContentHashUsable is the SECOND half of that condition and it is 3.1.3's
                // ThumbnailKey rule verbatim: a record this scan did not hash keeps an ALL-ZERO digest,
                // which is the empty file's real value and not a sentinel. Keying on it would either
                // serve a stale upload forever (the zero key never moves while the file does) or
                // re-cook on every flip between zero and real. A record with no usable hash has NO KEY
                // AT ALL, so the slot shows its built-in default until a scan hashes it -- which
                // rebuildSlots notices, because it runs every service pass rather than only on a
                // document change.
                if (record != nullptr && assetContentHashUsable(*record)) {
                    wanted = TextureKey{.guid = slot->guid,
                                        .hash = record->contentHash,
                                        // D7: the colour space comes from the SLOT, mirroring
                                        // defaultTextureKindForSlot's own kind split -- the "sRGB from
                                        // usage" derivation 3.3.2 deferred for want of somewhere to put
                                        // the answer. A .ktx2 source overrides it at load time, because
                                        // a cooked artifact's colour space IS its format.
                                        .srgb = materialSlotIsSrgb(i)};
                }
            }
        }
        if (slotKeys[i] != wanted) {
            slotKeys[i] = wanted;
            moved = true;
        }
        if (wanted.has_value() && findEntry(*wanted) == nullptr) {
            // A CACHE ENTRY, not a GPU object: nothing is created on the device until loadOneTexture
            // reaches it, one per tick.
            textures.push_back(TextureEntry{.key = *wanted, .state = PreviewTextureState::Loading});
            moved = true;
        }
    }
    return moved;
}

bool MaterialPreview::loadOneTexture(const AssetDatabase* database, std::string_view assetsRootAbs) {
    // AT MOST ONE PER TICK (AC-31). The first Loading entry in insertion order, which is slot order.
    TextureEntry* pending = nullptr;
    for (TextureEntry& entry : textures) {
        if (entry.state == PreviewTextureState::Loading) {
            pending = &entry;
            break;
        }
    }
    if (pending == nullptr || database == nullptr || device == nullptr) {
        return false;
    }
    ++loadAttempts;
    // STICKY FROM HERE ON: every path below ends in Ready or Failed, and a Failed key is never
    // retried this session -- the ThumbnailLedger rule, restated. An external edit changes the
    // record's contentHash, which is a DIFFERENT key and therefore a new entry (D7).
    const AssetRecord* record = database->findByGuid(pending->key.guid);
    if (record == nullptr) {
        pending->state = PreviewTextureState::Failed;
        pending->message = "This texture is no longer in this project.";
        return false;
    }
    // THE CHAIN ITSELF LIVES IN texture_load.cpp SINCE TASK 3.1.5 (§D-14): the record lookup above and
    // the path join below stay here, because the helper knows nothing about AssetDatabase -- which is
    // exactly what lets the scene-asset ledger reach the same chain without inheriting this cache. The
    // six sentences it returns are the ones this function used to print, verbatim (§0.25).
    const std::string absolutePath = std::string(assetsRootAbs) + "/" + record->relativePath;
    const LoadedTexture loaded = loadTextureFromSourceFile(*device, absolutePath, pending->key.srgb);
    if (!loaded.error.empty()) {
        pending->state = PreviewTextureState::Failed;
        pending->message = loaded.error;
        return false;
    }
    pending->texture = loaded.texture;
    pending->state = PreviewTextureState::Ready;
    pending->message.clear();
    return true;
}

void MaterialPreview::destroyOrphans() {
    // THE ONLY PLACE A PREVIEW TEXTURE IS DESTROYED OUTSIDE THE DESTRUCTOR (A-8/INV-5), and it runs in
    // the service pass by construction: the function is private and service() is its one caller.
    if (device == nullptr) {
        return;
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < textures.size(); ++i) {
        bool desired = false;
        for (const std::optional<TextureKey>& key : slotKeys) {
            desired = desired || (key.has_value() && *key == textures[i].key);
        }
        if (desired) {
            if (kept != i) {
                textures[kept] = std::move(textures[i]);
            }
            ++kept;
            continue;
        }
        if (textures[i].texture.valid()) {
            device->destroyTexture(textures[i].texture);
        }
    }
    textures.resize(kept);
}

void MaterialPreview::pushMaterial(const MaterialDocument& document) {
    // The pure bridge does the mapping; this function only decides create-vs-update and which cached
    // upload each slot is currently entitled to. An INVALID handle is not a hole: it resolves to that
    // slot's built-in default at BIND time (material.hpp's own rule), which is exactly what "defaults
    // bound while pending or failed" means in code.
    if (!renderer) {
        return;  // defensive, and the same reasoning viewport_panel.cpp records for its own pair:
                 // service() already checked, and re-checking HERE is what makes every access below a
                 // CHECKED optional access (bugprone-unchecked-optional-access is an error on the
                 // Linux Debug lane and does not reason across functions)
    }
    const render::MaterialParams params = materialParamsFor(document);
    render::MaterialTextureSlots slots{};
    // DECLARATION ORDER IS BINDING ORDER (material.hpp's contract), so this array and
    // documentSlotAt/materialSlotAt all index the same five things in the same order.
    const std::array<render::MaterialTextureSlot*, render::MATERIAL_TEXTURE_SLOT_COUNT> bound{
        &slots.baseColor, &slots.metallicRoughness, &slots.normal, &slots.occlusion, &slots.emissive};
    for (std::size_t i = 0; i < render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        const std::optional<MaterialTextureSlot>& docSlot = documentSlotAt(document, i);
        if (!docSlot.has_value()) {
            continue;  // unbound: the built-in default, with the desc's own defaults
        }
        if (docSlot->uvSet != 0 && !warnedUvSet) {
            // AC-22, and the SAMPLE's own sentence rather than a second wording
            // (samples/phase-3-materials/main.cpp's resolveSlot): v1 honours UV set 0 because MeshVertex
            // carries one set, so a non-zero set is stored by the format for fidelity and sampled by
            // nothing. render::MaterialTextureSlot has no uvSet field at all, so nothing downstream of
            // here CAN say it -- the preview is the only place the warning can come from. LATCHED, or a
            // drag on any other field would reprint it every frame the document is re-pushed.
            AERO_LOG_WARN("editor: material preview -- uvSet {} is stored but not sampled; v1 honours set 0 only",
                          docSlot->uvSet);
            warnedUvSet = true;
            ++uvSetWarnings;
        }
        bound[i]->sampler = materialSamplerDescFor(*docSlot);
        // Bound to a NAMED LOCAL before the dereference: bugprone-unchecked-optional-access does not
        // track a has_value() test through a std::array subscript, and it is an error on the Linux
        // Debug lane.
        const std::optional<TextureKey>& key = slotKeys[i];
        if (!key.has_value()) {
            continue;
        }
        const TextureEntry* entry = findEntry(*key);
        if (entry != nullptr && entry->state == PreviewTextureState::Ready) {
            bound[i]->texture = entry->texture;
        }
    }
    if (!material.valid()) {
        material = renderer->createMaterial(params, slots);
        return;
    }
    // updateMaterial's first production call site (AC-29). The handle is stable across every edit, so
    // nothing above this line ever re-reads it.
    (void)renderer->updateMaterial(material, params, slots);
}

void MaterialPreview::renderFrame(float deltaSeconds, const render::TonemapParams& tonemap) {
    AERO_PROFILE_ZONE;
    if (!target || !renderer || !post) {
        return;  // defensive, for pushMaterial's reason exactly
    }
    // NO resize() HERE, DELIBERATELY -- and that now covers BOTH targets (task 3.6.3). The allocation
    // was settled by prepareFrame, inside the draw walk, before the handle ImGui is about to bind was
    // read -- see prepareFrame's own note for why calling it from this pass is a use-after-free on
    // Vulkan and D3D12 and invisible on Metal.
    std::optional<render::Frame> frame = post->beginScene(PREVIEW_CLEAR_COLOR);
    if (!frame) {
        return;  // a transient command-buffer miss; the next service pass tries again
    }
    orbitAngle += deltaSeconds * PREVIEW_ORBIT_SPEED;
    if (orbitAngle >= TWO_PI) {
        orbitAngle -= TWO_PI;  // bounded, so a long-running editor never loses angular precision
    }
    const rhi::Extent2D extent = frame->extent();
    const float aspect =
        extent.height != 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0F;
    const Vec3 eye{PREVIEW_ORBIT_RADIUS * std::cos(orbitAngle), PREVIEW_ORBIT_HEIGHT,
                   PREVIEW_ORBIT_RADIUS * std::sin(orbitAngle)};

    instances.resize(1);
    render::MeshInstance& sphere = instances[0];
    sphere.primitive = render::PrimitiveId::Sphere;
    sphere.model = Mat4::identity();
    // The model IS the identity, so its normal matrix is too -- transpose(inverse(I)) == I. Spelled as
    // a statement rather than left to MeshInstance's default so a future moved or scaled preview mesh
    // has to answer the question rather than inherit a stale identity.
    sphere.normalMatrix = Mat4::identity();
    sphere.color = Vec3::one();  // the scene-side tint multiplies baseColorFactor; the preview is the
                                 // MATERIAL's picture, so it contributes nothing
    sphere.material = material;

    render::RenderView view;
    view.camera = {lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
                   perspective(radians(PREVIEW_FOV_DEGREES), aspect, PREVIEW_NEAR, PREVIEW_FAR), eye};
    view.directional = {
        .direction = normalize(PREVIEW_LIGHT_DIRECTION), .color = Vec3::one(), .intensity = PREVIEW_LIGHT_INTENSITY};
    // task E.2.1: Flat at the rig's constant, intensity 1 -- EXACTLY the pre-E.2.1 ambient (the delta
    // rule), and NO SkyPass, so the preview's picture is byte-identical. Lighting it from the open
    // scene's `Environment` is E.2.4's deliverable.
    view.environment = {
        .ambientMode = render::AmbientMode::Flat, .ambientColor = PREVIEW_AMBIENT, .ambientIntensity = 1.0F};
    sphere.mvp = view.camera.proj * view.camera.view * sphere.model;
    view.instances = instances;  // BORROWED: `instances` is a member and outlives this call (F6)

    renderer->draw(*frame, view);
    post->endScene(std::move(*frame));  // submits command buffer A

    std::optional<render::Frame> outFrame = target->beginFrame(PREVIEW_CLEAR_COLOR);
    if (!outFrame) {
        return;
    }
    post->resolve(*outFrame, tonemap);
    // ++frames MOVED to the OUTPUT target's endFrame (task 3.6.3): frameCount() means "frames the
    // panel could show", and a scene pass that resolved into nothing is not one.
    if (target->endFrame(std::move(*outFrame))) {
        ++frames;
    }
}

void MaterialPreview::service(const MaterialDocument* document, bool documentChanged, const AssetDatabase* database,
                              std::string_view assetsRootAbs, float deltaSeconds,
                              const render::TonemapParams& tonemap) {
    // Consumed UNCONDITIONALLY and FIRST, before any early return -- ViewportPanel::renderScene's E2/S6
    // rule: a latch left set by a frame that returned early makes a hidden panel render one stale frame.
    const bool drew = std::exchange(drewLastFrame, false);
    // HELD, never dropped: the session's one-shot is drained every service pass by the caller, and a
    // change that arrives before the renderer exists must still reach the first push.
    pushPending = pushPending || documentChanged;
    if (device == nullptr) {
        return;
    }
    // LAZY (A-9/R2): created on the first frame that is both TARGETED and DRAWN, never at
    // EditorApp::create, so a session that never opens a material pays nothing.
    if (drew && document != nullptr) {
        ensureInitialized(requestedExtent);
    }
    // task 3.6.3: `|| !post` -- creation is lazy, so this guard runs before ensureInitialized has
    // engaged anything on early frames, and every access below must be a CHECKED optional access.
    if (status != Status::Ready || !target || !renderer || !post) {
        return;
    }
    // 1. the desired slot set, then the document -> MaterialParams push (D-4 step 1). rebuildSlots
    // runs EVERY pass, not only on a document change: an external edit to a referenced texture moves
    // that record's contentHash, which is a different key and therefore a re-load, and nothing in the
    // session's own one-shot can report that.
    const bool slotsMoved = rebuildSlots(document, database);
    if ((pushPending || slotsMoved) && document != nullptr) {
        pushMaterial(*document);
        pushPending = false;
    }
    // 2. ONE queued texture per tick (D-4 step 2 / AC-31). A completed upload moves the slot table but
    // not the handle, so the re-push is an updateMaterial rather than anything larger.
    if (loadOneTexture(database, assetsRootAbs) && document != nullptr) {
        pushMaterial(*document);
    }
    // 3. superseded and orphaned uploads -- DESTROYED HERE AND NOWHERE ELSE (A-8/INV-5).
    destroyOrphans();
    // 4. render, only on a frame the panel actually drew and only with something to show.
    if (!drew || document == nullptr || !material.valid()) {
        return;
    }
    renderFrame(deltaSeconds, tonemap);
}

bool MaterialPreview::available() const noexcept { return status == Status::Ready; }

const char* MaterialPreview::unavailableReason() const noexcept { return status == Status::Ready ? "" : reason; }

void* MaterialPreview::nativeColorTexture() const noexcept {
    if (device == nullptr || !target) {
        return nullptr;
    }
    // A READ, and the only thing onDraw is allowed to ask for (INV-5).
    return rhi::internal::NativeDeviceAccessor::texture(*device, target->colorTexture());
}

rhi::Extent2D MaterialPreview::drawExtent() const noexcept { return target ? target->drawExtent() : rhi::Extent2D{}; }

rhi::Extent2D MaterialPreview::textureExtent() const noexcept {
    return target ? target->textureExtent() : rhi::Extent2D{};
}

std::size_t MaterialPreview::frameCount() const noexcept { return frames; }

PreviewTextureState MaterialPreview::slotTextureState(std::size_t slotIndex) const noexcept {
    if (slotIndex >= slotKeys.size()) {
        return PreviewTextureState::None;
    }
    const std::optional<TextureKey>& key = slotKeys[slotIndex];  // the named-local rule, again
    if (!key.has_value()) {
        return PreviewTextureState::None;
    }
    const TextureEntry* entry = findEntry(*key);
    // A desired key with no entry yet is a Loading one by construction (rebuildSlots creates it in the
    // same pass), so "not found" reads as Loading rather than as None.
    return entry != nullptr ? entry->state : PreviewTextureState::Loading;
}

std::string_view MaterialPreview::slotNotice(std::size_t slotIndex) const noexcept {
    if (slotIndex >= slotKeys.size()) {
        return {};
    }
    const std::optional<TextureKey>& key = slotKeys[slotIndex];
    if (!key.has_value()) {
        return {};
    }
    const TextureEntry* entry = findEntry(*key);
    return entry != nullptr ? std::string_view(entry->message) : std::string_view{};
}

std::size_t MaterialPreview::readyTextureCount() const noexcept {
    std::size_t ready = 0;
    for (const TextureEntry& entry : textures) {
        if (entry.state == PreviewTextureState::Ready) {
            ++ready;
        }
    }
    return ready;
}

std::size_t MaterialPreview::textureLoadAttempts() const noexcept { return loadAttempts; }

std::size_t MaterialPreview::imageCount() const noexcept { return images; }

std::size_t MaterialPreview::staleImageCount() const noexcept { return staleImages; }

std::size_t MaterialPreview::uvSetWarnCount() const noexcept { return uvSetWarnings; }

bool MaterialPreview::blendDrawnOpaque() const noexcept { return renderer && renderer->hasWarnedBlendOpaque(); }

}  // namespace engine::editor
