// engine/render/src/primitives.cpp — task 1.4.1: procedural geometry for the built-in primitive
// catalog, grown at task 3.4.1 to the 48-byte vertex (position + normal + tangent + uv), generated
// ANALYTICALLY rather than approximated — every primitive here has a closed-form parameterization, so
// there is nothing to solve numerically and no MikkTSpace-style pass to run. Plain loops throughout
// (misc-no-recursion is live); Uint16 indices everywhere (every primitive here comfortably fits: cube
// 24, plane 4, sphere 289 vertices).
//
// THE THREE PARAMETERIZATIONS, stated once (task 3.4.1, D10):
//   * planar quads (cube faces, plane) — texture u runs along +u, texture v runs DOWN along -v, so
//     the (0,0) texel lands on the -u+v corner. That is glTF's top-left image origin, which is also
//     the orientation the cooked texture declares (KTXorientation "rd"), so a checker reads upright.
//     The tangent is the face's own +u axis, unit by construction, with handedness w = +1.
//   * sphere — equirectangular: u = sector/SECTORS wrapping at the duplicated seam column, v =
//     ring/RINGS with v = 0 at the north pole. The tangent is the normalized dP/dtheta circle
//     direction (-sin t, 0, cos t): unit everywhere and orthogonal to the normal EXACTLY (their dot
//     product cancels algebraically), including at the poles where the positions degenerate but
//     theta does not.

#include "primitives.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace engine::render::detail {
namespace {

// Appends one quad (4 vertices, 2 CCW-from-outside triangles) spanned by `u`/`v` around `center`,
// with a constant `normal` — the samples/phase-0-cube/cube_mesh.hpp construction (u x v == normal,
// right-handed; corners in order (-u-v, +u-v, +u+v, -u+v); triangles (0,1,2)/(0,2,3)). `u` and `v`
// are unit-length axes scaled to the quad's extent; the tangent is `u` normalized, so the caller
// never has to spell it a second time and it can never disagree with the UVs.
void appendQuad(PrimitiveGeometry& geometry, Vec3 center, Vec3 u, Vec3 v, Vec3 normal) {
    const Vec3 halfU = u * 0.5F;
    const Vec3 halfV = v * 0.5F;
    const std::array<Vec3, 4> corners{
        center - halfU - halfV,
        center + halfU - halfV,
        center + halfU + halfV,
        center - halfU + halfV,
    };
    // Corner order above is (-u-v, +u-v, +u+v, -u+v); texture v runs DOWN along -v (see the header
    // note), so the -u+v corner is the image's (0,0).
    const std::array<Vec2, 4> uvs{
        Vec2{0.0F, 1.0F},
        Vec2{1.0F, 1.0F},
        Vec2{1.0F, 0.0F},
        Vec2{0.0F, 0.0F},
    };
    const Vec3 tangentAxis = normalize(u);
    const Vec4 tangent{tangentAxis.x, tangentAxis.y, tangentAxis.z, 1.0F};
    const auto base = static_cast<std::uint16_t>(geometry.vertices.size());
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        geometry.vertices.push_back(MeshVertex{corners[corner], normal, tangent, uvs[corner]});
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
    // A unit quad in XZ, normal +Y. Task 3.4.1 REORDERED the axis pair from (u=+Z, v=+X) to
    // (u=+X, v=-Z) so the tangent is +X (D10's stated convention for the plane). The normal invariant
    // still holds — (+X) x (-Z) = +Y — and the winding is construction-identical; only the order the
    // four vertices are emitted in shifts, and nothing pins that.
    PrimitiveGeometry geometry;
    geometry.vertices.reserve(4);
    geometry.indices.reserve(6);
    appendQuad(geometry, Vec3::zero(), Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
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
            // Equirectangular UVs; u wraps at the DUPLICATED seam column (which is exactly why the
            // loop emits SECTORS + 1 columns), v = 0 at the north pole per glTF's top-left origin.
            const Vec2 uv{static_cast<float>(sector) / static_cast<float>(SECTORS),
                          static_cast<float>(ring) / static_cast<float>(RINGS)};
            // The normalized dP/dtheta circle direction. Unit by construction (sin^2 + cos^2 == 1) and
            // orthogonal to the normal EXACTLY: N . T = sin(phi) * (-sin t cos t + cos t sin t) == 0,
            // which holds at the poles too, where the position degenerates but theta does not.
            const Vec4 tangent{-std::sin(theta), 0.0F, std::cos(theta), 1.0F};
            // Origin-centered: normal == normalized position. RADIUS > 0 so this is never zero-length.
            geometry.vertices.push_back(MeshVertex{position, position / RADIUS, tangent, uv});
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
