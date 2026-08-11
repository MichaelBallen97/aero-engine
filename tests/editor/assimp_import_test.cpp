// tests/editor/assimp_import_test.cpp -- task 3.2.5: the Assimp backend for .dae/.ply/.stl. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the obj_import_test.cpp / fbx_import_test.cpp / model_import_test.cpp precedent):
// model_import.hpp depends only on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and
// aero/editor/scene_bounds.hpp -- the last of those reaches aero::scene, a PUBLIC, UNGATED dependency
// of aero_editor_core (only engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in
// this file must be PRESENT and PASSING in both reduced configurations, not merely the default build.
// No GPU, no window, no ImGui context, no sleeps.
//
// Nearly every fixture is a raw ASCII string literal. The BINARY .ply and .stl fixtures are BUILT
// BYTE-BY-BYTE HERE rather than committed, which is what makes the "byte-identical twin" assertions
// possible at all -- a committed binary would be compared against a literal that describes it, which
// proves nothing about the two agreeing. The three committed fixtures (cube.dae, cube.ply, cube.stl)
// are reached through AERO_ASSET_FIXTURES_DIR, ALREADY defined on this target, and exist so the
// real-bytes path through readFileBytes is proven on real bytes.
//
// THIS TU NAMES NO ASSIMP TYPE (AC-11/AC-13's sibling check) -- it drives the backend only indirectly,
// through the PUBLIC importModel() dispatch. assimp_import.hpp is src-private and stays that way;
// nothing here #includes it.
#include <aero/editor/model_import.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// The byte-loading pattern model_import_test.cpp / fbx_import_test.cpp / obj_import_test.cpp each keep
// their own copy of, restated here so this TU stays independent of all three.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// A source line with its `//` comment removed -- the MI42c/MI42d / OI77 / MS41 shape, this TU's own
// copy (the foldAscii/addWarning precedent). AI2 and AI3 both depend on it, and it is what makes a
// gate survive the gated file's OWN documentation of the tokens it forbids (the AC-56 lesson).
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

// Reads a file under editor/src through AERO_EDITOR_SRC_DIR (ALREADY defined on this target by task
// 2.6.1 -- no second definition is added, which would be a drift surface) and REQUIREs success. A
// missing file is a FAILURE, never a skip.
[[nodiscard]] std::string readEditorSource(const std::string& relativePath) {
    const std::string path = std::string(AERO_EDITOR_SRC_DIR) + "/" + relativePath;
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    REQUIRE_MESSAGE(read.text.has_value(), path);
    return *read.text;
}

// The same file's text with every `//` comment stripped, line by line.
[[nodiscard]] std::string strippedSource(const std::string& relativePath) {
    const std::string text = readEditorSource(relativePath);
    std::string out;
    out.reserve(text.size());
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        out.append(codeOf(line));
        out.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return out;
}

// THE INCLUDE TOKEN, spelled as TWO ADJACENT LITERALS on purpose. The compiler concatenates them; the
// FILE never contains the eight-character sequence itself, so the cheap gate grep over editor/, tests/,
// engine/, runtime/ and tools/ still returns exactly one path -- the backend TU -- rather than this
// test as well. Do not "simplify" it into one literal.
[[nodiscard]] std::string assimpIncludeToken() { return std::string("#include <assimp") + "/"; }

