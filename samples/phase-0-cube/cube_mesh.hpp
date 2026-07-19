#pragma once
// Aero Engine — Phase 0 cube geometry + procedural texture (task 0.5.2, sample-local). Header-only;
// depends only on the engine math umbrella + the standard library — no engine-render/rhi type, no
// third-party type (rule #3). The six-face, 24-vertex, 36-index unit cube and its checkerboard
// texture are throwaway Phase-0 fixture data (0.5.2 spec D9); the mesh/material/asset systems that
// replace it arrive in Phase 2. Not shared with tests/render_cube_test.cpp on purpose (D9) — the
// authoritative contract is the vertex layout, and it lives in shaders/cube.vert.hlsl.
#include <aero/core/math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace cube {

// GPU vertex layout: position (offset 0), uv (offset 12), color (offset 20). Tightly packed, 32
// bytes — matches shaders/cube.vert.hlsl's VsInput (TEXCOORD0/1/2 -> VertexAttribute location 0/1/2).
struct Vertex {
    engine::Vec3 position;
    engine::Vec2 uv;
    engine::Vec3 color;
};
static_assert(sizeof(Vertex) == 32);
static_assert(std::is_standard_layout_v<Vertex>);

struct CubeMesh {
    std::array<Vertex, 24> vertices;
    std::array<std::uint16_t, 36> indices;
};

// A unit cube in [-0.5, 0.5]^3: 24 vertices (4 per face — per-face UV + per-face color need
// unshared corners), 36 Uint16 indices, outward-CCW winding (the default CullMode::Back +
// FrontFace::CounterClockwise culls the inside). Six faces, each described by (normal, u, v, color)
// with u x v == normal (right-handed, verified); the four corners are center +/- u*0.5 +/- v*0.5 in
// the order (-u-v, +u-v, +u+v, -u+v), with UVs (0,0), (1,0), (1,1), (0,1).
inline CubeMesh makeCube() {
    struct Face {
        engine::Vec3 normal;
        engine::Vec3 u;
        engine::Vec3 v;
        engine::Vec3 color;
    };
    const std::array<Face, 6> faces{{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {0.90F, 0.30F, 0.30F}},   // +X red
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}, {0.30F, 0.80F, 0.80F}},   // -X cyan
        {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.40F, 0.85F, 0.40F}},   // +Y green
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.90F, 0.40F, 0.85F}},   // -Y magenta
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.35F, 0.55F, 0.95F}},    // +Z blue
        {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.95F, 0.85F, 0.35F}},  // -Z yellow
    }};

    CubeMesh mesh{};
    for (std::size_t face = 0; face < faces.size(); ++face) {
        const Face& f = faces[face];
        const engine::Vec3 center = f.normal * 0.5F;
        const engine::Vec3 halfU = f.u * 0.5F;
        const engine::Vec3 halfV = f.v * 0.5F;
        const std::array<engine::Vec3, 4> corners{
            center - halfU - halfV,
            center + halfU - halfV,
            center + halfU + halfV,
            center - halfU + halfV,
        };
        const std::array<engine::Vec2, 4> uvs{
            engine::Vec2{0.0F, 0.0F},
            engine::Vec2{1.0F, 0.0F},
            engine::Vec2{1.0F, 1.0F},
            engine::Vec2{0.0F, 1.0F},
        };
        const std::size_t base = face * 4;
        for (std::size_t corner = 0; corner < 4; ++corner) {
            mesh.vertices[base + corner] = Vertex{corners[corner], uvs[corner], f.color};
        }
        const auto b = static_cast<std::uint16_t>(base);
        const std::array<std::uint16_t, 6> faceIndices{
            b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2),
            b, static_cast<std::uint16_t>(b + 2), static_cast<std::uint16_t>(b + 3),
        };
        for (std::size_t i = 0; i < 6; ++i) {
            mesh.indices[(face * 6) + i] = faceIndices[i];
        }
    }
    return mesh;
}

// A size x size RGBA8 checkerboard (grey/white, so a per-face color tint stays legible in both
// cells): checkPixels x checkPixels squares alternate. Defaults: 256x256, 32px squares (8x8 board).
inline std::vector<std::byte> makeCheckerboard(std::uint32_t size = 256, std::uint32_t checkPixels = 32) {
    std::vector<std::byte> pixels(static_cast<std::size_t>(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool white = (((x / checkPixels) + (y / checkPixels)) & 1U) != 0U;
            const std::byte value = white ? std::byte{240} : std::byte{120};
            const std::size_t idx = ((static_cast<std::size_t>(y) * size) + x) * 4;
            pixels[idx + 0] = value;
            pixels[idx + 1] = value;
            pixels[idx + 2] = value;
            pixels[idx + 3] = std::byte{255};
        }
    }
    return pixels;
}

}  // namespace cube
