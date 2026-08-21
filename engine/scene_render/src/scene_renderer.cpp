// engine/scene_render/src/scene_renderer.cpp — task 1.4.1: the World -> render bridge. The ONE TU in
// the tree that includes both scene component headers and render's vocabulary. buildRenderView is
// pure (no logging, no latches — see scene_renderer.hpp); SceneRenderer::render is the impure half
// that turns its diagnostic counts into once-per-lifetime latched WARNs around the pure build + draw.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace engine::scene_render {

namespace {

// >= PrimitiveId::Count (an out-of-range MeshRenderer::primitive) -> Cube (D3/AC-12).
[[nodiscard]] render::PrimitiveId clampPrimitive(std::uint32_t primitive) {
    return primitive < static_cast<std::uint32_t>(render::PrimitiveId::Count)
               ? static_cast<render::PrimitiveId>(primitive)
               : render::PrimitiveId::Cube;
}

// Embeds a Mat3 (a rotation+scale block) into the upper-left 3x3 of a Mat4, translation column
// {0,0,0,1} — MeshInstance::normalMatrix carries a Mat3's worth of data in a Mat4 slot (D8's cbuffer
// convenience: the shader reads it via a (float3x3) cast, needing no separate Mat3 push-uniform path).
[[nodiscard]] Mat4 embed(const Mat3& m) {
    Mat4 result;  // identity default
    result.columns[0] = {m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0F};
    result.columns[1] = {m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0F};
    result.columns[2] = {m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0F};
    return result;
}

// Vec4 has no .xyz() member (CORRECTION 2, plan) — the free function xyz(Vec4) does the narrowing.
[[nodiscard]] Vec3 translationOf(const Mat4& m) { return xyz(m.columns[3]); }

// AERO_LOG_WARN once per SceneRenderer lifetime — never per frame (D5/D6's "latched" WARNs).
void warnOnce(bool& latch, const char* message) {
    if (!latch) {
        AERO_LOG_WARN("{}", message);
        latch = true;
    }
}

// task 3.1.5 — which material an emitted submesh draws with. The D7 order, and THE ORDER IS THE
// SPECIFICATION:
//   1. an entity-level override that RESOLVES wins, on every submesh;
//   2. an override that does NOT resolve is COUNTED and falls through — it does not silently become
//      the submesh's own material without a trace, and it does not blank the draw;
//   3. otherwise the submesh's own bound handle, which may legitimately be INVALID (the source
//      assigned no material) and resolves to ForwardRenderer::defaultMaterial() at draw time,
//      3.4.1's contract.
// The count fires ONCE PER EMITTED SUBMESH, not once per entity: it answers "how many draws could not
// use the material they were asked for", which is the number a diagnostic reader wants — an
// entity-level count would understate a seven-submesh model by a factor of seven.
[[nodiscard]] render::MaterialHandle resolveMaterial(const MeshRenderer& meshRenderer, const MeshBindingSubmesh& sub,
                                                     const AssetBindingTable* bindings, render::RenderView& view) {
    if (meshRenderer.material.valid()) {
        const render::MaterialHandle overrideHandle =
            bindings != nullptr ? bindings->findMaterial(meshRenderer.material) : render::MaterialHandle{};
        if (overrideHandle.valid()) {
            return overrideHandle;
        }
        ++view.unresolvedMaterials;
    }
    return sub.material;
}

}  // namespace