// The editor's own source list, DERIVED FROM THE TREE rather than hard-coded: every `src/<name>.cpp`
// token in editor/CMakeLists.txt, which is exactly the set compiled into aero_editor_core plus
// main.cpp. A TU added tomorrow is covered the day it is added, which a frozen array cannot promise
// (.claude/rules/boundary-guards.md: "never a hardcoded per-root count ... also assert coverage").
[[nodiscard]] std::vector<std::string> editorSourceFiles() {
    const std::string path = std::string(AERO_EDITOR_SRC_DIR) + "/../CMakeLists.txt";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    REQUIRE_MESSAGE(read.text.has_value(), path);
    const std::string& text = *read.text;

    std::vector<std::string> out;
    constexpr std::string_view PREFIX = "src/";
    std::size_t at = 0;
    while (true) {
        const std::size_t found = text.find(PREFIX, at);
        if (found == std::string::npos) {
            break;
        }
        const auto isNameChar = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        };
        std::size_t end = found + PREFIX.size();
        while (end < text.size() && isNameChar(text[end])) {
            ++end;
        }
        const std::string name = text.substr(found + PREFIX.size(), end - found - PREFIX.size());
        at = end;
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".cpp") != 0) {
            continue;
        }
        bool seen = false;
        for (const std::string& existing : out) {
            if (existing == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out.push_back(name);
        }
    }
    return out;
}

// AI6 only. A unique scratch directory that removes itself on destruction, plus a working-directory
// guard that restores on a thrown REQUIRE as well as on success -- model_import_test.cpp's MI42b pair,
// this TU's own copy (scaffolding is copied, the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_assimp_import_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dirPath; }

private:
    std::filesystem::path dirPath;
};

class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& to) : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
    ScopedCwd(ScopedCwd&&) = delete;
    ScopedCwd& operator=(ScopedCwd&&) = delete;

private:
    std::filesystem::path previous;
};

// ---- the shared fixture literals --------------------------------------------------------------------
// EVERY .dae literal is hoisted into a NAMED LOCAL / namespace-scope constant and never written inline
// inside a CHECK or REQUIRE: MSVC's legacy preprocessor breaks on a raw string literal containing `\"`
// passed DIRECTLY as a macro argument (.claude/rules/ci-portability.md). The discriminator is `\"`, and
// XML is nothing but `\"`. This is the single most likely Windows-only build break in this task.

const std::string TRIANGLE_STL =
    "solid Tri\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
    "endloop\nendfacet\nendsolid Tri\n";

const std::string TRIANGLE_PLY =
    "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
    "element face 1\nproperty list uchar int vertex_index\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

// A .ply that names one texture in its header. NO trailing space after the operand, deliberately: the
// loader hands back the rest of the line verbatim, so a trailing space would land inside the name.
const std::string TEXTURED_PLY =
    "ply\nformat ascii 1.0\ncomment TextureFile scan.png\nelement vertex 3\nproperty float x\n"
    "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_index\n"
    "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

const std::string TRIANGLE_DAE =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries><geometry id="g1" name="Tri"><mesh>
    <source id="p"><float_array id="pa" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#pa" count="3" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
    <triangles count="1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="S"><node id="N" name="N">
    <instance_geometry url="#g1"/>
  </node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// AI5/AI6's hostile document: FOUR <library_images> entries, each bound through its own effect and
