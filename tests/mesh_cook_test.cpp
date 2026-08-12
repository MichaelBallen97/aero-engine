// tests/mesh_cook_test.cpp -- task 3.3.1: cookMesh, the parallel-arrays-to-container transform. A TU
// of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every case drives the PUBLIC cookMesh() and reads its output
// back through the PUBLIC parseCookedMesh(), so nothing here depends on an internal of either.
#include <aero/assets/cooked_mesh.hpp>
#include <aero/assets/mesh_cook.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

using engine::Guid;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::assets::CookedIndexType;
using engine::assets::CookedMeshStatus;
using engine::assets::CookedVertexFormat;
using engine::assets::CookedVertexSemantic;
using engine::assets::cookMesh;
using engine::assets::MeshCookInput;
using engine::assets::MeshCookPrimitive;
using engine::assets::MeshCookResult;
using engine::assets::MeshCookStatus;

namespace {

// A caller-owned source primitive. MeshCookPrimitive holds SPANS the caller owns, so every case
// keeps its Source objects alive for as long as it looks at the cook's output.
struct Source {
    std::uint32_t meshIndex = 0;
    std::uint32_t primIndex = 0;
    std::uint32_t material = engine::assets::COOKED_INVALID_MATERIAL;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec4> tangents;
    std::vector<Vec2> uv0;
    std::vector<Vec2> uv1;
    std::vector<Vec4> colors;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<Vec4> weights;
    std::vector<std::uint32_t> indices;
};

MeshCookPrimitive view(const Source& s) {
    MeshCookPrimitive p;
    p.sourceMeshIndex = s.meshIndex;
    p.sourcePrimitiveIndex = s.primIndex;
    p.materialIndex = s.material;
    p.positions = s.positions;
    p.normals = s.normals;
    p.tangents = s.tangents;
    p.uv0 = s.uv0;
    p.uv1 = s.uv1;
    p.colors = s.colors;
    p.joints = s.joints;
    p.weights = s.weights;
    p.indices = s.indices;
    return p;
}

std::vector<MeshCookPrimitive> views(const std::vector<Source>& sources) {
    std::vector<MeshCookPrimitive> out;
    out.reserve(sources.size());
    for (const Source& s : sources) {
        out.push_back(view(s));
    }
    return out;
}

// The canonical unit triangle every layout case is built from: (0,0,0) (1,0,0) (0,1,0), indices 0 1 2.
Source triangle(std::uint32_t meshIndex, std::uint32_t primIndex) {
    Source s;
    s.meshIndex = meshIndex;
    s.primIndex = primIndex;
    s.positions = {Vec3{0.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}};
    s.indices = {0, 1, 2};
    return s;
}

// cook + parse in one, with INV-C3 asserted on the spot: EVERY result with non-empty bytes parses Ok.
// The parse's span points into `result.bytes`, and moving this struct moves that vector -- whose heap
// pointer is preserved -- so returning it by value is safe.
struct Cooked {
    MeshCookResult result;
    engine::assets::CookedMeshParseResult parsed;
};

Cooked cookAndParse(const MeshCookInput& input) {
    Cooked c;
    c.result = cookMesh(input);
    REQUIRE_FALSE(c.result.bytes.empty());
    c.parsed = engine::assets::parseCookedMesh(std::span<const std::byte>(c.result.bytes));
    REQUIRE(c.parsed.status == CookedMeshStatus::Ok);
    return c;
}

Cooked cookSources(const std::vector<Source>& sources, Guid guid = Guid{}) {
    const std::vector<MeshCookPrimitive> prims = views(sources);
    MeshCookInput in;
    in.sourceGuid = guid;
    in.primitives = prims;
    return cookAndParse(in);
}

constexpr std::uint32_t bitOf(CookedVertexSemantic s) { return 1U << static_cast<std::uint32_t>(s); }

}  // namespace

TEST_CASE("mesh cook: zero primitives produce a valid 96-byte container with a point box (MC1)") {
    const MeshCookInput in;
    const Cooked c = cookAndParse(in);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.message.empty());
    CHECK(c.result.warnings.size() == 1U);
    CHECK(c.result.warningTotal == 1U);
    CHECK(c.result.bytes.size() == 96U);
    CHECK(c.result.stats.sectionCount == 0U);
    CHECK(c.result.stats.submeshCount == 0U);
    CHECK(c.result.stats.vertexCount == 0U);
    CHECK(c.result.stats.indexCount == 0U);
    CHECK(c.result.stats.byteSize == 96U);
    // A POINT box at the origin, never Aabb::empty()'s inverted sentinel, whose centre is NaN.
    CHECK(c.parsed.mesh.bounds.min == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(c.parsed.mesh.bounds.max == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(c.parsed.mesh.indexDataOffset == 96U);
    CHECK(c.parsed.mesh.sections.empty());
    CHECK(c.parsed.mesh.submeshes.empty());
}

TEST_CASE("mesh cook: one position-only triangle is one section of stride 12 (MC2)") {
    const std::vector<Source> sources = {triangle(0, 0)};
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.warnings.empty());
    REQUIRE(c.parsed.mesh.sections.size() == 1U);
    REQUIRE(c.parsed.mesh.attributes.size() == 1U);
    REQUIRE(c.parsed.mesh.submeshes.size() == 1U);
    CHECK(c.parsed.mesh.attributes[0].semantic == CookedVertexSemantic::Position);
    CHECK(c.parsed.mesh.attributes[0].format == CookedVertexFormat::Float3);
    CHECK(c.parsed.mesh.attributes[0].offset == 0U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    CHECK(c.parsed.mesh.sections[0].vertexCount == 3U);
    CHECK(c.parsed.mesh.indexType == CookedIndexType::Uint16);
    CHECK(c.parsed.mesh.indexCount == 3U);
    CHECK(c.result.bytes.size() == 272U);  // the size Golden B pins
}

TEST_CASE("mesh cook: position only yields exactly one attribute (MC3)") {
    const std::vector<Source> sources = {triangle(0, 0)};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 1U);
    CHECK(c.parsed.mesh.attributes[0].semantic == CookedVertexSemantic::Position);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
}

TEST_CASE("mesh cook: position + normal is Float3 at offset 12, stride 24 (MC4)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 2U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::Normal);
    CHECK(c.parsed.mesh.attributes[1].format == CookedVertexFormat::Float3);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 24U);
}

TEST_CASE("mesh cook: position + tangent is Float4 at offset 12, stride 28 (MC5)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].tangents = {Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 2U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::Tangent);
    CHECK(c.parsed.mesh.attributes[1].format == CookedVertexFormat::Float4);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 28U);
}

TEST_CASE("mesh cook: position + uv0 is Float2 at offset 12, stride 20 (MC6)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].uv0 = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 2U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::TexCoord0);
    CHECK(c.parsed.mesh.attributes[1].format == CookedVertexFormat::Float2);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 20U);
}

