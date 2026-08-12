// tests/editor/mesh_cook_source_test.cpp -- task 3.3.1: the ImportedModel -> mesh cook adapter, and
// the ONE place in the tree where editor::VertexAttribute and assets::CookedVertexSemantic are both
// visible. A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the model_import_test.cpp precedent): mesh_cook_source.hpp depends only on
// aero/editor/model_import.hpp and aero/assets/mesh_cook.hpp, and aero::assets is a PUBLIC, UNGATED
// dependency of aero_editor_core -- so every case here must be PRESENT and PASSING in all three build
// configurations. No GPU, no window, no ImGui context, no sleeps.
//
// It reads the SAME three frozen goldens aero_tests reads (tests/cooked_mesh_golden.hpp, through this
// target's existing ${CMAKE_CURRENT_SOURCE_DIR} include root) -- ONE GOLDEN, TWO BINARIES, NO DRIFT.
// MK7 is what makes that worth doing: it pins the whole editor path (fixture bytes -> importModel ->
// meshCookPrimitives -> cookMesh) against bytes aero_tests pins from the cook alone.
//
// DEVIATION from the plan's §T.3, recorded rather than worked around: MK12, MK13 and MK14 were
// specified against hierarchy.gltf, skinned.gltf and materials.gltf. Those three committed fixtures
// declare NO meshes at all (hierarchy.gltf is six nodes, skinned.gltf is a skin plus three clips,
// materials.gltf is one material and five images), so none of them can exercise a multi-mesh cook, a
// skinned vertex layout or a material index. Each case therefore drives a hand-authored glTF document
// with an embedded data URI -- the shape model_import_test.cpp already uses throughout -- and asserts
// exactly what the plan asked for.
#include <aero/assets/cooked_mesh.hpp>
#include <aero/assets/mesh_cook.hpp>
#include <aero/editor/mesh_cook_source.hpp>
#include <aero/editor/model_import.hpp>

#include "cooked_mesh_golden.hpp"
#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The byte-viewing pattern this whole suite uses: view a std::string as bytes with no copy. The
// returned span borrows from `text`, which the caller must keep alive.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

template <std::size_t N>
[[nodiscard]] std::vector<std::byte> goldenBytes(const std::array<std::uint8_t, N>& golden) {
    std::vector<std::byte> out;
    out.reserve(N);
    for (const std::uint8_t byte : golden) {
        out.push_back(static_cast<std::byte>(byte));
    }
    return out;
}

// REQUIRE the size first, then report the FIRST differing offset with both values. Never a bare
// CHECK over hundreds of bytes, whose failure output is unreadable.
void checkBytesEqual(std::span<const std::byte> actual, std::span<const std::byte> expected, const char* what) {
    REQUIRE_MESSAGE(actual.size() == expected.size(), what);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] == expected[i]) {
            continue;
        }
        const auto got = static_cast<unsigned>(actual[i]);
        const auto want = static_cast<unsigned>(expected[i]);
        const std::string detail =
            std::format("{}: first difference at offset {} -- got {:#04x}, expected {:#04x}", what, i, got, want);
        FAIL_CHECK(detail);
        return;
    }
}

