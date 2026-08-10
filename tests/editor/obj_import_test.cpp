// tests/editor/obj_import_test.cpp -- task 3.2.3: the Wavefront OBJ/MTL backend (tinyobjloader). A TU
// of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the fbx_import_test.cpp / model_import_test.cpp precedent): model_import.hpp
// depends only on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and
// aero/editor/scene_bounds.hpp -- the last of those reaches aero::scene, a PUBLIC, UNGATED dependency
// of aero_editor_core (only engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in
// this file must be PRESENT and PASSING in both reduced configurations, not merely the default build.
// No GPU, no window, no ImGui context, no sleeps: nearly every case is a raw string literal, so the
// suite runs with zero disk (the two exceptions are AERO_ASSET_FIXTURES_DIR's committed cube.obj/
// cube.mtl, reached through readFileBytes, once tinyobjloader's real-file path needs proving).
//
// THIS TU NAMES NO tinyobjloader TYPE (AC-11's sibling check, §V4/§V6) -- it drives the OBJ backend
// only indirectly, through the PUBLIC importModel() dispatch. obj_import.hpp is src-private and stays
// that way; nothing here #includes it.
#include <aero/editor/model_import.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The fastgltf/ufbx-free byte-loading pattern model_import_test.cpp's and fbx_import_test.cpp's own
// asBytes use, restated here so this TU stays independent of both.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

}  // namespace

using engine::Vec3;
using engine::Vec4;
using engine::editor::ExternalBuffer;
using engine::editor::has;
using engine::editor::ImportDepth;
using engine::editor::ImportedPrimitive;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::INVALID_SUBASSET;
using engine::editor::MAX_EXTERNAL_URIS;
using engine::editor::MAX_IMPORT_WARNINGS;
using engine::editor::MAX_PRIMITIVES_PER_MODEL;
using engine::editor::VertexAttribute;

// task 3.2.3, Step 5: PROMOTED from Step 2/3/4's placeholder to AC-25's real one-triangle Full case,
// now that geometry conversion exists. Exists from Step 2 so AC-69's "OI1 present in both reduced
// configurations" has something to find, and so this file is tracked before the guards next run.
TEST_CASE("obj_import: a one-triangle body imports as one mesh, one primitive, three vertices (OI1, AC-25)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.materialIndex == INVALID_SUBASSET);  // Step 6 buckets by material
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[0] == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(prim.positions[1] == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(prim.positions[2] == Vec3{0.0F, 1.0F, 0.0F});
    REQUIRE(prim.indices.size() == 3);
    CHECK(prim.indices[0] == 0);
    CHECK(prim.indices[1] == 1);
    CHECK(prim.indices[2] == 2);
    CHECK(has(prim.attributes, VertexAttribute::Position));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Normal));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::TexCoord0));
    CHECK(prim.normals.empty());
    CHECK(prim.uv0.empty());
    CHECK(result.model.summary.meshCount == 1);
    CHECK(result.model.summary.primitiveCount == 1);
    CHECK(result.model.summary.vertexCount == 3);
    CHECK(result.model.summary.triangleCount == 1);
    CHECK(result.model.summary.bounds.valid());
    CHECK(result.model.summary.bounds.min == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(result.model.summary.bounds.max == Vec3{1.0F, 1.0F, 0.0F});
}

TEST_CASE("obj_import: Structure depth on a well-formed body with an mtllib is empty except the URI set (OI2, AC-19)") {
    const std::string doc = "mtllib chair.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "chair.mtl");
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
    CHECK(result.model.materials.empty());
    CHECK(result.model.images.empty());
    CHECK(result.model.skins.empty());
    CHECK(result.model.animations.empty());
    CHECK_FALSE(result.model.summary.bounds.valid());  // Aabb::empty() -- nothing decoded at Structure
}

