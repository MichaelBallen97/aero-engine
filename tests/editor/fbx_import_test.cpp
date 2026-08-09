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
//                        exporter_version == 0, so SourceSpace::generator is EMPTY for every tier-0
//                        fixture (§A-21, FI19).
//
// The template below is the §D-7 document, VERIFIED TO PARSE under real ufbx v0.23.0 (§G-10 -- R1 is
// CLOSED by that spike, not deferred). Every future FI case is this document with one section varied
// through makeFbx()'s three parameters, never restated whole.
#include <aero/editor/model_import.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>

using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;

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
//   Step 5 (this engagement's last step): phase 3 lands (nodes, hierarchy). nodes.size() becomes 1,
//                  nodes[0].name becomes "box", roots.size() becomes 1.
//
// model.meshes.size() == 1 (the plan's literal §V3 wording, read as the vector rather than the
// summary count) is NOT asserted here -- it becomes true only once phase 6 (Step 7) exists, which is
// out of this engagement's scope. That is stated plainly rather than silently left out.
TEST_CASE("fbx_import: the §D-7 template through the real dispatch arm (FI1)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    // Step 4's true statement: phases 1-2 land here. ufbx_load_memory succeeds on this well-formed
    // document, so status becomes Ok, and summary.meshCount (a cheap, structural count read directly
    // from ufbx's own scene.meshes.count -- see fbx_import.cpp's own comment) becomes 1. nodes/roots
    // stay empty until Step 5's node walk (phase 3).
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.model.summary.meshCount == 1);
    CHECK(result.model.nodes.empty());
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
    "fbx_import: Structure and Full agree on every element COUNT this step wires (FI4 part 1, "
    "AC-20/INV-M4 as §A-4 scopes it -- extended in Step 5 to also cover the node list)") {
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
    // over it -- the load itself stays Ok, and the guard this case exists for is OUR OWN.
    CHECK(result.status == ImportStatus::Ok);
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