// Bit-exact, not merely equal: -0.0f == +0.0f is true and their bytes are not, and AC-27's whole
// point is that the cook folds the same floats in the same order the importer did.
[[nodiscard]] bool sameBits(float a, float b) noexcept {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

[[nodiscard]] std::string readFixture(const std::string& path) {
    const scene_golden::FileBytes bytes = scene_golden::readBytes(path);
    REQUIRE_MESSAGE(bytes.ok, bytes.error);
    return bytes.text;
}

// Every committed model fixture that carries geometry, across all four parsers. damaged.gltf is
// deliberately absent -- it is a malformed document by design.
[[nodiscard]] std::vector<std::string> geometryFixtureNames() {
    return {"triangle.gltf", "asymmetric.gltf", "cube.obj", "cube-binary.fbx", "cube.dae", "cube.ply", "cube.stl"};
}

// One primitive's worth of hand-authored glTF, position-only, three vertices, one triangle. The
// buffers below were computed byte by byte and are annotated at each use site.
constexpr std::string_view TWO_MESH_DOC =
    R"({"asset": {"version": "2.0"}, "meshes": [)"
    R"({"name": "A", "primitives": [{"attributes": {"POSITION": 0}, "indices": 2, "mode": 4}]}, )"
    R"({"name": "B", "primitives": [{"attributes": {"POSITION": 1}, "indices": 2, "mode": 4}]}], )"
    R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}, )"
    R"({"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}, )"
    R"({"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}], )"
    R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, )"
    R"({"buffer": 0, "byteOffset": 36, "byteLength": 36}, )"
    R"({"buffer": 0, "byteOffset": 72, "byteLength": 6}], )"
    R"("buffers": [{"byteLength": 78, "uri": "data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAQAAAAAAAAAAAAAAAQAAAgD8AAAAAAAAAQAAAAAAAAIA/AAABAAIA"}]})";

// POSITION + JOINTS_0 (UNSIGNED_SHORT VEC4) + WEIGHTS_0 (float VEC4), three vertices, one triangle.
constexpr std::string_view SKINNED_DOC =
    R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": )"
    R"({"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3, "mode": 4}]}], )"
    R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}, )"
    R"({"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"}, )"
    R"({"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"}, )"
    R"({"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}], )"
    R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, )"
    R"({"buffer": 0, "byteOffset": 36, "byteLength": 24}, )"
    R"({"buffer": 0, "byteOffset": 60, "byteLength": 48}, )"
    R"({"buffer": 0, "byteOffset": 108, "byteLength": 6}], )"
    R"("buffers": [{"byteLength": 114, "uri": "data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAwABAAAAAAAAAAIAAwAAAAEAAACAPwAAAAAAAAAA)"
    R"(AAAAAAAAAD8AAAA/AAAAAAAAAAAAAIA+AACAPgAAgD4AAIA+AAABAAIA"}]})";

// ONE mesh, TWO primitives sharing both accessors: the first names material 0, the second names none.
constexpr std::string_view MATERIAL_DOC =
    R"({"asset": {"version": "2.0"}, "materials": [{"name": "Red"}], "meshes": [{"primitives": [)"
    R"({"attributes": {"POSITION": 0}, "indices": 1, "mode": 4, "material": 0}, )"
    R"({"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}]}], )"
    R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}, )"
    R"({"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}], )"
    R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, )"
    R"({"buffer": 0, "byteOffset": 36, "byteLength": 6}], )"
    R"("buffers": [{"byteLength": 42, "uri": "data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}]})";

}  // namespace

using engine::Guid;
using engine::assets::CookedMeshParseResult;
using engine::assets::CookedMeshStatus;
using engine::assets::CookedVertexFormat;
using engine::assets::CookedVertexSemantic;
using engine::assets::cookMesh;
using engine::assets::MeshCookInput;
using engine::assets::MeshCookPrimitive;
using engine::assets::MeshCookResult;
using engine::assets::MeshCookStatus;
using engine::assets::parseCookedMesh;
using engine::editor::cookImportedModel;
using engine::editor::ImportDepth;
using engine::editor::ImportedMesh;
using engine::editor::ImportedModel;
using engine::editor::ImportedPrimitive;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::INVALID_SUBASSET;
using engine::editor::meshCookPrimitives;
using engine::editor::VertexAttribute;

namespace {

// importModel over a document literal, at Full depth, with an empty assetRelativeDir and no external
// buffers -- the cooker's own call shape (task 3.3.1 §D-8's AC-38).
[[nodiscard]] ImportResult importDoc(std::string_view name, const std::string& doc, const ImportSettings& settings) {
    return importModel(name, "", asBytes(doc), settings, ImportDepth::Full, {});
}

[[nodiscard]] CookedMeshParseResult parseCooked(const MeshCookResult& cooked) {
    return parseCookedMesh(std::span<const std::byte>(cooked.bytes));
}

}  // namespace

