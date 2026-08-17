#pragma once
// Aero Engine — render mesh vocabulary (task 1.4.1): the vertex layout, primitive selector, and
// per-instance draw data ForwardRenderer consumes. All engine/rhi/math types — zero scene types
// (render stays scene-free, D2). PrimitiveId indexes the built-in procedural mesh catalog
// (render::detail::make{Cube,Sphere,Plane}()) ForwardRenderer::create() builds once.

#include <aero/core/handle.hpp>  // task 3.5.1 — MeshHandle
#include <aero/core/math.hpp>
#include <aero/render/material.hpp>  // task 3.4.1 — MeshInstance::material

#include <cstdint>
#include <span>
#include <type_traits>

namespace engine::render {

// GPU vertex layout for the built-in primitives (D9 -> task 3.4.1): position + normal + tangent + uv.
// tangent.w is the glTF handedness (+1/-1) and the bitangent is computed in-shader as
// cross(N, T.xyz) * T.w — no fifth attribute. 48-byte stride, locations 0-3, matches
// shaders/scene.vert.hlsl's VsInput and ForwardRenderer::create's four vertex attributes.
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec4 tangent;
    Vec2 uv;
};
static_assert(sizeof(MeshVertex) == 12 * sizeof(float));
static_assert(std::is_standard_layout_v<MeshVertex>);

// Selects a built-in procedural mesh. Mirrors engine::MeshRenderer::primitive's raw uint32 (scene
// cannot include render — this is the resolved, render-side counterpart the bridge maps into). The
// base type is std::uint8_t (not uint32_t, unlike the scene-side raw selector) — performance-enum-size,
// matching every other engine enum; the value set never approaches even 8 bits.
enum class PrimitiveId : std::uint8_t { Cube = 0, Sphere = 1, Plane = 2, Count };

// Phantom tag for the mesh registry's generational handle (task 3.5.1) — the rhi handles.hpp shape.
// A Handle<MeshTag> is not interchangeable with a MaterialHandle or any rhi handle at compile time.
struct MeshTag {};
using MeshHandle = Handle<MeshTag>;

// One draw: a primitive mesh plus its resolved matrices and base color. mvp/model/normalMatrix are
// filled by the caller (scene_render::buildRenderView) once the camera is known; a MeshInstance built
// before that point has an unset mvp (documented at the call site, not defended here).
struct MeshInstance {
    PrimitiveId primitive = PrimitiveId::Cube;
    // task 3.5.1 — the registered cooked mesh to draw, and which of its submeshes. DEFAULT-INVALID,
    // which draws `primitive` through the path above, byte for byte as before: every caller written
    // before the registry existed (scene_render::buildRenderView, both editor owners and both samples
    // included) compiles and draws unchanged, exactly as 3.4.1's `material` field did.
    MeshHandle mesh{};
    std::uint32_t submesh = 0;
    // The skinning matrix palette for this draw, one entry per palette slot of the mesh's skeleton.
    // BORROWED, never owned — the RenderView span rule applies to it in full: whatever it points at
    // must outlive the draw() call. EMPTY means "draw this mesh static", which for a skinned mesh is
    // the bind pose the vertices are authored in, and is therefore the right picture rather than a
    // fallback (render::computeJointPalette fills one; MAX_SKINNING_JOINTS bounds its length).
    std::span<const Mat4> palette{};
    Mat4 mvp;           // clip = proj * view * model
    Mat4 model;         // world-space position source (point-light distance)
    Mat4 normalMatrix;  // transpose(inverse(toMat3(model))) embedded in a Mat4 (upper-left 3x3 used)
    Vec3 color = Vec3::one();
    // task 3.4.1: which registered material to draw with. DEFAULT-INVALID, which resolves to
    // ForwardRenderer::defaultMaterial() at draw time — so every caller written before materials
    // existed (scene_render::buildRenderView included) compiles and draws unchanged.
    MaterialHandle material{};
};

}  // namespace engine::render