render::RenderView buildRenderView(World& world, RenderViewScratch& scratch, rhi::Extent2D viewport,
                                   const render::CameraView* cameraOverride, const AssetBindingTable* bindings) {
    AERO_PROFILE_ZONE;
    scratch.instances.clear();
    scratch.points.clear();
    render::RenderView view;
    view.ambient = {0.03F, 0.03F, 0.03F};

    // --- renderable instances: each<Transform, MeshRenderer> (AC-6/AC-8 — no Transform => excluded) ---
    // The scene walk ALWAYS runs first, unchanged, so view.cameraCount stays filled on every path
    // below (task 2.3.1 AC-2/AC-3: it stays informational even when an override replaces the camera —
    // a future Phase 4 Game view will want it).
    world.each<Transform, MeshRenderer>([&](Entity e, Transform& /*transform*/, MeshRenderer& meshRenderer) {
        const Mat4 model = worldMatrix(world, e);
        // Hoisted above the branch, which is a PURE motion: both were computed unconditionally in the
        // pre-3.1.5 body too, and neither reads meshRenderer.
        const Mat4 normalMatrix = embed(transpose(inverse(toMat3(model))));  // correct normals under non-uniform scale

        // --- arm 1: no reference. TODAY'S PATH, BYTE FOR BYTE. Not "the fallback" — the primitive
        // path is what every pre-3.1.5 scene, both samples and every existing test exercise, and it
        // must stay observationally identical, which is INV-D3's first half.
        if (!meshRenderer.mesh.valid()) {
            render::MeshInstance instance;
            instance.primitive = clampPrimitive(meshRenderer.primitive);
            instance.model = model;
            instance.normalMatrix = normalMatrix;
            instance.color = meshRenderer.color;
            scratch.instances.push_back(instance);  // mvp filled below, once the camera is known
            return;
        }

        // --- arm 2: a reference with nothing to resolve it. ZERO instances, one count. `bindings ==
        // nullptr` is the sample/runtime/test case and is NOT an error; a missing entry is the
        // ordinary in-flight state between a drop and the ledger's upload.
        const MeshBinding* binding = bindings != nullptr ? bindings->findMesh(meshRenderer.mesh) : nullptr;
        if (binding == nullptr) {
            ++view.unresolvedMeshes;
            return;
        }

        // --- arm 3: resolved. ONE INSTANCE PER MATCHING SUBMESH. The filter is by sourceMeshIndex,
        // which is the D2 join key: one cooked container holds every mesh of the model, so an entity
        // referencing mesh 3 draws exactly the submeshes mesh 3 produced. Zero matches is a STALE
        // meshIndex and counts as unresolved — same observable as arm 2, different cause, and the
        // editor's ledger is where the cause is nameable.
        std::uint32_t emitted = 0;
        for (const MeshBindingSubmesh& sub : binding->submeshes) {
            if (sub.sourceMeshIndex != meshRenderer.meshIndex) {
                continue;
            }
            render::MeshInstance instance;
            instance.primitive = clampPrimitive(meshRenderer.primitive);  // IGNORED while `mesh` is valid; set
                                                                          // anyway so the struct never carries
                                                                          // a stale value
            instance.mesh = binding->mesh;
            instance.submesh = sub.submesh;
            instance.model = model;
            instance.normalMatrix = normalMatrix;
            instance.color = meshRenderer.color;
            instance.material = resolveMaterial(meshRenderer, sub, bindings, view);
            scratch.instances.push_back(instance);  // mvp filled below, once the camera is known
            ++emitted;
        }
        if (emitted == 0) {
            ++view.unresolvedMeshes;
        }
    });

    // --- camera: lowest entity index wins (D5) ---
    Entity camEntity{};
    const Camera* cam = nullptr;
    world.each<Camera>([&](Entity e, Camera& c) {
        ++view.cameraCount;
        if (!camEntity.valid() || e.index < camEntity.index) {
            camEntity = e;
            cam = &c;
        }
    });

    // Task 2.3.1: a three-arm decision, in this exact order. The instance loop above and the light
    // walk below are UNTOUCHED by any of the three arms (INV-4) — S7's sabotage is hoisting the light
    // walk above this resolution, which would change the empty-world diagnostics below.
    if (cameraOverride != nullptr) {
        // The override REPLACES the World's camera entirely (D3): all three CameraView fields, by
        // whole-struct assignment. `viewport` is deliberately NOT consulted here — the caller already
        // resolved the aspect into cameraOverride->proj.
        view.camera = *cameraOverride;
        view.hasCamera = true;
    } else if (!camEntity.valid()) {
        // 0 cameras (D5), no override: nothing to draw; the frame's own clear still shows. This is
        // BYTE-FOR-BYTE the pre-2.3.1 path, including the early return that skips the light walk below
        // (INV-4) — directionalCount stays 0 and pointsTruncated stays false on this arm.
        view.hasCamera = false;
        view.instances = scratch.instances;
        view.points = scratch.points;
        return view;
    } else {
        const float aspect =
            viewport.height != 0 ? static_cast<float>(viewport.width) / static_cast<float>(viewport.height) : 1.0F;
        const Mat4 camWorld = worldMatrix(world, camEntity);
        view.camera.view = inverse(camWorld);  // == viewMatrix(world, camEntity), computed once
        view.camera.proj = projectionMatrix(*cam, aspect);
        view.camera.eyePosition = translationOf(camWorld);
        view.hasCamera = true;
    }

    const Mat4 viewProj = view.camera.proj * view.camera.view;
    for (render::MeshInstance& instance : scratch.instances) {
        instance.mvp = viewProj * instance.model;
    }

    // --- lights (D6): one directional (lowest index), <= MAX_POINT_LIGHTS point lights ---
    Entity dirEntity{};
    world.each<DirectionalLight>([&](Entity le, DirectionalLight& dl) {
        ++view.directionalCount;
        if (!dirEntity.valid() || le.index < dirEntity.index) {
            dirEntity = le;
            view.directional = {normalize(transformDirection(worldMatrix(world, le), {0.0F, 0.0F, -1.0F})), dl.color,
                                dl.intensity};
        }
    });
    world.each<PointLight>([&](Entity le, PointLight& pl) {
        if (scratch.points.size() >= render::MAX_POINT_LIGHTS) {
            view.pointsTruncated = true;
            return;
        }
        scratch.points.push_back({translationOf(worldMatrix(world, le)), pl.color, pl.intensity, pl.range});
    });

    view.instances = scratch.instances;
    view.points = scratch.points;
    return view;
}

