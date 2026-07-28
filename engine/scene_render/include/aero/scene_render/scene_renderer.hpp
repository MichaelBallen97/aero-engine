#pragma once
// Aero Engine — engine::scene_render (task 1.4.1): the World -> render bridge. This is the ONLY code
// in the tree that sees BOTH engine::scene and engine::render — it sits ABOVE both (docs/03), which is
// what keeps `render` scene-free and `scene` GPU-free (D1). Two pieces:
//   * buildRenderView — a PURE, GPU-free free function (D7) that walks a World and resolves it into a
//     render::RenderView: instances from each<Transform, MeshRenderer>, one camera (lowest entity
//     index, D5), one directional + up to MAX_POINT_LIGHTS point lights (D6), a small fixed ambient.
//     Tier-0 unit-testable with no GPU.
//   * SceneRenderer — the facade: owns a render::ForwardRenderer + reused scratch storage; one call
//     per frame (render(World&, Frame&)) builds the view and draws it, turning buildRenderView's
//     diagnostic counts into latched WARNs (once per SceneRenderer lifetime, never per frame).
//
// The component headers (Transform/Camera/Light/MeshRenderer) are .cpp includes only — this public
// header names just World + render types (spec 3.5). rhi::Device is forward-declared, exactly as
// render's own headers do; create()'s .cpp includes device.hpp.

#include <aero/render/render.hpp>  // Frame, ForwardRenderer, RenderView, MeshInstance, PointLightData, rhi::Extent2D
#include <aero/scene/world.hpp>    // World, Entity

#include <optional>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared: create() takes it by ref; scene_renderer.cpp includes device.hpp
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; scene_renderer.cpp includes vfs.hpp
}  // namespace engine

namespace engine::scene_render {

// Reusable scratch storage so buildRenderView allocates nothing after warm-up (the vectors keep their
// capacity across calls). Owned by the caller — SceneRenderer keeps one internally.
struct RenderViewScratch {
    std::vector<render::MeshInstance> instances;
    std::vector<render::PointLightData> points;
};

// PURE, GPU-free (D7): walks `world`, resolves the camera + lights + renderable instances into
// `scratch`, and returns a RenderView VIEWING it. The returned view's instances/points spans point
// INTO `scratch` — valid only while `scratch` outlives the view and is not re-used for another call
// (a second buildRenderView invalidates a prior view's spans). Non-const World& because World::each<>
// is non-const (it only reads the bound components in practice). `viewport` (typically
// Frame::extent()) supplies the projection aspect ratio. Camera/light policy: D5/D6 — 0 cameras
// yields !hasCamera (nothing drawn, the clear still shows); >1 camera/directional picks the lowest
// entity index; point lights beyond MAX_POINT_LIGHTS are dropped, in iteration order. The diagnostic
// counts (cameraCount/directionalCount/pointsTruncated) are always filled — SceneRenderer::render
// turns them into latched WARNs; tier-0 tests assert them directly.
//
// `cameraOverride` (task 2.3.1): when non-null, it REPLACES the World's camera entirely — view, proj
// AND eyePosition, all three fields, by whole-struct assignment — and every MeshInstance::mvp is
// built from ITS view-projection; `viewport` is then not consulted for aspect at all (the caller has
// already resolved it, e.g. from its own render target's drawn sub-rect). The scene walk (instances,
// cameraCount, the light walk) is UNCHANGED and UNAFFECTED by the override — `view.cameraCount` stays
// informational either way, which is what a future Game view (Phase 4) will want. When null (the
// default), behaviour is unchanged in EVERY observable respect, including the 0-camera early return
// that skips the light walk.
[[nodiscard]] render::RenderView buildRenderView(World& world, RenderViewScratch& scratch, rhi::Extent2D viewport,
                                                 const render::CameraView* cameraOverride = nullptr);

// Room for future knobs (ambient override, max lights, ...); v0 uses defaults.
struct SceneRendererConfig {};

// The facade: owns a render::ForwardRenderer + reused scratch. One call per frame draws the whole
// World. Move-only (its only non-trivial member, ForwardRenderer, has user-defined noexcept moves);
// copies deleted transitively.
class SceneRenderer {
public:
    // colorFormat/depthFormat must equal the target Renderer's renderer.colorFormat()/depthFormat() —
    // depthFormat == Invalid (a non-depth Renderer, D11) fails inside ForwardRenderer::create and
    // this returns nullopt (+ ERROR), the same as ForwardRenderer::create's own contract.
    [[nodiscard]] static std::optional<SceneRenderer> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                             rhi::TextureFormat colorFormat,
                                                             rhi::TextureFormat depthFormat,
                                                             const SceneRendererConfig& config = {});

    ~SceneRenderer() = default;
    SceneRenderer(SceneRenderer&&) noexcept = default;  // ForwardRenderer's own moves do the transfer
    SceneRenderer& operator=(SceneRenderer&&) noexcept = default;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // buildRenderView(world, internal scratch, frame.extent(), cameraOverride) + the latched D5/D6
    // WARNs + forward.draw (a no-op when the resolved view has no camera). `world` is non-const for
    // the same reason buildRenderView's is. Task 2.3.1: when `cameraOverride` is non-null, the two
    // CAMERA WARNs ("no Camera in world" / "multiple Cameras") are SUPPRESSED — the override makes
    // both moot — while the directional and point-light WARNs are UNAFFECTED.
    void render(World& world, render::Frame& frame, const render::CameraView* cameraOverride = nullptr);

private:
    explicit SceneRenderer(render::ForwardRenderer&& fwd) noexcept;  // private — create() move-constructs

    render::ForwardRenderer forward;
    RenderViewScratch scratch;
    bool noCameraWarned = false;
    bool multiCameraWarned = false;
    bool multiDirWarned = false;
    bool pointTruncWarned = false;
};

}  // namespace engine::scene_render
