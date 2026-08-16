// Aero Engine — the Material panel's live preview (task 3.4.2, D6/D-4). See material_preview.hpp for
// the INV-5 lifetime rule this file exists to hold: every GPU create and every GPU destroy below sits
// inside service(), which runs in EditorApp::tick()'s post-draw slot and never in a draw walk.
//
// This is ForwardRenderer::updateMaterial's FIRST PRODUCTION CALL SITE. 3.4.1 built that seam for this
// task by name and it has had no caller outside tests since; every session-document change reaches the
// preview through it on the next service pass, token-only changes included.
#include "material_preview.hpp"

#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/profiler.hpp>
#include <aero/editor/material_edit.hpp>
#include <aero/rhi/internal/native_device.hpp>

#include <cmath>
#include <memory>
#include <optional>
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
    // order: the material (and, from step 7, the texture cache) first, then the renderer, then the
    // target -- all before ~Device, which EditorApp destroys after the panel registry. I88 runs this
    // whole chain under ASan.
    if (renderer && material.valid()) {
        renderer->destroyMaterial(material);
    }
    material = render::MaterialHandle{};
    renderer.reset();
    target.reset();
}

void MaterialPreview::requestFrame(rhi::Extent2D pixels) noexcept {
    // RECORD ONLY. Called from the draw walk, so there is deliberately nothing here that touches the
    // GPU, allocates, or reads the database.
    requestedExtent = pixels;
    drewLastFrame = true;
}

void MaterialPreview::ensureInitialized([[maybe_unused]] rhi::Extent2D firstExtent) {
    if (status != Status::Uninitialized) {
        return;  // ONE attempt, latched -- ViewportPanel::ensureInitialized's rule verbatim
    }
#if defined(AERO_EDITOR_SHADERS)
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    target = render::RenderTarget::create(*device, firstExtent,
                                          {.colorFormat = rhi::TextureFormat::RGBA8Unorm,
                                           .depth = true,
                                           .quantum = PREVIEW_EXTENT_QUANTUM,
                                           .maxExtent = PREVIEW_MAX_EXTENT});
    if (!target) {
        status = Status::Unavailable;
        reason = "Preview unavailable -- render target creation failed.";
        return;
    }
    renderer = render::ForwardRenderer::create(
        *device, shaderVfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    if (!renderer) {
        target.reset();  // created above and now unusable: released HERE, in the service pass (INV-5)
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

void MaterialPreview::pushMaterial(const MaterialDocument& document) {
    // The pure bridge does the mapping; this function only decides create-vs-update. Slots stay empty
    // in this step: an invalid texture handle resolves to that slot's built-in default at BIND time
    // (material.hpp's own rule), so an untextured preview needs no bookkeeping at all.
    if (!renderer) {
        return;  // defensive, and the same reasoning viewport_panel.cpp records for its own pair:
                 // service() already checked, and re-checking HERE is what makes every access below a
                 // CHECKED optional access (bugprone-unchecked-optional-access is an error on the
                 // Linux Debug lane and does not reason across functions)
    }
    const render::MaterialParams params = materialParamsFor(document);
    const render::MaterialTextureSlots slots{};
    if (!material.valid()) {
        material = renderer->createMaterial(params, slots);
        return;
    }
    // updateMaterial's first production call site (AC-29). The handle is stable across every edit, so
    // nothing above this line ever re-reads it.
    (void)renderer->updateMaterial(material, params, slots);
}

void MaterialPreview::renderFrame(float deltaSeconds) {
    AERO_PROFILE_ZONE;
    if (!target || !renderer) {
        return;  // defensive, for pushMaterial's reason exactly
    }
    if (!target->resize(requestedExtent)) {
        // A real allocation failure. Latched, like the viewport's: retrying every frame would spend the
        // whole frame budget failing.
        status = Status::Unavailable;
        reason = "Preview unavailable -- render target allocation failed.";
        return;
    }
    std::optional<render::Frame> frame = target->beginFrame(PREVIEW_CLEAR_COLOR);
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
    view.ambient = PREVIEW_AMBIENT;
    sphere.mvp = view.camera.proj * view.camera.view * sphere.model;
    view.instances = instances;  // BORROWED: `instances` is a member and outlives this call (F6)

    renderer->draw(*frame, view);
    if (target->endFrame(std::move(*frame))) {
        ++frames;
    }
}

void MaterialPreview::service(const MaterialDocument* document, bool documentChanged, const AssetDatabase* /*database*/,
                              std::string_view /*assetsRootAbs*/, float deltaSeconds) {
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
    if (status != Status::Ready || !target || !renderer) {
        return;
    }
    // 1. the document -> MaterialParams push (D-4 step 1).
    if (pushPending && document != nullptr) {
        pushMaterial(*document);
        pushPending = false;
    }
    // 2/3. the texture cache's load and its destroy pass land here at step 7 -- and NOWHERE else.
    // 4. render, only on a frame the panel actually drew and only with something to show.
    if (!drew || document == nullptr || !material.valid()) {
        return;
    }
    renderFrame(deltaSeconds);
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

bool MaterialPreview::blendDrawnOpaque() const noexcept { return renderer && renderer->hasWarnedBlendOpaque(); }

}  // namespace engine::editor