TEST_CASE(
    "obj_import: the AC-20 discriminator -- Structure never enters the library, so it survives what "
    "would fail Full (OI3, AC-20)") {
    // A well-formed mtllib over a body whose ONLY face uses a ZERO vertex index (F4b): once Step 4
    // lands, a Full parse of this body fails outright (LoadObj returns false on `f 0 1 2`). If Structure
    // entered the library at all, it would fail identically -- it does not, so it survives.
    //
    // task 3.2.3, Step 3: only the Structure half is meaningful yet -- the Full half of this
    // discriminating pair is added at Step 4, once the library is actually entered there.
    const std::string doc = "mtllib chair.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n";
    const ImportResult structure =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(structure.status == ImportStatus::Ok);
    REQUIRE(structure.externalUris.size() == 1);
    CHECK(structure.externalUris[0] == "chair.mtl");
}

TEST_CASE("obj_import: no mtllib directive at all -- empty URI set, no warning (OI4, E1)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    CHECK(result.warnings.empty());
    CHECK(result.warningTotal == 0);
}

TEST_CASE("obj_import: a Windows-authored backslash mtllib path is folded and accepted (OI5, AC-14, D15)") {
    const std::string doc = "mtllib textures\\chair.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "textures/chair.mtl");
    CHECK(result.warnings.empty());
}

TEST_CASE(
    "obj_import: an unescaped space in an mtllib operand offers the whole operand AND each token (OI6, "
    "AC-16 first half, D16)") {
    const std::string doc = "mtllib my file.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 3);
    CHECK(result.externalUris[0] == "my file.mtl");  // the WHOLE operand LEADS
    CHECK(result.externalUris[1] == "my");
    CHECK(result.externalUris[2] == "file.mtl");
}

TEST_CASE(
    "obj_import: a backslash-escaped space is NOT supported -- the intended reading is never a "
    "candidate (OI7, AC-16 second half, D16)") {
    // `mtllib my\ file.mtl` -- a backslash cannot be both a path separator (D15's fold, for Windows
    // exporters) and an escape (a POSIX convention). This is a STATED, ACCEPTED non-support, not a bug.
    const std::string doc = "mtllib my\\ file.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    for (const std::string& uri : result.externalUris) {
        CHECK(uri != "my file.mtl");  // the INTENDED reading never appears
    }
}

TEST_CASE("obj_import: a comment and leading whitespace, and CASE-SENSITIVE matching (OI8, E5, D16)") {
    const std::string doc = "# mtllib fake.mtl\n  mtllib real.mtl\nMTLLIB nope.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "real.mtl");
}

TEST_CASE("obj_import: an mtllib line with an empty operand produces no candidate and one warning (OI9, E4)") {
    const std::string doc = "mtllib   \nv 0 0 0\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    CHECK(result.warningTotal == 1);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("empty operand") != std::string::npos);
}

TEST_CASE("obj_import: several mtllib lines are ALL collected, in order (OI10, E3)") {
    const std::string doc = "mtllib a.mtl\nmtllib b.mtl\nmtllib c.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 3);
    CHECK(result.externalUris[0] == "a.mtl");
    CHECK(result.externalUris[1] == "b.mtl");
    CHECK(result.externalUris[2] == "c.mtl");
}

TEST_CASE("obj_import: CRLF line endings are handled -- the '\\r' never leaks into a candidate (OI11, E7)") {
    const std::string doc = "mtllib chair.mtl\r\nv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "chair.mtl");
}

TEST_CASE("obj_import: no trailing newline -- the last line is still scanned (OI12, E8)") {
    const std::string doc = "mtllib chair.mtl";  // deliberately NO trailing '\n'
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "chair.mtl");
}

TEST_CASE(
    "obj_import: hitting the external-reference cap reports Truncated with a message naming it "
    "(OI13, INV-O10)") {
    std::string doc;
    for (std::size_t i = 0; i < MAX_EXTERNAL_URIS + 6; ++i) {
        doc += "mtllib file" + std::to_string(i) + ".mtl\n";
    }
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.externalUris.size() == MAX_EXTERNAL_URIS);
}

