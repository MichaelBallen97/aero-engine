// tests/render_material_test.cpp — task 3.4.1: the render-side material system (PB*).
//
// Tier 0 (this section, no GPU, every lane): the primitive generators' tangent/UV invariants.
// Later steps grow this TU with the cooked-texture format mapping, the upload-size cross-check, and
// the GPU-gated registry / bridge cases.
//
// primitives.hpp is PRIVATE to engine/render (src/, never installed), so it is reached by a relative
// include — the tests/editor/blender_service_test.cpp precedent. The SYMBOLS come from aero_render,
// which aero_tests already links; no link-line and no include-directory change.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md). Enum comparisons
// use double parentheses, because engine::rhi::toString is found by ADL from doctest's stringifier.

#include <aero/render/render.hpp>

#include "../engine/render/src/primitives.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <ostream>
#include <string_view>

using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::render::MeshVertex;

namespace {

// The generator invariants a broken tangent frame must trip (task 3.4.1, R6). Tolerances are loose
// enough for float trig at 17 sphere rings and tight enough that a WRONG axis (the S21 seed: sphere
// tangents forced to +X, whose dot with the normal reaches 1) cannot slip through.
constexpr float ORTHOGONALITY_TOLERANCE = 1e-4F;
constexpr float UNIT_TOLERANCE = 1e-4F;

void checkVertexInvariants(const MeshVertex& vertex, std::string_view primitive) {
    INFO("primitive: ", primitive);
    const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
    CHECK(std::abs(engine::dot(engine::normalize(vertex.normal), tangent)) <= ORTHOGONALITY_TOLERANCE);
    CHECK(std::abs(engine::length(tangent) - 1.0F) <= UNIT_TOLERANCE);
    CHECK(vertex.tangent.w == 1.0F);
    CHECK(vertex.uv.x >= 0.0F);
    CHECK(vertex.uv.x <= 1.0F);
    CHECK(vertex.uv.y >= 0.0F);
    CHECK(vertex.uv.y <= 1.0F);
}

}  // namespace

TEST_CASE("render material: the 48-byte vertex layout is what the pipeline describes (PB3)") {
    // The offsets ForwardRenderer::create hands to rhi::VertexAttribute, restated as literals: a
    // silent member reorder in mesh.hpp would move them and the pipeline would read garbage.
    CHECK(sizeof(MeshVertex) == 48);
    CHECK(offsetof(MeshVertex, position) == 0);
    CHECK(offsetof(MeshVertex, normal) == 12);
    CHECK(offsetof(MeshVertex, tangent) == 24);
    CHECK(offsetof(MeshVertex, uv) == 40);
}

TEST_CASE("render material: cube/sphere/plane generate a legal tangent frame and bounded UVs (PB3)") {
    const engine::render::detail::PrimitiveGeometry cube = engine::render::detail::makeCube();
    const engine::render::detail::PrimitiveGeometry sphere = engine::render::detail::makeSphere();
    const engine::render::detail::PrimitiveGeometry plane = engine::render::detail::makePlane();

    // Literal counts, never .size() against itself: an emptied generator must redden here.
    CHECK(cube.vertices.size() == 24);
    CHECK(sphere.vertices.size() == 289);
    CHECK(plane.vertices.size() == 4);

    for (const MeshVertex& vertex : cube.vertices) {
        checkVertexInvariants(vertex, "cube");
    }
    for (const MeshVertex& vertex : sphere.vertices) {
        checkVertexInvariants(vertex, "sphere");
    }
    for (const MeshVertex& vertex : plane.vertices) {
        checkVertexInvariants(vertex, "plane");
    }
}

TEST_CASE("render material: the cube's per-face UV corners follow the generator's own convention (PB3)") {
    const engine::render::detail::PrimitiveGeometry cube = engine::render::detail::makeCube();
    REQUIRE(cube.vertices.size() == 24);

    // appendQuad emits (-u-v, +u-v, +u+v, -u+v) and assigns (0,1), (1,1), (1,0), (0,0): texture u runs
    // along +u and texture v runs DOWN along -v, so the image's (0,0) is the -u+v corner. This is
    // orientation WITHIN the generator's convention, which is tier-0 checkable; whether the checker
    // then reads upright ON SCREEN is the validation page's row (the S22 seed's only witness).
    constexpr std::array<Vec2, 4> EXPECTED_CORNER_UVS{Vec2{0.0F, 1.0F}, Vec2{1.0F, 1.0F}, Vec2{1.0F, 0.0F},
                                                      Vec2{0.0F, 0.0F}};
    CHECK(EXPECTED_CORNER_UVS.size() == 4);

    for (std::size_t face = 0; face < 6; ++face) {
        INFO("face: ", face);
        for (std::size_t corner = 0; corner < EXPECTED_CORNER_UVS.size(); ++corner) {
            CHECK(cube.vertices[(face * 4) + corner].uv == EXPECTED_CORNER_UVS[corner]);
        }
    }

    // The +X face is faces[0] (normal +X, u = -Z, v = +Y): its tangent must be the face's own u axis,
    // and its (0,0) texel must sit on the -u+v corner, i.e. at (+0.5, +0.5, +0.5).
    CHECK(cube.vertices[3].uv == Vec2{0.0F, 0.0F});
    CHECK(cube.vertices[3].position == Vec3{0.5F, 0.5F, 0.5F});
    CHECK(cube.vertices[3].tangent == Vec4{0.0F, 0.0F, -1.0F, 1.0F});
}

TEST_CASE("render material: the plane's tangent is +X and its normal invariant survives the axis reorder (PB3)") {
    const engine::render::detail::PrimitiveGeometry plane = engine::render::detail::makePlane();
    REQUIRE(plane.vertices.size() == 4);
    for (const MeshVertex& vertex : plane.vertices) {
        CHECK(vertex.normal == Vec3{0.0F, 1.0F, 0.0F});
        CHECK(vertex.tangent == Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    }
    // u x v == normal still holds for (u=+X, v=-Z): the quad spans x in [-0.5, 0.5], z in [-0.5, 0.5]
    // and its (0,0) texel is the -u+v corner, (-0.5, 0, -0.5).
    CHECK(plane.vertices[3].uv == Vec2{0.0F, 0.0F});
    CHECK(plane.vertices[3].position == Vec3{-0.5F, 0.0F, -0.5F});
}

TEST_CASE("render material: the sphere's UVs span the full [0,1] range in both axes (PB3)") {
    const engine::render::detail::PrimitiveGeometry sphere = engine::render::detail::makeSphere();
    REQUIRE(sphere.vertices.size() == 289);
    // North pole row: v == 0 and u sweeps 0 -> 1 across the 17 columns (the last is the duplicated
    // seam). A generator that forgot the seam column, or one that indexed rings and sectors the wrong
    // way round, cannot satisfy both ends.
    CHECK(sphere.vertices[0].uv == Vec2{0.0F, 0.0F});
    CHECK(sphere.vertices[16].uv == Vec2{1.0F, 0.0F});
    CHECK(sphere.vertices[288].uv == Vec2{1.0F, 1.0F});
    CHECK(sphere.vertices[272].uv == Vec2{0.0F, 1.0F});
}