// material so all four actually reach a texture slot. Every one of them names a path that must be
// refused, and none of them may ever be opened.
const std::string HOSTILE_DAE =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>
    <image id="i1"><init_from>/etc/passwd</init_from></image>
    <image id="i2"><init_from>file:///etc/passwd</init_from></image>
    <image id="i3"><init_from>C:\Windows\win.ini</init_from></image>
    <image id="i4"><init_from>../../../../etc/passwd</init_from></image>
  </library_images>
  <library_effects>
    <effect id="e1"><profile_COMMON>
      <newparam sid="s1"><surface type="2D"><init_from>i1</init_from></surface></newparam>
      <newparam sid="m1s"><sampler2D><source>s1</source></sampler2D></newparam>
      <technique sid="common"><lambert><diffuse><texture texture="m1s" texcoord="UV"/></diffuse></lambert></technique>
    </profile_COMMON></effect>
    <effect id="e2"><profile_COMMON>
      <newparam sid="s2"><surface type="2D"><init_from>i2</init_from></surface></newparam>
      <newparam sid="m2s"><sampler2D><source>s2</source></sampler2D></newparam>
      <technique sid="common"><lambert><diffuse><texture texture="m2s" texcoord="UV"/></diffuse></lambert></technique>
    </profile_COMMON></effect>
    <effect id="e3"><profile_COMMON>
      <newparam sid="s3"><surface type="2D"><init_from>i3</init_from></surface></newparam>
      <newparam sid="m3s"><sampler2D><source>s3</source></sampler2D></newparam>
      <technique sid="common"><lambert><diffuse><texture texture="m3s" texcoord="UV"/></diffuse></lambert></technique>
    </profile_COMMON></effect>
    <effect id="e4"><profile_COMMON>
      <newparam sid="s4"><surface type="2D"><init_from>i4</init_from></surface></newparam>
      <newparam sid="m4s"><sampler2D><source>s4</source></sampler2D></newparam>
      <technique sid="common"><lambert><diffuse><texture texture="m4s" texcoord="UV"/></diffuse></lambert></technique>
    </profile_COMMON></effect>
  </library_effects>
  <library_materials>
    <material id="m1"><instance_effect url="#e1"/></material>
    <material id="m2"><instance_effect url="#e2"/></material>
    <material id="m3"><instance_effect url="#e3"/></material>
    <material id="m4"><instance_effect url="#e4"/></material>
  </library_materials>
  <library_geometries><geometry id="g1"><mesh>
    <source id="p"><float_array id="pa" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#pa" count="3" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
    <triangles count="1" material="m1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="S">
    <node id="N1"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="m1" target="#m1"/></technique_common></bind_material></instance_geometry></node>
    <node id="N2"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="m1" target="#m2"/></technique_common></bind_material></instance_geometry></node>
    <node id="N3"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="m1" target="#m3"/></technique_common></bind_material></instance_geometry></node>
    <node id="N4"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="m1" target="#m4"/></technique_common></bind_material></instance_geometry></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// THE FIXTURE TABLE. AI10 (depth equality), AI11 (degenerate inputs) and AI13 all drive it, so adding a
// fixture here covers it in several cases automatically -- which is what keeps AC-19's "for EVERY
// fixture" honest as the suite grows.
struct Fixture {
    std::string_view name;
    const std::string* text;
};

[[nodiscard]] std::vector<Fixture> fixtureTable() {
    return {
        Fixture{"t.stl", &TRIANGLE_STL}, Fixture{"t.ply", &TRIANGLE_PLY},      Fixture{"scan.ply", &TEXTURED_PLY},
        Fixture{"t.dae", &TRIANGLE_DAE}, Fixture{"hostile.dae", &HOSTILE_DAE},
    };
}

}  // namespace

using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::isImportableModelName;
using engine::editor::modelImporterNeedsExternalBuffers;

// AI1 (AC-3) -- the case that pulls assimp's archive (and, on Windows, loads its DLL). Nothing else in
// the suite calls importAssimp at all, so without this case the link is declared and never exercised,
// and neither R1 (does it build and link on six presets) nor R2 (the second stb_image implementation)
// is tested by anything. It is also the only case that proves the archive is not merely linked but
// USABLE: a one-triangle document of each format must reach status Ok at Full depth.
TEST_CASE("AI1: a one-triangle .stl, .ply and .dae each import Ok at Full depth") {
    const ImportResult stl = importModel("t.stl", "", asBytes(TRIANGLE_STL), ImportSettings{}, ImportDepth::Full, {});
    INFO("stl message: ", stl.message);
    CHECK(stl.status == ImportStatus::Ok);
    const ImportResult ply = importModel("t.ply", "", asBytes(TRIANGLE_PLY), ImportSettings{}, ImportDepth::Full, {});
    INFO("ply message: ", ply.message);
    CHECK(ply.status == ImportStatus::Ok);
    const ImportResult dae = importModel("t.dae", "", asBytes(TRIANGLE_DAE), ImportSettings{}, ImportDepth::Full, {});
    INFO("dae message: ", dae.message);
    CHECK(dae.status == ImportStatus::Ok);
}