TEST_CASE("mesh cook: position + uv1 is Float2 at offset 12, stride 20 (MC7)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].uv1 = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 2U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::TexCoord1);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 20U);
}

TEST_CASE("mesh cook: position + colors is Float4 at offset 12, stride 28 (MC8)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].colors = {Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 2U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::Color0);
    CHECK(c.parsed.mesh.attributes[1].format == CookedVertexFormat::Float4);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 28U);
}

TEST_CASE("mesh cook: joints AND weights together are Uint4 + Float4, stride 44 (MC9)") {
    // AC-20 in its positive direction: the pair is cooked TOGETHER. The mismatched arms are MC39/MC40.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].joints = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};
    sources[0].weights = {Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 3U);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::Joints0);
    CHECK(c.parsed.mesh.attributes[1].format == CookedVertexFormat::Uint4);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.attributes[2].semantic == CookedVertexSemantic::Weights0);
    CHECK(c.parsed.mesh.attributes[2].format == CookedVertexFormat::Float4);
    CHECK(c.parsed.mesh.attributes[2].offset == 28U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 44U);
}

TEST_CASE("mesh cook: position + normal + uv0 lays out in ascending semantic code (MC10)") {
    // Supplied in a shape where a naive "in the order the arrays appear" builder would still be
    // right; MC11's eight-attribute case is where the ordering rule is actually load-bearing.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].uv0 = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}};
    sources[0].normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 3U);
    CHECK(c.parsed.mesh.attributes[0].semantic == CookedVertexSemantic::Position);
    CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::Normal);
    CHECK(c.parsed.mesh.attributes[2].semantic == CookedVertexSemantic::TexCoord0);
    CHECK(c.parsed.mesh.attributes[1].offset == 12U);
    CHECK(c.parsed.mesh.attributes[2].offset == 24U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 32U);
}

namespace {

// All eight attributes on one triangle. Reused by MC11, MC17 and MC52's sweep.
Source allAttributes(std::uint32_t meshIndex, std::uint32_t primIndex) {
    Source s = triangle(meshIndex, primIndex);
    s.normals = {Vec3{0, 0, 1}, Vec3{0, 1, 0}, Vec3{1, 0, 0}};
    s.tangents = {Vec4{1, 0, 0, 1}, Vec4{0, 1, 0, -1}, Vec4{0, 0, 1, 1}};
    s.uv0 = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}};
    s.uv1 = {Vec2{0.25F, 0.5F}, Vec2{0.75F, 0.5F}, Vec2{0.5F, 1.0F}};
    s.colors = {Vec4{1, 0, 0, 1}, Vec4{0, 1, 0, 1}, Vec4{0, 0, 1, 1}};
    s.joints = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 65535}};
    s.weights = {Vec4{1, 0, 0, 0}, Vec4{0.5F, 0.5F, 0, 0}, Vec4{0.25F, 0.25F, 0.25F, 0.25F}};
    return s;
}

}  // namespace

TEST_CASE("mesh cook: all eight attributes give stride 104 at 0/12/24/40/48/56/72/88 (MC11)") {
    const std::vector<Source> sources = {allAttributes(0, 0)};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.attributes.size() == 8U);
    const std::array<std::uint32_t, 8> expectedOffsets = {0, 12, 24, 40, 48, 56, 72, 88};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<std::uint32_t>(c.parsed.mesh.attributes[i].semantic) == static_cast<std::uint32_t>(i));
        CHECK(c.parsed.mesh.attributes[i].offset == expectedOffsets[i]);
    }
    CHECK(c.parsed.mesh.sections[0].vertexStride == 104U);
}

TEST_CASE("mesh cook: equal masks share one section and the second's indices are rebased (MC12)") {
    std::vector<Source> sources = {triangle(0, 0), triangle(0, 1)};
    sources[1].positions = {Vec3{5, 0, 0}, Vec3{6, 0, 0}, Vec3{5, 1, 0}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.sections.size() == 1U);
    REQUIRE(c.parsed.mesh.submeshes.size() == 2U);
    CHECK(c.parsed.mesh.sections[0].vertexCount == 6U);
    CHECK(c.parsed.mesh.submeshes[0].sourcePrimitiveIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[1].sourcePrimitiveIndex == 1U);
    // The second submesh's indices are SECTION-RELATIVE: 0,1,2 became 3,4,5.
    const auto idx = engine::assets::indexBytes(c.parsed.mesh);
    REQUIRE(idx.size() == 12U);
    CHECK(engine::assets::getU16(idx, 0) == 0U);
    CHECK(engine::assets::getU16(idx, 2) == 1U);
    CHECK(engine::assets::getU16(idx, 4) == 2U);
    CHECK(engine::assets::getU16(idx, 6) == 3U);
    CHECK(engine::assets::getU16(idx, 8) == 4U);
    CHECK(engine::assets::getU16(idx, 10) == 5U);
}

TEST_CASE("mesh cook: different masks give two sections in ASCENDING MASK order (MC13)") {
    // Supplied with the HIGHER mask first, so input order and output order disagree.
    std::vector<Source> sources = {triangle(1, 0), triangle(0, 0)};
    sources[0].normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.sections.size() == 2U);
    REQUIRE(c.parsed.mesh.submeshes.size() == 2U);
    // Section 0 is the position-only mask (0x01); section 1 is position+normal (0x03).
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    CHECK(c.parsed.mesh.sections[1].vertexStride == 24U);
    CHECK(c.parsed.mesh.submeshes[0].sectionIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[0].sourceMeshIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[1].sectionIndex == 1U);
    CHECK(c.parsed.mesh.submeshes[1].sourceMeshIndex == 1U);
}

TEST_CASE("mesh cook: three masks give three sections, ascending, each submesh naming its own (MC14)") {
    std::vector<Source> sources = {triangle(2, 0), triangle(0, 0), triangle(1, 0)};
    // masks: sources[0] = Position|Color0 (0x21), sources[1] = Position (0x01),
    //        sources[2] = Position|Normal (0x03).
    sources[0].colors = {Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}, Vec4{1, 0, 0, 1}};
    sources[2].normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.sections.size() == 3U);
    REQUIRE(c.parsed.mesh.submeshes.size() == 3U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);  // 0x01
    CHECK(c.parsed.mesh.sections[1].vertexStride == 24U);  // 0x03
    CHECK(c.parsed.mesh.sections[2].vertexStride == 28U);  // 0x21
    CHECK(c.parsed.mesh.submeshes[0].sourceMeshIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[0].sectionIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[1].sourceMeshIndex == 1U);
    CHECK(c.parsed.mesh.submeshes[1].sectionIndex == 1U);
    CHECK(c.parsed.mesh.submeshes[2].sourceMeshIndex == 2U);
    CHECK(c.parsed.mesh.submeshes[2].sectionIndex == 2U);
}