SceneRenderer::SceneRenderer(render::ForwardRenderer&& fwd) noexcept : forward(std::move(fwd)) {}

std::optional<SceneRenderer> SceneRenderer::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                   rhi::TextureFormat colorFormat, rhi::TextureFormat depthFormat,
                                                   const SceneRendererConfig& /*config*/) {
    AERO_PROFILE_ZONE;
    std::optional<render::ForwardRenderer> fwd =
        render::ForwardRenderer::create(device, shaderVfs, {.colorFormat = colorFormat, .depthFormat = depthFormat});
    if (!fwd.has_value()) {
        // D11/AC-12: a non-depth Renderer (depthFormat == Invalid) fails inside ForwardRenderer::create,
        // which already logged the ERROR — nothing to add here.
        return std::nullopt;
    }
    return SceneRenderer{std::move(*fwd)};
}

render::ForwardRenderer& SceneRenderer::renderer() noexcept { return forward; }
const render::ForwardRenderer& SceneRenderer::renderer() const noexcept { return forward; }
AssetBindingTable& SceneRenderer::bindings() noexcept { return bindingTable; }
const AssetBindingTable& SceneRenderer::bindings() const noexcept { return bindingTable; }
std::uint32_t SceneRenderer::lastUnresolvedMeshes() const noexcept { return lastUnresolvedMeshesValue; }
std::uint32_t SceneRenderer::lastUnresolvedMaterials() const noexcept { return lastUnresolvedMaterialsValue; }

void SceneRenderer::render(World& world, render::Frame& frame, const render::CameraView* cameraOverride) {
    AERO_PROFILE_ZONE;
    const render::RenderView view = buildRenderView(world, scratch, frame.extent(), cameraOverride, &bindingTable);
    if (cameraOverride == nullptr) {  // D3: an override suppresses the two CAMERA WARNs, nothing else
        if (view.cameraCount == 0) {
            warnOnce(noCameraWarned, "SceneRenderer: no Camera in world; nothing rendered");
        } else if (view.cameraCount > 1) {
            warnOnce(multiCameraWarned, "SceneRenderer: multiple Cameras; using lowest entity index");
        }
    }
    if (view.directionalCount > 1) {
        warnOnce(multiDirWarned, "SceneRenderer: multiple DirectionalLights; using lowest entity index");
    }
    if (view.pointsTruncated) {
        warnOnce(pointTruncWarned, "SceneRenderer: >MAX_POINT_LIGHTS PointLights; extras dropped");
    }
    // task 3.1.5 (D7): the two new diagnostics are LATCHED, not WARNed. Unresolved is transient BY
    // DESIGN — every frame between a drop and the ledger's upload legitimately counts nonzero — so a
    // latched WARN would fire once per session on correct behaviour and teach readers to ignore the
    // whole channel. The editor's scene-asset ledger owns the user-facing message; it is the only
    // layer that knows Loading from Failed. These two members exist because a RenderView does not
    // outlive this call.
    lastUnresolvedMeshesValue = view.unresolvedMeshes;
    lastUnresolvedMaterialsValue = view.unresolvedMaterials;
    forward.draw(frame, view);  // no-ops when !view.hasCamera
}

}  // namespace engine::scene_render
