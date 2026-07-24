#pragma once
// Aero Engine — procedural built-in primitive geometry (task 1.4.1). PRIVATE to engine/render — not
// installed, not part of the public surface. ForwardRenderer::create() calls these once per primitive
// to build its GPU catalog. Position+normal only (D9); Uint16 indices throughout (every primitive's
// vertex count fits comfortably).

#include <aero/render/mesh.hpp>

#include <cstdint>
#include <vector>

namespace engine::render::detail {

struct PrimitiveGeometry {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint16_t> indices;
};

// A unit cube in [-0.5, 0.5]^3 (matching samples/phase-0-cube/cube_mesh.hpp's construction): 24
// vertices (4 per face — per-face normals need unshared corners), 36 indices, outward-CCW winding.
[[nodiscard]] PrimitiveGeometry makeCube();

// A UV sphere of radius 0.5 (matching the cube's [-0.5, 0.5] scale), centered at the origin:
// RINGS=16 x SECTORS=16 (17x17 = 289 vertices, 16*16*6 = 1536 indices — both fit Uint16), normal =
// normalized position (a unit sphere's own outward normal).
[[nodiscard]] PrimitiveGeometry makeSphere();

// A 1x1 quad in the XZ plane centered at the origin, normal +Y, 4 vertices / 6 indices, CCW viewed
// from +Y (the plane's "front").
[[nodiscard]] PrimitiveGeometry makePlane();

}  // namespace engine::render::detail