TEST_CASE("mesh cook: firstIndex accumulates and the header's indexCount is the sum (MC15)") {
    std::vector<Source> sources = {triangle(0, 0), triangle(0, 1), triangle(0, 2)};
    sources[1].indices = {0, 1, 2, 0, 1, 2};  // six
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.submeshes.size() == 3U);
    CHECK(c.parsed.mesh.submeshes[0].firstIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[0].indexCount == 3U);
    CHECK(c.parsed.mesh.submeshes[1].firstIndex == 3U);
    CHECK(c.parsed.mesh.submeshes[1].indexCount == 6U);
    CHECK(c.parsed.mesh.submeshes[2].firstIndex == 9U);
    CHECK(c.parsed.mesh.submeshes[2].indexCount == 3U);
    CHECK(c.parsed.mesh.indexCount == 12U);
    CHECK(c.result.stats.indexCount == 12U);
}

TEST_CASE("mesh cook: every submesh records its three source fields VERBATIM (MC16)") {
    std::vector<Source> sources = {triangle(3, 7), triangle(3, 8)};
    sources[0].material = 42;
    sources[1].material = engine::assets::COOKED_INVALID_MATERIAL;  // the sentinel, preserved
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.submeshes.size() == 2U);
    CHECK(c.parsed.mesh.submeshes[0].materialIndex == 42U);
    CHECK(c.parsed.mesh.submeshes[0].sourceMeshIndex == 3U);
    CHECK(c.parsed.mesh.submeshes[0].sourcePrimitiveIndex == 7U);
    CHECK(c.parsed.mesh.submeshes[1].materialIndex == engine::assets::COOKED_INVALID_MATERIAL);
    CHECK(c.parsed.mesh.submeshes[1].sourcePrimitiveIndex == 8U);
}

TEST_CASE("mesh cook: interleaving reads back component by component (MC17)") {
    const std::vector<Source> sources = {allAttributes(0, 0)};
    const Cooked c = cookSources(sources);
    const auto vb = engine::assets::sectionVertexBytes(c.parsed.mesh, 0);
    REQUIRE(vb.size() == 3U * 104U);
    const Source& s = sources[0];
    for (std::size_t v = 0; v < 3; ++v) {
        const std::size_t base = v * 104;
        CHECK(engine::assets::getF32(vb, base + 0) == s.positions[v].x);
        CHECK(engine::assets::getF32(vb, base + 4) == s.positions[v].y);
        CHECK(engine::assets::getF32(vb, base + 8) == s.positions[v].z);
        CHECK(engine::assets::getF32(vb, base + 12) == s.normals[v].x);
        CHECK(engine::assets::getF32(vb, base + 24) == s.tangents[v].x);
        CHECK(engine::assets::getF32(vb, base + 36) == s.tangents[v].w);
        CHECK(engine::assets::getF32(vb, base + 40) == s.uv0[v].x);
        CHECK(engine::assets::getF32(vb, base + 44) == s.uv0[v].y);
        CHECK(engine::assets::getF32(vb, base + 48) == s.uv1[v].x);
        CHECK(engine::assets::getF32(vb, base + 56) == s.colors[v].x);
        CHECK(engine::assets::getF32(vb, base + 68) == s.colors[v].w);
        CHECK(engine::assets::getU32(vb, base + 72) == s.joints[v][0]);
        CHECK(engine::assets::getU32(vb, base + 84) == s.joints[v][3]);
        CHECK(engine::assets::getF32(vb, base + 88) == s.weights[v].x);
        CHECK(engine::assets::getF32(vb, base + 100) == s.weights[v].w);
    }
}

TEST_CASE("mesh cook: joints widen u16 to u32 losslessly, including 65535 (MC18)") {
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].joints = {{0, 1, 255, 256}, {65534, 65535, 0, 1}, {4096, 8192, 16384, 32768}};
    sources[0].weights = {Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}};
    const Cooked c = cookSources(sources);
    const auto vb = engine::assets::sectionVertexBytes(c.parsed.mesh, 0);
    REQUIRE(c.parsed.mesh.sections[0].vertexStride == 44U);
    for (std::size_t v = 0; v < 3; ++v) {
        for (std::size_t k = 0; k < 4; ++k) {
            CHECK(engine::assets::getU32(vb, (v * 44) + 12 + (k * 4)) ==
                  static_cast<std::uint32_t>(sources[0].joints[v][k]));
        }
    }
}

TEST_CASE("mesh cook: a tangent's w is copied VERBATIM, never renormalized (MC19)") {
    // The cook validates no attribute VALUE beyond position finiteness. A .w of 0.5 is not a legal
    // bitangent sign, and it survives untouched, because the bits are moved rather than derived.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].tangents = {Vec4{1, 0, 0, 0.5F}, Vec4{0, 1, 0, -3.25F}, Vec4{0, 0, 1, 0.0F}};
    const Cooked c = cookSources(sources);
    const auto vb = engine::assets::sectionVertexBytes(c.parsed.mesh, 0);
    REQUIRE(c.parsed.mesh.sections[0].vertexStride == 28U);
    CHECK(engine::assets::getF32(vb, 24) == 0.5F);
    CHECK(engine::assets::getF32(vb, 28 + 24) == -3.25F);
    CHECK(engine::assets::getF32(vb, 56 + 24) == 0.0F);
}

TEST_CASE("mesh cook: 65536 vertices stay Uint16, 65537 become Uint32 (MC20)") {
    // <= 65536, not < 65536: a section with exactly 65536 vertices has a maximum index of 65535,
    // which Uint16 represents. ~800 kB per arm.
    SUBCASE("exactly 65536") {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].positions.assign(65536, Vec3{1.0F, 2.0F, 3.0F});
        const Cooked c = cookSources(sources);
        CHECK(c.parsed.mesh.sections[0].vertexCount == 65536U);
        CHECK(c.parsed.mesh.indexType == CookedIndexType::Uint16);
    }
    SUBCASE("one more") {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].positions.assign(65537, Vec3{1.0F, 2.0F, 3.0F});
        const Cooked c = cookSources(sources);
        CHECK(c.parsed.mesh.sections[0].vertexCount == 65537U);
        CHECK(c.parsed.mesh.indexType == CookedIndexType::Uint32);
    }
}

TEST_CASE("mesh cook: the index width is a FILE-level choice, not a per-section one (MC21)") {
    std::vector<Source> sources = {triangle(0, 0), triangle(1, 0)};
    sources[0].positions.assign(65536, Vec3{1.0F, 0.0F, 0.0F});
    sources[1].positions.assign(65537, Vec3{0.0F, 1.0F, 0.0F});
    sources[1].normals.assign(65537, Vec3{0.0F, 0.0F, 1.0F});  // a different mask -> a second section
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.sections.size() == 2U);
    CHECK(c.parsed.mesh.sections[0].vertexCount == 65536U);
    CHECK(c.parsed.mesh.sections[1].vertexCount == 65537U);
    CHECK(c.parsed.mesh.indexType == CookedIndexType::Uint32);
    CHECK(engine::assets::indexBytes(c.parsed.mesh).size() == 6U * 4U);
}

