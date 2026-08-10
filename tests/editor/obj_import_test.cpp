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

using engine::editor::ExternalBuffer;
using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::MAX_EXTERNAL_URIS;

// task 3.2.3, Step 3: a PLACEHOLDER. It becomes AC-25's real one-triangle Full case at Step 5, once
// geometry conversion (Step 5) and material bucketing (Step 6) exist. Exists from Step 2 so AC-69's
// "OI1 present in both reduced configurations" has something to find, and so this file is tracked
// before the guards next run.
//
// A build-time finding from Step 2, recorded here rather than smoothed over: for the one commit between
// isImportableModelName claiming ".obj" (§D-4(a)) and the dispatch growing its OBJ/MTL arm (§D-4(d)), a
// ".obj" name fell through to the glTF arm's "everything else importable" catch-all and failed there --
// ParseFailed, not Unsupported -- because Wavefront text is not valid JSON. That was exactly the
// mis-route MI105c exists to catch, arising with no sabotage seed needed at all. AS OF THIS COMMIT the
// dispatch is correct again: ".obj" reaches importObjFile's Structure arm, which is a pure text scan
// (D5) -- a body with no `mtllib` line and well-formed vertex/face lines produces Ok with an empty URI
// set, geometry aside (geometry does not exist until Step 5).
TEST_CASE("obj_import: placeholder -- geometry conversion does not exist yet (OI1)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    CHECK(result.model.meshes.empty());  // Step 5 populates this
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
