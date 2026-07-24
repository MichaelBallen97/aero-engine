// engine/render/src/primitives.cpp — task 1.4.1: procedural pos+normal geometry for the built-in
// primitive catalog. Plain loops throughout (misc-no-recursion is live); Uint16 indices everywhere
// (every primitive here comfortably fits: cube 24, plane 4, sphere 289 vertices).

#include "primitives.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace engine::render::detail {
namespace {

// Appends one quad (4 vertices, 2 CCW-from-outside triangles) spanned by `u`/`v` around `center`,
// with a constant `normal` — the samples/phase-0-cube/cube_mesh.hpp construction (u x v == normal,
// right-handed; corners in order (-u-v, +u-v, +u+v, -u+v); triangles (0,1,2)/(0,2,3)), minus the
// UV/color this task's MeshVertex does not carry.
void appendQuad(PrimitiveGeometry& geometry, Vec3 center, Vec3 u, Vec3 v, Vec3 normal) {
    const Vec3 halfU = u * 0.5F;
    const Vec3 halfV = v * 0.5F;
    const std::array<Vec3, 4> corners{
        center - halfU - halfV,
        center + halfU - halfV,
        center + halfU + halfV,
        center - halfU + halfV,
    };
    const auto base = static_cast<std::uint16_t>(geometry.vertices.size());
    for (const Vec3& corner : corners) {
        geometry.vertices.push_back(MeshVertex{corner, normal});
    }
    const std::array<std::uint16_t, 6> faceIndices{
        base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
        base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3),
    };
    for (const std::uint16_t index : faceIndices) {
        geometry.indices.push_back(index);
    }
}

}  // namespace

PrimitiveGeometry makeCube() {
    struct Face {
        Vec3 normal;
        Vec3 u;
        Vec3 v;
    };
    // Same six faces as samples/phase-0-cube/cube_mesh.hpp (normal, u, v with u x v == normal).
    const std::array<Face, 6> faces{{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}},   // +X
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},   // -X
        {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},   // +Y
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},   // -Y
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},    // +Z
        {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},  // -Z
    }};

    PrimitiveGeometry geometry;
    geometry.vertices.reserve(24);
    geometry.indices.reserve(36);
    for (const Face& face : faces) {
        appendQuad(geometry, face.normal * 0.5F, face.u, face.v, face.normal);
    }
    return geometry;
}

PrimitiveGeometry makePlane() {
    // A unit quad in XZ, normal +Y: u=+Z, v=+X so u x v == normal (matching the cube's convention).
    PrimitiveGeometry geometry;
    geometry.vertices.reserve(4);
    geometry.indices.reserve(6);
    appendQuad(geometry, Vec3::zero(), Vec3{0.0F, 0.0F, 1.0F}, Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    return geometry;
}

PrimitiveGeometry makeSphere() {
    constexpr std::uint32_t RINGS = 16;
    constexpr std::uint32_t SECTORS = 16;
    constexpr float RADIUS = 0.5F;  // matches the cube's [-0.5, 0.5] scale

    PrimitiveGeometry geometry;
    geometry.vertices.reserve(static_cast<std::size_t>(RINGS + 1) * (SECTORS + 1));
    geometry.indices.reserve(static_cast<std::size_t>(RINGS) * SECTORS * 6);

    // phi: 0 (north pole, +Y) -> PI (south pole, -Y). theta: 0 -> 2*PI around Y. engine::PI/TWO_PI
    // (aero/core/math/constants.hpp) rather than a hand-rolled literal (modernize-use-std-numbers).
    for (std::uint32_t ring = 0; ring <= RINGS; ++ring) {
        const float phi = PI * static_cast<float>(ring) / static_cast<float>(RINGS);
        const float y = RADIUS * std::cos(phi);
        const float ringRadius = RADIUS * std::sin(phi);
        for (std::uint32_t sector = 0; sector <= SECTORS; ++sector) {
            const float theta = TWO_PI * static_cast<float>(sector) / static_cast<float>(SECTORS);
            const float x = ringRadius * std::cos(theta);
            const float z = ringRadius * std::sin(theta);
            const Vec3 position{x, y, z};
            // Origin-centered: normal == normalized position. RADIUS > 0 so this is never zero-length.
            geometry.vertices.push_back(MeshVertex{position, position / RADIUS});
        }
    }

    // CCW-from-outside quads (verified: cross(B-A, C-A) points outward for A/B/C in this order,
    // and cross(D-B, C-B) likewise for B/D/C) split into two triangles per ring/sector cell. The
    // pole rings produce zero-area triangles (A==B or C==D at the poles) — harmless, standard for a
    // naive UV sphere, and what yields the documented 289-vertex / 1536-index counts.
    const auto rowStride = static_cast<std::uint16_t>(SECTORS + 1);
    for (std::uint32_t ring = 0; ring < RINGS; ++ring) {
        for (std::uint32_t sector = 0; sector < SECTORS; ++sector) {
            const auto a = static_cast<std::uint16_t>((ring * rowStride) + sector);
            const auto b = static_cast<std::uint16_t>(a + 1);
            const auto c = static_cast<std::uint16_t>(a + rowStride);
            const auto d = static_cast<std::uint16_t>(c + 1);
            const std::array<std::uint16_t, 6> cell{a, b, c, b, d, c};
            for (const std::uint16_t index : cell) {
                geometry.indices.push_back(index);
            }
        }
    }
    return geometry;
}

}  // namespace engine::render::detail
