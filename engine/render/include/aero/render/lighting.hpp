#pragma once
// Aero Engine — render lighting/camera vocabulary (task 1.4.1): the RESOLVED shapes ForwardRenderer
// draws from — world-space direction/position already computed by the bridge (scene_render). These
// are deliberately distinct from scene's engine::DirectionalLight/PointLight, which render cannot
// include (D2) — render stays scene-free.

#include <aero/core/math.hpp>
#include <aero/render/mesh.hpp>

#include <cstdint>
#include <span>

namespace engine::render {

inline constexpr std::uint32_t MAX_POINT_LIGHTS = 8;

// A resolved camera: view/proj matrices and the eye's world position (for future specular/fresnel
// terms; unused by v0's Lambert-only shading but part of the resolved shape).
struct CameraView {
    Mat4 view;
    Mat4 proj;
    Vec3 eyePosition;
};

// A resolved directional light: intensity 0 means "no directional light in the scene" (D6).
struct DirectionalLightData {
    Vec3 direction;
    Vec3 color = Vec3::one();
    float intensity = 0.0F;
    // task 3.6.2 -- mirrored field for field from engine::DirectionalLight by buildRenderView,
    // defaults included. APPENDED, so every pre-3.6.2 designated initialiser compiles unchanged --
    // but note that scene_renderer.cpp's own assignment was POSITIONAL and had to become designated,
    // because appending to a positional brace-init leaves the new fields silently at their defaults.
    bool castsShadows = true;
    float shadowBias = 0.0015F;
    float shadowNormalBias = 0.02F;
    float shadowDistance = 50.0F;
};

// A resolved point light.
struct PointLightData {
    Vec3 position;
    Vec3 color = Vec3::one();
    float intensity = 1.0F;
    float range = 10.0F;
};

// The flat bundle ForwardRenderer::draw() consumes — a render-queue snapshot with zero scene types
// (D2). instances/points are BORROWED spans (typically into a scene_render::RenderViewScratch) valid
// only while the backing storage lives and is not re-used (see buildRenderView's own contract).
// What ForwardRenderer::renderShadowMap RETURNS and draw() reads. There is deliberately NO
// ForwardRenderer member holding any of this (D5): a cached light matrix is STALE BY DEFAULT -- a
// caller who draws twice, draws a different view, or skips the shadow pass on one frame gets last
// frame's light-space transform applied to this frame's geometry, and every automated observable
// stays green while it does. Putting it on the view makes that whole class unrepresentable rather
// than tested for, and it costs 84 bytes on a struct that is already built per frame.
//
// A default-constructed ShadowView (valid == false) means "shade unshadowed", SILENTLY. A caller who
// never calls renderShadowMap gets exactly that, and it is a legitimate opt-out, not a defect.
struct ShadowView {
    Mat4 lightViewProj{};
    float texelSize = 0.0F;     // 1 / resolution -- the PCF UV step, NOT the world texel size
    float constantBias = 0.0F;  // DirectionalLight::shadowBias, verbatim
    float normalBias = 0.0F;    // DirectionalLight::shadowNormalBias, verbatim (world units)
    bool valid = false;
};

struct RenderView {
    CameraView camera;
    DirectionalLightData directional;        // intensity 0 == "no directional"
    std::span<const PointLightData> points;  // <= MAX_POINT_LIGHTS
    Vec3 ambient = Vec3{0.03F, 0.03F, 0.03F};
    std::span<const MeshInstance> instances;
    bool hasCamera = true;  // false => draw() no-ops (D5's 0-camera case); camera/instances unset then

    // Informational diagnostics filled by buildRenderView; draw() IGNORES them. SceneRenderer::render
    // turns them into latched WARNs, and tier-0 tests assert them directly (no log-sink needed, the
    // 0.2.4 deferral).
    std::uint32_t cameraCount = 0;
    std::uint32_t directionalCount = 0;
    bool pointsTruncated = false;
    // task 3.1.5: how many referencing MeshRenderers this view could NOT resolve. Both are TRANSIENT
    // BY DESIGN — every frame between a drop and the ledger's upload legitimately counts nonzero —
    // which is why SceneRenderer::render deliberately does NOT turn them into latched WARNs, unlike
    // the three above. The editor's scene-asset ledger owns the user-facing message, because it is the
    // only layer that can tell Loading from Failed.
    std::uint32_t unresolvedMeshes = 0;
    std::uint32_t unresolvedMaterials = 0;

    // task 3.6.1: per-instance frustum culling in draw(). ON by default -- every existing caller was
    // verified consistent before the default was chosen (16 draw call sites; docs/10 records the
    // survey). THE CONTRACT THIS RESTS ON: for every instance, mvp == (camera.proj * camera.view) *
    // model, with camera from THIS view -- the shader reads mvp, the cull reads model, and a caller
    // that sets one without the other gets a wrong cull rather than a wrong picture, which is
    // harder to see. cullingEnabled = false is the escape hatch for a caller that cannot honour it
    // (and the phase-3-culling sample's --no-cull A/B). An opted-out view pays NOTHING: no frustum
    // is extracted and no box is resolved.
    //
    // AND THE TRAP THAT FOLLOWS FROM THE DEFAULTS: hasCamera defaults true and CameraView's view/proj
    // default to IDENTITY, so a hand-built view that fills every instance's mvp but never assigns
    // `camera` culls against extractFrustum(identity) -- the box [-1,1]x[-1,1]x[0,1] in WORLD units,
    // whose six planes are unit-length and finite, so valid() is TRUE and no WARN fires. Everything
    // beyond about one world unit from the origin then vanishes silently. Assign `camera`, or set
    // cullingEnabled = false.
    bool cullingEnabled = true;

    // task 3.6.2: the shadow pair, appended LAST so cullingEnabled and every field before it keep
    // their position and meaning.
    //
    // shadowsEnabled is the sample's A/B and an opted-out view pays NOTHING: renderShadowMap
    // returns before it acquires a command buffer, before it walks the instances and before it fits.
    // It is deliberately NOT the same flag as cullingEnabled -- that one carries a contract about
    // mvp == (proj * view) * model, which has nothing to do with the light, and the shadow pass
    // composes lightViewProj * model itself and never reads mvp.
    //
    // `shadow` is ASSIGNED by the caller from renderShadowMap's return value, immediately before
    // draw(). The default (valid == false) shades unshadowed and warns about nothing.
    bool shadowsEnabled = true;
    ShadowView shadow{};
};

}  // namespace engine::render