TEST_CASE("mesh cook: submesh boxes are folded and the model box is their union (MC22)") {
    // The fold is std::min/std::max with the ACCUMULATOR FIRST, matching Aabb::expand bit for bit --
    // MK9 is where that agreement is compared against the importer's own box.
    std::vector<Source> sources = {triangle(0, 0), triangle(1, 0)};
    sources[0].positions = {Vec3{-1.0F, 0.0F, 0.0F}, Vec3{2.0F, 0.0F, 0.0F}, Vec3{0.0F, 3.0F, 0.0F}};
    sources[1].positions = {Vec3{0.0F, -4.0F, 1.0F}, Vec3{0.0F, 0.0F, 5.0F}, Vec3{0.0F, 0.0F, 0.0F}};
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.submeshes.size() == 2U);
    CHECK(c.parsed.mesh.submeshes[0].bounds.min == Vec3{-1.0F, 0.0F, 0.0F});
    CHECK(c.parsed.mesh.submeshes[0].bounds.max == Vec3{2.0F, 3.0F, 0.0F});
    CHECK(c.parsed.mesh.submeshes[1].bounds.min == Vec3{0.0F, -4.0F, 0.0F});
    CHECK(c.parsed.mesh.submeshes[1].bounds.max == Vec3{0.0F, 0.0F, 5.0F});
    CHECK(c.parsed.mesh.bounds.min == Vec3{-1.0F, -4.0F, 0.0F});
    CHECK(c.parsed.mesh.bounds.max == Vec3{2.0F, 3.0F, 5.0F});
}

TEST_CASE("mesh cook: a vertex NO INDEX reaches still contributes to the box (MC23)") {
    // The fold is over every position WRITTEN, which is all of them. Folding over "only vertices an
    // index reaches" is the plausible-looking alternative that silently breaks AC-27 for any mesh
    // with an unreferenced vertex -- and the importer folds over all of them too.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].positions.push_back(Vec3{100.0F, -100.0F, 50.0F});  // never indexed
    const Cooked c = cookSources(sources);
    REQUIRE(c.parsed.mesh.submeshes.size() == 1U);
    CHECK(c.parsed.mesh.sections[0].vertexCount == 4U);
    CHECK(c.parsed.mesh.submeshes[0].bounds.max == Vec3{100.0F, 1.0F, 50.0F});
    CHECK(c.parsed.mesh.submeshes[0].bounds.min == Vec3{0.0F, -100.0F, 0.0F});
    CHECK(c.parsed.mesh.bounds.max == Vec3{100.0F, 1.0F, 50.0F});
}

TEST_CASE("mesh cook: a degenerate but in-range index triple cooks as-is (MC24)") {
    // The cook is not a mesh validator. Three indices addressing one vertex is a degenerate triangle
    // and a legitimate thing for an exporter to emit; refusing it would drop geometry over a shape.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].indices = {1, 1, 1};
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.warnings.empty());
    const auto idx = engine::assets::indexBytes(c.parsed.mesh);
    REQUIRE(idx.size() == 6U);
    CHECK(engine::assets::getU16(idx, 0) == 1U);
    CHECK(engine::assets::getU16(idx, 2) == 1U);
    CHECK(engine::assets::getU16(idx, 4) == 1U);
}

TEST_CASE("mesh cook: a nil sourceGuid is legal and writes sixteen zero bytes (MC25)") {
    const std::vector<Source> sources = {triangle(0, 0)};
    const Cooked nil = cookSources(sources);
    CHECK_FALSE(nil.parsed.mesh.sourceGuid.valid());
    CHECK(nil.parsed.mesh.sourceGuid.hi == 0U);
    CHECK(nil.parsed.mesh.sourceGuid.lo == 0U);

    const Cooked named = cookSources(sources, Guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL});
    CHECK(named.parsed.mesh.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(named.parsed.mesh.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    // The GUID is the ONLY difference: same size, and the sixteen bytes at offset 16 are all of it.
    REQUIRE(nil.result.bytes.size() == named.result.bytes.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < nil.result.bytes.size(); ++i) {
        if (nil.result.bytes[i] != named.result.bytes[i]) {
            ++differing;
            CHECK(i >= 16U);
            CHECK(i < 32U);
        }
    }
    CHECK(differing == 16U);
}

TEST_CASE("mesh cook: a REPEATED ordering key is warned about, never dropped (MC26)") {
    // The one non-total case in the sort key, DIAGNOSED rather than assumed away (C3). Nothing is
    // dropped -- dropping would lose geometry over a caller's bookkeeping -- and the status stays Ok.
    std::vector<Source> sources = {triangle(4, 9), triangle(4, 9)};
    sources[1].positions = {Vec3{7, 0, 0}, Vec3{8, 0, 0}, Vec3{7, 1, 0}};
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.message.empty());
    REQUIRE(c.result.warnings.size() == 1U);
    CHECK(c.result.warningTotal == 1U);
    CHECK(c.result.warnings[0].find("ordering key") != std::string::npos);
    CHECK(c.parsed.mesh.submeshes.size() == 2U);
    CHECK(c.parsed.mesh.sections[0].vertexCount == 6U);
    // The mask is part of the key, so two primitives with the same indices but DIFFERENT masks do
    // not collide -- which is what makes this warning about bookkeeping rather than about geometry.
    std::vector<Source> distinct = {triangle(4, 9), triangle(4, 9)};
    distinct[1].normals = {Vec3{0, 0, 1}, Vec3{0, 0, 1}, Vec3{0, 0, 1}};
    const Cooked d = cookSources(distinct);
    CHECK(d.result.warnings.empty());
    CHECK(bitOf(CookedVertexSemantic::Normal) == 2U);
}

// =================================================================================================
// The drop and demote arms. FIVE conditions drop a primitive WHOLE, one demotes an attribute whole,
// and nothing is ever partial (A-9).
//
// A note on warning counts. When a case's only primitive is dropped the container ends up empty, so
// the cook emits its own "no cookable primitives" warning as well -- these cases therefore see TWO
// warnings, the drop's and the empty container's, and they assert both rather than pretending the
// second one is not there.
// =================================================================================================