// ---- MK1: the ONE place both enums are visible ------------------------------------------------
TEST_CASE("mesh_cook_source: CookedVertexSemantic mirrors VertexAttribute's bit positions (MK1)") {
    // engine/assets may never include an editor header, so this correspondence has no home inside
    // either layer. The layout builder walks 0..7 and treats "bit n set" as "semantic n present";
    // renumbering either enum silently re-maps every cooked vertex.
    struct Row {
        VertexAttribute attribute;
        CookedVertexSemantic semantic;
    };
    const std::array<Row, 8> rows = {
        Row{VertexAttribute::Position, CookedVertexSemantic::Position},
        Row{VertexAttribute::Normal, CookedVertexSemantic::Normal},
        Row{VertexAttribute::Tangent, CookedVertexSemantic::Tangent},
        Row{VertexAttribute::TexCoord0, CookedVertexSemantic::TexCoord0},
        Row{VertexAttribute::TexCoord1, CookedVertexSemantic::TexCoord1},
        Row{VertexAttribute::Color0, CookedVertexSemantic::Color0},
        Row{VertexAttribute::Joints0, CookedVertexSemantic::Joints0},
        Row{VertexAttribute::Weights0, CookedVertexSemantic::Weights0},
    };
    for (const Row& row : rows) {
        const auto bit = static_cast<std::uint32_t>(row.attribute);
        const auto position = static_cast<std::uint32_t>(row.semantic);
        CHECK(bit == (1U << position));
    }
    CHECK(engine::assets::COOKED_SEMANTIC_COUNT == rows.size());
}

// ---- MK2: the two sentinels ---------------------------------------------------------------------
TEST_CASE("mesh_cook_source: INVALID_SUBASSET and COOKED_INVALID_MATERIAL are the same value (MK2)") {
    // mesh_cook_source.cpp carries the compile-time half; this is the runtime half, where a reader
    // looks. materialIndex is copied VERBATIM precisely because these agree.
    CHECK(INVALID_SUBASSET == engine::assets::COOKED_INVALID_MATERIAL);
}

// ---- MK3-MK6: the flattening --------------------------------------------------------------------
TEST_CASE("mesh_cook_source: an empty model flattens to an empty vector (MK3)") {
    const ImportedModel model;
    CHECK(meshCookPrimitives(model).empty());
}

TEST_CASE("mesh_cook_source: an empty mesh contributes nothing and renumbers nothing (MK4)") {
    ImportedModel model;
    model.meshes.emplace_back();  // mesh 0: no primitives at all
    ImportedMesh second;
    second.primitives.emplace_back();
    model.meshes.push_back(std::move(second));

    const std::vector<MeshCookPrimitive> flat = meshCookPrimitives(model);
    REQUIRE(flat.size() == 1);
    // The index recorded is the POSITION in model.meshes, which an empty neighbour cannot move.
    CHECK(flat[0].sourceMeshIndex == 1);
    CHECK(flat[0].sourcePrimitiveIndex == 0);
}

TEST_CASE("mesh_cook_source: primitives come out in ascending (meshIndex, primitiveIndex) order (MK5)") {
    ImportedModel model;
    const std::array<std::size_t, 3> counts = {2, 0, 3};
    for (const std::size_t count : counts) {
        ImportedMesh mesh;
        mesh.primitives.resize(count);
        model.meshes.push_back(std::move(mesh));
    }

    const std::vector<MeshCookPrimitive> flat = meshCookPrimitives(model);
    REQUIRE(flat.size() == 5);
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 5> expected = {
        std::pair<std::uint32_t, std::uint32_t>{0, 0}, {0, 1}, {2, 0}, {2, 1}, {2, 2},
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(flat[i].sourceMeshIndex == expected[i].first);
        CHECK(flat[i].sourcePrimitiveIndex == expected[i].second);
    }
}