TEST_CASE("obj_import: every reachable refusal class carries classifyUri's own exact reason (OI14, AC-17/AC-18)") {
    const std::string doc =
        "mtllib http://example.com/x.mtl\n"
        "mtllib /etc/x.mtl\n"
        "mtllib ../../outside.mtl\n"
        "mtllib ./\n"
        "mtllib bad\x01name.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);  // every candidate REFUSED, none of them a hard failure
    CHECK(result.externalUris.empty());
    REQUIRE(result.warnings.size() == 5);
    std::size_t sawScheme = 0;
    std::size_t sawAbsolute = 0;
    std::size_t sawEscape = 0;
    std::size_t sawEmpty = 0;
    std::size_t sawControlChar = 0;
    for (const std::string& warning : result.warnings) {
        sawScheme += warning.find("names a scheme") != std::string::npos ? 1 : 0;
        sawAbsolute += warning.find("is an absolute path") != std::string::npos ? 1 : 0;
        sawEscape += warning.find("resolves outside the project's assets folder") != std::string::npos ? 1 : 0;
        sawEmpty += warning.find("resolves to nothing") != std::string::npos ? 1 : 0;
        sawControlChar += warning.find("contains a control character") != std::string::npos ? 1 : 0;
    }
    CHECK(sawScheme == 1);
    CHECK(sawAbsolute == 1);
    CHECK(sawEscape == 1);
    CHECK(sawEmpty == 1);
    CHECK(sawControlChar == 1);
}

TEST_CASE(
    "obj_import: '..' is accepted from a subdirectory and refused at the assets root (OI15, AC-15, "
    "the .mtl half)") {
    const std::string doc = "mtllib ../shared/common.mtl\n";
    const ImportResult fromModels =
        importModel("chair.obj", "models", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(fromModels.status == ImportStatus::Ok);
    REQUIRE(fromModels.externalUris.size() == 1);
    CHECK(fromModels.externalUris[0] == "shared/common.mtl");

    const ImportResult fromRoot =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(fromRoot.status == ImportStatus::Ok);
    CHECK(fromRoot.externalUris.empty());
    REQUIRE(fromRoot.warnings.size() == 1);
    CHECK(fromRoot.warnings[0].find("resolves outside the project's assets folder") != std::string::npos);
}

TEST_CASE("obj_import: an operand that normalises to nothing is RefusedEmpty (OI16, E10)") {
    const std::string doc = "mtllib ./\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("resolves to nothing") != std::string::npos);
}

TEST_CASE(
    "obj_import: two differently-spelled candidates resolving to the SAME path deduplicate to one entry "
    "(OI17, E3)") {
    const std::string doc = "mtllib textures\\wood.mtl\nmtllib textures/wood.mtl\n";
    const ImportResult result =
        importModel("chair.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "textures/wood.mtl");
}

TEST_CASE("obj_import: a NUL byte in the first 1024 bytes is Malformed on the .obj arm (OI18, AC-54)") {
    std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    doc[3] = '\0';
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Malformed);
    CHECK(result.message == "this file is not text");
}

TEST_CASE("obj_import: PNG, JPEG and GLB magic all trip the binary check through the real backend (OI19, AC-54)") {
    // Each format's length/version field puts a NUL inside the first 16 bytes (§D-4(f)'s own reasoning).
    const std::string png("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR", 16);
    const std::string jpeg("\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01", 10);
    const std::string glb("glTF\x02\x00\x00\x00", 8);
    for (const std::string& magic : {png, jpeg, glb}) {
        const ImportResult result =
            importModel("t.obj", "", asBytes(magic), ImportSettings{}, ImportDepth::Structure, {});
        INFO("magic size: ", magic.size());
        CHECK(result.status == ImportStatus::Malformed);
        CHECK(result.message == "this file is not text");
    }
}

TEST_CASE(
    "obj_import: the binary probe is a WINDOW, not a whole-file requirement -- a long NUL-free body is "
    "never mistaken for binary (OI20)") {
    std::string doc;
    for (int i = 0; i < 80; ++i) {
        doc += "# padding comment line to push this file past the 1024-byte probe window\n";
    }
    doc += "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    REQUIRE(doc.size() > 1024);
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status != ImportStatus::Malformed);
}