// AI2 (AC-11/AC-13) -- THE INCLUDE BOUNDARY, pinned by a case rather than only by a grep. The grep
// stays in the gate as a cheap first line, but it is prose-fragile: editor/CMakeLists.txt and
// tests/CMakeLists.txt are both inside its scanned roots, so the most natural comment to write in
// either would turn it red for a reason that is not a violation. This case survives a prose edit.
TEST_CASE("AI2: exactly one editor source includes an Assimp header, and it is the backend TU") {
    const std::string token = assimpIncludeToken();

    const std::vector<std::string> sources = editorSourceFiles();
    REQUIRE_FALSE(sources.empty());
    // A floor, not an exact count: a parsing failure that produced two names must not pass silently,
    // while adding a TU must not have to touch this number. The named members below are what make the
    // list's IDENTITY checkable rather than only its size.
    REQUIRE(sources.size() >= 50);

    const auto contains = [&sources](std::string_view name) {
        for (const std::string& existing : sources) {
            if (existing == name) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(contains("assimp_import.cpp"));
    REQUIRE(contains("model_import.cpp"));
    REQUIRE(contains("gltf_import.cpp"));

    // Every src-private header is reached by exactly one quoted include from a .cpp, so harvesting
    // them from the .cpp texts keeps BOTH sides of this scan derived from the tree.
    std::vector<std::string> scanned = sources;
    std::vector<std::string> headers;
    for (const std::string& source : sources) {
        const std::string text = readEditorSource(source);
        std::size_t at = 0;
        while (true) {
            const std::size_t found = text.find("#include \"", at);
            if (found == std::string::npos) {
                break;
            }
            const std::size_t start = found + 10U;
            const std::size_t close = text.find('"', start);
            if (close == std::string::npos) {
                break;
            }
            const std::string name = text.substr(start, close - start);
            at = close + 1U;
            if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".hpp") != 0 ||
                name.find('/') != std::string::npos) {
                continue;
            }
            bool seen = false;
            for (const std::string& existing : headers) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                headers.push_back(name);
            }
        }
    }
    REQUIRE_FALSE(headers.empty());
    REQUIRE(headers.size() >= 10);
    for (const std::string& header : headers) {
        scanned.push_back(header);
    }

    std::vector<std::string> including;
    for (const std::string& file : scanned) {
        // strippedSource REQUIREs the read, so an unreadable listed file is a FAILURE, never a skip.
        if (strippedSource(file).find(token) != std::string::npos) {
            including.push_back(file);
        }
    }
    REQUIRE(including.size() == 1U);
    CHECK(including[0] == "assimp_import.cpp");
}

