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

// ---- FI0: the dispatch arm does not exist yet -------------------------------------------------
// A TRUE statement at THIS commit: Step 3 has not yet added ".fbx" to isImportableModelName's suffix
// table, so importModel returns Unsupported regardless of content -- exactly the MI28 precedent
// (tests/editor/model_import_test.cpp) restated against the real §D-7 document instead of a one-byte
// stand-in. Deleted and replaced by the real FI1 in Step 3, which asserts Ok / 1 node named "box" /
// 1 root / 1 mesh over this SAME makeFbx(...) document -- proving the harness this step lands is
// already correct, not merely present.
TEST_CASE("fbx_import: the FBX dispatch arm does not exist yet (FI0)") {
    const std::string doc = makeFbx(DEFAULT_GLOBALS_PROPERTIES, DEFAULT_OBJECTS, DEFAULT_CONNECTIONS);
    const ImportResult result = importModel("box.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Unsupported);
    CHECK(result.model.nodes.empty());
}