TEST_CASE("mesh cook: an empty positions array drops the primitive whole (MC27)") {
    std::vector<Source> sources = {triangle(2, 5)};
    sources[0].positions.clear();
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.stats.droppedPrimitiveCount == 1U);
    CHECK(c.result.stats.submeshCount == 0U);
    REQUIRE(c.result.warnings.size() == 2U);
    CHECK(c.result.warnings[0] == "mesh 2 primitive 5 has no positions and was dropped");
    CHECK(c.result.bytes.size() == 96U);
}

TEST_CASE("mesh cook: an empty indices array drops the primitive whole (MC28)") {
    std::vector<Source> sources = {triangle(2, 5)};
    sources[0].indices.clear();
    const Cooked c = cookSources(sources);
    CHECK(c.result.stats.droppedPrimitiveCount == 1U);
    REQUIRE(c.result.warnings.size() == 2U);
    CHECK(c.result.warnings[0] == "mesh 2 primitive 5 has no indices and was dropped");
}

TEST_CASE("mesh cook: an index count that is not a multiple of three drops the primitive (MC29)") {
    for (const std::size_t extra : {std::size_t{1}, std::size_t{2}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        for (std::size_t i = 0; i < extra; ++i) {
            sources[0].indices.push_back(0);
        }
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 1U);
        REQUIRE(c.result.warnings.size() == 2U);
        CHECK(c.result.warnings[0].find(std::format("has {} indices, not a multiple of 3", 3 + extra)) !=
              std::string::npos);
    }
}

TEST_CASE("mesh cook: an out-of-range index drops the primitive whole (MC30)") {
    SUBCASE("exactly positions.size(), the off-by-one an inclusive bound would accept") {
        std::vector<Source> sources = {triangle(1, 1)};
        sources[0].indices = {0, 1, 3};
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 1U);
        REQUIRE(c.result.warnings.size() == 2U);
        CHECK(c.result.warnings[0] == "mesh 1 primitive 1 index 2 addresses vertex 3 of 3, and was dropped");
    }
    SUBCASE("far past the end") {
        std::vector<Source> sources = {triangle(1, 1)};
        sources[0].indices = {0, 900000, 2};
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 1U);
        CHECK(c.result.warnings[0].find("addresses vertex 900000 of 3") != std::string::npos);
    }
}

TEST_CASE("mesh cook: any non-finite POSITION component drops the primitive whole (MC31)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    struct Arm {
        int component;
        float value;
    };
    const std::array<Arm, 5> arms = {Arm{0, nan}, Arm{1, nan}, Arm{2, nan}, Arm{0, inf}, Arm{1, -inf}};
    for (const Arm& arm : arms) {
        std::vector<Source> sources = {triangle(0, 4)};
        Vec3& p = sources[0].positions[1];
        if (arm.component == 0) {
            p.x = arm.value;
        } else if (arm.component == 1) {
            p.y = arm.value;
        } else {
            p.z = arm.value;
        }
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 1U);
        REQUIRE(c.result.warnings.size() == 2U);
        CHECK(c.result.warnings[0] == "mesh 0 primitive 4 has a non-finite position at vertex 1 and was dropped");
    }
}

TEST_CASE("mesh cook: a dropped primitive does not stop the others (MC32)") {
    std::vector<Source> sources = {triangle(0, 0), triangle(0, 1), triangle(0, 2)};
    sources[1].positions[0].y = std::numeric_limits<float>::quiet_NaN();
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.stats.droppedPrimitiveCount == 1U);
    REQUIRE(c.parsed.mesh.submeshes.size() == 2U);
    CHECK(c.parsed.mesh.submeshes[0].sourcePrimitiveIndex == 0U);
    CHECK(c.parsed.mesh.submeshes[1].sourcePrimitiveIndex == 2U);
    CHECK(c.result.warnings.size() == 1U);  // no empty-container warning: two submeshes survived
}

TEST_CASE("mesh cook: a dropped primitive contributes NOTHING to the model bounds (MC33)") {
    std::vector<Source> sources = {triangle(0, 0), triangle(0, 1)};
    // The dropped one is far away in every direction AND carries the non-finite component, so a cook
    // that folded before dropping would produce either a huge box or a NaN one.
    sources[1].positions = {Vec3{-50.0F, -50.0F, -50.0F}, Vec3{50.0F, 50.0F, 50.0F},
                            Vec3{0.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F}};
    const Cooked c = cookSources(sources);
    CHECK(c.result.stats.droppedPrimitiveCount == 1U);
    CHECK(c.parsed.mesh.bounds.min == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(c.parsed.mesh.bounds.max == Vec3{1.0F, 1.0F, 0.0F});
}

TEST_CASE("mesh cook: a mis-sized NORMALS array is demoted, not dropped (MC34)") {
    for (const std::size_t n : {std::size_t{2}, std::size_t{4}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].normals.assign(n, Vec3{0, 0, 1});
        const Cooked c = cookSources(sources);
        CHECK(c.result.status == MeshCookStatus::Ok);
        CHECK(c.result.stats.droppedPrimitiveCount == 0U);
        REQUIRE(c.result.warnings.size() == 1U);
        CHECK(c.result.warnings[0] == std::format("mesh 0 primitive 0's normals array has {} entries for 3 vertices "
                                                  "and was ignored",
                                                  n));
        REQUIRE(c.parsed.mesh.attributes.size() == 1U);
        CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    }
}

TEST_CASE("mesh cook: a mis-sized TANGENTS array is demoted, not dropped (MC35)") {
    for (const std::size_t n : {std::size_t{2}, std::size_t{4}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].tangents.assign(n, Vec4{1, 0, 0, 1});
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 0U);
        REQUIRE(c.result.warnings.size() == 1U);
        CHECK(c.result.warnings[0].find("tangents array has") != std::string::npos);
        CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    }
}

TEST_CASE("mesh cook: a mis-sized UV0 array is demoted, not dropped (MC36)") {
    for (const std::size_t n : {std::size_t{2}, std::size_t{4}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].uv0.assign(n, Vec2{0, 0});
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 0U);
        REQUIRE(c.result.warnings.size() == 1U);
        CHECK(c.result.warnings[0].find("uv0 array has") != std::string::npos);
        CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    }
}

TEST_CASE("mesh cook: a mis-sized COLORS array is demoted, not dropped (MC37)") {
    for (const std::size_t n : {std::size_t{2}, std::size_t{4}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].colors.assign(n, Vec4{1, 1, 1, 1});
        // A correctly-sized uv1 alongside proves the demotion is per-ATTRIBUTE, not per-primitive.
        sources[0].uv1.assign(3, Vec2{0.5F, 0.5F});
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 0U);
        REQUIRE(c.result.warnings.size() == 1U);
        CHECK(c.result.warnings[0].find("colors array has") != std::string::npos);
        REQUIRE(c.parsed.mesh.attributes.size() == 2U);
        CHECK(c.parsed.mesh.attributes[1].semantic == CookedVertexSemantic::TexCoord1);
        CHECK(c.parsed.mesh.sections[0].vertexStride == 20U);
    }
}