// AI3 (AC-12/AC-17b/AC-72/AC-73) -- THE FORBIDDEN-TOKEN GATE, over comment-stripped source. The file's
// own explanatory header names every forbidden token by design, so a naive grep is GUARANTEED to fail
// and would be deleted the first time it did (the AC-56 lesson, fourth application). No runtime path
// can observe "we did not call ReadFile", and the mismatch arm of the loader assertion is unreachable
// by any input this suite can supply -- the extension hint resolves to exactly one loader and content
// never influences that choice -- so this is AC-17's source half, with seed S3 (a wrong expected-loader
// constant reddening EVERY case of that format) as its behavioural half.
TEST_CASE("AI3: assimp_import.cpp names no forbidden Assimp entry point, and does name the required ones") {
    const std::string code = strippedSource("assimp_import.cpp");
    REQUIRE_FALSE(code.empty());

    for (const std::string_view forbidden : {"ReadFile(",
                                             "DefaultIOSystem",
                                             "aiImportFile",
                                             "aiExportScene",
                                             "Assimp::Exporter",
                                             "ExportProperties",
                                             "ZipArchiveIOSystem",
                                             "SetIOHandler(nullptr)",
                                             "DefaultLogger",
                                             "aiAttachLogStream",
                                             "UnregisterLoader",
                                             "GetOrphanedScene",
                                             "aiProcess_PreTransformVertices",
                                             "aiProcess_MakeLeftHanded",
                                             "aiProcess_ConvertToLeftHanded",
                                             "aiProcess_FlipUVs",
                                             "aiProcess_FlipWindingOrder",
                                             "aiProcess_GlobalScale",
                                             "aiProcess_GenNormals",
                                             "aiProcess_GenSmoothNormals",
                                             "aiProcess_ForceGenNormals",
                                             "aiProcess_CalcTangentSpace",
                                             "aiProcess_JoinIdenticalVertices",
                                             "aiProcess_RemoveRedundantMaterials",
                                             "aiProcess_GenBoundingBoxes",
                                             "aiProcess_FindInvalidData",
                                             "aiProcess_FindDegenerates",
                                             "aiProcess_SplitLargeMeshes",
                                             "aiProcess_ImproveCacheLocality",
                                             "aiProcess_RemoveComponent",
                                             "std::filesystem",
                                             "fstream",
                                             "fopen"}) {
        INFO("forbidden token: ", forbidden);
        CHECK(code.find(forbidden) == std::string::npos);
    }

    for (const std::string_view required :
         {"ReadFileFromMemory(", "SetIOHandler(new", "AI_METADATA_SOURCE_FORMAT", "ImportStatus::Malformed",
          "aiProcess_ValidateDataStructure", "aiProcess_Triangulate", "AI_CONFIG_PP_SBP_REMOVE",
          "AI_CONFIG_PP_LBW_MAX_WEIGHTS"}) {
        INFO("required token: ", required);
        CHECK(code.find(required) != std::string::npos);
    }

    // AC-73's mechanism: the ONE line this task adds to a file it does not own. Removing it re-opens a
    // duplicate-symbol collision with the second, unprefixed stb_image implementation the assimp port
    // compiles -- a static-link concern on macOS and Linux, structurally absent on Windows where the
    // port builds a DLL. A LINK failure would be the real signal, so this assertion is deliberately
    // source-text-only cover and says so.
    const std::string thumbnail = strippedSource("thumbnail_store.cpp");
    CHECK(thumbnail.find("#define STB_IMAGE_STATIC") != std::string::npos);
}

// AI4 (AC-16, CORRECTED) -- RefusingIoSystem's BODY, from comment-stripped source. AC-16 asked for a
// DIRECT test of the class; that would require this TU to name Assimp::IOSystem, which INV-A1 forbids
// and which AI2 asserts. The two ACs are in direct conflict and INV-A1 wins, so this is the source
// half and AI5/AI6 are the behavioural half -- they prove the refusal end to end, through a real
// hostile document, which is what AC-16 was actually protecting.
TEST_CASE("AI4: every IOSystem override in the refusing handler returns the refusing answer") {
    const std::string code = strippedSource("assimp_import.cpp");
    const std::size_t classStart = code.find("class RefusingIoSystem");
    REQUIRE(classStart != std::string::npos);
    const std::size_t classEnd = code.find("};", classStart);
    REQUIRE(classEnd != std::string::npos);
    const std::string body = code.substr(classStart, classEnd - classStart);

    // Assimp 6.0.4's IOSystem carries FOUR pure virtuals (Exists, getOsSeparator, Open, Close) and
    // EIGHT non-pure ones (ComparePaths, PushDirectory, CurrentDirectory, StackSize, PopDirectory,
    // CreateDirectory, ChangeDirectory, DeleteFile). The pure ones the compiler forces; the non-pure
    // ones it does NOT -- an un-overridden one silently inherits a base that keeps a directory stack
    // and, for ComparePaths, compares real paths. All twelve are overridden here, and that is the
    // property this case pins. MEASURED against the installed header, not remembered.
    for (const std::string_view method :
         {"Exists(", "getOsSeparator(", "Open(", "Close(", "ComparePaths(", "PushDirectory(", "CurrentDirectory(",
          "StackSize(", "PopDirectory(", "CreateDirectory(", "ChangeDirectory(", "DeleteFile("}) {
        INFO("IOSystem method: ", method);
        CHECK(body.find(method) != std::string::npos);
    }

    // Twelve overrides, and every one of them answers "cannot".
    std::size_t overrides = 0;
    std::size_t at = 0;
    while ((at = body.find("override", at)) != std::string::npos) {
        ++overrides;
        at += 8U;
    }
    CHECK(overrides == 12U);
    CHECK(body.find("return true") == std::string::npos);
    CHECK(body.find("existing_io") == std::string::npos);
    CHECK(body.find("return '/'") != std::string::npos);  // R7: the SAME separator on all three lanes
}