TEST_CASE("mesh_cook_source: the spans point INTO the model, never at a copy (MK6)") {
    const std::string doc(SKINNED_DOC);
    const ImportResult imported = importDoc("skin.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);
    REQUIRE(imported.model.meshes.size() == 1);
    REQUIRE(imported.model.meshes[0].primitives.size() == 1);

    const ImportedPrimitive& src = imported.model.meshes[0].primitives[0];
    const std::vector<MeshCookPrimitive> flat = meshCookPrimitives(imported.model);
    REQUIRE(flat.size() == 1);
    // Compared by data(), not by value: a copy would compare equal and dangle exactly as badly.
    CHECK(flat[0].positions.data() == src.positions.data());
    CHECK(flat[0].joints.data() == src.joints.data());
    CHECK(flat[0].weights.data() == src.weights.data());
    CHECK(flat[0].indices.data() == src.indices.data());
    CHECK(flat[0].materialIndex == src.materialIndex);
}

// ---- MK7/MK8: the golden, end to end from the fixture -------------------------------------------
TEST_CASE("mesh_cook_source: triangle.gltf imported Full and cooked nil is Golden B (MK7, AC-30)") {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/triangle.gltf");
    const ImportResult imported = importDoc("triangle.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);

    const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
    CHECK(cooked.status == MeshCookStatus::Ok);
    const std::vector<std::byte> expected = goldenBytes(aero_test::COOKED_GOLDEN_TRIANGLE);
    checkBytesEqual(cooked.bytes, expected, "triangle.gltf -> COOKED_GOLDEN_TRIANGLE");
}

TEST_CASE("mesh_cook_source: a non-nil sourceGuid changes exactly the sixteen GUID bytes (MK8)") {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/triangle.gltf");
    const ImportResult imported = importDoc("triangle.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);

    const MeshCookResult nil = cookImportedModel(imported.model, Guid{});
    // Every one of these sixteen bytes is non-zero, so "differs in exactly sixteen bytes" is an
    // exact count rather than an upper bound.
    const Guid guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const MeshCookResult named = cookImportedModel(imported.model, guid);
    REQUIRE(nil.bytes.size() == named.bytes.size());

    std::size_t differing = 0;
    for (std::size_t i = 0; i < nil.bytes.size(); ++i) {
        if (nil.bytes[i] != named.bytes[i]) {
            ++differing;
            CHECK(i >= 16);
            CHECK(i < 32);
        }
    }
    CHECK(differing == 16);
    const CookedMeshParseResult parsed = parseCooked(named);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    CHECK(parsed.mesh.sourceGuid == guid);
}

// ---- MK9: AC-27, bit for bit, over every committed geometry fixture -----------------------------
TEST_CASE("mesh_cook_source: cooked bounds equal the importer's own, bit for bit (MK9, AC-27)") {
    std::size_t fixturesCovered = 0;
    for (const std::string& name : geometryFixtureNames()) {
        CAPTURE(name);
        const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/" + name);
        const ImportResult imported = importDoc(name, doc, ImportSettings{});
        REQUIRE((imported.status == ImportStatus::Ok || imported.status == ImportStatus::Truncated));

        const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
        const CookedMeshParseResult parsed = parseCooked(cooked);
        REQUIRE(parsed.status == CookedMeshStatus::Ok);
        if (parsed.mesh.submeshes.empty()) {
            continue;  // a fixture with no geometry proves nothing here and is not an error
        }
        ++fixturesCovered;

        for (const auto& submesh : parsed.mesh.submeshes) {
            REQUIRE(submesh.sourceMeshIndex < imported.model.meshes.size());
            const ImportedMesh& mesh = imported.model.meshes[submesh.sourceMeshIndex];
            REQUIRE(submesh.sourcePrimitiveIndex < mesh.primitives.size());
            const ImportedPrimitive& prim = mesh.primitives[submesh.sourcePrimitiveIndex];
            CHECK(sameBits(submesh.bounds.min.x, prim.bounds.min.x));
            CHECK(sameBits(submesh.bounds.min.y, prim.bounds.min.y));
            CHECK(sameBits(submesh.bounds.min.z, prim.bounds.min.z));
            CHECK(sameBits(submesh.bounds.max.x, prim.bounds.max.x));
            CHECK(sameBits(submesh.bounds.max.y, prim.bounds.max.y));
            CHECK(sameBits(submesh.bounds.max.z, prim.bounds.max.z));
        }
        // The model box is the union of the EMITTED submeshes, folded exactly as
        // ImportSummary::bounds folds over the surviving primitives.
        const auto& summary = imported.model.summary.bounds;
        CHECK(sameBits(parsed.mesh.bounds.min.x, summary.min.x));
        CHECK(sameBits(parsed.mesh.bounds.min.y, summary.min.y));
        CHECK(sameBits(parsed.mesh.bounds.min.z, summary.min.z));
        CHECK(sameBits(parsed.mesh.bounds.max.x, summary.max.x));
        CHECK(sameBits(parsed.mesh.bounds.max.y, summary.max.y));
        CHECK(sameBits(parsed.mesh.bounds.max.z, summary.max.z));
    }
    // Anti-vacuity: a silently-empty fixture list would pass every assertion above.
    CHECK(fixturesCovered >= 4);
}

// ---- MK10: --scale flows end to end, and the cook itself applies nothing -------------------------
TEST_CASE("mesh_cook_source: ImportSettings::scale reaches cooked positions and bounds (MK10, AC-32)") {
    const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/triangle.gltf");
    const ImportResult plain = importDoc("triangle.gltf", doc, ImportSettings{});
    REQUIRE(plain.status == ImportStatus::Ok);
    ImportSettings doubled;
    doubled.scale = 2.0F;
    const ImportResult scaled = importDoc("triangle.gltf", doc, doubled);
    REQUIRE(scaled.status == ImportStatus::Ok);

    const MeshCookResult plainCooked = cookImportedModel(plain.model, Guid{});
    const MeshCookResult scaledCooked = cookImportedModel(scaled.model, Guid{});
    // Same shape, different numbers: scaling changes no count, no offset and no layout.
    REQUIRE(plainCooked.bytes.size() == scaledCooked.bytes.size());
    CHECK(plainCooked.bytes != scaledCooked.bytes);

    const CookedMeshParseResult parsed = parseCooked(scaledCooked);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    REQUIRE(parsed.mesh.sections.size() == 1);
    CHECK(parsed.mesh.bounds.max.x == doctest::Approx(2.0F));
    CHECK(parsed.mesh.bounds.max.y == doctest::Approx(2.0F));

    // Read the three vertices back through the section's own stride: (0,0,0) (2,0,0) (0,2,0).
    const std::span<const std::byte> vertexData = engine::assets::sectionVertexBytes(parsed.mesh, 0);
    const std::uint32_t stride = parsed.mesh.sections[0].vertexStride;
    REQUIRE(stride == 12);
    REQUIRE(vertexData.size() == 36);
    CHECK(engine::assets::getF32(vertexData, stride) == doctest::Approx(2.0F));
    CHECK(engine::assets::getF32(vertexData, (2 * stride) + 4) == doctest::Approx(2.0F));
}

// ---- MK11: the derived mask cross-checks the importer's own bitset ------------------------------
TEST_CASE("mesh_cook_source: the derived attribute mask agrees with ImportedPrimitive::attributes (MK11)") {
    std::size_t primitivesChecked = 0;
    for (const std::string& name : geometryFixtureNames()) {
        CAPTURE(name);
        const std::string doc = readFixture(std::string(AERO_ASSET_FIXTURES_DIR) + "/" + name);
        const ImportResult imported = importDoc(name, doc, ImportSettings{});
        REQUIRE((imported.status == ImportStatus::Ok || imported.status == ImportStatus::Truncated));
        const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
        const CookedMeshParseResult parsed = parseCooked(cooked);
        REQUIRE(parsed.status == CookedMeshStatus::Ok);

        for (const auto& submesh : parsed.mesh.submeshes) {
            REQUIRE(submesh.sectionIndex < parsed.mesh.sections.size());
            const auto& section = parsed.mesh.sections[submesh.sectionIndex];
            std::uint32_t derived = 0;
            for (std::uint32_t a = 0; a < section.attributeCount; ++a) {
                const auto& attribute = parsed.mesh.attributes[section.firstAttribute + a];
                derived |= 1U << static_cast<std::uint32_t>(attribute.semantic);
            }
            const ImportedPrimitive& prim =
                imported.model.meshes[submesh.sourceMeshIndex].primitives[submesh.sourcePrimitiveIndex];
            // A redundancy turned into a cross-check: the adapter never reads `attributes`, so this
            // is the importer's claim measured against what the arrays actually contain.
            CHECK(derived == static_cast<std::uint32_t>(prim.attributes));
            ++primitivesChecked;
        }
    }
    CHECK(primitivesChecked >= 4);
}

// ---- MK12: sourceMeshIndex is the POSITION in model.meshes --------------------------------------
TEST_CASE("mesh_cook_source: a multi-mesh model records each submesh's position in model.meshes (MK12)") {
    const std::string doc(TWO_MESH_DOC);
    const ImportResult imported = importDoc("two-meshes.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);
    REQUIRE(imported.model.meshes.size() == 2);

    const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
    const CookedMeshParseResult parsed = parseCooked(cooked);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    REQUIRE(parsed.mesh.submeshes.size() == 2);
    // Both primitives share one mask, so they share one section -- and the submeshes are ordered by
    // (mask, sourceMeshIndex, sourcePrimitiveIndex), which here is source order.
    CHECK(parsed.mesh.sections.size() == 1);
    for (std::uint32_t m = 0; m < 2; ++m) {
        const auto& submesh = parsed.mesh.submeshes[m];
        CHECK(submesh.sourceMeshIndex == m);
        CHECK(submesh.sourcePrimitiveIndex == 0);
        // The position resolves: mesh m's own first primitive is what this submesh describes.
        const ImportedPrimitive& prim = imported.model.meshes[submesh.sourceMeshIndex].primitives[0];
        CHECK(sameBits(submesh.bounds.max.x, prim.bounds.max.x));
    }
    // Mesh B sits at x == 2, mesh A at x <= 1: a swapped mapping would fail here and nowhere else.
    CHECK(parsed.mesh.submeshes[0].bounds.max.x == doctest::Approx(1.0F));
    CHECK(parsed.mesh.submeshes[1].bounds.max.x == doctest::Approx(2.0F));
}

// ---- MK13: the skinned layout -------------------------------------------------------------------
TEST_CASE("mesh_cook_source: joints cook as Uint4 and weights as Float4 (MK13)") {
    const std::string doc(SKINNED_DOC);
    const ImportResult imported = importDoc("skin.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);
    REQUIRE(imported.model.meshes.size() == 1);
    REQUIRE(imported.model.meshes[0].primitives[0].joints.size() == 3);

    const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
    const CookedMeshParseResult parsed = parseCooked(cooked);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    REQUIRE(parsed.mesh.sections.size() == 1);
    const auto& section = parsed.mesh.sections[0];
    REQUIRE(section.attributeCount == 3);

    bool sawJoints = false;
    bool sawWeights = false;
    std::uint32_t previousOffset = 0;
    for (std::uint32_t a = 0; a < section.attributeCount; ++a) {
        const auto& attribute = parsed.mesh.attributes[section.firstAttribute + a];
        if (a > 0) {
            CHECK(attribute.offset > previousOffset);  // ascending semantic code, ascending offset
        }
        previousOffset = attribute.offset;
        if (attribute.semantic == CookedVertexSemantic::Joints0) {
            sawJoints = true;
            CHECK(attribute.format == CookedVertexFormat::Uint4);  // never a u16x4: rhi has no such format
        }
        if (attribute.semantic == CookedVertexSemantic::Weights0) {
            sawWeights = true;
            CHECK(attribute.format == CookedVertexFormat::Float4);
        }
    }
    CHECK(sawJoints);
    CHECK(sawWeights);
    // Position 12 + Joints0 16 + Weights0 16.
    CHECK(section.vertexStride == 44);
}

// ---- MK14: materialIndex is preserved VERBATIM, sentinel included -------------------------------
TEST_CASE("mesh_cook_source: each submesh carries its primitive's materialIndex verbatim (MK14)") {
    const std::string doc(MATERIAL_DOC);
    const ImportResult imported = importDoc("materials-inline.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);
    REQUIRE(imported.model.meshes.size() == 1);
    REQUIRE(imported.model.meshes[0].primitives.size() == 2);

    const MeshCookResult cooked = cookImportedModel(imported.model, Guid{});
    const CookedMeshParseResult parsed = parseCooked(cooked);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    REQUIRE(parsed.mesh.submeshes.size() == 2);
    for (const auto& submesh : parsed.mesh.submeshes) {
        const ImportedPrimitive& prim =
            imported.model.meshes[submesh.sourceMeshIndex].primitives[submesh.sourcePrimitiveIndex];
        CHECK(submesh.materialIndex == prim.materialIndex);
    }
    CHECK(parsed.mesh.submeshes[0].materialIndex == 0);
    // The second primitive names no material at all: the sentinel travels, never a zero index.
    CHECK(parsed.mesh.submeshes[1].materialIndex == INVALID_SUBASSET);
}

// ---- MK15: the convenience is not a second policy ------------------------------------------------
TEST_CASE("mesh_cook_source: cookImportedModel equals cookMesh(meshCookPrimitives(model)) (MK15)") {
    const std::string doc(TWO_MESH_DOC);
    const ImportResult imported = importDoc("two-meshes.gltf", doc, ImportSettings{});
    REQUIRE(imported.status == ImportStatus::Ok);

    const Guid guid{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    const MeshCookResult viaHelper = cookImportedModel(imported.model, guid);
    const std::vector<MeshCookPrimitive> flat = meshCookPrimitives(imported.model);
    MeshCookInput input;
    input.sourceGuid = guid;
    input.primitives = flat;
    const MeshCookResult viaHand = cookMesh(input);
    checkBytesEqual(viaHelper.bytes, viaHand.bytes, "cookImportedModel vs cookMesh");
    CHECK(viaHelper.status == viaHand.status);
    CHECK(viaHelper.stats.submeshCount == viaHand.stats.submeshCount);
}

// ---- MK16: every primitive dropped is still a valid file ----------------------------------------
TEST_CASE("mesh_cook_source: a model whose every primitive is dropped cooks to the empty file (MK16, AC-16)") {
    ImportedModel model;
    ImportedMesh mesh;
    ImportedPrimitive prim;  // no positions, no indices -- dropped whole by the cook
    mesh.primitives.push_back(std::move(prim));
    model.meshes.push_back(std::move(mesh));

    const MeshCookResult cooked = cookImportedModel(model, Guid{});
    CHECK(cooked.status == MeshCookStatus::Ok);  // a dropped primitive is a warning, never a failure
    CHECK(cooked.stats.droppedPrimitiveCount == 1);
    CHECK(cooked.warnings.size() >= 2);  // the drop, and "no cookable primitives"
    const std::vector<std::byte> expected = goldenBytes(aero_test::COOKED_GOLDEN_EMPTY);
    checkBytesEqual(cooked.bytes, expected, "all-dropped model -> COOKED_GOLDEN_EMPTY");
    const CookedMeshParseResult parsed = parseCooked(cooked);
    CHECK(parsed.status == CookedMeshStatus::Ok);
}

// ---- MK17: the mask is DERIVED from the arrays, never read from the bitset ----------------------
TEST_CASE("mesh_cook_source: the cook derives the mask from the ARRAYS, not from `attributes` (MK17)") {
    // THE GAP SEED S34 FOUND. MK11 cross-checks the derived mask against ImportedPrimitive::attributes
    // on every committed fixture -- and every one of them is internally consistent, so an adapter
    // rewritten to GATE each array on that bitset produced identical output for all of them and MK11
    // stayed green. The property MK11 cannot state is which of the two is the AUTHORITY.
    //
    // Both directions are driven from one hand-built model, because a bitset can lie either way and
    // the two lies fail differently: an under-claim would silently drop a real attribute out of the
    // vertex layout, an over-claim would put an attribute in the layout with no data behind it.
    ImportedModel model;
    ImportedMesh mesh;

    // Primitive 0 -- the bitset UNDER-claims: real normals, and `attributes` says Position only.
    ImportedPrimitive understated;
    understated.attributes = VertexAttribute::Position;
    understated.positions = {engine::Vec3{0, 0, 0}, engine::Vec3{1, 0, 0}, engine::Vec3{0, 1, 0}};
    understated.normals = {engine::Vec3{0, 0, 1}, engine::Vec3{0, 0, 1}, engine::Vec3{0, 0, 1}};
    understated.indices = {0, 1, 2};
    mesh.primitives.push_back(std::move(understated));

    // Primitive 1 -- the bitset OVER-claims: it names TexCoord0, and there is no uv0 array at all.
    ImportedPrimitive overstated;
    overstated.attributes = VertexAttribute::Position | VertexAttribute::TexCoord0;
    overstated.positions = {engine::Vec3{5, 0, 0}, engine::Vec3{6, 0, 0}, engine::Vec3{5, 1, 0}};
    overstated.indices = {0, 1, 2};
    mesh.primitives.push_back(std::move(overstated));
    model.meshes.push_back(std::move(mesh));

    const MeshCookResult cooked = cookImportedModel(model, Guid{});
    CHECK(cooked.status == MeshCookStatus::Ok);
    const CookedMeshParseResult parsed = parseCooked(cooked);
    REQUIRE(parsed.status == CookedMeshStatus::Ok);
    REQUIRE(parsed.mesh.submeshes.size() == 2);
    // TWO sections, because the two derived masks differ -- 0x03 and 0x01 -- which is only true if the
    // arrays decided. Had the bitset decided, both would be Position-only and share ONE section.
    REQUIRE(parsed.mesh.sections.size() == 2);

    // Ascending mask, so section 0 is the position-only one and section 1 carries the normal.
    CHECK(parsed.mesh.sections[0].vertexStride == 12);
    CHECK(parsed.mesh.sections[1].vertexStride == 24);
    REQUIRE(parsed.mesh.sections[1].attributeCount == 2);
    CHECK(parsed.mesh.attributes[parsed.mesh.sections[1].firstAttribute + 1].semantic == CookedVertexSemantic::Normal);
    // The over-claiming primitive is the one in section 0: no uv0 array, so no TexCoord0 attribute,
    // whatever its bitset says.
    CHECK(parsed.mesh.submeshes[0].sourcePrimitiveIndex == 1);
    CHECK(parsed.mesh.submeshes[0].sectionIndex == 0);
    CHECK(parsed.mesh.submeshes[1].sourcePrimitiveIndex == 0);
    CHECK(parsed.mesh.submeshes[1].sectionIndex == 1);
}