TEST_CASE("mesh cook: a mis-sized WEIGHTS array with no joints is demoted alone (MC38)") {
    for (const std::size_t n : {std::size_t{2}, std::size_t{4}}) {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].weights.assign(n, Vec4{1, 0, 0, 0});
        const Cooked c = cookSources(sources);
        CHECK(c.result.stats.droppedPrimitiveCount == 0U);
        // ONE warning, not two: joints was absent to begin with, so the pairing rule has nothing to
        // say -- both are absent, which is a legal state.
        REQUIRE(c.result.warnings.size() == 1U);
        CHECK(c.result.warnings[0].find("weights array has") != std::string::npos);
        CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
    }
}

TEST_CASE("mesh cook: joints without weights drops BOTH (MC39)") {
    std::vector<Source> sources = {triangle(6, 2)};
    sources[0].joints = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};  // correctly sized
    const Cooked c = cookSources(sources);
    CHECK(c.result.stats.droppedPrimitiveCount == 0U);
    REQUIRE(c.result.warnings.size() == 1U);
    CHECK(c.result.warnings[0] == "mesh 6 primitive 2 has joints without weights; both were dropped");
    REQUIRE(c.parsed.mesh.attributes.size() == 1U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
}

TEST_CASE("mesh cook: weights present with mis-sized joints gives TWO independent warnings (MC40)") {
    std::vector<Source> sources = {triangle(6, 3)};
    sources[0].joints = {{0, 1, 2, 3}, {0, 1, 2, 3}};  // two entries for three vertices
    sources[0].weights = {Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}, Vec4{1, 0, 0, 0}};
    const Cooked c = cookSources(sources);
    CHECK(c.result.stats.droppedPrimitiveCount == 0U);
    // The mis-size warning and the pairing warning are TWO warnings, not one: they are independent
    // rules and either can fire without the other (MC38 is the mis-size alone, MC39 the pairing alone).
    REQUIRE(c.result.warnings.size() == 2U);
    CHECK(c.result.warnings[0].find("joints array has 2 entries for 3 vertices") != std::string::npos);
    CHECK(c.result.warnings[1] == "mesh 6 primitive 3 has weights without joints; both were dropped");
    REQUIRE(c.parsed.mesh.attributes.size() == 1U);
    CHECK(c.parsed.mesh.sections[0].vertexStride == 12U);
}

TEST_CASE("mesh cook: a non-finite NORMAL is copied through verbatim (MC41)") {
    // Non-finite values in attributes OTHER than position are copied through: they take part in no
    // computation, their bits are moved rather than derived, and refusing them would drop geometry
    // over a shading artifact.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].normals = {Vec3{nan, 0, 1}, Vec3{0, std::numeric_limits<float>::infinity(), 1}, Vec3{0, 0, 1}};
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.warnings.empty());
    CHECK(c.result.stats.droppedPrimitiveCount == 0U);
    const auto vb = engine::assets::sectionVertexBytes(c.parsed.mesh, 0);
    REQUIRE(vb.size() == 3U * 24U);
    CHECK(engine::assets::getU32(vb, 12) == std::bit_cast<std::uint32_t>(nan));
    CHECK(engine::assets::getF32(vb, 24 + 16) == std::numeric_limits<float>::infinity());
    // And the bounds are untouched by them, because the fold is over POSITIONS.
    CHECK(c.parsed.mesh.bounds.max == Vec3{1.0F, 1.0F, 0.0F});
}

// =================================================================================================
// The caps. FOUR of these cases are the largest allocations any test in this tree makes, and the
// cost is DISCLOSED here rather than discovered: MC42 ~96 MB, MC43 ~96 MB, MC44 ~10 MB, MC45 ~192 MB.
// The vertex and index caps cannot be reached without real memory, because phase 1 READS every
// position (finiteness) and every index (range) before the cap pass runs. A settable cook cap was
// rejected: the caps are part of the format's contract and the PARSER enforces them too, so a test
// cap would prove something the shipped configuration does not do.
// =================================================================================================

TEST_CASE("mesh cook: the vertex cap truncates and names itself (MC42)") {
    // ~96 MB for the positions vector in each subcase, and another ~96 MB of cooked output in the
    // at-the-cap arm. doctest re-runs the body per SUBCASE, so the two peaks do not coincide.
    SUBCASE("exactly at the cap is Ok -- the `>` versus `>=` discriminator") {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].positions.assign(engine::assets::MAX_COOKED_VERTICES, Vec3{0.5F, 0.5F, 0.5F});
        const Cooked c = cookSources(sources);
        CHECK(c.result.status == MeshCookStatus::Ok);
        CHECK(c.result.message.empty());
        CHECK(c.parsed.mesh.sections[0].vertexCount == engine::assets::MAX_COOKED_VERTICES);
    }
    SUBCASE("one past the cap is Truncated") {
        std::vector<Source> sources = {triangle(0, 0)};
        sources[0].positions.assign(engine::assets::MAX_COOKED_VERTICES + 1, Vec3{0.5F, 0.5F, 0.5F});
        const Cooked c = cookSources(sources);
        CHECK(c.result.status == MeshCookStatus::Truncated);
        CHECK(c.result.message.find("MAX_COOKED_VERTICES") != std::string::npos);
        CHECK(c.result.message.find(std::format("{}", engine::assets::MAX_COOKED_VERTICES)) != std::string::npos);
        // Acceptance stopped at the FIRST violating candidate, so nothing was emitted: a valid empty
        // container, never a partial one.
        CHECK(c.result.bytes.size() == 96U);
        CHECK(c.parsed.mesh.submeshes.empty());
    }
}

TEST_CASE("mesh cook: the index cap truncates and names itself (MC43)") {
    // ~96 MB for the index vector. Every index is in {0,1,2}, so phase 1's range scan passes and the
    // CAP is what refuses -- not the drop arm.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].indices.assign(engine::assets::MAX_COOKED_INDICES + 3, 1);
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Truncated);
    CHECK(c.result.message.find("MAX_COOKED_INDICES") != std::string::npos);
    CHECK(c.result.message.find(std::format("{}", engine::assets::MAX_COOKED_INDICES)) != std::string::npos);
    CHECK(c.result.bytes.size() == 96U);
}

namespace {

// 65537 primitives aliasing ONE three-vertex, three-index buffer. Cheap (~10 MB) precisely because
// MeshCookPrimitive holds spans the caller owns: the geometry is stored once.
struct SubmeshCapFixture {
    std::vector<Vec3> positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    std::vector<std::uint32_t> indices = {0, 1, 2};
    std::vector<MeshCookPrimitive> prims;