TEST_CASE("obj_import: zero 'v' lines at all is Malformed (OI21, AC-55)") {
    const std::string doc = "# a comment, and nothing else\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Malformed);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("obj_import: an empty file and a whitespace-only file are both Malformed (OI22, AC-56)") {
    const ImportResult empty =
        importModel("t.obj", "", asBytes(std::string("")), ImportSettings{}, ImportDepth::Full, {});
    CHECK(empty.status == ImportStatus::Malformed);
    const std::string whitespaceOnly = "   \n\t\n   \n";
    const ImportResult ws = importModel("t.obj", "", asBytes(whitespaceOnly), ImportSettings{}, ImportDepth::Full, {});
    CHECK(ws.status == ImportStatus::Malformed);
}

TEST_CASE(
    "obj_import: two mtllib directives -- BOTH of the library's own single-stream sentences are absent "
    "(OI23, AC-57b, §A-10)") {
    const std::string doc = "mtllib a.mtl\nmtllib b.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::vector<ExternalBuffer> externals = {
        ExternalBuffer{"a.mtl", "newmtl foo\nKd 1 0 0\n"},
        ExternalBuffer{"b.mtl", "newmtl bar\nKd 0 1 0\n"},
    };
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, externals);
    CHECK(result.status != ImportStatus::ParseFailed);
    for (const std::string& warning : result.warnings) {
        CHECK(warning.find("Material stream in error state") == std::string::npos);
        CHECK(warning.find("Failed to load material file") == std::string::npos);
    }
}

TEST_CASE("obj_import: a battery of hostile-but-valid inputs never throws (OI24, AC-60)") {
    {
        const std::string oneMegLine(static_cast<std::size_t>(1024) * 1024, 'x');
        const ImportResult result =
            importModel("t.obj", "", asBytes(oneMegLine), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Malformed);  // no recognisable 'v'/'f' content at all
    }
    {
        const std::string facesOnly = "f 1 2 3\nf 4 5 6\n";
        const ImportResult result =
            importModel("t.obj", "", asBytes(facesOnly), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Malformed);  // zero vertices declared
    }
    {
        std::string deepEscape = "mtllib ";
        for (int i = 0; i < 200; ++i) {
            deepEscape += "../";
        }
        deepEscape += "outside.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        const ImportResult result =
            importModel("t.obj", "", asBytes(deepEscape), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status != ImportStatus::Unsupported);
    }
}

TEST_CASE("obj_import: a {nullptr, 0} span under a CLAIMED .obj name never dereferences it (OI25, D19)") {
    const std::span<const std::byte> emptySpan;  // {nullptr, 0} -- any dereference would crash/ASan-trip
    const ImportResult result = importModel("t.obj", "", emptySpan, ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Malformed);  // zero vertices -- SpanStreamBuf(nullptr) is EOF
}

// ---- §D-7 geometry conversion (OI26-49) --------------------------------------------------------------

TEST_CASE("obj_import: a quad triangulates to 6 indices over 4 vertices (OI26, AC-26)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions.size() == 4);
    CHECK(prim.indices.size() == 6);
}

TEST_CASE("obj_import: a pentagon triangulates to 9 indices over 5 vertices (OI27, AC-27)") {
    const std::string doc = "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 3 0\nv 0 2 0\nf 1 2 3 4 5\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions.size() == 5);
    CHECK(prim.indices.size() == 9);
}

TEST_CASE("obj_import: v/vt/vn attributes are all read (OI28, AC-28)") {
    const std::string doc =
        "v 0 0 0\nvt 0 0\nvn 0 0 1\n"
        "v 1 0 0\nvt 1 0\nvn 0 0 1\n"
        "v 0 1 0\nvt 0 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/2 3/3/3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    REQUIRE(prim.normals.size() == 3);
    REQUIRE(prim.uv0.size() == 3);
    CHECK(prim.normals[0] == Vec3{0.0F, 0.0F, 1.0F});
    CHECK(has(prim.attributes, VertexAttribute::Position));
    CHECK(has(prim.attributes, VertexAttribute::Normal));
    CHECK(has(prim.attributes, VertexAttribute::TexCoord0));
}

