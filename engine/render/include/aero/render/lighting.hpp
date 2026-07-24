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
};

}  // namespace engine::render