    void build(bool reversed) {
        prims.clear();
        prims.reserve(engine::assets::MAX_COOKED_SUBMESHES + 1);
        for (std::uint32_t i = 0; i <= engine::assets::MAX_COOKED_SUBMESHES; ++i) {
            MeshCookPrimitive p;
            p.sourceMeshIndex = 0;
            p.sourcePrimitiveIndex = reversed ? engine::assets::MAX_COOKED_SUBMESHES - i : i;
            p.positions = positions;
            p.indices = indices;
            prims.push_back(p);
        }
    }
};

}  // namespace

TEST_CASE("mesh cook: the submesh cap emits exactly MAX_COOKED_SUBMESHES and names itself (MC44)") {
    SubmeshCapFixture fixture;
    fixture.build(false);
    MeshCookInput in;
    in.primitives = fixture.prims;
    const Cooked c = cookAndParse(in);
    CHECK(c.result.status == MeshCookStatus::Truncated);
    CHECK(c.result.message.find("MAX_COOKED_SUBMESHES") != std::string::npos);
    CHECK(c.result.message.find(std::format("{}", engine::assets::MAX_COOKED_SUBMESHES)) != std::string::npos);
    CHECK(c.parsed.mesh.submeshes.size() == engine::assets::MAX_COOKED_SUBMESHES);
    CHECK(c.result.stats.submeshCount == engine::assets::MAX_COOKED_SUBMESHES);
    // The prefix is COHERENT: the surviving submeshes are 0..65535, not an arbitrary subset.
    CHECK(c.parsed.mesh.submeshes.front().sourcePrimitiveIndex == 0U);
    CHECK(c.parsed.mesh.submeshes.back().sourcePrimitiveIndex == engine::assets::MAX_COOKED_SUBMESHES - 1);
}

TEST_CASE("mesh cook: two caps violated by one candidate give ONE status and TWO messages (MC45)") {
    // ~192 MB: the vertex array and the index array both go over their caps in the same primitive.
    // Every cap is evaluated against each candidate and each violated one latches its OWN bool, so
    // this produces two "; "-joined messages rather than N copies of one or only the first.
    std::vector<Source> sources = {triangle(0, 0)};
    sources[0].positions.assign(engine::assets::MAX_COOKED_VERTICES + 1, Vec3{0.5F, 0.5F, 0.5F});
    sources[0].indices.assign(engine::assets::MAX_COOKED_INDICES + 3, 1);
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Truncated);
    CHECK(c.result.message.find("MAX_COOKED_VERTICES") != std::string::npos);
    CHECK(c.result.message.find("MAX_COOKED_INDICES") != std::string::npos);
    CHECK(c.result.message.find("; ") != std::string::npos);
    // FIXED order -- vertices, then indices -- so the same input always produces the same string.
    CHECK(c.result.message.find("MAX_COOKED_VERTICES") < c.result.message.find("MAX_COOKED_INDICES"));
    CHECK(c.result.message.find("MAX_COOKED_SUBMESHES") == std::string::npos);
}

TEST_CASE("mesh cook: a cap-truncated file is still internally coherent (MC46)") {
    SubmeshCapFixture fixture;
    fixture.build(false);
    MeshCookInput in;
    in.primitives = fixture.prims;
    const Cooked c = cookAndParse(in);  // the REQUIRE inside is INV-C3 for this arm
    CHECK(c.result.status == MeshCookStatus::Truncated);
    CHECK(c.parsed.mesh.submeshes.size() == c.result.stats.submeshCount);
    CHECK(c.parsed.mesh.sections.size() == c.result.stats.sectionCount);
    CHECK(c.parsed.mesh.indexCount == c.result.stats.indexCount);
    CHECK(c.result.bytes.size() == c.result.stats.byteSize);
    // Every submesh's range lies inside the header's indexCount, and the last one ends exactly on it.
    std::uint64_t running = 0;
    for (const auto& m : c.parsed.mesh.submeshes) {
        CHECK(m.firstIndex == running);
        running += m.indexCount;
    }
    CHECK(running == c.parsed.mesh.indexCount);
}

TEST_CASE("mesh cook: acceptance walks the SORTED order, not the input order (MC47)") {
    // THE ONLY CASE THAT CAN SEE C2. With the cap pass before the sort, the surviving prefix is a
    // function of the caller's input order, so these two inputs -- the same SET in opposite orders --
    // would keep different primitives and produce different bytes. AC-28 (cook twice) stays green
    // either way; only a shuffled OVER-CAP input tells them apart.
    SubmeshCapFixture ascending;
    ascending.build(false);
    MeshCookInput inA;
    inA.primitives = ascending.prims;
    const MeshCookResult a = cookMesh(inA);

    SubmeshCapFixture descending;
    descending.build(true);
    MeshCookInput inB;
    inB.primitives = descending.prims;
    const MeshCookResult b = cookMesh(inB);

    CHECK(a.status == MeshCookStatus::Truncated);
    CHECK(b.status == MeshCookStatus::Truncated);
    CHECK(a.message == b.message);
    REQUIRE(a.bytes.size() == b.bytes.size());
    std::size_t firstDifference = a.bytes.size();
    for (std::size_t i = 0; i < a.bytes.size(); ++i) {
        if (a.bytes[i] != b.bytes[i]) {
            firstDifference = i;
            break;
        }
    }
    CHECK(firstDifference == a.bytes.size());
}

TEST_CASE("mesh cook: warnings are capped and the total is not (MC48)") {
    std::vector<Source> sources;
    sources.reserve(25);
    for (std::uint32_t i = 0; i < 25; ++i) {
        Source s = triangle(0, i);
        s.normals.assign(2, Vec3{0, 0, 1});  // a demote, so every primitive still cooks
        sources.push_back(s);
    }
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.warnings.size() == engine::assets::MAX_COOK_WARNINGS);
    CHECK(c.result.warnings.size() == 20U);
    CHECK(c.result.warningTotal == 25U);
    CHECK(c.parsed.mesh.submeshes.size() == 25U);
}

