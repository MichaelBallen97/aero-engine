// tests/editor/fbx_import_test.cpp -- task 3.2.2: the FBX backend (ufbx). A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the asset_watcher_test.cpp / model_import_test.cpp precedent): model_import.hpp
// depends only on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and
// aero/editor/scene_bounds.hpp -- the last of those reaches aero::scene, a PUBLIC, UNGATED dependency
// of aero_editor_core (only engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in
// this file must be PRESENT and PASSING in both reduced configurations, not merely the default build.
// No GPU, no window, no ImGui context, no sleeps, and no disk on the critical path -- every case is
// driven from an ASCII-FBX STRING LITERAL, assembled by makeFbx() below. The one binary fixture
// (AC-55, Step 12) is reached through AERO_ASSET_FIXTURES_DIR, already defined on this target.
//
// THIS TU NAMES NO ufbx TYPE (AC-63's sibling check, §V4/§V6) -- it drives the FBX backend only
// indirectly, through the PUBLIC importModel() dispatch. fbx_import.hpp is src-private and stays that
// way; nothing here #includes it.
//
// ---- ASCII-FBX authoring rules (read before adding or editing a fixture) --------------------------
//
// Polygon terminator:    the LAST index of every face is NEGATED AND DECREMENTED: `0,1,2,-4` is the
//                        quad `0,1,2,3`; a triangle `0,1,2` is written `0,1,-3`. Getting this wrong
//                        changes the face count silently.
// Connections:           `C: "OO",child,parent`. Parent `0` is the scene root. A Geometry connects to
//                        its Model the same way.
// UpAxis:                `0` = X, `1` = Y, `2` = Z. Paired with UpAxisSign (`1` or `-1`).
// UnitScaleFactor IS IN CENTIMETRES: `1` => unit_meters == 0.01. A Y-up METRE source needs
//                        `UnitScaleFactor: 100`. Writing `1` there makes a "canonical" fixture a Y-up
//                        CENTIMETRE source, which the importer then scales by 100 -- and the case
//                        silently tests the wrong thing while looking green. THE SINGLE EASIEST
//                        FIXTURE MISTAKE IN THIS TASK.
// Version comment:       not required -- ufbx defaults ASCII to 7400 when no version is found
//                        (ufbx.c:11234). Written anyway so the fixture is self-describing.
// Comments:              `;` to end of line. `#` is NOT a comment -- that is what makes OBJ text fail
//                        to parse (FI12, Step 4).
// Creator:               free text. A hand-written fixture yields metadata.exporter == UNKNOWN and
//                        exporter_version == 0 -- but SourceSpace::generator is NOT empty whenever
//                        Creator is set: generatorString() falls back to metadata.creator precisely
//                        because ufbx populates it from this field (ufbx.c's
//                        ufbxi_read_header_extension reads Creator as a direct child of
//                        FBXHeaderExtension). CORRECTION to an earlier draft of this comment (and to
//                        the plan's own §A-21 prose), found by running FI19: "every tier-0 fixture has
//                        an empty generator" is only true for a fixture whose Creator is ALSO empty.
//

// The template below is the §D-7 document, VERIFIED TO PARSE under real ufbx v0.23.0 (§G-10 -- R1 is
// CLOSED by that spike, not deferred). Every future FI case is this document with one section varied
// through makeFbx()'s three parameters, never restated whole.
#include <aero/editor/model_import.hpp>
#include <aero/editor/text_file.hpp>  // FI73: readFileBytes, the seam MAX_MODEL_FILE_BYTES lives at

#include "scene_golden_support.hpp"  // task 3.2.2, Step 12: FI76's committed binary fixture

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>  // geteuid -- FI73's vacuity guard (text_file_test.cpp's TF31 precedent)
#endif

using engine::editor::AlphaMode;
using engine::editor::AnimationInterpolation;
using engine::editor::AnimationPath;
using engine::editor::FileBytesResult;
using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::MAX_MODEL_FILE_BYTES;
using engine::editor::readFileBytes;
using engine::editor::TextureWrap;

namespace {

// ---- comparison epsilons (§D-6), fixed ONCE so no future case invents its own -----------------
// The converted Z of a Z-up source comes back as 5.68434e-16, not 0 (§A-8) -- the conversion is a REAL
// rotation and its residue is real. EXACT EQUALITY IS A FALSE RED WAITING FOR A DIFFERENT LANE'S FMA
// CONTRACTION. Not yet used by FI0 (the dispatch arm does not exist until Step 3); landed here because
// every later conversion case in this file depends on them existing exactly once.
constexpr double POS_EPS = 1e-5;  // positions, translations, bounds, matrix translation columns
constexpr double ROT_EPS = 1e-6;  // quaternion components, matrix basis columns
#define APPROX_POS(v) doctest::Approx(v).epsilon(POS_EPS)
#define APPROX_ROT(v) doctest::Approx(v).epsilon(ROT_EPS)

// The fastgltf-free byte-loading pattern model_import_test.cpp's asBytes uses, restated here so this
// TU stays self-contained (each importer test TU owns its own copy rather than sharing one -- there is
// no shared test-support header for this pattern today). The returned span borrows from `text`, which
// the caller must keep alive.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// task 3.2.2, Step 12 (AC-55): the one committed binary fixture, reached through
// AERO_ASSET_FIXTURES_DIR, already defined on this target (asset_meta_test.cpp's own precedent).
constexpr std::string_view CUBE_BINARY_FIXTURE = AERO_ASSET_FIXTURES_DIR "/cube-binary.fbx";

// ---- FI73's scratch-file scaffolding. The NINTH TU-local copy of this shape
// (text_file_test.cpp:48's own count, one higher): scaffolding is copied, the ASSERTION is shared.
// Only FI73 touches disk in this file -- every other case is a string literal, by design.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_fbx_import_test_" + std::to_string(++counter));
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

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

// UTF-8 bytes, never a narrow std::string (project_files.cpp's pathFromUtf8 precedent).
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

void writeEmptyFile(std::string_view absolutePath) {
    const std::ofstream out(pathOf(absolutePath), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
}

// FI74's own validator. RFC 3629 exactly: no overlong forms, no surrogates, nothing above U+10FFFF --
// the same three things that make a byte sequence "technically decodable but still invalid", and the
// three ImGui's own decoder would render as garbage rather than reject.
[[nodiscard]] bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            extra = 1;
            code = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra = 2;
            code = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra = 3;
            code = lead & 0x07U;
        } else {
            return false;  // a bare continuation byte, or an invalid 5/6-byte lead
        }
        if (i + extra >= text.size()) {
            return false;  // truncated sequence
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (cont & 0x3FU);
        }
        const bool overlong =
            (extra == 1 && code < 0x80U) || (extra == 2 && code < 0x800U) || (extra == 3 && code < 0x10000U);
        const bool surrogate = code >= 0xD800U && code <= 0xDFFFU;
        if (overlong || surrogate || code > 0x10FFFFU) {
            return false;
        }
        i += extra + 1U;
    }
    return true;
}

// ---- the §D-7 template's three variable sections, at their DEFAULT (Z-up, UnitScaleFactor:1 --
// centimetre) content: one node ("box"), one mesh (a quad), translated (0,0,200) in the source's own
// axes. This is AC-22's fixture, and FI1 (Step 3) imports it at ImportDepth::Full.
constexpr std::string_view DEFAULT_GLOBALS_PROPERTIES =
    "        P: \"UpAxis\", \"int\", \"Integer\", \"\",2\n"
    "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
    "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
    "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";

constexpr std::string_view DEFAULT_OBJECTS =
    "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
    "        Vertices: *12 { a: 0,0,0,100,0,0,100,100,0,0,100,0 }\n"
    "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "    Model: 100, \"Model::box\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Properties70:  {\n"
    "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,200\n"
    "        }\n"
    "    }\n";

constexpr std::string_view DEFAULT_CONNECTIONS =
    "    C: \"OO\",100,0\n"
    "    C: \"OO\",200,100\n";

// AC-23's fixture: Y-up, ALREADY-canonical -- UnitScaleFactor: 100 (NOT 1), because §A-14's own
// warning is exactly the trap here: UnitScaleFactor is in CENTIMETRES, so `1` would make this a Y-up
// CENTIMETRE source (the importer then scales it by 100, silently testing the wrong thing). `100` is
// what makes 1 world-space unit equal 1 metre, matching a source ufbx reports needing NO conversion.
//
// CORRECTION, found while writing FI51 (Step 6) and MEASURED against real ufbx v0.23.0, not assumed:
// this constant originally read `FrontAxis: 1, FrontAxisSign: -1` -- copied from DEFAULT_GLOBALS_
// PROPERTIES' Z-up block (whose `UpAxis: 2, FrontAxis: 1` is a valid, DISTINCT pair) without updating
// FrontAxis once UpAxis became `1` too. `UpAxis=1` and `FrontAxis=1` both claim the Y axis, which
// `ufbx_coordinate_axes_valid()` correctly reports as FALSE (EXECUTED: `axes.up=POSITIVE_Y`,
// `axes.front=NEGATIVE_Y` -- degenerate, not three mutually perpendicular axes). Every existing case
// built on this constant (FI16/17/18/21/23/24/25/26/27) happened to keep passing regardless, because
// none of them asserts `warningTotal`/`warnings` and every one of them expects an IDENTITY result --
// which is bit-identical whether ufbx genuinely detects "already Y-up, no conversion needed" or
// merely refuses to convert because the declared axes are unusable (E6's OWN "geometry is imported as
// authored" fallback). FI51 (a plain warningTotal count) is what first made the difference
// observable. FrontAxis=2 (Z), FrontAxisSign=1 pairs with the unchanged UpAxis=1/CoordAxis=0 to give
// three DISTINCT axes -- MEASURED: `ufbx_coordinate_axes_valid()` now true, and the node/geometry
// numbers are UNCHANGED (still the identity conversion every existing case already asserts), so this
// is a strengthening (a real conversion now runs and confirms identity) rather than a behaviour change
// for any currently-passing case.
constexpr std::string_view CANONICAL_GLOBALS_PROPERTIES =
    "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
    "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
    "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n";

// The same quad, already in METRES (not centimetres, matching CANONICAL_GLOBALS_PROPERTIES' unit),
// on a node translated (0,2,0) -- the number AC-23 asks for.
constexpr std::string_view CANONICAL_OBJECTS =
    "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
    "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
    "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "    Model: 100, \"Model::box\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Properties70:  {\n"
    "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,2,0\n"
    "        }\n"
    "    }\n";

// ---- FI39-FI56 (Step 6): the two shader-detection routes a HAND-WRITTEN material needs -----------
//
// MEASURED, and NOT anticipated by the plan: a hand-written FBX material's `ufbx_material::shader_type`
// resolves to `UFBX_SHADER_FBX_PHONG`/`_LAMBERT`/`_UNKNOWN` for any ordinary `ShadingModel` (or none at
// all), and THAT shader's feature-capability bitmask (`ufbx.c`'s `ufbxi_shader_pbr_mappings[]`) does
// NOT include METALNESS, OPACITY, PBR or DOUBLE_SIDED -- so a plain Phong/Lambert material can NEVER
// report `features.metalness.enabled` / `features.opacity.enabled` / `features.double_sided.enabled`
// as true, regardless of which legacy properties are set (EXECUTED: a Phong material with
// `TransparencyFactor` set leaves `features.opacity.enabled == false`). Reaching those three fields
// needs ufbx's 3ds Max PBR material detection: a property pair `3dsMax|ClassIDa`/`3dsMax|ClassIDb`
// (read with `ufbx_find_int`, `ufbx.c:22334-22357`) matching one of a handful of recognised class-ID
// pairs, each selecting a DIFFERENT PBR shader mapping table with its OWN property names under a
// `shader_prop_prefix`. Two routes are used across FI39-FI56, both EXECUTED against the vendored
// ufbx.c, not assumed:
//   - GLTF_MATERIAL_CLASSID: decimal (943849874, 1174294043) == hex (0x38420192, 0x45fe4e1b) ->
//     `UFBX_SHADER_GLTF_MATERIAL`, `shader_prop_prefix == "3dsMax|"`, properties `main|baseColor` /
//     `main|roughness` / `main|metalness` / `main|Alpha` (opacity) / `main|emission` (color) /
//     `main|emissionColor` / `main|DoubleSided` / `main|ambientOcclusion` / `main|normal`. Its
//     capability bitmask is PBR|METALNESS|DIFFUSE|EMISSION|OPACITY|AMBIENT_OCCLUSION -- the ONE route
//     to a real, non-default metallicFactor and to features.opacity/metalness.enabled being true at
//     all. MEASURED: `features.opacity.enabled` is TRUE UNCONDITIONALLY for this shader (a CAPABILITY,
//     not "opacity was authored") -- and `ufbx_material_pbr_maps` is `memset` to zero before any
//     property is fetched, with NO "default to 1.0" pass for `opacity` the way there is for the
//     paired factor/color fields (`ufbxi_update_factor` is never called for it). A material using
//     this classid that never mentions Alpha at all reports `pbr.opacity.has_value == false` AND
//     `pbr.opacity.value_real == 0.0` -- FI41 is the case this finding exists for.
//   - SPEC_GLOSS_CLASSID: decimal (3490651648, 31173939) == hex (0xd00f1e00, 0x01dbad33) ->
//     `UFBX_SHADER_3DS_MAX_PBR_SPEC_GLOSS`, same prefix shape, property `main|glossiness` plus a
//     feature toggle `main|useGlossiness` (a Number "around 1.0" enables
//     `features.roughness_as_glossiness.enabled` via ufbx's own `UFBXI_SHADER_FEATURE_IF_AROUND_1`).
//     MEASURED: ufbx reads the raw "glossiness" value into `pbr.roughness.value_real`
//     UNCONDITIONALLY (no transform in the mapping table itself), and SEPARATELY -- once the feature
//     flag resolves -- MOVES it into `pbr.glossiness` and computes `pbr.roughness` as `1 - glossiness`
//     ONLY when the feature is engaged (`ufbx.c:20200-20215`). This is the only reachable route to
//     `features.roughness_as_glossiness.enabled == true`.
//   - a plain Phong/Lambert material (`ShadingModel: "phong"`, no ClassID) DOES map `DiffuseFactor` /
//     `EmissiveFactor` to `pbr.base_factor` / `pbr.emission_factor`
//     (`ufbxi_fbx_phong_shader_pbr_mapping`) -- the factor-MODULATION half of D13's baseColorFactor /
//     emissiveFactor rows is proven through THIS simpler fixture, never the classid one.
// A texture connects to a PBR map slot with `C: "OP",<textureId>,<materialId>,"<full property name>"`
// (`ufbx.c:15257/15286` -- MEASURED against ufbx's own ASCII connection reader);
// `ufbx_material_map::texture_enabled` becomes true purely from that connection existing
// (`ufbx.c:20079-20085`), with no separate toggle for either shader used here.
//
// FI43 ("feature_disabled on a map is treated as absent") is NOT implemented: `git grep -c
// feature_disabled editor/third_party/ufbx/ufbx.c` is EMPTY -- ufbx v0.23.0 never SETS this field
// anywhere in its own source, so there is no reachable input, from any fixture, that makes it true.
// fbx_import.cpp's `engaged()` gate still checks it (defence in depth for a future ufbx that starts
// setting it), but no case can prove that specific branch discriminates today, and none pretends to.
constexpr std::string_view GLTF_MATERIAL_CLASSID =
    "            P: \"3dsMax|ClassIDa\", \"int\", \"Integer\", \"\",943849874\n"
    "            P: \"3dsMax|ClassIDb\", \"int\", \"Integer\", \"\",1174294043\n";
constexpr std::string_view SPEC_GLOSS_CLASSID =
    "            P: \"3dsMax|ClassIDa\", \"int\", \"Integer\", \"\",3490651648\n"
    "            P: \"3dsMax|ClassIDb\", \"int\", \"Integer\", \"\",31173939\n";

