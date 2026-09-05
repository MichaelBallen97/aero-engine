// engine/scene_render/src/scene_renderer.cpp — task 1.4.1: the World -> render bridge. The ONE TU in
// the tree that includes both scene component headers and render's vocabulary. buildRenderView is
// pure (no logging, no latches — see scene_renderer.hpp); SceneRenderer::render is the impure half
// that turns its diagnostic counts into once-per-lifetime latched WARNs around the pure build + draw.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/environment.hpp>  // task E.2.1 -- a .cpp include: the public header names no component
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
//
// task E.1.4: the last parameter is the COUNTER rather than the RenderView, so buildSelectionMaskSet
// -- which has no RenderView -- reaches the identical three-arm decision instead of carrying a copy
// that could drift. Nothing else about it moves, and SQ12 plus the untouched
// scene_render_bindings_test.cpp battery are what make that a claim rather than an assertion.
[[nodiscard]] render::MaterialHandle resolveMaterial(const MeshRenderer& meshRenderer, const MeshBindingSubmesh& sub,
                                                     const AssetBindingTable* bindings,
                                                     std::uint32_t& unresolvedMaterials) {
    if (meshRenderer.material.valid()) {
        const render::MaterialHandle overrideHandle =
            bindings != nullptr ? bindings->findMaterial(meshRenderer.material) : render::MaterialHandle{};
        if (overrideHandle.valid()) {
            return overrideHandle;
        }
        ++unresolvedMaterials;
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
            instance.material = resolveMaterial(meshRenderer, sub, bindings, view.unresolvedMaterials);
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
            // task 3.6.2: DESIGNATED, not positional. The old form was a three-value brace-init, and
            // appending four fields to DirectionalLightData would have left it compiling while the
            // four new ones silently took their DEFAULTS -- a shadow toggle that never reflects the
            // light, with every test green. Naming every field makes a future append a compile
            // error here instead of a silent one.
            view.directional = {.direction = normalize(transformDirection(worldMatrix(world, le), {0.0F, 0.0F, -1.0F})),
                                .color = dl.color,
                                .intensity = dl.intensity,
                                .castsShadows = dl.castsShadows,
                                .shadowBias = dl.shadowBias,
                                .shadowNormalBias = dl.shadowNormalBias,
                                .shadowDistance = dl.shadowDistance};
        }
    });
    // --- environment (task E.2.1): lowest entity index wins -- D5's rule, verbatim, as for the
    // camera, the directional light and the listener. NONE resolves to the defaults `view.environment`
    // already holds and is NOT a diagnostic, because a world without one is the ordinary state of
    // every scene authored before this task. It sits HERE, in the light block, so the 0-camera early
    // return above leaves environmentCount at 0 -- 2.3.1's INV-4, unchanged.
    Entity envEntity{};
    world.each<Environment>([&](Entity ee, Environment& env) {
        ++view.environmentCount;
        if (!envEntity.valid() || ee.index < envEntity.index) {
            envEntity = ee;
            // DESIGNATED, not positional (3.6.2's rule): an appended field must be a compile error
            // here, never a silent default. The two SELECTORS are CLAMPED -- the clampPrimitive rule,
            // so a hand-edited 7 renders mode 0 rather than reinterpreting a byte.
            view.environment = {.backgroundMode = render::clampBackgroundMode(env.backgroundMode),
                                .skyColor = env.skyColor,
                                .horizonColor = env.horizonColor,
                                .groundColor = env.groundColor,
                                .solidColor = env.solidColor,
                                .ambientMode = render::clampAmbientMode(env.ambientMode),
                                .ambientColor = env.ambientColor,
                                .ambientIntensity = env.ambientIntensity};
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

SelectionMaskSet buildSelectionMaskSet(World& world, std::span<const Entity> selected, Entity primary,
                                       const render::CameraView& camera, SelectionMaskScratch& scratch,
                                       const AssetBindingTable* bindings, std::size_t entityCap) {
    AERO_PROFILE_ZONE;
    scratch.secondary.clear();
    scratch.primary.clear();
    scratch.withoutGeometry.clear();
    SelectionMaskSet set;
    const Mat4 viewProj = camera.proj * camera.view;  // ONE composition, reused per instance
    std::size_t processed = 0;

    for (const Entity entity : selected) {
        // THE DEAD-HANDLE TEST COMES FIRST, and the order is not cosmetic. 2.3.2's A7 rule verbatim:
        // dead and null handles are skipped SILENTLY and do NOT advance the counter -- a stale handle
        // must not consume another entity's budget. buildSelectionOverlay tests its cap first only
        // because it BREAKS; here the cap arm counts and continues, so a run of dead handles at the
        // front would exhaust the budget and a dead handle past position 256 would be counted as
        // over-cap.
        if (!world.alive(entity)) {
            continue;
        }
        if (processed >= entityCap) {
            ++set.skippedOverCap;  // neither outlined nor markered, and COUNTED so it is not silent
            continue;
        }
        ++processed;
        // The ONLY place `primary` is consulted. A primary handle NOT in `selected` contributes
        // nothing and is not an error -- Selection guarantees the pairing, and this takes a span plus
        // a handle rather than a const Selection& so a tier-0 case can drive it from a plain array.
        //
        // CHOSEN BEFORE THE ARMS, so an entity that lands in withoutGeometry still costs its cap slot
        // -- which is buildSelectionOverlay's existing behaviour, where a marker entity consumes
        // budget exactly as a boxed one does.
        std::vector<render::MeshInstance>& bucket = (entity == primary) ? scratch.primary : scratch.secondary;

        // THE Transform CLAUSE IS NOT DEFENSIVE: buildRenderView walks each<Transform, MeshRenderer>,
        // so an entity without one is NOT DRAWN by the forward pass at all and must get a marker
        // rather than an outline that can never appear.
        if (!world.has<Transform>(entity) || !world.has<MeshRenderer>(entity)) {
            scratch.withoutGeometry.push_back(entity);
            continue;
        }
        const MeshRenderer& meshRenderer = *world.get<MeshRenderer>(entity);
        const Mat4 model = worldMatrix(world, entity);
        const Mat4 normalMatrix = embed(transpose(inverse(toMat3(model))));

        // --- arm 1: no reference -> ONE primitive instance, buildRenderView's own arm.
        if (!meshRenderer.mesh.valid()) {
            render::MeshInstance instance;
            instance.primitive = clampPrimitive(meshRenderer.primitive);
            instance.model = model;
            instance.normalMatrix = normalMatrix;
            instance.color = meshRenderer.color;
            // COMPOSED PER INSTANCE as viewProj * model, never accumulated and never read back off
            // another instance -- SQ8 recomputes it independently and would be vacuous otherwise.
            instance.mvp = viewProj * model;
            // instance.material is left DEFAULT here, exactly as buildRenderView leaves it. That is
            // E.5.1's confirmed defect and E.5.1's to fix: changing it here would make the mask
            // disagree with the picture in the one direction INV-1 forbids.
            bucket.push_back(instance);
            continue;
        }

        // --- arm 2: a reference with nothing to resolve it -> the marker list.
        const MeshBinding* binding = bindings != nullptr ? bindings->findMesh(meshRenderer.mesh) : nullptr;
        if (binding == nullptr) {
            ++set.unresolvedMeshes;
            scratch.withoutGeometry.push_back(entity);
            continue;
        }

        // --- arm 3: resolved -> ONE INSTANCE PER MATCHING SUBMESH. Instances are NOT capped: one
        // entity with seven submeshes emits seven, because the forward pass has no instance cap
        // either and inventing one here would make the mask disagree with the picture for exactly the
        // models most likely to be selected.
        std::uint32_t emitted = 0;
        for (const MeshBindingSubmesh& sub : binding->submeshes) {
            if (sub.sourceMeshIndex != meshRenderer.meshIndex) {
                continue;
            }
            render::MeshInstance instance;
            instance.primitive = clampPrimitive(meshRenderer.primitive);  // IGNORED while `mesh` is valid
            instance.mesh = binding->mesh;
            instance.submesh = sub.submesh;
            instance.model = model;
            instance.normalMatrix = normalMatrix;
            instance.color = meshRenderer.color;
            instance.material = resolveMaterial(meshRenderer, sub, bindings, set.unresolvedMaterials);
            instance.mvp = viewProj * model;
            bucket.push_back(instance);
            ++emitted;
        }
        if (emitted == 0) {
            // A STALE meshIndex: the same observable as arm 2, a different cause, and the editor's
            // ledger is where the cause is nameable.
            ++set.unresolvedMeshes;
            scratch.withoutGeometry.push_back(entity);
        }
    }

    set.secondary = scratch.secondary;
    set.primary = scratch.primary;
    set.withoutGeometry = scratch.withoutGeometry;
    return set;
}

SceneRenderer::SceneRenderer(render::ForwardRenderer&& fwd, render::SkyPass&& skyPassValue) noexcept
    : forward(std::move(fwd)), sky(std::move(skyPassValue)) {}

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
    // task E.2.1: from the SAME two formats as the ForwardRenderer, because the sky records into the
    // SAME pass. Ordered AFTER it deliberately: a sky failure destroys the already-built
    // ForwardRenderer through its own destructor on the way out, with no leak and no second ERROR --
    // SkyPass::create has already logged the cause.
    std::optional<render::SkyPass> skyPassValue =
        render::SkyPass::create(device, shaderVfs, {.colorFormat = colorFormat, .depthFormat = depthFormat});
    if (!skyPassValue.has_value()) {
        return std::nullopt;
    }
    return SceneRenderer{std::move(*fwd), std::move(*skyPassValue)};
}

render::ForwardRenderer& SceneRenderer::renderer() noexcept { return forward; }
const render::ForwardRenderer& SceneRenderer::renderer() const noexcept { return forward; }
render::SkyPass& SceneRenderer::skyPass() noexcept { return sky; }
const render::SkyPass& SceneRenderer::skyPass() const noexcept { return sky; }
AssetBindingTable& SceneRenderer::bindings() noexcept { return bindingTable; }
const AssetBindingTable& SceneRenderer::bindings() const noexcept { return bindingTable; }
std::uint32_t SceneRenderer::lastUnresolvedMeshes() const noexcept { return lastUnresolvedMeshesValue; }
std::uint32_t SceneRenderer::lastUnresolvedMaterials() const noexcept { return lastUnresolvedMaterialsValue; }

void SceneRenderer::render(World& world, render::Frame& frame, const render::CameraView* cameraOverride) {
    AERO_PROFILE_ZONE;
    // task 3.6.2 (D5/AC-51): NOT const -- view.shadow is assigned from renderShadowMap below. That
    // is the ONE channel between the two ForwardRenderer calls, and it is why there is no renderer
    // member holding a light matrix.
    render::RenderView view = buildRenderView(world, scratch, frame.extent(), cameraOverride, &bindingTable);
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
    // task E.2.1: ZERO Environments is NOT a diagnostic -- it is the ordinary state of every scene
    // authored before this task, and it renders with EnvironmentData's own defaults.
    if (view.environmentCount > 1) {
        warnOnce(multiEnvironmentWarned, "SceneRenderer: multiple Environments; using lowest entity index");
    }
    // task 3.1.5 (D7): the two new diagnostics are LATCHED, not WARNed. Unresolved is transient BY
    // DESIGN — every frame between a drop and the ledger's upload legitimately counts nonzero — so a
    // latched WARN would fire once per session on correct behaviour and teach readers to ignore the
    // whole channel. The editor's scene-asset ledger owns the user-facing message; it is the only
    // layer that knows Loading from Failed. These two members exist because a RenderView does not
    // outlive this call.
    lastUnresolvedMeshesValue = view.unresolvedMeshes;
    lastUnresolvedMaterialsValue = view.unresolvedMaterials;
    // task 3.6.2: the depth pass records onto its OWN command buffer and submits it, so it orders
    // BEFORE the frame's -- which is what lets draw() sample the map with no explicit barrier
    // (render_target.hpp's own note states the same guarantee for its colour texture). It is legal
    // inside the caller's open frame because the two passes are on different command buffers and
    // SDL's pass-in-progress guard is per command buffer.
    view.shadow = forward.renderShadowMap(view);
    // task E.2.1: THE SKY, BEFORE OPAQUE, into the caller's OPEN pass. Depth test and depth write are
    // both OFF on its pipeline, so geometry overdraws it and E.1.1's Tested debug lines still
    // depth-test against a CLEARED 1.0 wherever only sky was drawn -- which is what keeps E.1.2's grid
    // visible over the ground half. It no-ops when !view.hasCamera, exactly as draw() does, and it
    // sets no viewport and no scissor, exactly as draw() does not.
    sky.draw(frame, view);
    forward.draw(frame, view);  // no-ops when !view.hasCamera
}

}  // namespace engine::scene_render