TEST_CASE("mesh cook: every REACHABLE mask gets its own section, and the cap cannot be reached (MC49)") {
    // 2^7 = 128 masks is the arithmetic bound over seven optional semantics -- but AC-20's pairing
    // rule makes half of them unreachable, because a mask carrying exactly one of joints/weights is
    // cleared to NEITHER. Only 2^5 x 2 = 64 masks can ever be produced, against a cap of 128.
    //
    // So MAX_COOKED_SECTIONS is DOUBLY unreachable from the cook, and the plan's own "128 distinct
    // masks yield exactly 128 sections" cannot be constructed. The cook's Truncated arm for this cap
    // is dead code with no case, and the parser -- where a header claiming 129 sections is refused
    // (CM21) -- is where the constant is actually proven.
    std::vector<Source> sources;
    sources.reserve(64);
    std::uint32_t meshIndex = 0;
    for (std::uint32_t free = 0; free < 32; ++free) {
        for (std::uint32_t skinned = 0; skinned < 2; ++skinned) {
            Source s = triangle(meshIndex++, 0);
            if ((free & 1U) != 0) {
                s.normals.assign(3, Vec3{0, 0, 1});
            }
            if ((free & 2U) != 0) {
                s.tangents.assign(3, Vec4{1, 0, 0, 1});
            }
            if ((free & 4U) != 0) {
                s.uv0.assign(3, Vec2{0, 0});
            }
            if ((free & 8U) != 0) {
                s.uv1.assign(3, Vec2{0, 0});
            }
            if ((free & 16U) != 0) {
                s.colors.assign(3, Vec4{1, 1, 1, 1});
            }
            if (skinned != 0) {
                s.joints.assign(3, std::array<std::uint16_t, 4>{0, 0, 0, 0});
                s.weights.assign(3, Vec4{1, 0, 0, 0});
            }
            sources.push_back(s);
        }
    }
    const Cooked c = cookSources(sources);
    CHECK(c.result.status == MeshCookStatus::Ok);
    CHECK(c.result.warnings.empty());
    CHECK(c.parsed.mesh.sections.size() == 64U);
    CHECK(c.parsed.mesh.sections.size() < engine::assets::MAX_COOKED_SECTIONS);
    CHECK(c.parsed.mesh.attributes.size() < engine::assets::MAX_COOKED_ATTRIBUTES);
    CHECK(c.parsed.mesh.submeshes.size() == 64U);
    // Ascending mask order shows up as a strictly non-decreasing stride across the 64 sections only
    // loosely, so the real ordering proof is that every submesh names a DISTINCT section.
    for (std::size_t i = 0; i < c.parsed.mesh.submeshes.size(); ++i) {
        CHECK(c.parsed.mesh.submeshes[i].sectionIndex == static_cast<std::uint32_t>(i));
    }
}

TEST_CASE("mesh cook: stats agree with the parsed container on every count (MC50)") {
    std::vector<Source> sources = {allAttributes(0, 0), triangle(1, 0), triangle(1, 1)};
    sources[2].indices = {0, 1, 2, 2, 1, 0};
    const Cooked c = cookSources(sources);
    CHECK(c.result.stats.sectionCount == c.parsed.mesh.sections.size());
    CHECK(c.result.stats.submeshCount == c.parsed.mesh.submeshes.size());
    CHECK(c.result.stats.indexCount == c.parsed.mesh.indexCount);
    CHECK(c.result.stats.byteSize == c.result.bytes.size());
    std::uint64_t vertices = 0;
    for (const auto& s : c.parsed.mesh.sections) {
        vertices += s.vertexCount;
    }
    CHECK(c.result.stats.vertexCount == vertices);
    CHECK(c.result.stats.droppedPrimitiveCount == 0U);
}

TEST_CASE("mesh cook: a battery of degenerate inputs never throws (MC51)") {
    // Empty spans, one-vertex primitives, and spans ALIASING each other -- the cook reads them and
    // copies nothing it does not write, so aliasing is legal by construction rather than by luck.
    std::vector<Vec3> shared = {Vec3{1, 2, 3}};
    std::vector<Vec4> shared4 = {Vec4{1, 2, 3, 4}};
    std::vector<std::uint32_t> ones = {0, 0, 0};

    MeshCookPrimitive aliased;
    aliased.sourceMeshIndex = 0;
    aliased.sourcePrimitiveIndex = 0;
    aliased.positions = shared;
    aliased.normals = shared;  // THE SAME BUFFER as positions
    aliased.tangents = shared4;
    aliased.weights = shared4;  // and again
    aliased.indices = ones;

    MeshCookPrimitive empty;
    empty.sourceMeshIndex = 1;
    empty.sourcePrimitiveIndex = 0;

    MeshCookPrimitive noIndices;
    noIndices.sourceMeshIndex = 2;
    noIndices.sourcePrimitiveIndex = 0;
    noIndices.positions = shared;

    const std::array<MeshCookPrimitive, 3> prims = {aliased, empty, noIndices};
    MeshCookInput in;
    in.primitives = prims;
    const MeshCookResult r = cookMesh(in);
    CHECK(r.status == MeshCookStatus::Ok);
    CHECK(r.stats.droppedPrimitiveCount == 2U);
    CHECK(r.stats.submeshCount == 1U);
    const auto parsed = engine::assets::parseCookedMesh(std::span<const std::byte>(r.bytes));
    CHECK(parsed.status == CookedMeshStatus::Ok);
    // joints was absent, so weights was dropped with it -- position, normal and tangent survive.
    CHECK(parsed.mesh.sections[0].vertexStride == 12U + 12U + 16U);
}

TEST_CASE("mesh cook: INV-C3 holds over a sweep of every input shape in this TU (MC52)") {
    // Table-driven, so a shape added later is covered without a new case. EVERY result with non-empty
    // bytes parses Ok -- including the drop arms, the demote arms, the cap arms and the empty cook.
    struct Shape {
        const char* name;
        std::vector<Source> sources;
    };
    std::vector<Shape> shapes;
    shapes.push_back({"empty", {}});
    shapes.push_back({"one triangle", {triangle(0, 0)}});
    shapes.push_back({"all attributes", {allAttributes(0, 0)}});
    shapes.push_back({"two masks", {triangle(0, 0), allAttributes(1, 0)}});
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].positions.clear();
        shapes.push_back({"dropped: no positions", s});
    }
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].indices = {0, 1};
        shapes.push_back({"dropped: index count", s});
    }
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].positions[2].z = std::numeric_limits<float>::quiet_NaN();
        shapes.push_back({"dropped: non-finite", s});
    }
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].normals.assign(1, Vec3{0, 0, 1});
        shapes.push_back({"demoted: normals", s});
    }
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].joints.assign(3, std::array<std::uint16_t, 4>{1, 2, 3, 4});
        shapes.push_back({"demoted: joints without weights", s});
    }
    shapes.push_back({"duplicate key", {triangle(7, 7), triangle(7, 7)}});
    {
        std::vector<Source> s = {triangle(0, 0)};
        s[0].positions.assign(65537, Vec3{1, 1, 1});
        shapes.push_back({"uint32 indices", s});
    }

    for (const Shape& shape : shapes) {
        CAPTURE(shape.name);
        const std::vector<MeshCookPrimitive> prims = views(shape.sources);
        MeshCookInput in;
        in.primitives = prims;
        const MeshCookResult r = cookMesh(in);
        REQUIRE_FALSE(r.bytes.empty());
        const auto parsed = engine::assets::parseCookedMesh(std::span<const std::byte>(r.bytes));
        CHECK(parsed.status == CookedMeshStatus::Ok);
        CHECK(parsed.message.empty());
        CHECK(r.bytes.size() == r.stats.byteSize);
    }
}