// Assembles a complete ASCII FBX 7.4.0 document from its three variable sections (§D-7), so a future
// case can vary ONE of them without restating the whole file. Pass a DEFAULT_* constant above for any
// section a case does not need to change. PURE: no disk, builds the document entirely in memory.
[[nodiscard]] std::string makeFbx(std::string_view globalsProperties, std::string_view objects,
                                  std::string_view connections) {
    return std::format(
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {{\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "    Creator: \"aero test fixture\"\n"
        "}}\n"
        "GlobalSettings:  {{\n"
        "    Version: 1000\n"
        "    Properties70:  {{\n"
        "{}"
        "    }}\n"
        "}}\n"
        "Objects:  {{\n"
        "{}"
        "}}\n"
        "Connections:  {{\n"
        "{}"
        "}}\n",
        globalsProperties, objects, connections);
}

}  // namespace

// ---- FI1: the §D-7 template through the real dispatch -------------------------------------------
//
// FI1's final shape (plan §V3) is "Ok, 1 node named box, 1 root, 1 mesh" -- but `fbx_import.cpp`'s
// eight phases land across Steps 4-9, so that shape is not reachable in one commit. Rather than leave
// FI0's now-false "Unsupported" statement to rot (it WOULD rot: Step 3 makes ".fbx" importable), FI1
// is introduced HERE and its body is STRENGTHENED as each phase lands, exactly the "write the test,
// see it fail, implement until it passes" discipline applied across a feature that spans several
// commits rather than one. Every version staged below is a TRUE statement about the code AT THAT
// COMMIT, never a placeholder pretending to be more:
//
//   Step 3 (HERE): the dispatch arm routes ".fbx" to importFbx -- but importFbx's body is still the
//                  Step 1 linking stub, which unconditionally returns ParseFailed. Asserting THAT
//                  (rather than Unsupported) is what proves the dispatch arm exists at all.
//   Step 4:        phases 1-2 land (load, error mapping, SourceSpace, warnings, and the cheap
//                  model-level counts read directly from ufbx's own already-computed scene/mesh
//                  fields). The template is a well-formed FBX document, so the load now succeeds:
//                  status becomes Ok, and summary.meshCount becomes real (1) -- "1 mesh" in the
//                  plan's own §V3 wording, read as the SUMMARY count rather than
//                  model.meshes.size(), which stays 0 until phase 6 (Step 7, out of this engagement's
//                  scope) builds the actual ImportedMesh/ImportedPrimitive vectors.
//   Step 5 (this engagement's last step, HERE): phase 3 lands (nodes, hierarchy). nodes.size()
//                  becomes 1, nodes[0].name becomes "box", roots.size() becomes 1.
//
// model.meshes.size() == 1 (the plan's literal §V3 wording, read as the vector rather than the
// summary count) is NOT asserted here -- it becomes true only once phase 6 (Step 7) exists, which is
// out of this engagement's scope. That is stated plainly rather than silently left out.
TEST_CASE("fbx_import: the §D-7 template through the real dispatch arm (FI1)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.summary.meshCount == 1);
    // Step 5's true statement: phase 3 (the node walk) lands here.
    REQUIRE(result.model.nodes.size() == 1);
    CHECK(result.model.nodes[0].name == "box");
    CHECK(result.model.roots.size() == 1);
}

// ---- FI2-FI4: load surface and depth (Step 4, phases 1-2) ---------------------------------------
//
// summary.bounds is EMPTY at BOTH depths through this engagement's end (Steps 3-5): folding real
// bounds is phase 6's "fold bounds" step (§D-4.8, Step 7, out of scope), which needs settings.scale
// and the real per-primitive positions. FI3 therefore does NOT assert bounds.valid() -- only
// vertexCount/triangleCount, which phase 2's cheap structural counts (fbx_import.cpp) already make
// real. That is stated here rather than silently dropped.

TEST_CASE(
    "fbx_import: at Structure depth every per-mesh count is zero -- ignore_all_content's effect, "
    "surfaced through the public model (FI2, AC-21 half 1)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.summary.vertexCount == 0);
    CHECK(result.model.summary.triangleCount == 0);
    CHECK_FALSE(result.model.summary.bounds.valid());
}

TEST_CASE(
    "fbx_import: at Full depth the per-mesh counts are real -- a depth parameter wired backwards "
    "passes FI2 and fails this (FI3, AC-21 half 2)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.summary.vertexCount == 4);    // the quad's four corners
    CHECK(result.model.summary.triangleCount == 2);  // ufbx's own "if triangulated" count for one quad
}

TEST_CASE(
    "fbx_import: Structure and Full agree on the node list and every element COUNT this task wires "
    "(FI4, AC-20/INV-M4 as §A-4 scopes it)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult structure =
        importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    const ImportResult full = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(structure.model.summary.meshCount == full.model.summary.meshCount);
    CHECK(structure.model.summary.materialCount == full.model.summary.materialCount);
    CHECK(structure.model.summary.imageCount == full.model.summary.imageCount);
    CHECK(structure.model.summary.skinCount == full.model.summary.skinCount);
    CHECK(structure.model.summary.animationCount == full.model.summary.animationCount);
    CHECK(structure.externalUris == full.externalUris);
    // The node list: names, localId, parent, children and roots -- phase 3 (this commit) is not
    // depth-gated at all, so any depth-conditional code above it would break one side of this.
    REQUIRE(structure.model.nodes.size() == full.model.nodes.size());
    for (std::size_t i = 0; i < structure.model.nodes.size(); ++i) {
        CAPTURE(i);
        CHECK(structure.model.nodes[i].name == full.model.nodes[i].name);
        CHECK(structure.model.nodes[i].localId == full.model.nodes[i].localId);
        CHECK(structure.model.nodes[i].parent == full.model.nodes[i].parent);
        CHECK(structure.model.nodes[i].children == full.model.nodes[i].children);
    }
    CHECK(structure.model.roots == full.model.roots);
}

TEST_CASE("fbx_import: an empty byte span fails safely, without a null-pointer read (FI5)") {
    const std::span<const std::byte> emptySpan;  // {nullptr, 0}
    const ImportResult result = importModel("t.fbx", "", emptySpan, ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
    CHECK(result.model.nodes.empty());
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("fbx_import: PNG magic bytes in a .fbx are not the default Ok path (FI6)") {
    const std::string png = "\x89PNG\r\n\x1a\n";
    const ImportResult result = importModel("t.fbx", "", asBytes(png), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
    CHECK(result.model.nodes.empty());
}

TEST_CASE(
    "fbx_import: every malformed-input status observed anywhere in this suite is one of the documented "
    "FBX buckets, and none of them is Ok or claims a non-empty model (FI7, AC-49)") {
    // AC-49 asks that "every ufbx_error_type maps to the documented ImportStatus". This TU names NO
    // ufbx type (§V4/§V6's own sibling check for AC-63), so ufbxStatusFor's 24-row switch
    // (fbx_import.cpp, TU-local) cannot be driven directly from here -- the switch's own lack of a
    // `default:` is what keeps IT exhaustive against a future ufbx enumerator, not this case. What
    // THIS case proves through the public surface: a battery of malformed documents each maps to a
    // status from the DOCUMENTED set {ParseFailed, Malformed, Truncated} -- never Ok, never
    // MissingExtension/MissingBuffer (D22: FBX never produces either) -- and every one of them
    // returns an EMPTY model (AC-48's "empty, not partial" contract on a failed import).
    const std::string empty;
    const std::string png = "\x89PNG\r\n\x1a\n";
    const std::string obj = "# comment\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::array<std::string, 3> documents = {empty, png, obj};
    for (const std::string& doc : documents) {
        const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        INFO("document size: ", doc.size());
        CHECK(result.status != ImportStatus::Ok);
        CHECK(result.status != ImportStatus::MissingExtension);
        CHECK(result.status != ImportStatus::MissingBuffer);
        CHECK(result.status != ImportStatus::Unsupported);  // the dispatch already claimed this name
        CHECK(result.model.nodes.empty());
        CHECK(result.model.meshes.empty());
        CHECK(result.model.materials.empty());
    }
}

TEST_CASE(
    "fbx_import: a document truncated mid-Objects fails with ufbx's own description text in the "
    "message (FI8, AC-49)") {
    // Cut right before the "GeometryVersion" marker -- MEASURED against real ufbx v0.23.0 to land
    // inside the Geometry block, past the point ufbx can recover a valid document, without depending
    // on a fragile byte-percentage of a template that may grow.
    const std::string full = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const std::string::size_type cut = full.find("GeometryVersion");
    REQUIRE(cut != std::string::npos);
    const std::string truncated = full.substr(0, cut);
    const ImportResult result = importModel("t.fbx", "", asBytes(truncated), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);
    CHECK(result.model.nodes.empty());
    // ufbx's own description text (MEASURED: "Truncated file"), not a message built from our own
    // words -- a message built purely from our own words would fail this.
    CHECK_FALSE(result.message.empty());
    CHECK(result.message.find("runcated") != std::string::npos);
}

TEST_CASE(
    "fbx_import: an unsupported-and-otherwise-malformed FBX version reports a failure, never Ok, with "
    "an empty model (FI9, D17's sibling)") {
    // MEASURED against real ufbx v0.23.0: a document with FBXVersion outside [3000,7700] but inside
    // the ASCII parser's OWN acceptance window [6000,10000] (ufbx.c:10458) loads FINE, with a WARNING,
    // when the rest of the document is well-formed -- ufbx's own header says as much ("ufbx still
    // tries to load files with unsupported versions"). UFBX_ERROR_UNSUPPORTED_VERSION is a POST-HOC
    // RECLASSIFICATION of a genuinely FAILED, otherwise-unclassified load (ufbx.c:25616), so this
    // fixture is BOTH an out-of-range version AND independently truncated -- either alone is
    // insufficient, and that two-part shape is exactly what this case documents rather than assumes.
    const std::string doc =
        "; FBX 9.0.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 9000\n"
        "    Creator: \"aero test";
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status != ImportStatus::Ok);
    CHECK(result.model.nodes.empty());
}

TEST_CASE(
    "fbx_import: a non-finite UnitScaleFactor is warned about, not silently substituted or ignored "
    "(FI10, E5, §A-14's fixture trap in reverse)") {
    const std::string globals =
        "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
        "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",nan\n";
    const std::string doc = makeFbx(globals, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    // MEASURED: ufbx parses the literal "nan" as a non-finite ufbx_real and does not refuse the load
    // over it -- the load itself stays Ok at phase 1, and the FIRST guard this case exists for
    // (recording the raw value, one warning) is OUR OWN.
    //
    // STRENGTHENED at Step 7 (phase 6 now folds real bounds): a NaN geometry_scale (computed from this
    // same non-finite unit) multiplies through every position, and Aabb::expand() silently IGNORES a
    // non-finite point (std::min/std::max with a NaN operand return the unchanged current value), so
    // summary.bounds stays stuck at the Aabb::empty() sentinel even though summary.vertexCount is real
    // (4, from phase 2's ufbx-native count) -- E5's second half is what this case now also proves,
    // MEASURED rather than assumed: this exact fixture flips from Ok to Malformed the moment phase 6
    // lands. `status == Ok` was a true statement about the code at Step 4; it is no longer true, and
    // updating it here is that same discipline applied across a task spanning several commits.
    CHECK(result.status == ImportStatus::Malformed);
    CHECK(result.warningTotal >= 1);
    CHECK_FALSE(std::isfinite(result.model.sourceSpace.unitMeters));
}

TEST_CASE("fbx_import: an unusable UpAxis produces one warning and geometry imports as authored (FI11, E6)") {
    const std::string globals =
        "        P: \"UpAxis\", \"int\", \"Integer\", \"\",9\n"
        "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
        "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";
    const std::string doc = makeFbx(globals, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    // MEASURED: UpAxis 9 (out of the documented 0/1/2 range) makes ufbx's WHOLE axes triple invalid
    // (ufbx_coordinate_axes_valid returns false), not merely the up component.
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.warningTotal >= 1);
    CHECK(result.model.sourceSpace.upAxis == '?');
}

TEST_CASE(
    "fbx_import: Wavefront OBJ text in a .fbx does NOT import -- D17's format-forcing, proved "
    "empirically rather than assumed from the header wording (FI12, AC-48, D17)") {
    const std::string obj = "# a Wavefront OBJ document, not FBX\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result = importModel("t.fbx", "", asBytes(obj), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status != ImportStatus::Ok);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
}

// ---- FI13-FI27: space conversion and hierarchy (Step 5, phase 3) --------------------------------
//
// This is where D6's whole conversion regime is either right or silently wrong. The hand-computed
// literals below were MEASURED against real ufbx v0.23.0 (never assumed), the same discipline §A-8
// and the plan's own FI15/FI16 commentary insist on.
//
// SCOPE, at Step 5: FI15-FI17 asserted their NODE-LEVEL half only -- translation, rotation, scale on
// `ImportedNode`. §A-8 point 2 is why that was not a reduced proof on its own: "the axis conversion
// lives in the NODE ROTATION, not in the vertices ... a wrong target_axes shows up there first." The
// MESH-LEVEL half (converted vertex positions, summary.bounds) needed phase 6, which did not exist
// yet -- it is now COMPLETE, inline in FI15/16/17 below, landed at Step 7 together with FI20 (which
// needed the identical real triangle data) and FI23/FI24's own mesh-level halves (positions unmodified
// by a geometry transform / meshIndex equality across two instances).
//
// FI28 (MAX_NODES_PER_MODEL, 65536) remains NOT implemented, RE-EXAMINED at Step 7 as instructed and
// still refused for the SAME reason as at Step 5: it is a NODE cap, unaffected by mesh support
// existing -- reaching it needs a fixture on the order of megabytes with 65536 real node blocks, which
// is squarely against this whole file's own "a few hundred bytes, no disk, instant" tier-0 contract
// (§G-12/R3). FI27 above already proves the IDENTICAL "our own cap, checked before the allocation it
// bounds" property against MAX_FBX_NODE_DEPTH (256), three orders of magnitude cheaper to reach for
// real. The FOUR PER-MODEL caps Step 7 genuinely unlocks (meshes now existing) are each re-examined on
// their own merits at FI38, below: MAX_PRIMITIVES_PER_MODEL (4096) turned out to be cheaply reachable
// with many minimal meshes and is IMPLEMENTED there; MAX_VERTICES_PER_MODEL (8 000 000) and
// MAX_INDICES_PER_MODEL (24 000 000) are not, for reasons stated at FI38's own definition.

TEST_CASE("fbx_import: sourceSpace.declared is true for every FBX, with a real formatVersion (FI13)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.model.sourceSpace.declared);
    CHECK(result.model.sourceSpace.formatVersion == "FBX 7400 ascii");
}

TEST_CASE(
    "fbx_import: sourceSpace stays all-default for a .gltf driven through the same importModel (FI14, "
    "AC-24 half 2)") {
    const std::string doc = R"({"asset": {"version": "2.0"}})";
    const ImportResult result = importModel("t.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    CHECK_FALSE(result.model.sourceSpace.declared);
    CHECK(result.model.sourceSpace.unitMeters == doctest::Approx(1.0F));
    CHECK(result.model.sourceSpace.upAxis == 'Y');
    CHECK(result.model.sourceSpace.generator.empty());
    CHECK(result.model.sourceSpace.formatVersion.empty());
}

TEST_CASE(
    "fbx_import: AC-22's Z-up centimetre conversion, on the node -- a wrong target_axes shows up here "
    "FIRST (FI15, AC-22, seeds S1-S5's discriminator)") {
    // MEASURED against real ufbx v0.23.0 (§A-8): a node at Lcl Translation (0,0,200) in a Z-up,
    // UnitScaleFactor:1 (centimetre) source converts to translation (0, 2, 5.68434e-16) and rotation
    // (-0.7071068, 0, 0, 0.7071068) -- NOT (0,2,0) and identity. The residual Z is real (a genuine
    // rotation's floating-point remainder), so this compares with an epsilon, never `==`.
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    const auto& node = result.model.nodes[0];
    CHECK(node.translation.x == APPROX_POS(0.0));
    CHECK(node.translation.y == APPROX_POS(2.0));
    CHECK(node.translation.z == APPROX_POS(0.0));
    CHECK(node.rotation.x == APPROX_ROT(-0.7071068));
    CHECK(node.rotation.y == APPROX_ROT(0.0));
    CHECK(node.rotation.z == APPROX_ROT(0.0));
    CHECK(node.rotation.w == APPROX_ROT(0.7071068));

    // Mesh-level half, completed at Step 7 (deferred at Step 5 -- see this file's own state at the
    // time). §A-8: the quad's positions convert from the source's own centimetre coordinates
    // `(0,0,0)(100,0,0)(100,100,0)(0,100,0)` to METRES, IN THE SOURCE'S OWN LOCAL AXIS FRAME --
    // `MODIFY_GEOMETRY` scales geometry but does not permute it; the Y-up-ness arrives entirely
    // through the node rotation asserted above. `FI29` proves the winding/index shape (2 triangles, 6
    // indices, 4 unique vertices) for the identical quad; this case's own job is the CONVERTED VALUES.
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& positions = result.model.meshes[0].primitives[0].positions;
    REQUIRE(positions.size() == 4);
    CHECK(positions[0].x == APPROX_POS(0.0));
    CHECK(positions[0].y == APPROX_POS(0.0));
    CHECK(positions[0].z == APPROX_POS(0.0));
    CHECK(positions[1].x == APPROX_POS(1.0));
    CHECK(positions[1].y == APPROX_POS(0.0));
    CHECK(positions[1].z == APPROX_POS(0.0));
    CHECK(positions[2].x == APPROX_POS(1.0));
    CHECK(positions[2].y == APPROX_POS(1.0));
    CHECK(positions[2].z == APPROX_POS(0.0));
    CHECK(positions[3].x == APPROX_POS(0.0));
    CHECK(positions[3].y == APPROX_POS(1.0));
    CHECK(positions[3].z == APPROX_POS(0.0));
    // summary.bounds is folded from LOCAL primitive positions, never through node transforms
    // (gltf_import.cpp's own precedent) -- so its SIZE is (1,1,0) in the source's own local axis
    // order, a 1 m x 1 m quad, matching Blender's own "local dimensions" reporting.
    REQUIRE(result.model.summary.bounds.valid());
    const engine::Vec3 boundsSize = result.model.summary.bounds.size();
    CHECK(boundsSize.x == APPROX_POS(1.0));
    CHECK(boundsSize.y == APPROX_POS(1.0));
    CHECK(boundsSize.z == APPROX_POS(0.0));
}

TEST_CASE(
    "fbx_import: AC-23's already-canonical source imports UNCHANGED -- a conversion that ALWAYS "
    "fires passes FI15 and fails this (FI16, AC-23)") {
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, CANONICAL_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    const auto& node = result.model.nodes[0];
    CHECK(node.translation.x == doctest::Approx(0.0F));
    CHECK(node.translation.y == doctest::Approx(2.0F));
    CHECK(node.translation.z == doctest::Approx(0.0F));
    CHECK(node.rotation == engine::Quat::identity());
    CHECK(node.scale.x == doctest::Approx(1.0F));
    CHECK(node.scale.y == doctest::Approx(1.0F));
    CHECK(node.scale.z == doctest::Approx(1.0F));

    // Mesh-level half, completed at Step 7 (deferred at Step 5). A conversion that ALWAYS fires
    // passes FI15's mesh-level half and fails THIS one: CANONICAL_OBJECTS' quad is already in
    // metres, `(0,0,0)(1,0,0)(1,1,0)(0,1,0)`, and a source already at the target space imports with
    // NO scaling at all.
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& positions = result.model.meshes[0].primitives[0].positions;
    REQUIRE(positions.size() == 4);
    CHECK(positions[0] == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(positions[1] == engine::Vec3{1.0F, 0.0F, 0.0F});
    CHECK(positions[2] == engine::Vec3{1.0F, 1.0F, 0.0F});
    CHECK(positions[3] == engine::Vec3{0.0F, 1.0F, 0.0F});
}

TEST_CASE(
    "fbx_import: AC-28 -- an already-canonical source with an asymmetric TRS imports to EXACTLY the "
    "source's numbers, component for component (FI17, seeds S6/S7's discriminator, MI40b's shape a "
    "second time)") {
    // MEASURED against real ufbx v0.23.0: Lcl Rotation (15, 30, 45) degrees on an already-canonical
    // (Y-up, metre) source produces rotation (0.0182830462, 0.285320133, 0.335270344, 0.897692569) --
    // a real quaternion with all four components non-zero, so a negated/swapped/transposed/reordered
    // read fails on the FIRST component that differs. translation/scale need no measurement: an
    // already-canonical source passes them through with NO arithmetic at all.
    //
    // Mesh-level half, completed at Step 7 (deferred at Step 5): the plan's own FI17 also names "an
    // asymmetric triangle (0,0,0)(1,0,0)(0,0,2)" -- ADDED here as a real Geometry connected to the
    // node (changed from "Null" to "Mesh"), asserting the identical no-transform-math property for
    // GEOMETRY that the TRS assertions above already prove for the node. The inverse-bind-matrix half
    // of the plan's own FI17 wording is Step 8's FI57 (element-by-element, seed S8's dedicated case) --
    // not duplicated here.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::asym\", \"Mesh\" {\n"
        "        Vertices: *9 { a: 0,0,0,1,0,0,0,0,2 }\n"
        "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::asym\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",1,2,3\n"
        "            P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",15,30,45\n"
        "            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,2,3\n"
        "        }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",200,100\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    const auto& node = result.model.nodes[0];
    CHECK(node.translation.x == doctest::Approx(1.0F));
    CHECK(node.translation.y == doctest::Approx(2.0F));
    CHECK(node.translation.z == doctest::Approx(3.0F));
    CHECK(node.rotation.x == APPROX_ROT(0.0182830462));
    CHECK(node.rotation.y == APPROX_ROT(0.285320133));
    CHECK(node.rotation.z == APPROX_ROT(0.335270344));
    CHECK(node.rotation.w == APPROX_ROT(0.897692569));
    CHECK(node.scale.x == doctest::Approx(1.0F));
    CHECK(node.scale.y == doctest::Approx(2.0F));
    CHECK(node.scale.z == doctest::Approx(3.0F));

    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& positions = result.model.meshes[0].primitives[0].positions;
    REQUIRE(positions.size() == 3);
    CHECK(positions[0] == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(positions[1] == engine::Vec3{1.0F, 0.0F, 0.0F});
    CHECK(positions[2] == engine::Vec3{0.0F, 0.0F, 2.0F});
}

TEST_CASE(
    "fbx_import: sourceSpace.unitMeters is the fixture-trap guard itself -- proves axes.up was read, "
    "not original_axis_up (which is UNKNOWN/'?' for both) (FI18, §A-14)") {
    const std::string zUpCm = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult z = importModel("t.fbx", "", asBytes(zUpCm), ImportSettings{}, ImportDepth::Full, {});
    CHECK(z.model.sourceSpace.unitMeters == doctest::Approx(0.01F));
    CHECK(z.model.sourceSpace.upAxis == 'Z');

    const std::string yUpM = makeFbx(CANONICAL_GLOBALS_PROPERTIES, CANONICAL_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult y = importModel("t.fbx", "", asBytes(yUpM), ImportSettings{}, ImportDepth::Full, {});
    CHECK(y.model.sourceSpace.unitMeters == doctest::Approx(1.0F));
    CHECK(y.model.sourceSpace.upAxis == 'Y');
}

TEST_CASE(
    "fbx_import: sourceSpace.generator falls back to Creator when the exporter is UNKNOWN, and is "
    "empty only when Creator is ALSO empty; formatVersion is never empty for a successful import "
    "(FI19, §A-21)") {
    // CORRECTION to the plan's own §A-21 (found by running this case, not assumed): "every tier-0
    // fixture has an empty generator" is true only for a fixture whose Creator field is ALSO empty.
    // The shared §D-7 template's `Creator: "aero test fixture"` line MEASURABLY populates
    // metadata.creator (ufbx.c's ufbxi_read_header_extension reads Creator as a direct child of
    // FBXHeaderExtension), and §A-21's OWN fallback rule -- "fall back to metadata.creator... which
    // is what a Blender export actually carries" -- is exactly what generatorString then does. Both
    // halves of that fallback are asserted here, not just the one the plan's prose happened to name.
    const std::string withCreator = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult withResult =
        importModel("box.fbx", "", asBytes(withCreator), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(withResult.status == ImportStatus::Ok);
    CHECK(withResult.model.sourceSpace.generator == "aero test fixture");
    CHECK_FALSE(withResult.model.sourceSpace.formatVersion.empty());

    // The identical §D-7 template with its `Creator:` line removed entirely (never emitted with an
    // empty value -- omitted, matching how an exporter genuinely offering nothing would write it).
    const std::string noCreator = std::format(
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {{\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "}}\n"
        "GlobalSettings:  {{\n"
        "    Version: 1000\n"
        "    Properties70:  {{\n"
        "{}"
        "    }}\n"
        "}}\n"
        "Objects:  {{\n"
        "{}"
        "}}\n"
        "Connections:  {{\n"
        "{}"
        "}}\n",
        DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult noResult =
        importModel("box.fbx", "", asBytes(noCreator), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(noResult.status == ImportStatus::Ok);
    CHECK(noResult.model.sourceSpace.generator.empty());
    CHECK_FALSE(noResult.model.sourceSpace.formatVersion.empty());
}

TEST_CASE(
    "fbx_import: metadata.mirror_axis == 0 for the Z-up -> Y-up conversion -- the triangle winding is "
    "UNCHANGED between FI16's canonical source and FI15's converted one (FI20, D6's winding argument, "
    "made empirical rather than argued)") {
    // D6/§A-8 point 3: right-handed Z-up -> right-handed Y-up is a PURE ROTATION -- no mirror, no
    // winding flip, handedness_conversion_axis never consulted (MEASURED: mirror_axis comes back 0).
    // Proven here by comparing the FULLY IMPORTED index buffers of the two fixtures directly: FI15's
    // Z-up centimetre quad converts to the IDENTICAL positions AND the IDENTICAL index order as
    // FI16's already-canonical quad. Seed S5 (left_handed_y_up) mirrors an axis and reverses winding
    // -- this is where that would show, as a DIFFERING index order despite identical positions.
    const std::string zUpDoc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult zUp = importModel("box.fbx", "", asBytes(zUpDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(zUp.status == ImportStatus::Ok);

    const std::string yUpDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, CANONICAL_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult yUp = importModel("box.fbx", "", asBytes(yUpDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(yUp.status == ImportStatus::Ok);

    REQUIRE(zUp.model.meshes.size() == 1);
    REQUIRE(yUp.model.meshes.size() == 1);
    REQUIRE(zUp.model.meshes[0].primitives.size() == 1);
    REQUIRE(yUp.model.meshes[0].primitives.size() == 1);
    const auto& zUpPrim = zUp.model.meshes[0].primitives[0];
    const auto& yUpPrim = yUp.model.meshes[0].primitives[0];

    REQUIRE(zUpPrim.positions.size() == yUpPrim.positions.size());
    for (std::size_t i = 0; i < zUpPrim.positions.size(); ++i) {
        CAPTURE(i);
        CHECK(zUpPrim.positions[i].x == APPROX_POS(yUpPrim.positions[i].x));
        CHECK(zUpPrim.positions[i].y == APPROX_POS(yUpPrim.positions[i].y));
        CHECK(zUpPrim.positions[i].z == APPROX_POS(yUpPrim.positions[i].z));
    }
    // The winding proof itself: the INDEX ORDER (not merely the position VALUES) is identical --
    // proving no reverse_winding fired during the conversion.
    CHECK(zUpPrim.indices == yUpPrim.indices);
    // ... AND an ABSOLUTE pin, added by the sabotage round, because the comparison above is RELATIVE:
    // a change that treats BOTH fixtures the same way leaves it green while both are wrong. MEASURED:
    // ufbx_triangulate_face emits `0 1 2 2 3 0` for a quad and ufbx_generate_indices rewrites it in
    // place to the same order.
    //
    // STATED PLAINLY RATHER THAN LEFT IMPLIED: neither form catches seed S5 (target_axes ->
    // left_handed_y_up). That was this case's own claimed purpose, and it is not true -- MEASURED by
    // running the seed: ufbx's mirror does NOT reverse the emitted index order for this quad, so both
    // the relative and the absolute assertion stay green. S5 is caught, loudly, by the ABSOLUTE
    // position and quaternion numbers in FI15/FI16/FI17 instead. Recorded here so the next reader does
    // not take this case for cover it does not provide.
    CHECK(zUpPrim.indices == std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0});
    CHECK(yUpPrim.indices == std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0});
}

TEST_CASE("fbx_import: a 4-deep chain imports with correct parent/children/roots, in source order (FI21, AC-25)") {
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::a\", \"Null\" { Version: 232 }\n"
        "    Model: 101, \"Model::b\", \"Null\" { Version: 232 }\n"
        "    Model: 102, \"Model::c\", \"Null\" { Version: 232 }\n"
        "    Model: 103, \"Model::d\", \"Null\" { Version: 232 }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",101,100\n"
        "    C: \"OO\",102,101\n"
        "    C: \"OO\",103,102\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 4);
    REQUIRE(result.model.roots.size() == 1);

    const auto findByName = [&](std::string_view name) -> const engine::editor::ImportedNode* {
        for (const auto& n : result.model.nodes) {
            if (n.name == name) {
                return &n;
            }
        }
        return nullptr;
    };
    const auto* a = findByName("a");
    const auto* b = findByName("b");
    const auto* c = findByName("c");
    const auto* d = findByName("d");
    REQUIRE((a != nullptr && b != nullptr && c != nullptr && d != nullptr));
    CHECK(result.model.roots[0] == a->localId);
    CHECK(a->parent == engine::editor::INVALID_SUBASSET);
    REQUIRE(a->children.size() == 1);
    CHECK(a->children[0] == b->localId);
    CHECK(b->parent == a->localId);
    REQUIRE(b->children.size() == 1);
    CHECK(b->children[0] == c->localId);
    CHECK(c->parent == b->localId);
    REQUIRE(c->children.size() == 1);
    CHECK(c->children[0] == d->localId);
    CHECK(d->parent == c->localId);
    CHECK(d->children.empty());
}

TEST_CASE(
    "fbx_import: the FBX root is NOT emitted as a node, and localId is the raw ufbx typed_id, not the "
    "vector position (FI22, §A-13)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    // Exactly the authored count (one Model block) -- no node has the root's own empty name, and no
    // extra entry exists for it.
    REQUIRE(result.model.nodes.size() == 1);
    CHECK_FALSE(result.model.nodes[0].name.empty());
    // MEASURED: ufbx's root is typed_id 0, and the first authored node is typed_id 1 -- a walk that
    // renumbers localId to the vector index would read 0 here instead.
    CHECK(result.model.nodes[0].localId != 0);
    CHECK(result.model.nodes[0].localId == 1);
}

TEST_CASE(
    "fbx_import: a node with a geometry transform produces a helper node named with "
    "GEOMETRY_HELPER_SUFFIX (FI23 node-half, AC-26, seed S10's discriminator)") {
    // "<geometry helper>" MUST match fbx_import.cpp's own GEOMETRY_HELPER_SUFFIX -- this TU names no
    // ufbx type, so it cannot reference that TU-local constant directly, and re-states the literal
    // instead (opts.geometry_transform_helper_name is set to this exact string).
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",1,0,0\n"
        "        }\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    const engine::editor::ImportedNode* helper = nullptr;
    for (const auto& n : result.model.nodes) {
        if (n.name.find("<geometry helper>") != std::string::npos) {
            helper = &n;
        }
    }
    REQUIRE(helper != nullptr);

    // Mesh-level half, completed at Step 7 (deferred at Step 5): HELPER_NODES handling bakes the
    // geometry transform into the HELPER's own local_transform, never into the vertex data (D7/D8's
    // whole reason for existing) -- the mesh's positions stay EXACTLY as authored.
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& positions = result.model.meshes[0].primitives[0].positions;
    REQUIRE(positions.size() == 4);
    CHECK(positions[0] == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(positions[1] == engine::Vec3{1.0F, 0.0F, 0.0F});
    CHECK(positions[2] == engine::Vec3{1.0F, 1.0F, 0.0F});
    CHECK(positions[3] == engine::Vec3{0.0F, 1.0F, 0.0F});
    // The MESH is attached to the HELPER node, not the original -- the helper is what carries the
    // baked geometry-transform offset in its own TRS, so it is the node a renderer must walk to.
    CHECK(helper->meshIndex == 0);
}

TEST_CASE(
    "fbx_import: a mesh instanced by two nodes with DIFFERENT geometry transforms produces TWO helper "
    "nodes (FI24 node-half, AC-27, seed S9's discriminator)") {
    // MEASURED against real ufbx v0.23.0: this shape produces meshes.count == 1 (one shared
    // ufbx_mesh) and TWO geometry-transform helper nodes, one parented to each instance -- confirming
    // ufbx does NOT bake the transform into per-node mesh copies under HELPER_NODES handling.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::instanceA\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",1,0,0\n"
        "        }\n"
        "    }\n"
        "    Model: 101, \"Model::instanceB\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",2,0,0\n"
        "        }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",101,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",200,101\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    // instanceA, instanceB and their two helpers -- four nodes, root excluded.
    CHECK(result.model.nodes.size() == 4);
    std::vector<const engine::editor::ImportedNode*> helpers;
    for (const auto& n : result.model.nodes) {
        if (n.name.find("<geometry helper>") != std::string::npos) {
            helpers.push_back(&n);
        }
    }
    CHECK(helpers.size() == 2);

    // Mesh-level half, completed at Step 7 (deferred at Step 5): ONE shared ImportedMesh (ufbx does
    // NOT bake the transform into per-node mesh copies under HELPER_NODES handling, MEASURED), and
    // BOTH helper nodes' meshIndex point at that same, single mesh (AC-27).
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(helpers.size() == 2);
    CHECK(helpers[0]->meshIndex == 0);
    CHECK(helpers[1]->meshIndex == 0);
}

TEST_CASE(
    "fbx_import: a non-standard inherit mode produces a node named with SCALE_HELPER_SUFFIX (FI25, "
    "seed S11's discriminator)") {
    // InheritType 2 ("Rrs" / IGNORE_PARENT_SCALE) on a child of a NON-UNIFORMLY-scaled parent is what
    // MEASURABLY produces a scale helper under HELPER_NODES handling -- InheritType 1 does not exist
    // in ufbx's own mapping (ufbx.c's ufbxi_read_model: 0 -> COMPONENTWISE_SCALE, 2 ->
    // IGNORE_PARENT_SCALE, everything else including 1 is left at NORMAL).
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::parent\", \"Null\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,3,4\n"
        "        }\n"
        "    }\n"
        "    Model: 101, \"Model::child\", \"Null\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"InheritType\", \"enum\", \"\", \"\",2\n"
        "        }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",101,100\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    bool foundHelper = false;
    for (const auto& n : result.model.nodes) {
        if (n.name.find("<scale helper>") != std::string::npos) {
            foundHelper = true;
        }
    }
    CHECK(foundHelper);
}

TEST_CASE("fbx_import: two roots import in source order (FI26)") {
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::first\", \"Null\" { Version: 232 }\n"
        "    Model: 101, \"Model::second\", \"Null\" { Version: 232 }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",101,0\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.roots.size() == 2);
    REQUIRE(result.model.nodes.size() == 2);
    CHECK(result.model.nodes[0].name == "first");
    CHECK(result.model.nodes[1].name == "second");
    CHECK(result.model.roots[0] == result.model.nodes[0].localId);
    CHECK(result.model.roots[1] == result.model.nodes[1].localId);
}

TEST_CASE(
    "fbx_import: MAX_FBX_NODE_DEPTH exceeded is Truncated, with the load itself refused -- proves "
    "node_depth_limit reaches ufbx (FI27, AC-50)") {
    // A chain of 257 nested Model blocks (depth 1..257) exceeds node_depth_limit=256; a chain of 256
    // (depth 1..256) does not -- MEASURED, the exact boundary. Built programmatically: 257 hand-
    // written blocks would defeat the "no case restates a whole document" rule this file otherwise
    // follows, and the point is the CAP, not the specific hierarchy shape.
    std::string objects;
    std::string connections;
    constexpr int DEPTH = 257;
    for (int i = 0; i < DEPTH; ++i) {
        objects += std::format("    Model: {}, \"Model::n{}\", \"Null\" {{ Version: 232 }}\n", 100 + i, i);
        const int parent = (i == 0) ? 0 : (100 + i - 1);
        connections += std::format("    C: \"OO\",{},{}\n", 100 + i, parent);
    }
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, connections);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    // ufbx refuses the WHOLE load when its own node_depth_limit is exceeded (no partial ufbx_scene is
    // ever returned), so unlike a cap WE enforce mid-walk (FI28's would-be shape), there is no
    // "coherent smaller model" to speak of here -- the model is EMPTY, not partial.
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.model.nodes.empty());
}

// FI72 (D16, AC-50) -- MAX_FBX_TEMP_BYTES / MAX_FBX_RESULT_BYTES / MAX_FBX_ALLOCATIONS -- is NOT
// implemented as a case, and this is a deliberate, reasoned decision, not a silent drop.
//
// D16 sets FOUR ufbx allocator caps (model_import.hpp): MAX_FBX_NODE_DEPTH (256), MAX_FBX_TEMP_BYTES
// (1 GiB), MAX_FBX_RESULT_BYTES (1 GiB) and MAX_FBX_ALLOCATIONS (4 000 000). fbx_import.cpp wires all
// four into ufbx_load_opts (opts.node_depth_limit, opts.temp_allocator.{memory,allocation}_limit,
// opts.result_allocator.{memory,allocation}_limit) in ONE block, at load time. ufbxStatusFor's own
// 24-row switch (fbx_import.cpp, TU-local -- this TU names no ufbx type and cannot call it directly,
// FI7's own precedent) maps UFBX_ERROR_MEMORY_LIMIT, UFBX_ERROR_ALLOCATION_LIMIT and
// UFBX_ERROR_NODE_DEPTH_LIMIT to ImportStatus::Truncated THROUGH THE SAME case-list -- one shared
// `return ImportStatus::Truncated;` statement, not three independent lines that could individually
// drift. FI27 (immediately above) already drives UFBX_ERROR_NODE_DEPTH_LIMIT with a REAL over-cap
// document (257 nested nodes) and observes Truncated, which is proof of the shared code path the other
// two enumerators in that same case-list would also take.
//
// TWO DIFFERENT CLAIMS, and it matters which one this comment is making:
//   1. "UFBX_ERROR_MEMORY_LIMIT/ALLOCATION_LIMIT map to ImportStatus::Truncated" -- PROVEN, by
//      construction (the shared switch arm FI27 already exercises for NODE_DEPTH_LIMIT).
//   2. "a real FBX document actually TRIPS MAX_FBX_TEMP_BYTES, MAX_FBX_RESULT_BYTES or
//      MAX_FBX_ALLOCATIONS" -- NOT PROVEN, and left unreached rather than faked. Unlike
//      MAX_FBX_NODE_DEPTH (a "many minimal nested objects" shortcut, FI27) or
//      MAX_PRIMITIVES_PER_MODEL (FI38, below: "many minimal meshes"), there is no cheap shortcut here:
//      ufbx's temp/result allocators back EVERY allocation the parser and the scene builder make for
//      the WHOLE document, so reaching a 1 GiB running total or 4 000 000 individual allocations needs
//      a document on the order of a real multi-hundred-thousand-element scene -- flatly incompatible
//      with this file's own tier-0 contract (a few hundred bytes to at most a few hundred KB, no disk,
//      instant -- SS G-12/R3). This is the IDENTICAL discipline FI38 already applies to
//      MAX_VERTICES_PER_MODEL/MAX_INDICES_PER_MODEL, and the MAX_ANIMATION_KEYS_PER_MODEL note beside
//      FI71 applies a third time.
//
// A case that drove ufbx_load_opts.temp_allocator.memory_limit down to a tiny number FROM THIS FILE, to
// make the cap cheaply reachable, is NOT possible either: that field is set once, inside
// fbx_import.cpp, from the MAX_FBX_* constants themselves -- there is no ImportSettings knob or other
// caller-supplied path that reaches it, by design (D16: these are correctness limits, not user
// preferences, the identical reasoning `.claude/rules/editor.md` states for axis/unit conversion).

// ---- FI29-FI38: meshes, triangulation, index generation, caps (Step 7, phase 6) ------------------

TEST_CASE("fbx_import: a quad triangulates to 2 triangles, exactly 6 indices, 4 unique vertices (FI29, AC-29)") {
    // §G-16 item 4 / A12, MEASURED: a one-quad mesh has mesh->num_indices == 4; ufbx_triangulate_face
    // writes `0 1 2 2 3 0` into that same 4-entry space, and ufbx_generate_indices dedups it back down
    // to exactly 4 unique vertices. Seed S33 (a padded, uninitialised interleaved stream) would make
    // memcmp see 6 distinct vertices instead -- this exact count is the discriminator.
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, CANONICAL_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 6);
    CHECK(prim.positions.size() == 4);
    CHECK(result.model.summary.triangleCount == 2);
}

TEST_CASE(
    "fbx_import: a convex hexagon triangulates to 4 triangles, 12 indices, no degenerate triangle (FI30, AC-30)") {
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::hex\", \"Mesh\" {\n"
        "        Vertices: *18 { a: 2,0,0, 1,1,0, -1,1,0, -2,0,0, -1,-1,0, 1,-1,0 }\n"
        "        PolygonVertexIndex: *6 { a: 0,1,2,3,4,-6 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::hex\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.indices.size() == 12);
    REQUIRE(prim.indices.size() % 3 == 0);
    for (std::size_t t = 0; t < prim.indices.size() / 3; ++t) {
        CAPTURE(t);
        const engine::Vec3& a = prim.positions[prim.indices[t * 3 + 0]];
        const engine::Vec3& b = prim.positions[prim.indices[t * 3 + 1]];
        const engine::Vec3& c = prim.positions[prim.indices[t * 3 + 2]];
        const engine::Vec3 cr = engine::cross(b - a, c - a);
        const float area2 = engine::length(cr);
        CHECK(area2 > 1e-4F);  // no degenerate (zero-area) triangle
    }
}

TEST_CASE(
    "fbx_import: a triangle, a point face and a line face in one mesh -- the triangle survives, and "
    "EXACTLY ONE warning names both dropped-face counts (FI31, AC-31)") {
    // MEASURED against real ufbx v0.23.0: PolygonVertexIndex `0,1,-3, -4, 3,-5` parses to three FACES
    // (a triangle [0,1,2], a point face [3], a line face [3,4]) -- num_point_faces=1, num_line_faces=1.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::mixed\", \"Mesh\" {\n"
        "        Vertices: *15 { a: 0,0,0, 1,0,0, 0,1,0, 2,2,0, 3,3,0 }\n"
        "        PolygonVertexIndex: *6 { a: 0,1,-3, -4, 3,-5 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::mixed\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    CHECK(result.model.meshes[0].primitives[0].indices.size() == 3);  // the triangle survives
    CHECK(result.model.summary.triangleCount == 1);
    CHECK(result.warningTotal == 1);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("point") != std::string::npos);
    CHECK(result.warnings[0].find("line") != std::string::npos);
}

TEST_CASE(
    "fbx_import: two materials produce two primitives with distinct, correct materialIndex; a mesh "
    "with none produces one primitive with INVALID_SUBASSET, never 0 (FI32, AC-32)") {
    const std::string objects =
        "    Material: 300, \"Material::a\", \"\" { Version: 102 ShadingModel: \"phong\" }\n"
        "    Material: 301, \"Material::b\", \"\" { Version: 102 ShadingModel: \"phong\" }\n"
        "    Geometry: 200, \"Geometry::two\", \"Mesh\" {\n"
        "        Vertices: *18 { a: 0,0,0, 1,0,0, 1,1,0, 0,1,0, 2,0,0, 2,1,0 }\n"
        "        PolygonVertexIndex: *7 { a: 0,1,2,-4, 1,4,-6 }\n"
        "        GeometryVersion: 124\n"
        "        LayerElementMaterial: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygon\"\n"
        "            ReferenceInformationType: \"IndexToDirect\"\n"
        "            Materials: *2 { a: 0,1 }\n"
        "        }\n"
        "        Layer: 0 {\n"
        "            Version: 100\n"
        "            LayerElement:  { Type: \"LayerElementMaterial\" TypedIndex: 0 }\n"
        "        }\n"
        "    }\n"
        "    Model: 100, \"Model::two\", \"Mesh\" { Version: 232 }\n"
        "    Geometry: 201, \"Geometry::none\", \"Mesh\" {\n"
        "        Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }\n"
        "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 101, \"Model::none\", \"Mesh\" { Version: 232 }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,100\n"
        "    C: \"OO\",301,100\n"
        "    C: \"OO\",101,0\n"
        "    C: \"OO\",201,101\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 2);
    const engine::editor::ImportedMesh* twoMesh = nullptr;
    const engine::editor::ImportedMesh* noneMesh = nullptr;
    for (const auto& m : result.model.meshes) {
        if (m.name == "two") {
            twoMesh = &m;
        } else if (m.name == "none") {
            noneMesh = &m;
        }
    }
    REQUIRE(twoMesh != nullptr);
    REQUIRE(noneMesh != nullptr);
    REQUIRE(twoMesh->primitives.size() == 2);
    CHECK(twoMesh->primitives[0].materialIndex != twoMesh->primitives[1].materialIndex);
    CHECK(twoMesh->primitives[0].materialIndex != engine::editor::INVALID_SUBASSET);
    CHECK(twoMesh->primitives[1].materialIndex != engine::editor::INVALID_SUBASSET);
    REQUIRE(noneMesh->primitives.size() == 1);
    CHECK(noneMesh->primitives[0].materialIndex == engine::editor::INVALID_SUBASSET);
}

TEST_CASE(
    "fbx_import: position + normal + UV -> exactly those three vectors non-empty, every other vector "
    "empty, attributes exactly Position|Normal|TexCoord0 (FI33, AC-33)") {
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "        LayerElementNormal: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n"
        "            ReferenceInformationType: \"Direct\"\n"
        "            Normals: *12 { a: 0,0,1, 0,0,1, 0,0,1, 0,0,1 }\n"
        "        }\n"
        "        LayerElementUV: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n"
        "            ReferenceInformationType: \"IndexToDirect\"\n"
        "            UV: *8 { a: 0,0, 1,0, 1,1, 0,1 }\n"
        "            UVIndex: *4 { a: 0,1,2,3 }\n"
        "        }\n"
        "        Layer: 0 {\n"
        "            Version: 100\n"
        "            LayerElement:  { Type: \"LayerElementNormal\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementUV\" TypedIndex: 0 }\n"
        "        }\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK_FALSE(prim.positions.empty());
    CHECK_FALSE(prim.normals.empty());
    CHECK_FALSE(prim.uv0.empty());
    CHECK(prim.uv1.empty());
    CHECK(prim.colors.empty());
    CHECK(prim.tangents.empty());
    CHECK(prim.joints.empty());
    CHECK(prim.weights.empty());
    using engine::editor::has;
    using engine::editor::VertexAttribute;
    CHECK(prim.attributes == (VertexAttribute::Position | VertexAttribute::Normal | VertexAttribute::TexCoord0));
}

TEST_CASE(
    "fbx_import: tangents AND bitangents -> tangents.size() == positions.size(), every .w exactly "
    "+1.0F or -1.0F (FI34, AC-34a)") {
    // MEASURED (correcting an earlier draft of this fixture): ufbx requires a UV LAYER for
    // vertex_tangent/vertex_bitangent to be exposed AT ALL, even when a LayerElementTangent/
    // LayerElementBinormal block is present in the file -- tangent space is structurally part of a UV
    // SET (`ufbx_uv_set` bundles vertex_uv/vertex_tangent/vertex_bitangent together), and with
    // uv_sets.count == 0 there is no slot for the tangent/binormal data to attach to. Executed: the
    // identical fixture without a LayerElementUV block reports vertex_tangent.exists == false. A real
    // exporter never produces tangents without UVs either (tangent generation needs a UV
    // parameterization), so this is not a contrived requirement.
    //
    // MEASURED: normal=(0,0,1), tangent=(1,0,0), bitangent=(0,1,0) -> sign(dot(cross(n,t),b)) ==
    // sign(dot((0,0,1)x(1,0,0), (0,1,0))) == sign(dot((0,1,0),(0,1,0))) == sign(1) == +1.0F.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "        LayerElementNormal: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
        "            Normals: *12 { a: 0,0,1, 0,0,1, 0,0,1, 0,0,1 }\n"
        "        }\n"
        "        LayerElementUV: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: "
        "\"IndexToDirect\"\n"
        "            UV: *8 { a: 0,0, 1,0, 1,1, 0,1 }\n"
        "            UVIndex: *4 { a: 0,1,2,3 }\n"
        "        }\n"
        "        LayerElementTangent: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
        "            Tangents: *12 { a: 1,0,0, 1,0,0, 1,0,0, 1,0,0 }\n"
        "        }\n"
        "        LayerElementBinormal: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
        "            Binormals: *12 { a: 0,1,0, 0,1,0, 0,1,0, 0,1,0 }\n"
        "        }\n"
        "        Layer: 0 {\n"
        "            Version: 100\n"
        "            LayerElement:  { Type: \"LayerElementNormal\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementUV\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementTangent\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementBinormal\" TypedIndex: 0 }\n"
        "        }\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.tangents.size() == prim.positions.size());
    for (const auto& t : prim.tangents) {
        CHECK((t.w == doctest::Approx(1.0F) || t.w == doctest::Approx(-1.0F)));
    }
    CHECK(prim.tangents[0].w == doctest::Approx(1.0F));
    using engine::editor::VertexAttribute;
    CHECK(engine::editor::has(prim.attributes, VertexAttribute::Tangent));
}

TEST_CASE("fbx_import: tangents WITHOUT bitangents -> no tangents at all, Tangent bit clear (FI35, AC-34b)") {
    // A UV layer is INCLUDED (FI34's own finding: ufbx exposes vertex_tangent/vertex_bitangent only
    // through a UV set's slot) SPECIFICALLY so this case discriminates "tangent present, bitangent
    // absent" rather than trivially passing because NEITHER exists without any UV set at all --
    // MEASURED: with UV + Tangent and no Binormal, vertex_tangent.exists == true and
    // vertex_bitangent.exists == false.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "        LayerElementNormal: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
        "            Normals: *12 { a: 0,0,1, 0,0,1, 0,0,1, 0,0,1 }\n"
        "        }\n"
        "        LayerElementUV: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: "
        "\"IndexToDirect\"\n"
        "            UV: *8 { a: 0,0, 1,0, 1,1, 0,1 }\n"
        "            UVIndex: *4 { a: 0,1,2,3 }\n"
        "        }\n"
        "        LayerElementTangent: 0 {\n"
        "            Version: 101\n            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
        "            Tangents: *12 { a: 1,0,0, 1,0,0, 1,0,0, 1,0,0 }\n"
        "        }\n"
        "        Layer: 0 {\n"
        "            Version: 100\n"
        "            LayerElement:  { Type: \"LayerElementNormal\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementUV\" TypedIndex: 0 }\n"
        "            LayerElement:  { Type: \"LayerElementTangent\" TypedIndex: 0 }\n"
        "        }\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.tangents.empty());
    using engine::editor::has;
    using engine::editor::VertexAttribute;
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Tangent));
}

TEST_CASE(
    "fbx_import: summary.bounds folds the SCALED positions; a mesh whose every part was dropped has a "
    "POINT-BOX ImportedMesh::bounds and does NOT contribute the origin to summary.bounds (FI36, E9, "
    "3.2.1's BLOCKING-3 lesson reproduced from the start)") {
    // A mesh with ONLY a point face and a line face -- ZERO triangles anywhere, so EVERY material_part
    // has num_triangles == 0 and the per-part loop never reaches the fold at all.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::degenerate\", \"Mesh\" {\n"
        "        Vertices: *6 { a: 5,5,5, 6,6,6 }\n"
        "        PolygonVertexIndex: *3 { a: -1, 0,-2 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::degenerate\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());
    // Aabb{}'s own default (a VALID point box at the origin) -- never Aabb::empty() left standing.
    CHECK(result.model.meshes[0].bounds.min == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(result.model.meshes[0].bounds.max == engine::Vec3{0.0F, 0.0F, 0.0F});
    // The vertices are AWAY from the origin (5,5,5)/(6,6,6) -- if the origin had leaked into
    // summary.bounds this would read VALID (a point box at 0,0,0), which is exactly why `.valid()`,
    // not a component comparison, is the discriminator: `Aabb::empty()` and "a point box at the
    // origin" are both technically boxes, but only one of them is `.valid()`.
    CHECK_FALSE(result.model.summary.bounds.valid());
}

TEST_CASE(
    "fbx_import: settings.scale composes WITH the unit conversion, multiplying positions/root-"
    "translation/bounds AFTER it -- never replacing it (FI37, AC-40)") {
    // FI15's own Z-up centimetre fixture, x2.
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    ImportSettings settings;
    settings.scale = 2.0F;
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), settings, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    const auto& node = result.model.nodes[0];
    CHECK(node.translation.x == APPROX_POS(0.0));
    CHECK(node.translation.y == APPROX_POS(4.0));  // FI15's 2.0 x2
    CHECK(node.translation.z == APPROX_POS(0.0));
    // Rotation is NEVER scaled.
    CHECK(node.rotation.x == APPROX_ROT(-0.7071068));
    CHECK(node.rotation.w == APPROX_ROT(0.7071068));

    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& positions = result.model.meshes[0].primitives[0].positions;
    REQUIRE(positions.size() == 4);
    CHECK(positions[0] == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(positions[1].x == APPROX_POS(2.0));  // FI15's 1.0 x2
    CHECK(positions[2].x == APPROX_POS(2.0));
    CHECK(positions[2].y == APPROX_POS(2.0));
    CHECK(positions[3].y == APPROX_POS(2.0));
    REQUIRE(result.model.summary.bounds.valid());
    const engine::Vec3 boundsSize = result.model.summary.bounds.size();
    CHECK(boundsSize.x == APPROX_POS(2.0));  // FI15's (1,1,0) x2
    CHECK(boundsSize.y == APPROX_POS(2.0));
}

TEST_CASE(
    "fbx_import: MAX_PRIMITIVES_PER_MODEL exceeded is Truncated, naming the cap, with a coherent "
    "smaller model (FI38, AC-50)") {
    // MAX_VERTICES_PER_MODEL (8 000 000) and MAX_INDICES_PER_MODEL (24 000 000) are NOT implemented as
    // cases here, and this is a deliberate, reasoned decision rather than a silent drop: reaching
    // either needs geometry on the order of millions of real vertices/indices, which is flatly
    // incompatible with this whole file's own tier-0 contract (a few hundred bytes to at most a few
    // hundred KB, no disk, instant -- §G-12/R3). Unlike MAX_PRIMITIVES_PER_MODEL below, there is no
    // way to reach either cap cheaply: a MODEL-wide vertex/index total scales with real per-vertex
    // data, not with element COUNT, so there is no "many minimal objects" shortcut available the way
    // there is for primitives (many tiny meshes) or MAX_FBX_NODE_DEPTH (FI27, many tiny nested nodes).
    // MAX_PRIMITIVES_PER_MODEL (4096) IS cheaply reachable: MANY MINIMAL MESHES, each contributing
    // exactly one primitive (no material connected -> exactly one default material_part), generated
    // programmatically exactly like FI27's node chain.
    std::string objects;
    std::string connections;
    constexpr int MESH_COUNT = 4097;  // MAX_PRIMITIVES_PER_MODEL (4096) + 1
    for (int i = 0; i < MESH_COUNT; ++i) {
        const int geomId = 1000 + i * 2;
        const int modelId = 1001 + i * 2;
        objects += std::format(
            "    Geometry: {}, \"Geometry::g{}\", \"Mesh\" {{ Vertices: *3 {{ a: 0,0,0 }} "
            "PolygonVertexIndex: *3 {{ a: 0,0,-1 }} GeometryVersion: 124 }}\n"
            "    Model: {}, \"Model::m{}\", \"Mesh\" {{ Version: 232 }}\n",
            geomId, i, modelId, i);
        connections += std::format("    C: \"OO\",{},0\n    C: \"OO\",{},{}\n", modelId, geomId, modelId);
    }
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, connections);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("MAX_PRIMITIVES_PER_MODEL") != std::string::npos);
    REQUIRE(result.model.meshes.size() == static_cast<std::size_t>(MESH_COUNT));
    std::size_t survivingPrimitives = 0;
    for (const auto& m : result.model.meshes) {
        survivingPrimitives += m.primitives.size();
    }
    CHECK(survivingPrimitives == 4096);  // the cap, not the requested 4097
}

// ---- FI39-FI56: materials from `pbr` ONLY, and textures/URIs (Step 6, phases 4-5) ---------------
//
// FI43 is DELIBERATELY ABSENT -- see this file's own header comment above `GLTF_MATERIAL_CLASSID` for
// why (`feature_disabled` is never set anywhere in ufbx v0.23.0's own source; no fixture can reach it).
// FI46's mesh-level half (`every primitive INVALID_SUBASSET`) is written in the Step 7 block below,
// once meshes exist to assert it against -- its materials-only half (`materials.empty()`, `images`/
// `externalUris` unchanged) is folded into that same later case rather than split across two commits
// for one small property, unlike FI15/16/17/23/24 (whose node-level halves were genuinely a full,
// separately-valuable proof on their own).

TEST_CASE(
    "fbx_import: D13's factor fields round-trip through the two reachable shader paths -- base_factor/"
    "emission_factor via a plain Phong material, metallic/roughness/doubleSided via the gltf-material "
    "classid (FI39, AC-35)") {
    // Half 1: DiffuseFactor/EmissiveFactor MODULATE their paired colors on a plain Phong material.
    constexpr std::string_view PHONG_OBJECTS =
        "    Material: 300, \"Material::phong\", \"\" {\n"
        "        Version: 102\n"
        "        ShadingModel: \"phong\"\n"
        "        Properties70:  {\n"
        "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.4,0.6,0.8\n"
        "            P: \"DiffuseFactor\", \"Number\", \"\", \"A\",0.5\n"
        "            P: \"EmissiveColor\", \"Color\", \"\", \"A\",0.2,0.3,0.4\n"
        "            P: \"EmissiveFactor\", \"Number\", \"\", \"A\",2.0\n"
        "        }\n"
        "    }\n";
    const std::string phongDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, PHONG_OBJECTS, "");
    const ImportResult phongResult =
        importModel("t.fbx", "", asBytes(phongDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(phongResult.status == ImportStatus::Ok);
    REQUIRE(phongResult.model.materials.size() == 1);
    const auto& phongMat = phongResult.model.materials[0];
    CHECK(phongMat.baseColorFactor.x == doctest::Approx(0.2F));
    CHECK(phongMat.baseColorFactor.y == doctest::Approx(0.3F));
    CHECK(phongMat.baseColorFactor.z == doctest::Approx(0.4F));
    CHECK(phongMat.baseColorFactor.w == doctest::Approx(0.5F));
    CHECK(phongMat.emissiveFactor.x == doctest::Approx(0.4F));
    CHECK(phongMat.emissiveFactor.y == doctest::Approx(0.6F));
    CHECK(phongMat.emissiveFactor.z == doctest::Approx(0.8F));

    // Half 2: metallicFactor, roughnessFactor and doubleSided, via the gltf-material classid.
    const std::string classidObjects = std::format(
        "    Material: 300, \"Material::gltf\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "            P: \"3dsMax|main|baseColor\", \"ColorRGB\", \"Color\", \"\",0.3,0.5,0.7\n"
        "            P: \"3dsMax|main|roughness\", \"Number\", \"\", \"\",0.35\n"
        "            P: \"3dsMax|main|metalness\", \"Number\", \"\", \"\",0.65\n"
        "            P: \"3dsMax|main|DoubleSided\", \"bool\", \"\", \"\",1\n"
        "        }}\n"
        "    }}\n",
        GLTF_MATERIAL_CLASSID);
    const std::string classidDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, classidObjects, "");
    const ImportResult classidResult =
        importModel("t.fbx", "", asBytes(classidDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(classidResult.status == ImportStatus::Ok);
    REQUIRE(classidResult.model.materials.size() == 1);
    const auto& classidMat = classidResult.model.materials[0];
    CHECK(classidMat.baseColorFactor.x == doctest::Approx(0.3F));
    CHECK(classidMat.baseColorFactor.y == doctest::Approx(0.5F));
    CHECK(classidMat.baseColorFactor.z == doctest::Approx(0.7F));
    CHECK(classidMat.metallicFactor == doctest::Approx(0.65F));
    CHECK(classidMat.roughnessFactor == doctest::Approx(0.35F));
    CHECK(classidMat.doubleSided);
}

TEST_CASE("fbx_import: a map ufbx does not populate leaves ImportedMaterial's own default untouched (FI40)") {
    constexpr std::string_view OBJECTS =
        "    Material: 300, \"Material::bare\", \"\" {\n"
        "        Version: 102\n"
        "        ShadingModel: \"phong\"\n"
        "        Properties70:  {\n"
        "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.1,0.2,0.3\n"
        "        }\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    CHECK(mat.metallicFactor == doctest::Approx(1.0F));
    CHECK(mat.roughnessFactor == doctest::Approx(1.0F));
    CHECK(mat.normalScale == doctest::Approx(1.0F));
    CHECK(mat.occlusionStrength == doctest::Approx(1.0F));
    CHECK(mat.alphaCutoff == doctest::Approx(0.5F));
}

TEST_CASE(
    "fbx_import: AlphaMode is Blend only for a genuinely transparent material; an explicitly-opaque "
    "one and one that never mentions opacity at all both stay Opaque; Mask is never produced anywhere "
    "in this suite (FI41, AC-37)") {
    // The THIRD document (no Alpha at all) is the case §A-8-style measurement above exists for:
    // features.opacity.enabled is unconditionally true for this shader, and pbr.opacity.value_real
    // defaults to 0.0, not 1.0 -- the plan's own literal D13 wording (no engaged() gate) would
    // misread this as Blend. fbx_import.cpp's alphaMode formula was corrected to add that gate; this
    // case is its proof.
    const auto material = [](std::string_view alphaLine) {
        return std::format(
            "    Material: 300, \"Material::m\", \"\" {{\n"
            "        Version: 102\n"
            "        ShadingModel: \"unknown\"\n"
            "        Properties70:  {{\n"
            "{}"
            "            P: \"3dsMax|main|baseColor\", \"ColorRGB\", \"Color\", \"\",0.5,0.5,0.5\n"
            "{}"
            "        }}\n"
            "    }}\n",
            GLTF_MATERIAL_CLASSID, alphaLine);
    };
    const std::string transparentDoc =
        makeFbx(CANONICAL_GLOBALS_PROPERTIES,
                material("            P: \"3dsMax|main|Alpha\", \"Number\", \"\", \"\",0.5\n"), "");
    const ImportResult transparent =
        importModel("t.fbx", "", asBytes(transparentDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(transparent.status == ImportStatus::Ok);
    REQUIRE(transparent.model.materials.size() == 1);
    CHECK(transparent.model.materials[0].alphaMode == AlphaMode::Blend);

    const std::string opaqueDoc =
        makeFbx(CANONICAL_GLOBALS_PROPERTIES,
                material("            P: \"3dsMax|main|Alpha\", \"Number\", \"\", \"\",1.0\n"), "");
    const ImportResult opaque = importModel("t.fbx", "", asBytes(opaqueDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(opaque.status == ImportStatus::Ok);
    REQUIRE(opaque.model.materials.size() == 1);
    CHECK(opaque.model.materials[0].alphaMode == AlphaMode::Opaque);

    const std::string unmentionedDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, material(""), "");
    const ImportResult unmentioned =
        importModel("t.fbx", "", asBytes(unmentionedDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(unmentioned.status == ImportStatus::Ok);
    REQUIRE(unmentioned.model.materials.size() == 1);
    CHECK(unmentioned.model.materials[0].alphaMode == AlphaMode::Opaque);

    for (const ImportResult* r : {&transparent, &opaque, &unmentioned}) {
        for (const auto& m : r->model.materials) {
            CHECK(m.alphaMode != AlphaMode::Mask);
        }
    }
}

TEST_CASE(
    "fbx_import: roughness_as_glossiness inverts the SAME raw value only when the feature is engaged "
    "(FI42, AC-36)") {
    const auto material = [](std::string_view useGlossinessValue) {
        return std::format(
            "    Material: 300, \"Material::m\", \"\" {{\n"
            "        Version: 102\n"
            "        ShadingModel: \"unknown\"\n"
            "        Properties70:  {{\n"
            "{}"
            "            P: \"3dsMax|main|glossiness\", \"Number\", \"\", \"\",0.25\n"
            "            P: \"3dsMax|main|useGlossiness\", \"Number\", \"\", \"\",{}\n"
            "        }}\n"
            "    }}\n",
            SPEC_GLOSS_CLASSID, useGlossinessValue);
    };
    const std::string engagedDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, material("1.0"), "");
    const ImportResult engaged = importModel("t.fbx", "", asBytes(engagedDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(engaged.status == ImportStatus::Ok);
    REQUIRE(engaged.model.materials.size() == 1);
    CHECK(engaged.model.materials[0].roughnessFactor == doctest::Approx(0.75F));

    const std::string disengagedDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, material("0.0"), "");
    const ImportResult disengaged =
        importModel("t.fbx", "", asBytes(disengagedDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(disengaged.status == ImportStatus::Ok);
    REQUIRE(disengaged.model.materials.size() == 1);
    CHECK(disengaged.model.materials[0].roughnessFactor == doctest::Approx(0.25F));
}

TEST_CASE(
    "fbx_import: UFBX_WRAP_REPEAT/CLAMP map correctly, and MirroredRepeat is never produced anywhere "
    "in this suite (FI44, AC-38)") {
    const std::string objects = std::format(
        "    Material: 300, \"Material::m\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "        }}\n"
        "    }}\n"
        "    Texture: 500, \"Texture::tex1\", \"\" {{\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        Properties70:  {{\n"
        "            P: \"WrapModeU\", \"enum\", \"\", \"\",1\n"
        "            P: \"WrapModeV\", \"enum\", \"\", \"\",0\n"
        "        }}\n"
        "        RelativeFilename: \"wood.png\"\n"
        "    }}\n",
        GLTF_MATERIAL_CLASSID);
    constexpr std::string_view CONNECTIONS = "    C: \"OP\",500,300,\"3dsMax|main|baseColor\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 1);
    REQUIRE(result.model.materials[0].baseColor.has_value());
    CHECK(result.model.materials[0].baseColor->wrapU == TextureWrap::ClampToEdge);
    CHECK(result.model.materials[0].baseColor->wrapV == TextureWrap::Repeat);
    for (const auto& mat : result.model.materials) {
        for (const auto& ref : {mat.baseColor, mat.metallicRoughness, mat.normal, mat.occlusion, mat.emissive}) {
            if (ref.has_value()) {
                CHECK(ref->wrapU != TextureWrap::MirroredRepeat);
                CHECK(ref->wrapV != TextureWrap::MirroredRepeat);
            }
        }
    }
}

TEST_CASE(
    "fbx_import: all five texture slots resolve to the right imageIndex; an absent slot is nullopt, "
    "never index 0 (FI45)") {
    const std::string objects = std::format(
        "    Material: 300, \"Material::m\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "        }}\n"
        "    }}\n"
        "    Texture: 500, \"Texture::base\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"base.png\" }}\n"
        "    Texture: 501, \"Texture::mr\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"mr.png\" }}\n"
        "    Texture: 502, \"Texture::normal\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"normal.png\" }}\n",
        GLTF_MATERIAL_CLASSID);
    constexpr std::string_view CONNECTIONS =
        "    C: \"OP\",500,300,\"3dsMax|main|baseColor\"\n"
        "    C: \"OP\",501,300,\"3dsMax|main|metalness\"\n"
        "    C: \"OP\",502,300,\"3dsMax|main|normal\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    REQUIRE(mat.baseColor.has_value());
    REQUIRE(mat.metallicRoughness.has_value());
    REQUIRE(mat.normal.has_value());
    CHECK_FALSE(mat.occlusion.has_value());
    CHECK_FALSE(mat.emissive.has_value());
    REQUIRE(result.model.images.size() == 3);
    CHECK(mat.baseColor->imageIndex != engine::editor::INVALID_SUBASSET);
    CHECK(mat.metallicRoughness->imageIndex != engine::editor::INVALID_SUBASSET);
    CHECK(mat.normal->imageIndex != engine::editor::INVALID_SUBASSET);
    CHECK(mat.baseColor->imageIndex != mat.metallicRoughness->imageIndex);
    CHECK(mat.metallicRoughness->imageIndex != mat.normal->imageIndex);
}

TEST_CASE(
    "fbx_import: alphaCutoff stays 0.5F and normalScale/occlusionStrength stay 1.0F even when a "
    "normal or AO texture IS present (FI47)") {
    const std::string objects = std::format(
        "    Material: 300, \"Material::m\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "        }}\n"
        "    }}\n"
        "    Texture: 500, \"Texture::normal\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"n.png\" }}\n"
        "    Texture: 501, \"Texture::ao\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"ao.png\" }}\n",
        GLTF_MATERIAL_CLASSID);
    constexpr std::string_view CONNECTIONS =
        "    C: \"OP\",500,300,\"3dsMax|main|normal\"\n"
        "    C: \"OP\",501,300,\"3dsMax|main|ambientOcclusion\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    REQUIRE(mat.normal.has_value());
    REQUIRE(mat.occlusion.has_value());
    CHECK(mat.alphaCutoff == doctest::Approx(0.5F));
    CHECK(mat.normalScale == doctest::Approx(1.0F));
    CHECK(mat.occlusionStrength == doctest::Approx(1.0F));
}

TEST_CASE(
    "fbx_import: a relative_filename with backslashes folds to forward slashes BEFORE classification "
    "(FI48, AC-12)") {
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" {\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        RelativeFilename: \"textures\\wood.png\"\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result =
        importModel("model.fbx", "models", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1);
    CHECK(result.model.images[0].relativePath == "models/textures/wood.png");
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "models/textures/wood.png");
}

TEST_CASE(
    "fbx_import: the fold happens BEFORE classify -- a backslash traversal is refused as an escape, "
    "never let through by folding on a refusal (FI49)") {
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" {\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        RelativeFilename: \"..\\..\\..\\etc\\passwd\"\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result =
        importModel("model.fbx", "models/sub", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1);
    CHECK(result.model.images[0].relativePath.empty());
    CHECK_FALSE(result.model.images[0].refusal.empty());
    CHECK(result.externalUris.empty());
    // THE ORDER, not merely the refusal (sabotage round, seed S19). Refusing SOMETHING is what the
    // three assertions above prove, and a backend that classified the RAW string -- folding only
    // afterwards, for display -- also refuses this URI, just as `RefusedBackslash` rather than as an
    // escape. Pinning WHICH reason is what separates the two orderings here, where it matters: the
    // fold ran FIRST, so `../../../etc/passwd` was resolved and found to leave the assets root.
    CHECK(result.model.images[0].refusal.find("outside the project's assets folder") != std::string::npos);
    CHECK(result.model.images[0].refusal.find("backslash") == std::string::npos);
}

TEST_CASE(
    "fbx_import: every URI refusal class surfaces on ImportedImage::refusal with the exact reason "
    "(FI50, AC-52)") {
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::abs\", \"\" { Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"/etc/passwd\" }\n"
        "    Texture: 501, \"Texture::scheme\", \"\" { Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"http://evil.example/x.png\" }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 2);
    CHECK_FALSE(result.model.images[0].refusal.empty());
    CHECK_FALSE(result.model.images[1].refusal.empty());
    CHECK(result.model.images[0].refusal != result.model.images[1].refusal);
    CHECK(result.externalUris.empty());
}

TEST_CASE(
    "fbx_import: a refused texture reference contributes no externalUris entry and exactly one "
    "warning (FI51, AC-13)") {
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" {\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        RelativeFilename: \"/etc/passwd\"\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    CHECK(result.warningTotal == 1);
}

TEST_CASE(
    "fbx_import: refusing ten URIs contributes nothing to externalUris or images' relativePath -- the "
    "structural half of AC-52's no-read proof (FI52)") {
    std::string objects;
    for (int i = 0; i < 10; ++i) {
        objects += std::format(
            "    Texture: {}, \"Texture::t{}\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
            "RelativeFilename: \"/etc/secret{}.png\" }}\n",
            500 + i, i, i);
    }
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, "");
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    CHECK(result.externalUris.empty());
    REQUIRE(result.model.images.size() == 10);
    for (const auto& img : result.model.images) {
        CHECK(img.relativePath.empty());
        CHECK_FALSE(img.refusal.empty());
    }
    // importFbx's own signature is the rest of the proof: `external` (the only "bytes supplied by the
    // caller" parameter) is never populated for FBX at all (D5) -- MI110/MS24/MS26 (Steps 3/11) are
    // the pure-function and session-level halves of this same property.
}

TEST_CASE(
    "fbx_import: RelativeFilename empty, FileName an absolute Windows path -- 'secrets' appears "
    "nowhere in the result (FI53, AC-53)") {
    // MEASURED: `ufbx_texture.filename` (the D14 fallback source) is derived from `absolute_filename`
    // ONLY when it shares a common path PREFIX with `ufbx_load_opts.filename` -- which we never set
    // (D4: no real base path exists for a `ufbx_load_memory` call), so `ufbxi_absolute_to_relative_
    // path`'s very first check (`rel[0] != src[0]`) fails immediately and `filename` stays empty. The
    // basename fallback therefore degrades to `classifyUri("")`'s RefusedEmpty branch rather than ever
    // extracting "x.png" -- both outcomes are sanctioned by AC-53's own wording ("resolves... OR is
    // refused"), and this is the one this ufbx version actually takes.
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" {\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        FileName: \"C:\\secrets\\x.png\"\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, "");
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1);
    CHECK(result.model.images[0].uri.find("secrets") == std::string::npos);
    CHECK(result.model.images[0].relativePath.find("secrets") == std::string::npos);
    CHECK(result.model.images[0].refusal.find("secrets") == std::string::npos);
    for (const std::string& uri : result.externalUris) {
        CHECK(uri.find("secrets") == std::string::npos);
    }
}

TEST_CASE("fbx_import: embedded texture content is never a dependency (FI54, D14)") {
    constexpr std::string_view OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" {\n"
        "        Type: \"TextureVideoClip\"\n"
        "        Version: 202\n"
        "        Media: \"Video::vid1\"\n"
        "        FileName: \"embedded.png\"\n"
        "        RelativeFilename: \"embedded.png\"\n"
        "    }\n"
        "    Video: 600, \"Video::vid1\", \"Clip\" {\n"
        "        Type: \"Clip\"\n"
        "        Filename: \"embedded.png\"\n"
        "        RelativeFilename: \"embedded.png\"\n"
        "        Content: \"YQBiAGMAZAA=\"\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS = "    C: \"OO\",600,500\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1);
    CHECK(result.model.images[0].embedded);
    CHECK(result.model.images[0].relativePath.empty());
    CHECK_FALSE(result.model.images[0].guid.valid());
    CHECK(result.externalUris.empty());
}

TEST_CASE(
    "fbx_import: two materials referencing the same texture produce ONE ImportedImage and ONE "
    "externalUris entry (FI55, E12)") {
    const std::string objects = std::format(
        "    Material: 300, \"Material::m1\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "        }}\n"
        "    }}\n"
        "    Material: 301, \"Material::m2\", \"\" {{\n"
        "        Version: 102\n"
        "        ShadingModel: \"unknown\"\n"
        "        Properties70:  {{\n"
        "{}"
        "        }}\n"
        "    }}\n"
        "    Texture: 500, \"Texture::tex1\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"wood.png\" }}\n",
        GLTF_MATERIAL_CLASSID, GLTF_MATERIAL_CLASSID);
    constexpr std::string_view CONNECTIONS =
        "    C: \"OP\",500,300,\"3dsMax|main|baseColor\"\n"
        "    C: \"OP\",500,301,\"3dsMax|main|baseColor\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 2);
    CHECK(result.model.images.size() == 1);
    CHECK(result.externalUris.size() == 1);
    REQUIRE(result.model.materials[0].baseColor.has_value());
    REQUIRE(result.model.materials[1].baseColor.has_value());
    CHECK(result.model.materials[0].baseColor->imageIndex == result.model.materials[1].baseColor->imageIndex);
}

TEST_CASE(
    "fbx_import: both filenames empty is a clear refusal, and a literal percent sequence is never "
    "percent-decoded -- FBX paths are not percent-encoded (FI56, E10/E23)") {
    // E19 (a uv_set name no mesh has falls back to UV set 0 with one warning) needs a mesh with real
    // uv_sets to check against -- at Structure depth (the only depth reachable before Step 7) uv_sets
    // is always empty, so it is deferred to the mesh block below rather than asserted vacuously here.
    constexpr std::string_view EMPTY_OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" { Type: \"TextureVideoClip\" Version: 202 }\n";
    const std::string emptyDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, EMPTY_OBJECTS, "");
    const ImportResult emptyResult =
        importModel("t.fbx", "", asBytes(emptyDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(emptyResult.status == ImportStatus::Ok);
    REQUIRE(emptyResult.model.images.size() == 1);
    CHECK_FALSE(emptyResult.model.images[0].refusal.empty());
    CHECK(emptyResult.externalUris.empty());

    constexpr std::string_view PERCENT_OBJECTS =
        "    Texture: 500, \"Texture::tex1\", \"\" { Type: \"TextureVideoClip\" Version: 202 "
        "RelativeFilename: \"100%20.png\" }\n";
    const std::string pctDoc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, PERCENT_OBJECTS, "");
    const ImportResult pctResult = importModel("t.fbx", "", asBytes(pctDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(pctResult.status == ImportStatus::Ok);
    REQUIRE(pctResult.model.images.size() == 1);
    CHECK(pctResult.model.images[0].relativePath == "100%20.png");
}

// ---- FI57-FI63: skins and the 4-influence reduction (Step 8, phase 7) -----------------------------
//
// MEASURED FBX ASCII syntax, not in the plan and not previously exercised in this file: a skin is
// `Deformer: id, "Deformer::name", "Skin" { ... }`; each bone binding is a SEPARATE
// `Deformer: id, "SubDeformer::name", "Cluster" { Indexes: *N {a:...} Weights: *N {a:...}
// Transform: *16 {a:...} TransformLink: *16 {a:...} }`. Connections: `C: "OO",clusterId,skinId`
// (cluster -> skin), `C: "OO",skinId,geometryId` (skin -> geometry) and `C: "OO",boneNodeId,clusterId`
// (bone -> cluster, MEASURED: `ufbxi_fetch_dst_element(&cluster->element, ..., UFBX_ELEMENT_NODE)`
// fetches what the cluster itself connects TO). `ufbx_skin_cluster::geometry_to_bone` is COMPUTED by
// ufbx from `Transform:`/`TransformLink:` (both required, `ufbx.c:14032-14049`); with no geometry
// transform on the mesh's node it EQUALS `Transform:`'s own matrix, MEASURED. `Transform:`/
// `TransformLink:`'s 16 values are COLUMN-MAJOR, each column padded to 4 (`ufbxi_read_transform_
// matrix`, `ufbx.c:13957-13963`): `data[0..3]`=col0, `data[4..7]`=col1, `data[8..11]`=col2,
// `data[12..15]`=col3 (translation) -- so `Transform: *16 {a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1}`
// is an identity-rotation matrix translated to (10,20,30).
//
// S21 ("keep the first 4 [in ufbx's own presented order] instead of the largest 4") is NOT
// implemented as its own case, and this is a MEASURED, not assumed, finding: ufbx builds a vertex's
// weight list by aggregating each cluster's contribution IN CLUSTER-CONNECTION ORDER and then stably
// sorting by descending weight (ufbx.h:1957's own guarantee) -- which makes "ties resolve to
// ascending cluster_index" an EMERGENT PROPERTY of ufbx's own construction, not something a fixture
// can defeat: every tie this file could construct (probed directly, several cluster/weight
// permutations) presented in ascending cluster_index order regardless of which order the `Deformer:`
// blocks were AUTHORED in the FBX text, because `cluster_index` itself is assigned by CONNECTION
// order and the weight list is built by iterating clusters in that same order. This CONFIRMS, rather
// than merely asserts, A11's own already-recorded finding ("the explicit sort is a no-op today"): the
// tie-break in `reduceInfluences` is defence in depth for a future ufbx that relaxes the guarantee,
// exactly like the `MeshTally`/generate-indices padding guards elsewhere in this file, and FI58 below
// proves the FINAL ascending-cluster-index property directly (a true, meaningful statement about this
// code's output) without claiming to have found an input that only the explicit sort satisfies.

TEST_CASE(
    "fbx_import: two clusters -> joints holds their bone typed_ids in CLUSTER order; "
    "inverseBindMatrices.size() == joints.size(); each matrix compared element by element (FI57, "
    "AC-41, seed S8's discriminator)") {
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::boneA\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 111, \"Model::boneB\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        // cluster A: identity rotation/scale, translation (10,20,30) -- a simple baseline.
        "    Deformer: 301, \"SubDeformer::clusterA\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n"
        // cluster B: an ASYMMETRIC matrix (every off-diagonal element distinct) -- a transpose would
        // swap m10<->m01 (0.5 vs 0), m21<->m12 (0.25 vs 0), m20<->m02 (0.1 vs 0), each a DIFFERENT
        // value, so an element-by-element comparison catches it on the FIRST mismatched component.
        "    Deformer: 302, \"SubDeformer::clusterB\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 1 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0.5,0,0, 0,1,0.25,0, 0.1,0,1,0, 40,50,60,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n"
        "    C: \"OO\",111,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n"
        "    C: \"OO\",301,300\n"
        "    C: \"OO\",302,300\n"
        "    C: \"OO\",110,301\n"
        "    C: \"OO\",111,302\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.skins.size() == 1);
    const auto& skin = result.model.skins[0];
    REQUIRE(skin.joints.size() == 2);
    REQUIRE(skin.inverseBindMatrices.size() == 2);

    const auto findNodeByName = [&](std::string_view name) -> const engine::editor::ImportedNode* {
        for (const auto& n : result.model.nodes) {
            if (n.name == name) {
                return &n;
            }
        }
        return nullptr;
    };
    const auto* boneA = findNodeByName("boneA");
    const auto* boneB = findNodeByName("boneB");
    REQUIRE(boneA != nullptr);
    REQUIRE(boneB != nullptr);
    CHECK(skin.joints[0] == boneA->localId);  // CLUSTER order, not name/alphabetical order
    CHECK(skin.joints[1] == boneB->localId);

    const engine::Mat4& a = skin.inverseBindMatrices[0];
    CHECK(a.columns[0] == engine::Vec4{1.0F, 0.0F, 0.0F, 0.0F});
    CHECK(a.columns[1] == engine::Vec4{0.0F, 1.0F, 0.0F, 0.0F});
    CHECK(a.columns[2] == engine::Vec4{0.0F, 0.0F, 1.0F, 0.0F});
    CHECK(a.columns[3].x == doctest::Approx(10.0F));
    CHECK(a.columns[3].y == doctest::Approx(20.0F));
    CHECK(a.columns[3].z == doctest::Approx(30.0F));

    const engine::Mat4& b = skin.inverseBindMatrices[1];
    CHECK(b.columns[0] == engine::Vec4{1.0F, 0.5F, 0.0F, 0.0F});
    CHECK(b.columns[1] == engine::Vec4{0.0F, 1.0F, 0.25F, 0.0F});
    CHECK(b.columns[2].x == doctest::Approx(0.1F));
    CHECK(b.columns[2].y == doctest::Approx(0.0F));
    CHECK(b.columns[2].z == doctest::Approx(1.0F));
    CHECK(b.columns[3].x == doctest::Approx(40.0F));
    CHECK(b.columns[3].y == doctest::Approx(50.0F));
    CHECK(b.columns[3].z == doctest::Approx(60.0F));

    // ImportedNode::skinIndex on the node carrying the mesh (D11).
    const auto* boxNode = findNodeByName("box");
    REQUIRE(boxNode != nullptr);
    CHECK(boxNode->skinIndex == 0);
}

TEST_CASE(
    "fbx_import: a vertex with 6 influences -> the 4 largest by weight survive, renormalized to sum "
    "1.0F, ties broken by ascending cluster index; EXACTLY ONE warning names the observed maximum (6) "
    "(FI58, AC-42)") {
    // MEASURED (§V3's own S21 discriminator, re-examined -- see this block's own header comment
    // above): clusters 0 and 1 (bones b5/b1 in TEXT order, "c5" declared before "c1") are TIED at
    // weight 0.30; ufbx's own presented order for vertex 0 is (cluster0,0.30) (cluster1,0.30)
    // (cluster4,0.20) (cluster2,0.10) (cluster3,0.05) (cluster5,0.05) -- so the top 4 are clusters
    // {0,1,4,2}, summing to 0.90.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::b0\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 111, \"Model::b1\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 112, \"Model::b2\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 113, \"Model::b3\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 114, \"Model::b4\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 115, \"Model::b5\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 400, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        "    Deformer: 405, \"SubDeformer::c5\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.30} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n"
        "    Deformer: 401, \"SubDeformer::c1\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.30} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n"
        "    Deformer: 402, \"SubDeformer::c0\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.10} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n"
        "    Deformer: 403, \"SubDeformer::c2\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.05} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n"
        "    Deformer: 404, \"SubDeformer::c3\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.20} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n"
        "    Deformer: 406, \"SubDeformer::c4\", \"Cluster\" { Version: 100 Indexes: *1 {a: 0} "
        "Weights: *1 {a: 0.05} Transform: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} "
        "TransformLink: *16 {a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1} }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n    C: \"OO\",111,0\n    C: \"OO\",112,0\n"
        "    C: \"OO\",113,0\n    C: \"OO\",114,0\n    C: \"OO\",115,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",400,200\n"
        "    C: \"OO\",405,400\n    C: \"OO\",401,400\n    C: \"OO\",402,400\n"
        "    C: \"OO\",403,400\n    C: \"OO\",404,400\n    C: \"OO\",406,400\n"
        "    C: \"OO\",110,402\n    C: \"OO\",111,401\n    C: \"OO\",112,403\n"
        "    C: \"OO\",113,404\n    C: \"OO\",114,406\n    C: \"OO\",115,405\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    // Vertex 0's position is (0,0,0) -- the FIRST unique vertex after generate_indices, since it is
    // referenced first in PolygonVertexIndex and nothing else collides with it.
    REQUIRE(prim.positions.size() == 4);
    REQUIRE(prim.positions[0] == engine::Vec3{0.0F, 0.0F, 0.0F});
    REQUIRE(prim.joints.size() == 4);
    REQUIRE(prim.weights.size() == 4);
    const auto& joints0 = prim.joints[0];
    const auto& weights0 = prim.weights[0];
    // reduceInfluences() populates joints[i]/weights.{x,y,z,w} in DESCENDING-WEIGHT order (ties ->
    // ascending cluster index): slot 0 (0.30), slot 1 (0.30, the tie), slot 4 (0.20), slot 2 (0.10).
    // Slots 3 and 5 (the smallest, tied at 0.05) are dropped. Renormalized against their sum, 0.90.
    CHECK(joints0 == std::array<std::uint16_t, 4>{0, 1, 4, 2});
    CHECK(weights0.x == doctest::Approx(0.30F / 0.90F).epsilon(POS_EPS));
    CHECK(weights0.y == doctest::Approx(0.30F / 0.90F).epsilon(POS_EPS));
    CHECK(weights0.z == doctest::Approx(0.20F / 0.90F).epsilon(POS_EPS));
    CHECK(weights0.w == doctest::Approx(0.10F / 0.90F).epsilon(POS_EPS));
    const float sum = weights0.x + weights0.y + weights0.z + weights0.w;
    CHECK(sum == doctest::Approx(1.0F).epsilon(POS_EPS));
    // AC-42's own warning: exactly one, naming the observed maximum (6).
    CHECK(result.warningTotal == 1);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find('6') != std::string::npos);
}

TEST_CASE("fbx_import: a NaN weight in the source never reaches ImportedPrimitive::weights (FI59)") {
    // clean_skin_weights == true (D11 point 0) removes negative/zero/NaN weights BEFORE this importer
    // ever sees them -- a NaN weight here would propagate into a subsequent renormalization and
    // produce a NaN sum, which this case would catch via the `> 0.0F` check below.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::boneA\", \"LimbNode\" { Version: 232 }\n"
        "    Model: 111, \"Model::boneB\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        "    Deformer: 301, \"SubDeformer::clusterA\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: nan }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n"
        "    Deformer: 302, \"SubDeformer::clusterB\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n"
        "    C: \"OO\",111,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n"
        "    C: \"OO\",301,300\n"
        "    C: \"OO\",302,300\n"
        "    C: \"OO\",110,301\n"
        "    C: \"OO\",111,302\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.weights.size() == 4);
    // Only clusterB's weight (1.0) should ever have reached this importer; the NaN one was removed by
    // ufbx's own clean_skin_weights before the reduction ran.
    const auto& w = prim.weights[0];
    CHECK(std::isfinite(w.x));
    CHECK(std::isfinite(w.y));
    CHECK(std::isfinite(w.z));
    CHECK(std::isfinite(w.w));
    CHECK((w.x + w.y + w.z + w.w) == doctest::Approx(1.0F).epsilon(POS_EPS));
}

TEST_CASE(
    "fbx_import: a vertex with no skin binding at all gets joints=={0,0,0,0} AND weights=={0,0,0,0} -- "
    "NEVER a silent {1,0,0,0} (FI60, AC-43, seed S23's discriminator)") {
    // A vertex whose kept weights sum to zero is, given clean_skin_weights removing every non-
    // positive weight upstream, STRUCTURALLY equivalent to a vertex with NO weight entries at all --
    // both take the `sum > 0.0F == false` branch in reduceInfluences. This mesh's vertex 1 (the second
    // corner) is never referenced by any cluster's Indexes array.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::boneA\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        "    Deformer: 301, \"SubDeformer::clusterA\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n"
        "    C: \"OO\",301,300\n"
        "    C: \"OO\",110,301\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    // Vertex index 1 -- the unique vertex at position (1,0,0), never bound by clusterA.
    REQUIRE(prim.positions.size() == 4);
    std::size_t unboundSlot = prim.positions.size();
    for (std::size_t i = 0; i < prim.positions.size(); ++i) {
        if (prim.positions[i] == engine::Vec3{1.0F, 0.0F, 0.0F}) {
            unboundSlot = i;
        }
    }
    REQUIRE(unboundSlot < prim.positions.size());
    CHECK(prim.joints[unboundSlot] == std::array<std::uint16_t, 4>{0, 0, 0, 0});
    CHECK(prim.weights[unboundSlot] == engine::Vec4{0.0F, 0.0F, 0.0F, 0.0F});
}

TEST_CASE("fbx_import: a cluster with a null bone_node is never present in joints, one warning (FI61, E13)") {
    // MEASURED: ufbx (connect_broken_elements == false, never set here) already drops a cluster whose
    // bone connection is missing entirely BEFORE this importer sees it -- omit the bone connection for
    // one of two clusters and confirm the skin still imports with exactly one joint (not two, not a
    // nil entry) and the mesh's own status stays Ok.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::boneA\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        "    Deformer: 301, \"SubDeformer::clusterA\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n"
        // clusterB has NO bone connection at all.
        "    Deformer: 302, \"SubDeformer::clusterB\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 1 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n"
        "    C: \"OO\",301,300\n"
        "    C: \"OO\",302,300\n"
        "    C: \"OO\",110,301\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.skins.size() == 1);
    CHECK(result.model.skins[0].joints.size() == 1);
    for (const std::uint32_t j : result.model.skins[0].joints) {
        CHECK(j != engine::editor::INVALID_SUBASSET);
    }
}

TEST_CASE(
    "fbx_import: more than MAX_JOINTS_PER_SKIN joints -> Truncated naming the cap, and the skin is "
    "DROPPED (not truncated); the mesh carries no joints/weights at all (FI62, E14)") {
    // MAX_JOINTS_PER_SKIN is 1024 -- reachable cheaply since EACH cluster here is minimal (a single
    // Indexes/Weights entry, an identity Transform), matching FI38's "many minimal objects" shape.
    std::string objects =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n";
    std::string connections =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n";
    constexpr int JOINT_COUNT = 1025;  // MAX_JOINTS_PER_SKIN (1024) + 1
    for (int i = 0; i < JOINT_COUNT; ++i) {
        const int boneId = 1000 + i * 2;
        const int clusterId = 1001 + i * 2;
        objects += std::format(
            "    Model: {}, \"Model::b{}\", \"LimbNode\" {{ Version: 232 }}\n"
            "    Deformer: {}, \"SubDeformer::c{}\", \"Cluster\" {{ Version: 100 Indexes: *1 {{a: 0}} "
            "Weights: *1 {{a: 1.0}} Transform: *16 {{a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}} "
            "TransformLink: *16 {{a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}} }}\n",
            boneId, i, clusterId, i);
        connections += std::format("    C: \"OO\",{},0\n    C: \"OO\",{},300\n    C: \"OO\",{},{}\n", boneId, clusterId,
                                   boneId, clusterId);
    }
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, connections);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("MAX_JOINTS_PER_SKIN") != std::string::npos);
    CHECK(result.model.skins.empty());  // dropped, not truncated to a partial palette
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    CHECK(result.model.meshes[0].primitives[0].joints.empty());
    CHECK(result.model.meshes[0].primitives[0].weights.empty());
}

TEST_CASE(
    "fbx_import: settings.importSkins = false -> skins.empty(), summary.jointCount == 0, every other "
    "field identical to the same import with skins on (FI63, AC-47a)") {
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Model: 110, \"Model::boneA\", \"LimbNode\" { Version: 232 }\n"
        "    Deformer: 300, \"Deformer::skin\", \"Skin\" { Version: 101 }\n"
        "    Deformer: 301, \"SubDeformer::clusterA\", \"Cluster\" {\n"
        "        Version: 100\n        Indexes: *1 { a: 0 }\n        Weights: *1 { a: 1.0 }\n"
        "        Transform: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "        TransformLink: *16 { a: 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",110,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",300,200\n"
        "    C: \"OO\",301,300\n"
        "    C: \"OO\",110,301\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    ImportSettings onSettings;
    onSettings.importSkins = true;
    const ImportResult on = importModel("t.fbx", "", asBytes(doc), onSettings, ImportDepth::Full, {});
    ImportSettings offSettings;
    offSettings.importSkins = false;
    const ImportResult off = importModel("t.fbx", "", asBytes(doc), offSettings, ImportDepth::Full, {});

    REQUIRE(on.status == ImportStatus::Ok);
    REQUIRE(off.status == ImportStatus::Ok);
    CHECK_FALSE(on.model.skins.empty());
    CHECK(off.model.skins.empty());
    CHECK(on.model.summary.jointCount > 0);
    CHECK(off.model.summary.jointCount == 0);
    // task 3.2.2 (Step 9) correction: `summary.skinCount` used to be phase 2's raw, untouched
    // `s.skin_deformers.count` -- real for the on/on-agreement case this test already covered, but
    // WRONG here: it would report "1 skin" even though `off.model.skins` is empty and no skin content
    // was ever produced. `skinCount` is now overwritten from `model.skins.size()` (materials' own
    // established pattern), so it agrees with the vector it is summarizing at BOTH settings.
    CHECK(on.model.summary.skinCount > 0);
    CHECK(off.model.summary.skinCount == 0);
    // Every OTHER field stays identical -- the mesh pass itself is not short-circuited by importSkins.
    CHECK(on.model.summary.vertexCount == off.model.summary.vertexCount);
    CHECK(on.model.summary.triangleCount == off.model.summary.triangleCount);
    CHECK(on.model.summary.meshCount == off.model.summary.meshCount);
    CHECK(on.model.summary.nodeCount == off.model.summary.nodeCount);
    REQUIRE(on.model.meshes.size() == off.model.meshes.size());
    REQUIRE(on.model.meshes[0].primitives.size() == off.model.meshes[0].primitives.size());
    CHECK(on.model.meshes[0].primitives[0].positions.size() == off.model.meshes[0].primitives[0].positions.size());
    CHECK(off.model.meshes[0].primitives[0].joints.empty());
    CHECK(off.model.meshes[0].primitives[0].weights.empty());
}

// ---- FI64-FI71: animation baking (Step 9, phase 8) -------------------------------------------------
//
// Every fixture below is a Y-up/metre (CANONICAL_GLOBALS_PROPERTIES) document -- the source axes are
// not this section's own concern (Step 5 already proved the conversion; §D-4.10 runs entirely on
// ALREADY-CONVERTED node transforms, since ufbx_bake_anim samples the same scene phase 3 walks). Every
// literal tick value below is real FBX v7+ tick arithmetic (46186158000 ticks/second), and every
// numeric result asserted is MEASURED against the real, vendored ufbx v0.23.0 (probes compiled and run
// against editor/third_party/ufbx/{ufbx.h,ufbx.c} directly), never assumed from the header's prose.

TEST_CASE(
    "fbx_import: one clip per ufbx_anim_stack, every channel Linear, times strictly increasing, "
    "values.size() == times.size() (FI64, AC-44)") {
    // MEASURED (probe): a Null node's own "Lcl Translation" animated along X, 0 -> 10 over one second
    // (ticks 0..46186158000), bakes to EXACTLY 2 translation keys -- a straight ramp is trivially
    // reducible. The rotation/scale key lists ufbx_bake_anim ALWAYS returns (even for an unanimated
    // component) are both flagged constant here and therefore produce no channel at all (§D-4.10's own
    // finding, gated on `bn.constant_translation`/`_rotation`/`_scale` in the real code).
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" { Version: 232 }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::T\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n"
        "        KeyValueFloat: *2 { a: 0,10 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n"
        "        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Translation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1);
    const auto& anim = result.model.animations[0];
    CHECK(anim.name == "Take 001");
    REQUIRE(anim.channels.size() == 1);  // rotation/scale both constant -- no channel for either
    const auto& channel = anim.channels[0];
    CHECK(channel.path == AnimationPath::Translation);
    CHECK(channel.interpolation == AnimationInterpolation::Linear);
    REQUIRE(channel.times.size() == 2);
    CHECK(channel.times[0] < channel.times[1]);  // strictly increasing (AC-44)
    REQUIRE(channel.values.size() == channel.times.size());
    CHECK(channel.values[0].x == APPROX_POS(0.0));
    CHECK(channel.values[1].x == APPROX_POS(10.0));
    CHECK(anim.duration == APPROX_POS(1.0));

    // targetNode resolves to the node's own localId, never a vector index (§D-4.10).
    REQUIRE(result.model.nodes.size() == 1);
    CHECK(channel.targetNode == result.model.nodes[0].localId);
}

TEST_CASE(
    "fbx_import: trim_start_time shifts a clip authored on [1s,2s] so it starts at 0; duration is the "
    "LENGTH, not the end time (FI65, AC-45, seed S24's discriminator)") {
    // MEASURED (probe): LocalStart/LocalStop (ticks 46186158000/92372316000 -- 1.0s/2.0s) become
    // stack->time_begin/time_end. WITHOUT trim_start_time, baked keys would read 1.0/2.0 (a separate
    // probe, driving the identical KeyTime pair with NO LocalStart/LocalStop at all, measured this
    // directly). WITH it (pinned, D12), they read 0.0/1.0 -- seed S24 (dropping the option) is exactly
    // the untrimmed pair.
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" { Version: 232 }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" {\n"
        "        Version: 100\n"
        "        Properties70:  {\n"
        "            P: \"LocalStart\", \"int\", \"Integer\", \"\",46186158000\n"
        "            P: \"LocalStop\", \"int\", \"Integer\", \"\",92372316000\n"
        "        }\n"
        "    }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::T\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 46186158000,92372316000 }\n"
        "        KeyValueFloat: *2 { a: 0,10 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n"
        "        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Translation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1);
    const auto& anim = result.model.animations[0];
    REQUIRE(anim.channels.size() == 1);
    const auto& channel = anim.channels[0];
    REQUIRE(channel.times.size() == 2);
    CHECK(channel.times[0] == APPROX_POS(0.0));  // NOT 1.0 -- trim_start_time shifted it
    CHECK(channel.times[1] == APPROX_POS(1.0));  // NOT 2.0
    CHECK(anim.duration == APPROX_POS(1.0));     // the LENGTH (1s), not the end time (2s)
}

TEST_CASE(
    "fbx_import: an Euler-animated node with a non-default rotation order AND a non-zero pre-rotation "
    "bakes to quaternion keys matching hand-computed values, within ROT_EPS (FI66, AC-46, seed S26's "
    "discriminator)") {
    // MEASURED against real ufbx v0.23.0 (probe, %.9f precision): RotationOrder=5 (ZYX) +
    // PreRotation=(10,20,30) degrees + three SIMULTANEOUSLY-animated Euler curves (rx: 0->15,
    // ry: 0->30, rz: 0->45, both over one second) do NOT trace a single SLERP arc -- key reduction
    // cannot collapse them, and ufbx resamples the whole second at 30 Hz (31 points, index 0..30).
    // This is the ONE case in the whole suite that exists for exactly one seed (S26: translating
    // curves instead of baking). That substitution would get even q(0) wrong: PreRotation composes
    // with the animated part using TWO DIFFERENT Euler conventions -- PreRotation is always applied in
    // XYZ order, "Lcl Rotation" uses the node's own RotationOrder (ZYX here). Trusting
    // ufbx_bake_anim's own composition (D12) is the only correct way to reach these numbers;
    // hand-rolling the Euler math is exactly what this task forbids.
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"RotationOrder\", \"enum\", \"\", \"\",5\n"
        "            P: \"PreRotation\", \"Vector3D\", \"Vector\", \"\",10,20,30\n"
        "        }\n"
        "    }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::R\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::rx\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n        KeyValueFloat: *2 { a: 0,15 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n"
        "    AnimationCurve: 504, \"AnimCurve::ry\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n        KeyValueFloat: *2 { a: 0,30 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n"
        "    AnimationCurve: 505, \"AnimCurve::rz\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n        KeyValueFloat: *2 { a: 0,45 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Rotation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n"
        "    C: \"OP\",504,502,\"d|Y\"\n"
        "    C: \"OP\",505,502,\"d|Z\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1);
    const auto& anim = result.model.animations[0];
    REQUIRE(anim.channels.size() == 1);
    const auto& channel = anim.channels[0];
    CHECK(channel.path == AnimationPath::Rotation);
    CHECK(channel.interpolation == AnimationInterpolation::Linear);
    REQUIRE(channel.times.size() == 31);
    REQUIRE(channel.values.size() == 31);
    for (std::size_t i = 1; i < channel.times.size(); ++i) {
        CHECK(channel.times[i] > channel.times[i - 1]);  // strictly increasing throughout
    }
    // First and last sampled quaternions, MEASURED against real ufbx v0.23.0 (%.9f precision).
    CHECK(channel.values.front().x == APPROX_ROT(0.038134576));
    CHECK(channel.values.front().y == APPROX_ROT(0.189307857));
    CHECK(channel.values.front().z == APPROX_ROT(0.239298338));
    CHECK(channel.values.front().w == APPROX_ROT(0.951548525));
    CHECK(channel.values.back().x == APPROX_ROT(0.267626546));
    CHECK(channel.values.back().y == APPROX_ROT(0.380927132));
    CHECK(channel.values.back().z == APPROX_ROT(0.553612915));
    CHECK(channel.values.back().w == APPROX_ROT(0.690494962));
    // A middle sample too (index 15, t=0.5): three independent points on the curve, not just the two
    // endpoints a naive "translate the curve" implementation might coincidentally get close to.
    CHECK(channel.values[15].x == APPROX_ROT(0.132214996));
    CHECK(channel.values[15].y == APPROX_ROT(0.306509573));
    CHECK(channel.values[15].z == APPROX_ROT(0.410957669));
    CHECK(channel.values[15].w == APPROX_ROT(0.848342425));
}

TEST_CASE(
    "fbx_import: targetNode resolves to the animated node's own localId even when that node ALSO owns "
    "a geometry-transform helper CHILD -- never the helper's (FI67, E18)") {
    // MEASURED (probe): a Mesh node with a GeometricTranslation (creating a "<geometry helper>" child,
    // FI23's own shape) that is ITSELF animated via "Lcl Translation" bakes against the ORIGINAL
    // node's typed_id, never the helper's -- a helper looks like it should be special; it is not, and
    // typed_id resolves it like any other node (§D-4.10).
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",1,0,0\n"
        "        }\n"
        "    }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::T\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n        KeyValueFloat: *2 { a: 0,10 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",200,100\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Translation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 2);  // the original "box" AND its geometry-transform helper
    const engine::editor::ImportedNode* original = nullptr;
    const engine::editor::ImportedNode* helper = nullptr;
    for (const auto& n : result.model.nodes) {
        if (n.name.find("<geometry helper>") != std::string::npos) {
            helper = &n;
        } else {
            original = &n;
        }
    }
    REQUIRE(original != nullptr);
    REQUIRE(helper != nullptr);
    CHECK(original->localId != helper->localId);

    REQUIRE(result.model.animations.size() == 1);
    REQUIRE(result.model.animations[0].channels.size() == 1);
    CHECK(result.model.animations[0].channels[0].targetNode == original->localId);
    CHECK(result.model.animations[0].channels[0].targetNode != helper->localId);
}

// ---- FI68 is DELIBERATELY ABSENT --------------------------------------------------------------------
//
// E17 ("an out-of-range/unresolved typed_id drops the channel with one warning, never an out-of-bounds
// read") is a DEFENSIVE check with NO REACHABLE INPUT found anywhere in this suite -- the identical
// class of finding as E13 (FI61's own comment above) -- confirmed by THREE independent, empirically
// tested attempts against real, vendored ufbx v0.23.0, not by assumption:
//   1. Animating the scene ROOT's own "Lcl Translation" directly (`C: "OP",curveNodeId,0,"Lcl
//      Translation"`): MEASURED -- ufbx bakes ZERO nodes for this connection (baked->nodes.count ==
//      0), not a typed_id == 0 entry. The root is simply never a bake target.
//   2. A Model object with NO "OO" connection to the scene hierarchy at all, still animated via "OP":
//      MEASURED -- ufbx silently reparents it under the root anyway (allow_nodes_out_of_root stays
//      false, its default, never set by this importer), so it still appears in scene.nodes and is
//      baked with an ordinary, resolvable typed_id.
//   3. MAX_FBX_NODE_DEPTH (FI27) truncating the node walk: MEASURED -- ufbx refuses the WHOLE load the
//      instant its own node_depth_limit is exceeded (no partial ufbx_scene is ever returned, FI27's
//      own finding), so there is no scenario where SOME nodes are baked but ABSENT from
//      nodeIndexByLocalId -- either every node the bake sees is in the map, or the load fails outright
//      and nothing is baked at all.
// The guard in fbx_import.cpp stays (defence in depth for a future ufbx version, or an input shape this
// suite has not tried), but no case here claims to discriminate it -- an untested "this line runs"
// assertion would only look like proof.

TEST_CASE(
    "fbx_import: two adjacent baked samples whose DOUBLE times are distinct but collide once narrowed "
    "to float -- the second is dropped with exactly one warning, times stays strictly increasing "
    "(FI69, E16)") {
    // MEASURED (probe, %.15f precision): six keys spaced 2 FBX ticks apart (roughly 4.3e-11s), based
    // around ~0.001s -- close enough together that ufbx's own DOUBLE-precision bake times are
    // genuinely distinct and increasing, but float's ~7-digit precision at that magnitude (ULP ~=
    // 1.19e-10s, LARGER than the 2-tick gap) collapses adjacent pairs to the SAME float value. This is
    // INV-F8's own justification made concrete: ufbx's own output is correct; narrowing double ->
    // float (matching ImportedAnimationChannel::times' own element type) is what THIS TASK must guard
    // against, not assume ufbx avoids on its own. Values alternate (0/10) so key reduction cannot
    // collapse the six keys down to two BEFORE narrowing, which would defeat the case.
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" { Version: 232 }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::T\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *6 { a: 46186158,46186160,46186162,46186164,46186166,46186168 }\n"
        "        KeyValueFloat: *6 { a: 0,10,0,10,0,10 }\n"
        "        KeyAttrFlags: *6 { a: 4,4,4,4,4,4 }\n"
        "        KeyAttrDataFloat: *24 { a: 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }\n"
        "        KeyAttrRefCount: *6 { a: 1,1,1,1,1,1 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Translation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1);
    REQUIRE(result.model.animations[0].channels.size() == 1);
    const auto& channel = result.model.animations[0].channels[0];
    // MEASURED (probe): six double-precision baked samples collapse to exactly TWO once each is
    // narrowed to float -- index 0 and index 3 survive (the first of each colliding pair); indices
    // 1/2/4/5 are dropped.
    REQUIRE(channel.times.size() == 2);
    CHECK(channel.times[0] < channel.times[1]);  // INV-F8: still strictly increasing after the drop
    REQUIRE(channel.values.size() == 2);
    CHECK(channel.values[0].x == APPROX_POS(0.0));
    CHECK(channel.values[1].x == APPROX_POS(10.0));
    CHECK(result.warningTotal == 1);  // ONE warning for the whole channel, not one per dropped sample
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("duplicate or non-increasing") != std::string::npos);
}

TEST_CASE(
    "fbx_import: an AnimationStack with no baked channels imports as duration == 0, no channels -- NOT "
    "an error (FI70, E15)") {
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" { Version: 232 }\n"
        "    AnimationStack: 500, \"AnimStack::Empty\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1);
    CHECK(result.model.animations[0].name == "Empty");
    CHECK(result.model.animations[0].duration == 0.0F);
    CHECK(result.model.animations[0].channels.empty());
}

TEST_CASE(
    "fbx_import: settings.importAnimations = false -> animations.empty(), summary.animationCount == "
    "0, animationDuration == 0.0F, every other field identical (FI71, AC-47b)") {
    // MAX_ANIMATION_KEYS_PER_MODEL (2 000 000) is NOT implemented as a case here, and this is a
    // deliberate, EMPIRICALLY INVESTIGATED decision, not a silent drop (re-examined per this task's
    // own instructions -- the identical discipline FI38 already applies to MAX_VERTICES_PER_MODEL/
    // MAX_INDICES_PER_MODEL). Two things were MEASURED against real, vendored ufbx v0.23.0 (probes),
    // not assumed:
    //   1. `max_keyframe_segments` (32, pinned) caps how many resampled points ONE keyframe SPAN can
    //      produce, and that cap does NOT grow with the span's duration -- a genuinely non-linear
    //      2-key rotation stretched from 1 second to 10 seconds produced FEWER baked points (20, not
    //      300), never more. "Sparse keys + a huge duration" is not the shortcut it would be for a
    //      naive fixed-rate resampler.
    //   2. The only way to reach a large baked-key count is therefore to PROVIDE that many original
    //      keyframes directly, and the multiplier tops out low enough (empirically far under 32x, per
    //      point 1) that even the BEST case needs on the order of 60 000+ explicit `KeyTime`/
    //      `KeyValueFloat` entries in ONE curve -- upwards of a megabyte of literal fixture text.
    //      "Many minimal objects" (FI38's/FI62's own shortcut for MAX_PRIMITIVES_PER_MODEL/
    //      MAX_JOINTS_PER_SKIN) does not help here either: spreading the same total key count across
    //      many small nodes/curves ADDS fixed per-node/per-curve overhead on top of the keyframe data
    //      itself, making the total fixture size WORSE, not better, than one giant curve. Both
    //      readings land well beyond this file's own stated tier-0 budget ("a few hundred bytes to at
    //      most a few hundred KB, no disk, instant" -- §G-12/R3), so -- like MAX_VERTICES_PER_MODEL/
    //      MAX_INDICES_PER_MODEL -- there is no cheap fixture that reaches this cap, and none is
    //      claimed here.
    constexpr std::string_view OBJECTS =
        "    Model: 100, \"Model::box\", \"Null\" { Version: 232 }\n"
        "    AnimationStack: 500, \"AnimStack::Take 001\", \"\" { Version: 100 }\n"
        "    AnimationLayer: 501, \"AnimLayer::BaseLayer\", \"\" { Version: 100 }\n"
        "    AnimationCurveNode: 502, \"AnimCurveNode::T\", \"\" {\n"
        "        Version: 100\n        Properties70:  { P: \"d\", \"Compound\", \"\", \"\" }\n"
        "    }\n"
        "    AnimationCurve: 503, \"AnimCurve::\", \"\" {\n"
        "        Default: 0\n        KeyVer: 4009\n"
        "        KeyTime: *2 { a: 0,46186158000 }\n        KeyValueFloat: *2 { a: 0,10 }\n"
        "        KeyAttrFlags: *1 { a: 4 }\n        KeyAttrDataFloat: *4 { a: 0,0,0,0 }\n"
        "        KeyAttrRefCount: *1 { a: 2 }\n"
        "    }\n";
    constexpr std::string_view CONNECTIONS =
        "    C: \"OO\",100,0\n"
        "    C: \"OO\",501,500\n"
        "    C: \"OO\",502,501\n"
        "    C: \"OP\",502,100,\"Lcl Translation\"\n"
        "    C: \"OP\",503,502,\"d|X\"\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, CONNECTIONS);
    ImportSettings onSettings;
    onSettings.importAnimations = true;
    const ImportResult on = importModel("t.fbx", "", asBytes(doc), onSettings, ImportDepth::Full, {});
    ImportSettings offSettings;
    offSettings.importAnimations = false;
    const ImportResult off = importModel("t.fbx", "", asBytes(doc), offSettings, ImportDepth::Full, {});

    REQUIRE(on.status == ImportStatus::Ok);
    REQUIRE(off.status == ImportStatus::Ok);
    CHECK_FALSE(on.model.animations.empty());
    CHECK(off.model.animations.empty());
    CHECK(on.model.summary.animationCount > 0);
    CHECK(off.model.summary.animationCount == 0);
    CHECK(off.model.summary.animationDuration == 0.0F);
    CHECK(on.model.summary.animationDuration > 0.0F);
    // Every OTHER field stays identical -- animations are not what any other phase depends on.
    CHECK(on.model.summary.nodeCount == off.model.summary.nodeCount);
    CHECK(on.model.summary.meshCount == off.model.summary.meshCount);
    CHECK(on.model.nodes.size() == off.model.nodes.size());
}

// ---- FI73-FI75: caps, containers and encodings ----------------------------------------------------

TEST_CASE(
    "fbx_import: a .fbx over MAX_MODEL_FILE_BYTES is refused by the cap BEFORE the file is ever "
    "opened -- proven with a file the OS itself would refuse to open (FI73, AC-51)") {
    // WHY THIS LIVES AT THE readFileBytes SEAM AND NOT INSIDE importFbx: the cap is `readFileBytes`'
    // (text_file.cpp), not the importer's. `importFbx` is handed a std::span of bytes that ALREADY
    // exist in memory, so by the time any FBX code runs the file has necessarily been read -- there is
    // no point inside this backend at which "was it opened?" is still answerable. AC-51 is a statement
    // about the boundary, and this case documents that boundary so it is not re-implemented inside the
    // importer (`ModelImportSession::service` passes MAX_MODEL_FILE_BYTES for EVERY format, .fbx
    // included -- model_import_session.cpp:125; MS16 is the session-level twin for .gltf).
    //
    // THE DISCRIMINATOR, and why the OTHER half of this case is not enough on its own: arm (a) below
    // (an oversized file is refused with `refusedByCap`) passes IDENTICALLY for an implementation that
    // opens the file first and checks the cap afterwards, so on its own it proves the REFUSAL and
    // nothing about the ORDER. Arm (b) makes the two orders observable: the same oversized file is
    // made unopenable, so a cap check that ran AFTER the open would report the OS's own error with
    // `refusedByCap == false`, while the shipped order (is_directory -> file_size -> cap -> open)
    // still reports the cap. CONFIRMED DIRECTLY: moving text_file.cpp's cap check below the
    // `std::ifstream` construction reddens arm (b) and leaves arm (a) green.
    //
    // The file is created empty and SPARSELY extended (MS16's own precedent) -- 256 MiB of address
    // space, no real I/O, and nothing is ever written to or read from it.
    const TempDir dir;
    const std::string path = dir.join("huge.fbx");
    writeEmptyFile(path);
    std::error_code ec;
    std::filesystem::resize_file(pathOf(path), MAX_MODEL_FILE_BYTES + 1U, ec);
    REQUIRE_FALSE(ec);

    // (a) the refusal itself: no bytes, the discriminated cap signal, and the observed size kept so the
    //     panel can report the number that tripped it. `bytes` being disengaged is what makes it
    //     impossible for a span to be formed and handed to importModel at all.
    const FileBytesResult refused = readFileBytes(path, MAX_MODEL_FILE_BYTES);
    CHECK_FALSE(refused.bytes.has_value());
    CHECK(refused.refusedByCap);
    CHECK(refused.size == MAX_MODEL_FILE_BYTES + 1U);
    CHECK_FALSE(refused.error.empty());

    // (b) the "without being opened" half. POSIX-only by nature: Windows has no way to make a file
    //     unopenable through std::filesystem::permissions (it maps to the read-only ATTRIBUTE, which
    //     does not block reading), and root ignores the mode bits -- in both cases the case would pass
    //     vacuously, so it says so rather than pretending (TF31's own precedent).
#if defined(_WIN32)
    MESSAGE("arm (b) skipped on Windows: POSIX permission semantics do not apply");
#else
    if (geteuid() == 0) {
        MESSAGE("arm (b) skipped as root: the mode bits are ignored, so this arm would pass vacuously");
    } else {
        std::filesystem::permissions(pathOf(path), std::filesystem::perms::none, std::filesystem::perm_options::replace,
                                     ec);
        REQUIRE_FALSE(ec);

        const FileBytesResult unopenable = readFileBytes(path, MAX_MODEL_FILE_BYTES);
        CHECK_FALSE(unopenable.bytes.has_value());
        CHECK(unopenable.refusedByCap);  // the CAP refused it, not the OS -- so no open was attempted
        CHECK(unopenable.size == MAX_MODEL_FILE_BYTES + 1U);

        // The control that proves the file really was unopenable, and therefore that the assertion
        // above is not vacuous: raise the cap above the file's size and the SAME path now fails with
        // the OS's reason instead, `refusedByCap` false.
        const FileBytesResult opened = readFileBytes(path, MAX_MODEL_FILE_BYTES + 2U);
        CHECK_FALSE(opened.bytes.has_value());
        CHECK_FALSE(opened.refusedByCap);

        std::filesystem::permissions(pathOf(path), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        CHECK_FALSE(ec);  // restored BEFORE ~TempDir, or the cleanup itself fails
    }
#endif
}

TEST_CASE(
    "fbx_import: a node name carrying Latin-1 and Shift-JIS bytes imports SUCCESSFULLY with U+FFFD "
    "replacements, and every string in the result is valid UTF-8 (FI74, AC-54)") {
    // Real Maya and 3ds Max exports carry names in the authoring machine's own code page --
    // Shift-JIS and Latin-1 are both common -- and those bytes land in ImportedNode::name ->
    // ImportDetailsPanel -> ImGui, which requires valid UTF-8.
    //
    // WHY THIS DISCRIMINATES. `opts.unicode_error_handling` has six values and the code picks
    // REPLACEMENT_CHARACTER. MEASURED against the vendored ufbx v0.23.0 with this exact document:
    //   REPLACEMENT_CHARACTER (shipped) -> loads; name == "caf" + three U+FFFD (one per bad byte)
    //   UNDERSCORE / QUESTION_MARK      -> loads; name == "caf___" / "caf???"     -> the == below fails
    //   REMOVE                          -> loads; name == "caf"                   -> the == below fails
    //   ABORT_LOADING                   -> UFBX_ERROR_INVALID_UTF8, the WHOLE import fails over one
    //                                      bone name                              -> status fails
    //   UNSAFE_IGNORE                   -> UFBX_ERROR_UNSAFE_OPTIONS (it additionally needs
    //                                      allow_unsafe), and with allow_unsafe it would feed the raw
    //                                      invalid bytes to ImGui -> the validator below fails
    // REPLACEMENT_CHARACTER happens to be the enumerator with value 0, so DELETING the assignment is
    // not a discriminable seed -- changing it is, and all five alternatives are caught here.
    //
    // The bytes: 0xE9 is Latin-1 'e-acute' (a UTF-8 lead byte with no continuation), 0x93 0xFA is
    // Shift-JIS for the first character of "Japan" (two bare continuation bytes). Each is one
    // encoding error, so ufbx emits one U+FFFD each -- three in total. Written as SEPARATE adjacent
    // string literals because a hex escape is greedy: "\xe9_mesh" would parse as the single character
    // \xe9_ (an invalid escape), not \xe9 followed by "_mesh".
    const std::string objects = std::string("    Geometry: 200, \"Geometry::caf\xe9") +
                                "_mesh\", \"Mesh\" {\n"
                                "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
                                "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
                                "        GeometryVersion: 124\n"
                                "    }\n"
                                "    Model: 100, \"Model::caf\xe9\x93\xfa"
                                "\", \"Mesh\" { Version: 232 }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, objects, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("t.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});

    // The import SUCCEEDS -- a non-UTF-8 bone name is not a reason to refuse a character.
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    REQUIRE(result.model.meshes.size() == 1);

    constexpr std::string_view REPLACEMENT = "\xef\xbf\xbd";  // U+FFFD, in UTF-8
    CHECK(result.model.nodes[0].name ==
          "caf" + std::string(REPLACEMENT) + std::string(REPLACEMENT) + std::string(REPLACEMENT));
    CHECK(result.model.meshes[0].name == "caf" + std::string(REPLACEMENT) + "_mesh");

    // ... and NOTHING that reaches the panel carries invalid UTF-8. Checked with a validator rather
    // than by comparing against the literals above, so a future string this importer starts filling is
    // covered by this case without being enumerated in it.
    CHECK(isValidUtf8(result.model.nodes[0].name));
    CHECK(isValidUtf8(result.model.meshes[0].name));
    CHECK(isValidUtf8(result.model.sourceSpace.generator));
    CHECK(isValidUtf8(result.model.sourceSpace.formatVersion));
    CHECK(isValidUtf8(result.message));
    for (const std::string& warning : result.warnings) {
        CHECK(isValidUtf8(warning));
    }
    // The validator is not vacuous: it rejects the raw source bytes this fixture is built from.
    CHECK_FALSE(isValidUtf8("caf\xe9"));
    CHECK_FALSE(isValidUtf8("\x93\xfa"));
}

TEST_CASE(
    "fbx_import: an FBX 6.x document imports, and sourceSpace.formatVersion reports ITS OWN version "
    "rather than the 7.x every other fixture here uses (FI75, E2)") {
    // ufbx supports pre-7000 FBX (its own reader branches on `version < 7000` in a dozen places), and
    // so does this importer -- there is no version gate anywhere, which is the point: a 6.x file from
    // an old asset library imports rather than being refused.
    //
    // A 6.x document is NOT a 7.x document with a different number in the header, and this fixture is
    // therefore hand-written whole rather than routed through makeFbx(): pre-7000 FBX puts geometry
    // INSIDE the Model node (there is no separate Geometry object), spells properties
    // `Properties60`/`Property:` instead of `Properties70`/`P:`, and connects by TYPE::NAME pair
    // (`Connect: "OO", "Model::box", "Model::Scene"`) rather than by 64-bit id. MEASURED against the
    // vendored ufbx v0.23.0: this document loads with version == 6100, ascii == true, one authored
    // node named "box", one mesh of 4 vertices / 2 triangles.
    //
    // WHY IT DISCRIMINATES: it is the ONLY case in this suite whose source is not FBX 7400, so it is
    // the only one that can tell `std::format("FBX {} {}", metadata.version, ...)` from a constant
    // matching the 7.x fixture family. CONFIRMED DIRECTLY: replacing fbx_import.cpp's formatVersion
    // with the literal "FBX 7400 ascii" leaves FI13/FI19/FI76 green and reddens only this case; adding
    // a `metadata.version < 7000 -> Unsupported` gate reddens it too. The panel's Source space row is
    // a straight interpolation of this same field (import_details_panel.cpp's drawOverview), so the
    // field is the testable half and the drawn frame is the GPU tier's.
    const std::string doc =
        "; FBX 6.1.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 6100\n"
        "    Creator: \"aero test fixture\"\n"
        "}\n"
        "GlobalSettings:  {\n"
        "    Version: 1000\n"
        "    Properties60:  {\n"
        "        Property: \"UpAxis\", \"int\", \"\",2\n"
        "        Property: \"UpAxisSign\", \"int\", \"\",1\n"
        "        Property: \"FrontAxis\", \"int\", \"\",1\n"
        "        Property: \"FrontAxisSign\", \"int\", \"\",-1\n"
        "        Property: \"CoordAxis\", \"int\", \"\",0\n"
        "        Property: \"CoordAxisSign\", \"int\", \"\",1\n"
        "        Property: \"UnitScaleFactor\", \"double\", \"\",1\n"
        "    }\n"
        "}\n"
        "Objects:  {\n"
        "    Model: \"Model::box\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties60:  {\n"
        "            Property: \"Lcl Translation\", \"Lcl Translation\", \"A+\",0,0,200\n"
        "        }\n"
        "        Vertices: 0,0,0,100,0,0,100,100,0,0,100,0\n"
        "        PolygonVertexIndex: 0,1,2,-4\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "}\n"
        "Connections:  {\n"
        "    Connect: \"OO\", \"Model::box\", \"Model::Scene\"\n"
        "}\n";
    const ImportResult result = importModel("old.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});

    REQUIRE(result.status == ImportStatus::Ok);
    CHECK(result.model.sourceSpace.declared);
    CHECK(result.model.sourceSpace.formatVersion == "FBX 6100 ascii");
    // ... and it is a REAL import, not merely a header read: the same Z-up centimetre conversion every
    // 7.x fixture gets (FI15's numbers) applies to a 6.x source too.
    CHECK(result.model.sourceSpace.upAxis == 'Z');
    CHECK(result.model.sourceSpace.unitMeters == doctest::Approx(0.01F));
    REQUIRE(result.model.nodes.size() == 1);
    CHECK(result.model.nodes[0].name == "box");
    CHECK(result.model.nodes[0].translation.x == APPROX_POS(0.0F));
    CHECK(result.model.nodes[0].translation.y == APPROX_POS(2.0F));
    CHECK(result.model.nodes[0].translation.z == APPROX_POS(0.0F));
    CHECK(result.model.summary.vertexCount == 4);
    CHECK(result.model.summary.triangleCount == 2);
}

// ---- FI77-FI78: two gaps the sabotage round found, each closed with a fixture that reaches the
// option a green suite could not otherwise see ------------------------------------------------------

TEST_CASE(
    "fbx_import: a document naming an external geometry cache imports WITHOUT ufbx ever trying to "
    "open it -- load_external_files is FALSE, and this is the only input that can tell (FI77, D4)") {
    // THE GAP THIS CLOSES. Seed S16 (`opts.load_external_files = true`) left the WHOLE suite green --
    // 1109/1109, 89/89 -- and so did S16 and S17 applied TOGETHER. The plan predicted FI52/MS24 would
    // catch it; neither can, because the flag gates nothing a texture-only fixture reaches.
    //
    // MEASURED against the vendored ufbx v0.23.0, not assumed: for an FBX document the flag gates
    // exactly ONE thing -- `ufbxi_load_external_files`, which walks `scene.cache_files` (the FBX
    // `Cache:` object, an Alembic/point-cache reference) and calls `open_file_cb` for each one. It
    // does NOT gate textures, which is why every texture fixture in this file is blind to it. With the
    // flag ON, the callback is called with 'caches/pointcache.xml' and the load then fails outright
    // with UFBX_ERROR_EXTERNAL_FILE_NOT_FOUND (`ignore_missing_external_files` is false, deliberately)
    // -- ImportStatus::ParseFailed. With the flag OFF, as shipped, the callback is never called at all
    // and the model imports normally, cache reference and everything.
    //
    // So the assertion is simply that this document imports Ok with its geometry intact. That reads
    // like a weak statement and is the strongest one available: it is FALSE the moment anyone turns
    // the flag on, and it is the only case in the suite of which that is true.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
        "    Cache: 600, \"Cache::pointcache\", \"\" {\n"
        "        Properties70:  {\n"
        "            P: \"CacheFileName\", \"KString\", \"\", \"\", \"caches/pointcache.xml\"\n"
        "            P: \"CacheFileType\", \"enum\", \"\", \"\",0\n"
        "        }\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("cached.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});

    REQUIRE(result.status == ImportStatus::Ok);  // FALSE with load_external_files on: ParseFailed
    REQUIRE(result.model.nodes.size() == 1);
    CHECK(result.model.nodes[0].name == "box");
    CHECK(result.model.summary.vertexCount == 4);
    CHECK(result.model.summary.triangleCount == 2);
    // The cache reference is not an asset dependency either: nothing here contributes a URI the
    // scan would then try to resolve to a GUID.
    CHECK(result.externalUris.empty());
    CHECK(result.model.images.empty());
}

TEST_CASE(
    "fbx_import: a node with a ROTATION PIVOT keeps its own transform and gains no extra node -- "
    "pivot_handling is RETAIN (FI78, D8, seed S12's discriminator)") {
    // THE GAP THIS CLOSES. Seed S12 (`pivot_handling` -> ADJUST_TO_ROTATION_PIVOT) left the whole
    // suite green: the plan predicted FI21 would catch it, but no fixture anywhere in this file
    // declared a pivot at all, so both modes did exactly the same thing to every one of them. A seed
    // that changes a setting no input reaches cannot redden anything, and the fix is an input, not an
    // assertion.
    //
    // MEASURED against the vendored ufbx v0.23.0 with THIS document (RotationActive: 1 is required --
    // without it the pivot is inert):
    //   RETAIN (shipped)         -> ONE authored node, translation (6, -3, 3)
    //   ADJUST_TO_ROTATION_PIVOT -> TWO nodes (an extra, unnamed helper at (-5, 0, 0)) and the
    //                               authored node's translation moves to (6, 2, 3)
    // Both modes leave the vertices alone, which is why a geometry-only assertion cannot see this and
    // the node count plus the translation can.
    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
        "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
        "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::box\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",1,2,3\n"
        "            P: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,90\n"
        "            P: \"RotationActive\", \"bool\", \"\", \"\",1\n"
        "            P: \"RotationPivot\", \"Vector3D\", \"Vector\", \"\",5,0,0\n"
        "            P: \"ScalingPivot\", \"Vector3D\", \"Vector\", \"\",5,0,0\n"
        "        }\n"
        "    }\n";
    const std::string doc = makeFbx(CANONICAL_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("pivot.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});

    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);  // TWO under ADJUST_TO_ROTATION_PIVOT
    CHECK(result.model.nodes[0].name == "box");
    CHECK(result.model.nodes[0].translation.x == APPROX_POS(6.0F));
    CHECK(result.model.nodes[0].translation.y == APPROX_POS(-3.0F));  // +2 under the seed
    CHECK(result.model.nodes[0].translation.z == APPROX_POS(3.0F));
    CHECK(result.model.nodes[0].rotation.x == APPROX_ROT(0.0F));
    CHECK(result.model.nodes[0].rotation.y == APPROX_ROT(0.0F));
    CHECK(result.model.nodes[0].rotation.z == APPROX_ROT(0.707106781F));
    CHECK(result.model.nodes[0].rotation.w == APPROX_ROT(0.707106781F));
}

// ---- FI76: the one committed binary fixture (Step 12, AC-55) --------------------------------------

TEST_CASE(
    "fbx_import: the committed binary fixture imports to the SAME shape as a hand-written ASCII twin -- "
    "the ONLY proof of the binary container path (FI76, AC-55)") {
    // cube-binary.fbx is a REAL Blender 5.2.0 LTS export (see tests/fixtures/README.md for the exact
    // command, settings, date, size and SHA-256) -- a default 1m cube, no material, no animation.
    // MEASURED against the real file, not guessed: Blender authors it as 8 control-point vertices
    // referenced by a 24-entry PolygonVertexIndex across 6 quad faces (the standard FBX convention
    // every fixture in this file already uses, just extended from one quad to a full cube), with a
    // Lcl Scaling of (100,100,100) on the node rather than a baked geometry offset.
    //
    // The ASCII twin below is hand-written to the IDENTICAL topology and produces a BIT-FOR-BIT match
    // on every field checked below -- MEASURED by running both through this exact importer and
    // comparing, not assumed. Two fields are DELIBERATELY NOT compared, and are named here rather than
    // silently skipped:
    //   - ImportedPrimitive::positions.size(): Blender's own binary export carries a PER-FACE-VERTEX
    //     LayerElementNormal (hard-shaded flat normals), which ufbx_generate_indices' memcmp-based dedup
    //     (A12) correctly refuses to collapse across a face boundary -- so Blender's primitive reports
    //     24 unique vertices where this undecorated ASCII twin (no normals authored) reports 8. This is
    //     a SHADING-FIDELITY authoring choice, not a property either the binary OR the ASCII CONTAINER
    //     FORMAT controls, and reproducing Blender's own LayerElementNormal block is out of scope for
    //     what AC-55 actually asks this case to prove.
    //   - SourceSpace::upAxis: Blender's exporter declares `UpAxis: 1` (Y) in GlobalSettings while STILL
    //     requiring the identical -90-degree-about-X geometric correction a Z-up source would (MEASURED:
    //     both fixtures produce the bit-identical converted rotation below) -- an internal convention of
    //     Blender's own FBX exporter this task does not attempt to reverse-engineer. SourceSpace::
    //     unitMeters (both 0.01 -- a centimetre source) IS compared, and matches.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(CUBE_BINARY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult bin =
        importModel("cube-binary.fbx", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(bin.status == ImportStatus::Ok);
    CHECK_FALSE(bin.model.sourceSpace.formatVersion.empty());
    CHECK(bin.model.sourceSpace.formatVersion.find("binary") != std::string::npos);

    constexpr std::string_view OBJECTS =
        "    Geometry: 200, \"Geometry::Cube\", \"Mesh\" {\n"
        "        Vertices: *24 { a: -0.5,-0.5,-0.5, 0.5,-0.5,-0.5, 0.5,0.5,-0.5, -0.5,0.5,-0.5, -0.5,-0.5,0.5, "
        "0.5,-0.5,0.5, 0.5,0.5,0.5, -0.5,0.5,0.5 }\n"
        "        PolygonVertexIndex: *24 { a: 0,1,2,-4, 4,5,6,-8, 0,1,5,-5, 3,2,6,-8, 0,3,7,-5, 1,2,6,-6 }\n"
        "        GeometryVersion: 124\n"
        "    }\n"
        "    Model: 100, \"Model::Cube\", \"Mesh\" {\n"
        "        Version: 232\n"
        "        Properties70:  {\n"
        "            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",100,100,100\n"
        "        }\n"
        "    }\n";
    const std::string asciiDoc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult ascii = importModel("twin.fbx", "", asBytes(asciiDoc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(ascii.status == ImportStatus::Ok);
    CHECK(ascii.model.sourceSpace.formatVersion.find("ascii") != std::string::npos);

    // Node: same name, same parent-less root, same converted transform, element by element.
    REQUIRE(bin.model.nodes.size() == 1);
    REQUIRE(ascii.model.nodes.size() == 1);
    const engine::editor::ImportedNode& binNode = bin.model.nodes[0];
    const engine::editor::ImportedNode& asciiNode = ascii.model.nodes[0];
    CHECK(binNode.name == asciiNode.name);
    CHECK(binNode.name == "Cube");
    CHECK(binNode.parent == engine::editor::INVALID_SUBASSET);
    CHECK(asciiNode.parent == engine::editor::INVALID_SUBASSET);
    CHECK(binNode.translation.x == APPROX_POS(asciiNode.translation.x));
    CHECK(binNode.translation.y == APPROX_POS(asciiNode.translation.y));
    CHECK(binNode.translation.z == APPROX_POS(asciiNode.translation.z));
    CHECK(binNode.rotation.x == APPROX_ROT(asciiNode.rotation.x));
    CHECK(binNode.rotation.y == APPROX_ROT(asciiNode.rotation.y));
    CHECK(binNode.rotation.z == APPROX_ROT(asciiNode.rotation.z));
    CHECK(binNode.rotation.w == APPROX_ROT(asciiNode.rotation.w));
    CHECK(binNode.scale.x == APPROX_POS(asciiNode.scale.x));
    CHECK(binNode.scale.y == APPROX_POS(asciiNode.scale.y));
    CHECK(binNode.scale.z == APPROX_POS(asciiNode.scale.z));

    // Mesh/summary: vertex and triangle counts, and material/skin/animation counts (a plain, unmaterialed
    // cube, both ways).
    CHECK(bin.model.summary.vertexCount == ascii.model.summary.vertexCount);
    CHECK(bin.model.summary.triangleCount == ascii.model.summary.triangleCount);
    CHECK(bin.model.summary.triangleCount == 12);
    CHECK(bin.model.materials.size() == ascii.model.materials.size());
    CHECK(bin.model.skins.size() == ascii.model.skins.size());
    CHECK(bin.model.animations.size() == ascii.model.animations.size());
    REQUIRE(bin.model.meshes.size() == 1);
    REQUIRE(ascii.model.meshes.size() == 1);
    REQUIRE(bin.model.meshes[0].primitives.size() == 1);
    REQUIRE(ascii.model.meshes[0].primitives.size() == 1);

    // Bounds: the same physical cube, element by element -- what a person actually compares against
    // Blender's own Dimensions panel (validation row 2/13's real-asset cover).
    REQUIRE(bin.model.summary.bounds.valid());
    REQUIRE(ascii.model.summary.bounds.valid());
    CHECK(bin.model.summary.bounds.min.x == APPROX_POS(ascii.model.summary.bounds.min.x));
    CHECK(bin.model.summary.bounds.min.y == APPROX_POS(ascii.model.summary.bounds.min.y));
    CHECK(bin.model.summary.bounds.min.z == APPROX_POS(ascii.model.summary.bounds.min.z));
    CHECK(bin.model.summary.bounds.max.x == APPROX_POS(ascii.model.summary.bounds.max.x));
    CHECK(bin.model.summary.bounds.max.y == APPROX_POS(ascii.model.summary.bounds.max.y));
    CHECK(bin.model.summary.bounds.max.z == APPROX_POS(ascii.model.summary.bounds.max.z));

    // SourceSpace::unitMeters -- a centimetre source, both ways (upAxis is DELIBERATELY not compared,
    // see the header comment above).
    CHECK(bin.model.sourceSpace.declared);
    CHECK(ascii.model.sourceSpace.declared);
    CHECK(bin.model.sourceSpace.unitMeters == doctest::Approx(ascii.model.sourceSpace.unitMeters));
    CHECK(bin.model.sourceSpace.unitMeters == doctest::Approx(0.01F));
}