TEST_CASE("obj_import: an identical (v, vn, vt) triplet across two faces dedups to ONE output vertex (OI29, AC-29)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 3\nf 2 4 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions.size() == 4);  // 4 DISTINCT triplets, not 6 corner-instances
    CHECK(prim.indices.size() == 6);
}

TEST_CASE("obj_import: negative (relative) indices resolve identically to positive ones (OI30, AC-30)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    REQUIRE(prim.indices.size() == 3);
    CHECK(prim.indices[0] == 0);
    CHECK(prim.indices[1] == 1);
    CHECK(prim.indices[2] == 2);
}

TEST_CASE(
    "obj_import: INV-O4 -- an out-of-range VERTEX index drops its whole triangle; a sibling survives "
    "(OI31, AC-31, the sharpest minimal case)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 99999\nf 1 2 4\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 3);  // ONE face survived, not two, and not a partial one
    // build-time finding: LoadObj ITSELF emits one aggregate "Vertex indices out of bounds" warning
    // (F4's own end-of-parse check), IN ADDITION to our own per-face "out of range, dropped" warning --
    // two warnings for one bad index is expected, not a double-count bug.
    CHECK(result.warningTotal == 2);
    REQUIRE(result.warnings.size() == 2);
    bool sawOurs = false;
    for (const std::string& warning : result.warnings) {
        sawOurs = sawOurs || warning.find("out of range") != std::string::npos;
    }
    CHECK(sawOurs);
}

TEST_CASE("obj_import: INV-O4 -- an out-of-range vertex index drops a whole QUAD face (OI32, AC-31/AC-32)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 5 5 5\nf 1 2 3 99999\nf 1 2 3 4\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 6);  // the surviving quad's two triangles ONLY -- the bad quad's OWN
                                      // fast path bounds-checks all four corners BEFORE splitting, so
                                      // it drops ATOMICALLY, unlike the finer-grained n-gon path below
    CHECK(result.warningTotal == 2);  // ours, plus the library's own aggregate (see OI31)
}

TEST_CASE(
    "obj_import: INV-O4 -- an out-of-range vertex index in a 6-GON drops only the triangles that "
    "actually reference it (OI33, AC-31/AC-32)") {
    // Unlike the quad fast path (OI32), the general n-gon ear-clipping path bounds-checks and drops
    // PER DERIVED TRIANGLE, not the whole original polygon -- INV-O4 validates every ALREADY-
    // TRIANGULATED face (D-7's own vocabulary), and a hexagon's ear-clipping does not touch every
    // vertex in every derived triangle. This is a STRONGER proof than "the whole hexagon vanishes":
    // some of it can legitimately survive, and every survivor is still a WHOLE, valid triangle.
    const std::string doc =
        "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 3 0\nv 0 2 0\nv -1 1 0\nv 9 9 9\n"
        "f 1 2 3 4 5 99999\nf 1 2 3 4 5 6\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    // Both hexagons triangulate to 4 triangles each (6-2) if nothing were dropped -- 24 indices. At
    // least the second (fully valid) hexagon's 4 triangles (12 indices) must survive; strictly fewer
    // than 24 must survive (at least one triangle referencing vertex 99999 was dropped); and the
    // result is ALWAYS whole triangles, never a partial one.
    CHECK(prim.indices.size() >= 12);
    CHECK(prim.indices.size() < 24);
    CHECK(prim.indices.size() % 3 == 0);
    CHECK(result.warningTotal >= 1);
}