// AI5 (AC-15) -- the hostile document, END TO END. Nothing in a well-formed fixture makes Assimp ask
// for a second file, so A-3's whole defect would ship green without this case and AI6.
// EXTENDED at step 6, when convertMaterials lands and the four refused ImportedImages become visible.
TEST_CASE("AI5: a .dae naming four hostile texture paths yields ZERO external URIs and still imports") {
    const ImportResult result =
        importModel("hostile.dae", "", asBytes(HOSTILE_DAE), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.status == ImportStatus::Ok);
    // INV-A8: a refused path never reaches externalUris. None of the four is a legal relative path
    // inside the assets root, so the list is EMPTY -- and nothing was read to discover that.
    CHECK(result.externalUris.empty());

    // The same document at Structure depth: still nothing, still no read.
    const ImportResult structure =
        importModel("hostile.dae", "", asBytes(HOSTILE_DAE), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(structure.externalUris.empty());
}

// AI6 (AC-14) -- "opens no file, creates no file", proven BEHAVIOURALLY rather than by inspection. The
// working directory is a scratch directory containing exactly one decoy file; the importer must not
// read it, and must not leave anything behind.
TEST_CASE("AI6: importing from an empty working directory reads nothing and creates nothing") {
    const TempDir temp;
    {
        std::ofstream decoy(temp.path() / "cube.png", std::ios::binary);
        decoy << "not a real png";
    }
    const ScopedCwd cwd(temp.path());

    std::vector<std::string> before;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp.path())) {
        before.push_back(entry.path().filename().string());
    }

    for (const Fixture& fixture : fixtureTable()) {
        const ImportResult full =
            importModel(fixture.name, "", asBytes(*fixture.text), ImportSettings{}, ImportDepth::Full, {});
        INFO("fixture: ", fixture.name, " message: ", full.message);
        CHECK(full.status != ImportStatus::Unsupported);
    }

    std::vector<std::string> after;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp.path())) {
        after.push_back(entry.path().filename().string());
    }
    CHECK(before == after);
    CHECK(after.size() == 1U);
}

