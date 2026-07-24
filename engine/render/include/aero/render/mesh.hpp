#pragma once
// Aero Engine — render mesh vocabulary (task 1.4.1): the vertex layout, primitive selector, and
// per-instance draw data ForwardRenderer consumes. All engine/rhi/math types — zero scene types
// (render stays scene-free, D2). PrimitiveId indexes the built-in procedural mesh catalog
// (render::detail::make{Cube,Sphere,Plane}()) ForwardRenderer::create() builds once.

#include <aero/core/math.hpp>

#include <cstdint>
#include <type_traits>

namespace engine::render {

// GPU vertex layout for the built-in primitives (D9): position + normal, no UV/color — v0 lit
// primitives are untextured; the base color is per-object (MeshInstance::color), lighting needs the
// normal. 24-byte stride, matches shaders/scene.vert.hlsl's VsInput.
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
};
static_assert(sizeof(MeshVertex) == 6 * sizeof(float));
static_assert(std::is_standard_layout_v<MeshVertex>);

// Selects a built-in procedural mesh. Mirrors engine::MeshRenderer::primitive's raw uint32 (scene
// cannot include render — this is the resolved, render-side counterpart the bridge maps into). The
// base type is std::uint8_t (not uint32_t, unlike the scene-side raw selector) — performance-enum-size,
// matching every other engine enum; the value set never approaches even 8 bits.
enum class PrimitiveId : std::uint8_t { Cube = 0, Sphere = 1, Plane = 2, Count };

// One draw: a primitive mesh plus its resolved matrices and base color. mvp/model/normalMatrix are
// filled by the caller (scene_render::buildRenderView) once the camera is known; a MeshInstance built
// before that point has an unset mvp (documented at the call site, not defended here).
struct MeshInstance {
    PrimitiveId primitive = PrimitiveId::Cube;
    Mat4 mvp;           // clip = proj * view * model
    Mat4 model;         // world-space position source (point-light distance)
    Mat4 normalMatrix;  // transpose(inverse(toMat3(model))) embedded in a Mat4 (upper-left 3x3 used)
    Vec3 color = Vec3::one();
};

}  // namespace engine::render