TEST_CASE("obj_import: INV-O4 -- an out-of-range NORMAL index drops its whole face (OI34, AC-31/AC-32)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//99999 2//1 3//1\nf 1//1 2//1 3//1\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 3);
    CHECK(result.warningTotal == 2);  // ours, plus the library's own "normal indices out of bounds"
}

TEST_CASE("obj_import: INV-O4 -- an out-of-range TEXCOORD index drops its whole face (OI35, AC-31/AC-32)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/99999 2/1 3/1\nf 1/1 2/1 3/1\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 3);
    CHECK(result.warningTotal == 2);  // ours, plus the library's own "texture coordinate indices..."
}

TEST_CASE("obj_import: F4b row 1 -- a literal zero vertex index fails the WHOLE file (OI36, AC-33)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
}

TEST_CASE(
    "obj_import: F4b row 2 -- a relative vertex index underflowing below zero fails the WHOLE file "
    "(OI37, AC-33)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nf -99 1 2\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
}

TEST_CASE(
    "obj_import: F4b row 3 -- a relative NORMAL index underflowing below zero fails the WHOLE file "
    "(OI38, AC-33)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//-99 2//1 3//1\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
}

TEST_CASE(
    "obj_import: F4b row 4 -- a relative TEXCOORD index underflowing below zero fails the WHOLE file "
    "(OI39, AC-33)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/-99 2/1 3/1\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
}

TEST_CASE("obj_import: vertex colours -- every vertex declares one, and they are read (OI40, AC-35)") {
    const std::string doc = "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.colors.size() == 3);
    CHECK(prim.colors[0] == Vec4{1.0F, 0.0F, 0.0F, 1.0F});  // widened, a = 1
    CHECK(has(prim.attributes, VertexAttribute::Color0));
}

TEST_CASE("obj_import: vertex colours -- NOT every vertex declares one, so NONE are read (OI41, AC-35)") {
    const std::string doc = "v 0 0 0 1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.colors.empty());
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Color0));
}

TEST_CASE(
    "obj_import: all-or-nothing normals -- one face in the primitive lacks a normal, so NONE survive "
    "(OI42, AC-36, D9)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 1 2 4\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.normals.empty());
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Normal));
}

TEST_CASE(
    "obj_import: all-or-nothing UVs -- one face in the primitive lacks a texcoord, so NONE survive "
    "(OI43, AC-36, D9)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nf 1/1 2/1 3/1\nf 1 2 4\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.uv0.empty());
    CHECK_FALSE(has(prim.attributes, VertexAttribute::TexCoord0));
}

TEST_CASE("obj_import: settings.scale multiplies positions and NEVER normals (OI44, AC-37)") {
    const std::string doc = "v 1 2 3\nvn 0 0 1\nv 0 0 0\nvn 0 0 1\nv 0 1 0\nvn 0 0 1\nf 1//1 2//2 3//3\n";
    const ImportResult result =
        importModel("t.obj", "", asBytes(doc), ImportSettings{.scale = 2.0F}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions[0] == Vec3{2.0F, 4.0F, 6.0F});
    CHECK(prim.normals[0] == Vec3{0.0F, 0.0F, 1.0F});  // unscaled
}

TEST_CASE("obj_import: the importer converts NOTHING -- no axis flip, no winding reversal (OI45, AC-38, D12/INV-O6)") {
    const std::string doc = "v 1 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[0] == Vec3{1.0F, 2.0F, 3.0F});
    CHECK(prim.positions[1] == Vec3{4.0F, 5.0F, 6.0F});
    CHECK(prim.positions[2] == Vec3{7.0F, 8.0F, 9.0F});
    REQUIRE(prim.indices.size() == 3);
    CHECK(prim.indices[0] == 0);
    CHECK(prim.indices[1] == 1);
    CHECK(prim.indices[2] == 2);  // NOT reversed to {0, 2, 1}
}