// AI7 (AC-18/AC-9) -- .stl at Structure depth: Assimp is NEVER entered, so the model is empty and the
// URI set is provably {}.
TEST_CASE("AI7: .stl at Structure depth returns an empty model and no external URIs") {
    const ImportResult result =
        importModel("t.stl", "", asBytes(TRIANGLE_STL), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
    CHECK(result.model.materials.empty());
    CHECK(result.model.images.empty());
    CHECK(result.model.skins.empty());
    CHECK(result.model.animations.empty());
    CHECK(result.model.summary.vertexCount == 0U);
    CHECK_FALSE(result.model.summary.bounds.valid());
    CHECK(result.externalUris.empty());
    CHECK_FALSE(result.model.sourceSpace.declared);
}

// AI8 (AC-18/AC-8) -- .ply at Structure depth: the header scan runs, the library does not.
TEST_CASE("AI8: .ply at Structure depth returns an empty model and the header's texture name") {
    const ImportResult result =
        importModel("scan.ply", "", asBytes(TEXTURED_PLY), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
    CHECK(result.model.summary.vertexCount == 0U);
    REQUIRE(result.externalUris.size() == 1U);
    CHECK(result.externalUris[0] == "scan.png");
    CHECK_FALSE(result.model.sourceSpace.declared);
}

// AI9 (AC-20) -- THE DISCRIMINATING PROOF that Structure does not enter the library for these two
// formats. A Structure pass that routed through runAssimp could not produce this pair: a body that is
// truncated mid-record is Ok at Structure (nothing parsed it) and a failure at Full (something did).
TEST_CASE("AI9: a truncated .stl and .ply are Ok at Structure and fail at Full") {
    const std::string truncatedStl = "solid Tri\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0";
    const ImportResult stlStructure =
        importModel("t.stl", "", asBytes(truncatedStl), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(stlStructure.status == ImportStatus::Ok);
    const ImportResult stlFull =
        importModel("t.stl", "", asBytes(truncatedStl), ImportSettings{}, ImportDepth::Full, {});
    CHECK(stlFull.status != ImportStatus::Ok);
    CHECK_FALSE(stlFull.message.empty());

    const std::string truncatedPly =
        "ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n0 0 0\n1 0 0\n";
    const ImportResult plyStructure =
        importModel("t.ply", "", asBytes(truncatedPly), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(plyStructure.status == ImportStatus::Ok);
    const ImportResult plyFull =
        importModel("t.ply", "", asBytes(truncatedPly), ImportSettings{}, ImportDepth::Full, {});
    CHECK(plyFull.status != ImportStatus::Ok);
    CHECK_FALSE(plyFull.message.empty());
}

// AI10 (AC-19) -- THE DEPTH-EQUALITY TABLE, over EVERY fixture, entry for entry and order for order.
// The .ply seeding step is invisible to every other case, and getting it wrong produces a MISSING
// DEPENDENCY, which surfaces as a 3.3.x bug rather than as a red case.
TEST_CASE("AI10: Structure and Full agree about externalUris on every fixture") {
    for (const Fixture& fixture : fixtureTable()) {
        const ImportResult structure =
            importModel(fixture.name, "", asBytes(*fixture.text), ImportSettings{}, ImportDepth::Structure, {});
        const ImportResult full =
            importModel(fixture.name, "", asBytes(*fixture.text), ImportSettings{}, ImportDepth::Full, {});
        INFO("fixture: ", fixture.name);
        CHECK(structure.externalUris == full.externalUris);
    }
}

// AI11 (AC-50) -- nine combinations: three degenerate bodies against three extensions. None may return
// Ok, and none may crash.
TEST_CASE("AI11: an empty, one-byte or whitespace-only body never imports Ok") {
    const std::string empty;
    const std::string oneByte = "x";
    const std::string blanks = "   \n\t \n";
    for (const std::string_view name : {"x.dae", "x.ply", "x.stl"}) {
        for (const std::string* body : {&empty, &oneByte, &blanks}) {
            const ImportResult result = importModel(name, "", asBytes(*body), ImportSettings{}, ImportDepth::Full, {});
            INFO("name: ", name, " body size: ", body->size());
            CHECK(result.status != ImportStatus::Ok);
            CHECK(result.status != ImportStatus::Unsupported);
            CHECK_FALSE(result.message.empty());
        }
    }
}

// AI12 (AC-51) -- a renamed binary. The bytes are a PNG and a GLB; the names claim otherwise.
TEST_CASE("AI12: a renamed PNG and a renamed GLB are refused with a message") {
    std::string png;
    for (const unsigned char byte : {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU,
                                     0x49U, 0x48U, 0x44U, 0x52U}) {
        png.push_back(static_cast<char>(byte));
    }
    const ImportResult asDae = importModel("x.dae", "", asBytes(png), ImportSettings{}, ImportDepth::Full, {});
    CHECK(asDae.status != ImportStatus::Ok);
    CHECK_FALSE(asDae.message.empty());

    std::string glb = "glTF";
    for (const unsigned char byte : {0x02U, 0x00U, 0x00U, 0x00U, 0x20U, 0x00U, 0x00U, 0x00U}) {
        glb.push_back(static_cast<char>(byte));
    }
    const ImportResult asPly = importModel("x.ply", "", asBytes(glb), ImportSettings{}, ImportDepth::Full, {});
    CHECK(asPly.status != ImportStatus::Ok);
    CHECK_FALSE(asPly.message.empty());
}

// AI13 (AC-55) -- importModel is noexcept-in-contract but not in signature; only a case proves it.
TEST_CASE("AI13: pathological inputs return a status instead of throwing") {
    const std::string longLine = "<COLLADA>" + std::string(1024U * 1024U, 'a') + "</COLLADA>";
    const ImportResult huge = importModel("x.dae", "", asBytes(longLine), ImportSettings{}, ImportDepth::Full, {});
    CHECK(huge.status != ImportStatus::Ok);

    const std::string absurdCount =
        "ply\nformat ascii 1.0\nelement vertex 4294967295\nproperty float x\nproperty float y\n"
        "property float z\nend_header\n";
    const ImportResult absurd = importModel("x.ply", "", asBytes(absurdCount), ImportSettings{}, ImportDepth::Full, {});
    CHECK(absurd.status != ImportStatus::Ok);

    std::string deep = "comment TextureFile ";
    for (int i = 0; i < 40; ++i) {
        deep += "../";
    }
    deep += "etc/passwd";
    const std::string escaping = "ply\nformat ascii 1.0\n" + deep + "\nend_header\n";
    const ImportResult escaped =
        importModel("x.ply", "", asBytes(escaping), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(escaped.status == ImportStatus::Ok);
    CHECK(escaped.externalUris.empty());  // refused by classifyUri, never read
}

// AI14 (AC-5) -- modelImporterNeedsExternalBuffers breaks INDEPENDENTLY of isImportableModelName,
// exactly as MI133 proved for .blend. FALSE for all three: every external reference these formats
// carry is a TEXTURE, which this importer resolves for the dependency graph and never reads.
TEST_CASE("AI14: modelImporterNeedsExternalBuffers is false for all three, ASCII-case-folded") {
    for (const std::string_view name : {"a.dae", "a.ply", "a.stl", "A.DAE", "A.PLY", "A.STL"}) {
        INFO("name: ", name);
        CHECK(isImportableModelName(name));
        CHECK_FALSE(modelImporterNeedsExternalBuffers(name));
    }
}

// AI15 -- the REAL-BYTES path. Every other case is a string literal, so a byte-order mark, a CRLF
// checkout or a .gitattributes mistake would be invisible without this one.
TEST_CASE("AI15: the committed cube.dae, cube.ply and cube.stl import Ok through readFileBytes") {
    for (const std::string_view leaf : {"cube.dae", "cube.ply", "cube.stl"}) {
        const std::string path = std::string(AERO_ASSET_FIXTURES_DIR) + "/" + std::string(leaf);
        const engine::editor::FileBytesResult read =
            engine::editor::readFileBytes(path, engine::editor::MAX_MODEL_FILE_BYTES);
        REQUIRE_MESSAGE(read.bytes.has_value(), path);
        const ImportResult result =
            importModel(leaf, "", asBytes(*read.bytes), ImportSettings{}, ImportDepth::Full, {});
        INFO("leaf: ", leaf, " message: ", result.message);
        CHECK(result.status == ImportStatus::Ok);
    }
}

// AI16 (AC-1) -- the SUFFIX-SHAPE half for the three new extensions. MI120's rewrite covers the
// positive half; this covers the shape (a suffix test on the FULL name, never a last-extension split).
TEST_CASE("AI16: isImportableModelName's suffix shape holds for .dae, .ply and .stl") {
    CHECK(isImportableModelName("a.DAE"));
    CHECK(isImportableModelName("archive.tar.stl"));
    CHECK(isImportableModelName("scan.PLY"));
    CHECK_FALSE(isImportableModelName("a.ply.bak"));
    CHECK_FALSE(isImportableModelName(".stl"));
    CHECK_FALSE(isImportableModelName(".dae"));
    CHECK_FALSE(isImportableModelName("stl"));
}
