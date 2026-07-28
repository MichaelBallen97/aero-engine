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

}  // namespace

render::RenderView buildRenderView(World& world, RenderViewScratch& scratch, rhi::Extent2D viewport,
                                   const render::CameraView* cameraOverride) {
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
        render::MeshInstance instance;
        instance.primitive = clampPrimitive(meshRenderer.primitive);
        instance.model = model;
        instance.normalMatrix = embed(transpose(inverse(toMat3(model))));  // correct normals under non-uniform scale
        instance.color = meshRenderer.color;
        scratch.instances.push_back(instance);  // mvp filled below, once the camera is known
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

void SceneRenderer::render(World& world, render::Frame& frame, const render::CameraView* cameraOverride) {
    AERO_PROFILE_ZONE;
    const render::RenderView view = buildRenderView(world, scratch, frame.extent(), cameraOverride);
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
    forward.draw(frame, view);  // no-ops when !view.hasCamera
}

}  // namespace engine::scene_render