TEST_CASE(
    "obj_import: attributes bitset and array emptiness always agree -- never a bit whose array is "
    "empty (OI46, AC-39, INV-O5)") {
    const std::string doc = "v 0 0 0\nvn 0 0 1\nv 1 0 0\nvn 0 0 1\nv 0 1 0\nvn 0 0 1\nf 1//1 2//2 3//3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = result.model.meshes[0].primitives[0];
    CHECK(has(prim.attributes, VertexAttribute::Position) == !prim.positions.empty());
    CHECK(has(prim.attributes, VertexAttribute::Normal) == !prim.normals.empty());
    CHECK(has(prim.attributes, VertexAttribute::TexCoord0) == !prim.uv0.empty());
    CHECK(has(prim.attributes, VertexAttribute::Color0) == !prim.colors.empty());
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Tangent));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::TexCoord1));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Joints0));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Weights0));
    CHECK(prim.tangents.empty());
    CHECK(prim.uv1.empty());
    CHECK(prim.joints.empty());
    CHECK(prim.weights.empty());
}

TEST_CASE(
    "obj_import: §A-12's bounds pair -- an empty mesh gets a POINT box, and summary.bounds ignores it "
    "(OI47, AC-39b)") {
    const std::string doc =
        "o RealShape\n"
        "v 10 10 10\nv 11 10 10\nv 10 11 10\n"
        "f 1 2 3\n"
        "o EmptyShape\n"
        "v 20 20 20\n"
        "f 4 4 99999\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 2);
    CHECK(result.model.meshes[0].bounds.min == Vec3{10.0F, 10.0F, 10.0F});
    CHECK(result.model.meshes[0].bounds.max == Vec3{11.0F, 11.0F, 10.0F});
    CHECK(result.model.meshes[1].primitives.empty());
    CHECK(result.model.meshes[1].bounds.valid());  // a POINT box, never the Aabb::empty() sentinel
    CHECK(result.model.meshes[1].bounds.min == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(result.model.meshes[1].bounds.max == Vec3{0.0F, 0.0F, 0.0F});
    // the origin from EmptyShape's point box must NEVER leak into summary.bounds
    CHECK(result.model.summary.bounds.min == Vec3{10.0F, 10.0F, 10.0F});
    CHECK(result.model.summary.bounds.max == Vec3{11.0F, 11.0F, 10.0F});
}

TEST_CASE("obj_import: MAX_PRIMITIVES_PER_MODEL is enforced -- Truncated, a coherent smaller model (OI48, AC-58)") {
    std::string doc;
    const std::size_t shapeCount = MAX_PRIMITIVES_PER_MODEL + 10;
    for (std::size_t i = 0; i < shapeCount; ++i) {
        doc += "o part" + std::to_string(i) + "\n";
        doc += "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
        doc += "f -3 -2 -1\n";
    }
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.model.summary.primitiveCount == MAX_PRIMITIVES_PER_MODEL);
    CHECK(result.model.meshes.size() <= shapeCount);
    // NOTE (matching the FBX MAX_FBX_* precedent, .claude/rules/editor.md): MAX_VERTICES_PER_MODEL and
    // MAX_INDICES_PER_MODEL are wired identically, but their OWN "fires on a real document" half is
    // UNPROVEN here -- an 8,000,000-vertex or 24,000,000-index fixture is not a fast unit test. The
    // MAPPING (a hit -> Truncated with a coherent smaller model) is proven by construction, sharing this
    // SAME code path as the primitive cap above.
}

TEST_CASE("obj_import: the warning list caps at MAX_IMPORT_WARNINGS while warningTotal stays UNCAPPED (OI49, AC-59)") {
    std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    constexpr int BAD_FACES = 25;
    for (int i = 0; i < BAD_FACES; ++i) {
        doc += "f 1 2 99999\n";
    }
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);  // every face dropped, none of it a hard failure
    CHECK(result.warnings.size() == MAX_IMPORT_WARNINGS);
    // BAD_FACES of OUR OWN "out of range" warnings, PLUS the library's own one aggregate "Vertex
    // indices out of bounds" line (OI31's own finding) -- all UNCAPPED in the total.
    CHECK(result.warningTotal == BAD_FACES + 1);
}
