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

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// The byte-loading pattern model_import_test.cpp / fbx_import_test.cpp / obj_import_test.cpp each keep
// their own copy of, restated here so this TU stays independent of all three.
// std::string_view, not const std::string&, so the namespace-scope fixtures below can be
// `constexpr std::string_view` -- which is this tree's own shape for a test fixture literal
// (fbx_import_test.cpp) AND what keeps readability-identifier-naming's UPPER_CASE rule, which applies
// to constexpr variables and not to plain const ones. Every call site binds a NAMED local; never call
// this on a temporary.
[[nodiscard]] std::span<const std::byte> asBytes(std::string_view text) noexcept {
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

constexpr std::string_view TRIANGLE_STL =
    "solid Tri\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
    "endloop\nendfacet\nendsolid Tri\n";

constexpr std::string_view TRIANGLE_PLY =
    "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
    "element face 1\nproperty list uchar int vertex_index\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

// A .ply that names one texture in its header. NO trailing space after the operand, deliberately: the
// loader hands back the rest of the line verbatim, so a trailing space would land inside the name.
constexpr std::string_view TEXTURED_PLY =
    "ply\nformat ascii 1.0\ncomment TextureFile scan.png\nelement vertex 3\nproperty float x\n"
    "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_index\n"
    "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

constexpr std::string_view TRIANGLE_DAE =
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
constexpr std::string_view HOSTILE_DAE =
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

// ---- Collada fixtures (step 5) ----------------------------------------------------------------------
// One triangle geometry, reused by every hierarchy fixture below so the only thing that varies between
// them is the thing the case is about.
constexpr std::string_view DAE_TRIANGLE_GEOMETRY =
    R"(  <library_geometries><geometry id="g1" name="Tri"><mesh>
    <source id="p"><float_array id="pa" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#pa" count="3" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
    <triangles count="1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
)";

// A THREE-LEVEL chain under the visual scene. Assimp makes the <visual_scene> itself the root node, so
// this is four nodes: S -> A -> B -> C, with the geometry on the deepest.
constexpr std::string_view DAE_CHAIN =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
%GEOMETRY%
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="A" name="A"><translate>1 2 3</translate>
      <node id="B" name="B"><scale>2 2 2</scale>
        <node id="C" name="C"><instance_geometry url="#g1"/></node>
      </node>
    </node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// THREE SIBLINGS, in document order. A one-child fixture is order-blind, which is exactly why this one
// exists: the pre-order walk pushes children in REVERSE so the first is popped first.
constexpr std::string_view DAE_SIBLINGS =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
%GEOMETRY%
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="a" name="a"/><node id="b" name="b"/><node id="c" name="c"><instance_geometry url="#g1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// ONE node instancing TWO geometries: ImportedNode::meshIndex holds exactly one index, so the extra is
// split into a child named `<parent>.1` and the split is NAMED in a warning.
constexpr std::string_view DAE_TWO_MESHES_ONE_NODE =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries>
    <geometry id="g1" name="One"><mesh>
      <source id="p1"><float_array id="pa1" count="9">0 0 0 1 0 0 0 1 0</float_array>
        <technique_common><accessor source="#pa1" count="3" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="v1"><input semantic="POSITION" source="#p1"/></vertices>
      <triangles count="1"><input semantic="VERTEX" source="#v1" offset="0"/><p>0 1 2</p></triangles>
    </mesh></geometry>
    <geometry id="g2" name="Two"><mesh>
      <source id="p2"><float_array id="pa2" count="9">5 5 5 6 5 5 5 6 5</float_array>
        <technique_common><accessor source="#pa2" count="3" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="v2"><input semantic="POSITION" source="#p2"/></vertices>
      <triangles count="1"><input semantic="VERTEX" source="#v2" offset="0"/><p>0 1 2</p></triangles>
    </mesh></geometry>
  </library_geometries>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="Both" name="Both"><instance_geometry url="#g1"/><instance_geometry url="#g2"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// A quad and a pentagon, so aiProcess_Triangulate's 3(n-2) split is asserted rather than assumed.
constexpr std::string_view DAE_POLYGONS =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries>
    <geometry id="quad" name="Quad"><mesh>
      <source id="qp"><float_array id="qpa" count="12">0 0 0 1 0 0 1 1 0 0 1 0</float_array>
        <technique_common><accessor source="#qpa" count="4" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="qv"><input semantic="POSITION" source="#qp"/></vertices>
      <polylist count="1"><input semantic="VERTEX" source="#qv" offset="0"/><vcount>4</vcount><p>0 1 2 3</p></polylist>
    </mesh></geometry>
    <geometry id="pent" name="Pent"><mesh>
      <source id="pp"><float_array id="ppa" count="15">0 0 0 2 0 0 3 1 0 1 2 0 -1 1 0</float_array>
        <technique_common><accessor source="#ppa" count="5" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="pv"><input semantic="POSITION" source="#pp"/></vertices>
      <polylist count="1"><input semantic="VERTEX" source="#pv" offset="0"/><vcount>5</vcount><p>0 1 2 3 4</p></polylist>
    </mesh></geometry>
  </library_geometries>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="Q" name="Q"><instance_geometry url="#quad"/></node>
    <node id="P" name="P"><instance_geometry url="#pent"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// GAP-CLOSING FIXTURE (sabotage seed S19). A .ply header naming TWO DIFFERENT textures is the only
// input that distinguishes the Full pass's own seeding step from the loader's material: Assimp keeps
// only the LAST TextureFile line (each overwrites the previous), while scanPlyTextureFiles returns them
// all -- so without the seed the two depths report DIFFERENT URI sets. With a single TextureFile the
// loader's own copy happens to reproduce the scan's answer exactly, which is why deleting the seeding
// step reddened nothing at all until this fixture existed.
constexpr std::string_view TWO_TEXTURE_PLY =
    "ply\nformat ascii 1.0\ncomment TextureFile first.png\ncomment TextureFile second.png\n"
    "element vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\n"
    "property list uchar int vertex_index\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

// GAP-CLOSING (code-review finding 3). A TAB-indented header line. PLY::Element::ParseElement opens
// with PLY::DOM::SkipSpaces -> Assimp::SkipSpaces, whose test is `in == ' ' || in == '\t'`, so the
// LOADER sees this TextureFile line; scanPlyTextureFiles skipped only ' ' and did not. Structure
// returned {} while Full returned {tabbed.png} -- the depth disagreement AC-19 forbids, and a
// dependency phase 7.5's Structure-depth probe would never record, so editing tabbed.png would never
// mark the model DependencyChanged. It lives IN THE TABLE so AI10 asserts the agreement on it.
constexpr std::string_view TAB_INDENTED_PLY =
    "ply\nformat ascii 1.0\n\tcomment TextureFile tabbed.png\nelement vertex 3\nproperty float x\n"
    "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_index\n"
    "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

// The table had no CRLF .ply either. IOStreamBuffer::getNextLine treats '\r' as a line end and appends
// its own '\n', so CRLF and LF are indistinguishable to the loader; this fixture is what asserts the
// scan's one-trailing-'\r' rule agrees END TO END rather than only at the pure level (MI140).
constexpr std::string_view CRLF_TEXTURED_PLY =
    "ply\r\nformat ascii 1.0\r\ncomment TextureFile crlf.png\r\nelement vertex 3\r\nproperty float x\r\n"
    "property float y\r\nproperty float z\r\nelement face 1\r\nproperty list uchar int vertex_index\r\n"
    "end_header\r\n0 0 0\r\n1 0 0\r\n0 1 0\r\n3 0 1 2\r\n";

// THE FIXTURE TABLE. AI10 (depth equality), AI11 (degenerate inputs) and AI13 all drive it, so adding a
// fixture here covers it in several cases automatically -- which is what keeps AC-19's "for EVERY
// fixture" honest as the suite grows.
struct Fixture {
    std::string_view name;
    std::string_view text;
};

[[nodiscard]] std::vector<Fixture> fixtureTable() {
    return {
        Fixture{"t.stl", TRIANGLE_STL},       Fixture{"t.ply", TRIANGLE_PLY},
        Fixture{"scan.ply", TEXTURED_PLY},    Fixture{"t.dae", TRIANGLE_DAE},
        Fixture{"hostile.dae", HOSTILE_DAE},  Fixture{"two.ply", TWO_TEXTURE_PLY},
        Fixture{"tab.ply", TAB_INDENTED_PLY}, Fixture{"crlf.ply", CRLF_TEXTURED_PLY},
    };
}

// ---- byte builders (AI18/AI19/AI20/AI22/AI23) -------------------------------------------------------
// The BINARY .stl and .ply fixtures are BUILT here, byte by byte, from the SAME triangle list the ASCII
// literal describes. That is what makes the "byte-identical twin" assertions mean anything: a COMMITTED
// binary compared against a literal that describes it proves only that someone typed both consistently
// once, never that the two encodings agree after a round trip through the loader.
struct Tri {
    std::array<float, 3> normal{};
    std::array<std::array<float, 3>, 3> vertices{};
};

void appendU16Le(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void appendU32Le(std::string& out, std::uint32_t value) {
    for (unsigned int i = 0; i < 4U; ++i) {
        out.push_back(static_cast<char>((value >> (i * 8U)) & 0xFFU));
    }
}

void appendU32Be(std::string& out, std::uint32_t value) {
    for (unsigned int i = 4U; i > 0U; --i) {
        out.push_back(static_cast<char>((value >> ((i - 1U) * 8U)) & 0xFFU));
    }
}

void appendFloatLe(std::string& out, float value) { appendU32Le(out, std::bit_cast<std::uint32_t>(value)); }
void appendFloatBe(std::string& out, float value) { appendU32Be(out, std::bit_cast<std::uint32_t>(value)); }

// 80 bytes of header (padded/truncated), a uint32 facet count, then 50 bytes per facet: normal, three
// vertices, and the 16-bit attribute word the per-face colour extension lives in.
[[nodiscard]] std::string buildBinaryStl(std::string_view header, const std::vector<Tri>& tris,
                                         const std::vector<std::uint16_t>& attributes) {
    std::string out;
    for (std::size_t i = 0; i < 80U; ++i) {
        out.push_back(i < header.size() ? header[i] : '\0');
    }
    appendU32Le(out, static_cast<std::uint32_t>(tris.size()));
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (const float component : tris[t].normal) {
            appendFloatLe(out, component);
        }
        for (const std::array<float, 3>& vertex : tris[t].vertices) {
            for (const float component : vertex) {
                appendFloatLe(out, component);
            }
        }
        appendU16Le(out, t < attributes.size() ? attributes[t] : static_cast<std::uint16_t>(0));
    }
    return out;
}

// The cube AI22/AI23 use, in one place, so the ASCII text and the two binary encodings can never drift.
[[nodiscard]] std::vector<std::array<float, 3>> cubeVertices() {
    return {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, {0.0F, 1.0F, 1.0F}};
}

[[nodiscard]] std::vector<std::array<std::uint32_t, 3>> cubeFaces() {
    return {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
            {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
}

[[nodiscard]] std::string buildAsciiPly() {
    std::string out =
        "ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 12\nproperty list uchar int vertex_index\nend_header\n";
    for (const std::array<float, 3>& vertex : cubeVertices()) {
        out += std::format("{} {} {}\n", vertex[0], vertex[1], vertex[2]);
    }
    for (const std::array<std::uint32_t, 3>& face : cubeFaces()) {
        out += std::format("3 {} {} {}\n", face[0], face[1], face[2]);
    }
    return out;
}

[[nodiscard]] std::string buildBinaryPly(bool bigEndian) {
    std::string out = std::string("ply\nformat ") + (bigEndian ? "binary_big_endian" : "binary_little_endian") +
                      " 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
                      "element face 12\nproperty list uchar int vertex_index\nend_header\n";
    for (const std::array<float, 3>& vertex : cubeVertices()) {
        for (const float component : vertex) {
            if (bigEndian) {
                appendFloatBe(out, component);
            } else {
                appendFloatLe(out, component);
            }
        }
    }
    for (const std::array<std::uint32_t, 3>& face : cubeFaces()) {
        out.push_back(static_cast<char>(3));
        for (const std::uint32_t index : face) {
            if (bigEndian) {
                appendU32Be(out, index);
            } else {
                appendU32Le(out, index);
            }
        }
    }
    return out;
}

// ---- comparison helpers -----------------------------------------------------------------------------
[[nodiscard]] bool approxEq(float a, float b, float epsilon = 1e-5F) {
    const float diff = a - b;
    return (diff < 0.0F ? -diff : diff) <= epsilon;
}

[[nodiscard]] bool approxEq(engine::Vec3 a, engine::Vec3 b, float epsilon = 1e-5F) {
    return approxEq(a.x, b.x, epsilon) && approxEq(a.y, b.y, epsilon) && approxEq(a.z, b.z, epsilon);
}

// Every geometric field of one primitive against another, NAMES EXCLUDED. Used by AI18/AI22/AI23, where
// the two encodings agree about geometry and DELIBERATELY disagree about names -- assimp's binary STL
// loader names the root `<STL_BINARY>` and leaves the mesh unnamed, while the ASCII loader puts the
// `solid` name on both the mesh and its node. That asymmetry is the LOADER's, measured, not ours.
[[nodiscard]] bool primitivesMatch(const engine::editor::ImportedPrimitive& a,
                                   const engine::editor::ImportedPrimitive& b) {
    if (a.attributes != b.attributes || a.indices != b.indices || a.positions.size() != b.positions.size() ||
        a.normals.size() != b.normals.size() || a.colors.size() != b.colors.size() || a.uv0.size() != b.uv0.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        if (!approxEq(a.positions[i], b.positions[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.normals.size(); ++i) {
        if (!approxEq(a.normals[i], b.normals[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.colors.size(); ++i) {
        if (!approxEq(a.colors[i].x, b.colors[i].x) || !approxEq(a.colors[i].y, b.colors[i].y) ||
            !approxEq(a.colors[i].z, b.colors[i].z) || !approxEq(a.colors[i].w, b.colors[i].w)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.uv0.size(); ++i) {
        if (!approxEq(a.uv0[i].x, b.uv0[i].x) || !approxEq(a.uv0[i].y, b.uv0[i].y)) {
            return false;
        }
    }
    return approxEq(a.bounds.min, b.bounds.min) && approxEq(a.bounds.max, b.bounds.max);
}

[[nodiscard]] bool modelsMatchIgnoringNames(const engine::editor::ImportedModel& a,
                                            const engine::editor::ImportedModel& b) {
    if (a.meshes.size() != b.meshes.size() || a.summary.vertexCount != b.summary.vertexCount ||
        a.summary.triangleCount != b.summary.triangleCount || a.summary.primitiveCount != b.summary.primitiveCount) {
        return false;
    }
    for (std::size_t m = 0; m < a.meshes.size(); ++m) {
        if (a.meshes[m].primitives.size() != b.meshes[m].primitives.size()) {
            return false;
        }
        for (std::size_t p = 0; p < a.meshes[m].primitives.size(); ++p) {
            if (!primitivesMatch(a.meshes[m].primitives[p], b.meshes[m].primitives[p])) {
                return false;
            }
        }
        if (!approxEq(a.meshes[m].bounds.min, b.meshes[m].bounds.min) ||
            !approxEq(a.meshes[m].bounds.max, b.meshes[m].bounds.max)) {
            return false;
        }
    }
    return approxEq(a.summary.bounds.min, b.summary.bounds.min) && approxEq(a.summary.bounds.max, b.summary.bounds.max);
}

// %GEOMETRY% -> the shared triangle library. A placeholder rather than string concatenation so each
// fixture above still reads as ONE document, which is what makes it diffable beside its assertion.
[[nodiscard]] std::string dae(std::string_view templateText) {
    std::string out(templateText);
    const std::size_t at = out.find("%GEOMETRY%\n");
    if (at != std::string::npos) {
        out.replace(at, std::string_view("%GEOMETRY%\n").size(), DAE_TRIANGLE_GEOMETRY);
    }
    return out;
}

// AI42: a <node> chain `depth` levels deep, built here because a 300-level literal is not a fixture
// anybody can read. The geometry sits on the deepest node, so a truncated walk loses it.
[[nodiscard]] std::string buildDeepDae(unsigned int depth) {
    std::string out = R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
)";
    out += DAE_TRIANGLE_GEOMETRY;
    out += R"(  <library_visual_scenes><visual_scene id="S" name="S">
)";
    for (unsigned int i = 0; i < depth; ++i) {
        out += std::format(R"(<node id="n{}" name="n{}">)", i, i);
    }
    out += R"(<instance_geometry url="#g1"/>)";
    for (unsigned int i = 0; i < depth; ++i) {
        out += "</node>";
    }
    out += R"(
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";
    return out;
}

// A Z-up / Y-up pair describing the SAME solid at two declared unit scales. A-9's behaviour, made
// assertable: the loader bakes both the unit factor and the axis correction into the ROOT NODE's
// transform, never into the geometry.
[[nodiscard]] std::string buildSpacedDae(std::string_view meter, std::string_view upAxis) {
    return std::format(
        "<?xml version=\"1.0\"?>\n<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" "
        "version=\"1.4.1\">\n  <asset><unit meter=\"{}\" name=\"u\"/><up_axis>{}</up_axis></asset>\n{}"
        "  <library_visual_scenes><visual_scene id=\"S\" name=\"S\">\n"
        "    <node id=\"N\" name=\"N\"><instance_geometry url=\"#g1\"/></node>\n"
        "  </visual_scene></library_visual_scenes>\n  <scene><instance_visual_scene url=\"#S\"/></scene>\n"
        "</COLLADA>\n",
        meter, upAxis, DAE_TRIANGLE_GEOMETRY);
}

// The one surviving primitive of a single-mesh model. REQUIREs the shape rather than indexing blindly:
// a shape regression must fail in the case that owns it, not crash a later positional read (2.6.2's
// shapedGroups lesson).
[[nodiscard]] const engine::editor::ImportedPrimitive& onlyPrimitive(const engine::editor::ImportedModel& model) {
    REQUIRE(model.meshes.size() == 1U);
    REQUIRE(model.meshes[0].primitives.size() == 1U);
    return model.meshes[0].primitives[0];
}

}  // namespace

using engine::editor::has;
using engine::editor::ImportDepth;
using engine::editor::ImportedPrimitive;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::isImportableModelName;
using engine::editor::modelImporterNeedsExternalBuffers;
using engine::editor::VertexAttribute;

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

    // `Get(AI_METADATA_SOURCE_FORMAT` and NOT the bare key, which is GAP-CLOSING (sabotage seed S4):
    // the bare token is a PREFIX of AI_METADATA_SOURCE_FORMAT_VERSION, an unrelated key this file reads
    // for a display string, so deleting the whole loader assertion left the naive check green.
    for (const std::string_view required :
         {"ReadFileFromMemory(", "SetIOHandler(new", "Get(AI_METADATA_SOURCE_FORMAT,", "expectedLoaderFragment",
          "ImportStatus::Malformed", "aiProcess_ValidateDataStructure", "aiProcess_Triangulate",
          "AI_CONFIG_PP_SBP_REMOVE", "AI_CONFIG_PP_LBW_MAX_WEIGHTS"}) {
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
            importModel(fixture.name, "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
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
            importModel(fixture.name, "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
        const ImportResult full =
            importModel(fixture.name, "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
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
    const std::string longLine = "<COLLADA>" + std::string(std::size_t{1024} * 1024, 'a') + "</COLLADA>";
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

// ---- step 4: .stl and .ply at Full depth -------------------------------------------------------------

// AI17 (AC-21) -- ONE ImportedMesh per aiMesh with ONE primitive inside it: aiMesh carries exactly one
// material index, so 3.2.3's first-appearance bucketing has no analogue here.
TEST_CASE("AI17: a one-triangle ASCII .stl yields one mesh, one primitive and three source-order indices") {
    const ImportResult result =
        importModel("t.stl", "", asBytes(TRIANGLE_STL), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);

    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    REQUIRE(prim.positions.size() == 3U);
    CHECK(approxEq(prim.positions[0], engine::Vec3{0.0F, 0.0F, 0.0F}));
    CHECK(approxEq(prim.positions[1], engine::Vec3{1.0F, 0.0F, 0.0F}));
    CHECK(approxEq(prim.positions[2], engine::Vec3{0.0F, 1.0F, 0.0F}));
    CHECK(prim.indices == std::vector<std::uint32_t>{0U, 1U, 2U});
    CHECK(prim.attributes == (VertexAttribute::Position | VertexAttribute::Normal));
    CHECK(approxEq(prim.bounds.min, engine::Vec3{0.0F, 0.0F, 0.0F}));
    CHECK(approxEq(prim.bounds.max, engine::Vec3{1.0F, 1.0F, 0.0F}));
    CHECK(approxEq(result.model.summary.bounds.min, engine::Vec3{0.0F, 0.0F, 0.0F}));
    CHECK(approxEq(result.model.summary.bounds.max, engine::Vec3{1.0F, 1.0F, 0.0F}));
    CHECK(result.model.summary.vertexCount == 3U);
    CHECK(result.model.summary.triangleCount == 1U);
    CHECK(result.model.summary.primitiveCount == 1U);
    CHECK(result.model.meshes[0].name == "Tri");  // ASCII STL: the `solid` name IS the mesh name
}

// AI18 (AC-22) -- the byte-identical BINARY twin of the same solid, built here rather than committed.
// EQUAL FIELD FOR FIELD EXCEPT EVERY NAME, and the name asymmetry is the LOADER's, measured against the
// pinned source: the binary path names the root `<STL_BINARY>` and leaves the mesh unnamed, while the
// ASCII path puts the `solid` name on BOTH the mesh and its node. The plan predicted one name site; the
// tree has three.
TEST_CASE("AI18: an ASCII .stl and its binary twin yield the same geometry and differ only in names") {
    const ImportResult ascii = importModel("t.stl", "", asBytes(TRIANGLE_STL), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(ascii.status == ImportStatus::Ok);

    const std::vector<Tri> tris = {
        Tri{{0.0F, 0.0F, 1.0F}, {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}}}};
    const std::string binary = buildBinaryStl("binary header, not a name", tris, {});
    const ImportResult decoded = importModel("t.stl", "", asBytes(binary), ImportSettings{}, ImportDepth::Full, {});
    INFO("binary message: ", decoded.message);
    REQUIRE(decoded.status == ImportStatus::Ok);

    CHECK(modelsMatchIgnoringNames(ascii.model, decoded.model));
    CHECK(decoded.model.meshes[0].name != "Tri");
}

// AI19 (AC-23) -- the 16-bit per-face colour extension. With the high bit set the loader produces a
// colour channel; WITHOUT it there is none, and NO white is fabricated.
TEST_CASE("AI19: a binary .stl with per-face colours yields Color0 and one without yields none") {
    const std::vector<Tri> tris = {
        Tri{{0.0F, 0.0F, 1.0F}, {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}}}};
    // bit 15 set == "this face carries a colour"; the low 15 bits are 5:5:5.
    const std::string coloured = buildBinaryStl("colours", tris, {static_cast<std::uint16_t>(0x8000U | 0x001FU)});
    const ImportResult withColour =
        importModel("t.stl", "", asBytes(coloured), ImportSettings{}, ImportDepth::Full, {});
    INFO("coloured message: ", withColour.message);
    REQUIRE(withColour.status == ImportStatus::Ok);
    const ImportedPrimitive& colouredPrim = onlyPrimitive(withColour.model);
    CHECK(has(colouredPrim.attributes, VertexAttribute::Color0));
    REQUIRE(colouredPrim.colors.size() == 3U);
    CHECK(approxEq(colouredPrim.colors[0].w, 1.0F));

    const std::string plain = buildBinaryStl("no colours", tris, {});
    const ImportResult withoutColour =
        importModel("t.stl", "", asBytes(plain), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(withoutColour.status == ImportStatus::Ok);
    const ImportedPrimitive& plainPrim = onlyPrimitive(withoutColour.model);
    CHECK_FALSE(has(plainPrim.attributes, VertexAttribute::Color0));
    CHECK(plainPrim.colors.empty());
}

// AI20 (E2) -- THE CLASSIC STL MISDETECTION, pinned. A binary file whose 80-byte header begins with the
// ASCII text `solid` is still binary: the loader decides on the EXACT file size (84 + 50*faces), never
// on the leading token.
TEST_CASE("AI20: a binary .stl whose header begins with the text 'solid' still imports as binary") {
    const std::vector<Tri> tris = {
        Tri{{0.0F, 0.0F, 1.0F}, {{{0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}}}}};
    const std::string binary = buildBinaryStl("solid MisleadingHeader", tris, {});
    const ImportResult result = importModel("t.stl", "", asBytes(binary), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    REQUIRE(prim.positions.size() == 3U);
    CHECK(approxEq(prim.positions[1], engine::Vec3{2.0F, 0.0F, 0.0F}));
}

// AI21 (E1) -- an ASCII `.stl` whose solid carries no name: one mesh, name "".
TEST_CASE("AI21: an ASCII .stl with no solid name yields one unnamed mesh") {
    const std::string unnamed =
        "solid\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
        "endloop\nendfacet\nendsolid\n";
    const ImportResult result = importModel("t.stl", "", asBytes(unnamed), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1U);
    CHECK(result.model.meshes[0].name.empty());
}

// AI22 (AC-24) -- an ASCII `.ply` cube and its little-endian binary twin, both built from ONE vertex and
// face list, must decode to the same model.
TEST_CASE("AI22: an ASCII .ply and its little-endian binary twin yield equal models") {
    const std::string ascii = buildAsciiPly();
    const ImportResult asciiResult =
        importModel("cube.ply", "", asBytes(ascii), ImportSettings{}, ImportDepth::Full, {});
    INFO("ascii message: ", asciiResult.message);
    REQUIRE(asciiResult.status == ImportStatus::Ok);

    const std::string binary = buildBinaryPly(false);
    const ImportResult binaryResult =
        importModel("cube.ply", "", asBytes(binary), ImportSettings{}, ImportDepth::Full, {});
    INFO("binary message: ", binaryResult.message);
    REQUIRE(binaryResult.status == ImportStatus::Ok);

    CHECK(modelsMatchIgnoringNames(asciiResult.model, binaryResult.model));
    CHECK(asciiResult.model.summary.vertexCount == 8U);
    CHECK(asciiResult.model.summary.triangleCount == 12U);
}

// AI23 (E4) -- byte-order handling PROVEN rather than assumed.
TEST_CASE("AI23: a big-endian binary .ply decodes to the same model as its little-endian twin") {
    const std::string little = buildBinaryPly(false);
    const std::string big = buildBinaryPly(true);
    CHECK(little != big);  // the two encodings really are different bytes

    const ImportResult littleResult =
        importModel("cube.ply", "", asBytes(little), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(littleResult.status == ImportStatus::Ok);
    const ImportResult bigResult = importModel("cube.ply", "", asBytes(big), ImportSettings{}, ImportDepth::Full, {});
    INFO("big-endian message: ", bigResult.message);
    REQUIRE(bigResult.status == ImportStatus::Ok);
    CHECK(modelsMatchIgnoringNames(littleResult.model, bigResult.model));
}

// AI24 (AC-25) -- `red green blue` vertex properties become Color0; without them there is no channel and
// no fabricated white.
TEST_CASE("AI24: .ply vertex colours become Color0, and their absence leaves no channel") {
    const std::string coloured =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "0 0 0 255 0 0\n1 0 0 0 255 0\n0 1 0 0 0 255\n3 0 1 2\n";
    const ImportResult result = importModel("c.ply", "", asBytes(coloured), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    CHECK(has(prim.attributes, VertexAttribute::Color0));
    REQUIRE(prim.colors.size() == 3U);
    CHECK(approxEq(prim.colors[0].x, 1.0F));
    CHECK(approxEq(prim.colors[0].y, 0.0F));
    CHECK(approxEq(prim.colors[0].w, 1.0F));  // alpha assumed 1 when the file declares none

    const ImportResult plain = importModel("t.ply", "", asBytes(TRIANGLE_PLY), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(plain.status == ImportStatus::Ok);
    CHECK_FALSE(has(onlyPrimitive(plain.model).attributes, VertexAttribute::Color0));
    CHECK(onlyPrimitive(plain.model).colors.empty());
}

// AI25 (AC-26) -- BOTH spellings of a PLY texture coordinate: `s`/`t` and `u`/`v`. Two arms because the
// loader matches them in two separate branches.
TEST_CASE("AI25: .ply texture coordinates become TexCoord0 for both the s/t and u/v spellings") {
    const auto build = [](std::string_view first, std::string_view second) {
        return std::format(
            "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
            "property float {}\nproperty float {}\n"
            "element face 1\nproperty list uchar int vertex_index\nend_header\n"
            "0 0 0 0.25 0.5\n1 0 0 0.75 0.5\n0 1 0 0.5 0.9\n3 0 1 2\n",
            first, second);
    };
    for (const auto& spelling : {std::pair<std::string_view, std::string_view>{"s", "t"},
                                 std::pair<std::string_view, std::string_view>{"u", "v"}}) {
        const std::string text = build(spelling.first, spelling.second);
        const ImportResult result = importModel("uv.ply", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
        INFO("spelling: ", spelling.first, spelling.second, " message: ", result.message);
        REQUIRE(result.status == ImportStatus::Ok);
        const ImportedPrimitive& prim = onlyPrimitive(result.model);
        CHECK(has(prim.attributes, VertexAttribute::TexCoord0));
        REQUIRE(prim.uv0.size() == 3U);
        CHECK(approxEq(prim.uv0[0].x, 0.25F));
        CHECK(approxEq(prim.uv0[0].y, 0.5F));
    }

    const ImportResult plain = importModel("t.ply", "", asBytes(TRIANGLE_PLY), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(plain.status == ImportStatus::Ok);
    CHECK_FALSE(has(onlyPrimitive(plain.model).attributes, VertexAttribute::TexCoord0));
    CHECK(onlyPrimitive(plain.model).uv0.empty());
}

// AI26 (AC-30) -- THE IMPORTER CONVERTS NOTHING. A position in the file is that position in the model,
// and a triangle's index order is the file's order. Hand-written expected values, no derivation.
TEST_CASE("AI26: .stl and .ply positions and winding survive verbatim") {
    const std::string stl =
        "solid Verbatim\nfacet normal 0 0 1\nouter loop\nvertex 1 2 3\nvertex 4 5 6\nvertex 7 8 9\n"
        "endloop\nendfacet\nendsolid Verbatim\n";
    const ImportResult stlResult = importModel("v.stl", "", asBytes(stl), ImportSettings{}, ImportDepth::Full, {});
    INFO("stl message: ", stlResult.message);
    REQUIRE(stlResult.status == ImportStatus::Ok);
    const ImportedPrimitive& stlPrim = onlyPrimitive(stlResult.model);
    REQUIRE(stlPrim.positions.size() == 3U);
    CHECK(approxEq(stlPrim.positions[0], engine::Vec3{1.0F, 2.0F, 3.0F}));
    CHECK(approxEq(stlPrim.positions[1], engine::Vec3{4.0F, 5.0F, 6.0F}));
    CHECK(approxEq(stlPrim.positions[2], engine::Vec3{7.0F, 8.0F, 9.0F}));
    CHECK(stlPrim.indices == std::vector<std::uint32_t>{0U, 1U, 2U});

    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "1 2 3\n4 5 6\n7 8 9\n3 2 0 1\n";
    const ImportResult plyResult = importModel("v.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    INFO("ply message: ", plyResult.message);
    REQUIRE(plyResult.status == ImportStatus::Ok);
    const ImportedPrimitive& plyPrim = onlyPrimitive(plyResult.model);
    REQUIRE(plyPrim.positions.size() == 3U);
    CHECK(approxEq(plyPrim.positions[0], engine::Vec3{1.0F, 2.0F, 3.0F}));
    CHECK(approxEq(plyPrim.positions[2], engine::Vec3{7.0F, 8.0F, 9.0F}));
    CHECK(plyPrim.indices == std::vector<std::uint32_t>{2U, 0U, 1U});  // the FILE's order, not sorted
}

// AI27 (AC-29) -- ImportSettings::scale multiplies POSITIONS and the bounds folded from them, and NOTHING
// else: normals, UVs and colours are untouched (import_settings.hpp's own rule, A22).
TEST_CASE("AI27: scale doubles positions and bounds and leaves normals, UVs and colours alone") {
    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\nproperty float s\nproperty float t\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "0 0 0 0 0 1 0.25 0.5 255 0 0\n1 0 0 0 0 1 0.75 0.5 0 255 0\n0 1 0 0 0 1 0.5 0.9 0 0 255\n3 0 1 2\n";
    ImportSettings scaled;
    scaled.scale = 2.0F;
    const ImportResult plain = importModel("s.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult doubled = importModel("s.ply", "", asBytes(ply), scaled, ImportDepth::Full, {});
    INFO("message: ", doubled.message);
    REQUIRE(plain.status == ImportStatus::Ok);
    REQUIRE(doubled.status == ImportStatus::Ok);

    const ImportedPrimitive& a = onlyPrimitive(plain.model);
    const ImportedPrimitive& b = onlyPrimitive(doubled.model);
    REQUIRE(a.positions.size() == b.positions.size());
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        CHECK(approxEq(b.positions[i], a.positions[i] * 2.0F));
    }
    CHECK(approxEq(b.bounds.max, a.bounds.max * 2.0F));
    CHECK(approxEq(doubled.model.summary.bounds.max, plain.model.summary.bounds.max * 2.0F));
    REQUIRE(a.normals.size() == b.normals.size());
    for (std::size_t i = 0; i < a.normals.size(); ++i) {
        CHECK(approxEq(b.normals[i], a.normals[i]));
    }
    REQUIRE(a.uv0.size() == b.uv0.size());
    for (std::size_t i = 0; i < a.uv0.size(); ++i) {
        CHECK(approxEq(b.uv0[i].x, a.uv0[i].x));
        CHECK(approxEq(b.uv0[i].y, a.uv0[i].y));
    }
    REQUIRE(a.colors.size() == b.colors.size());
    for (std::size_t i = 0; i < a.colors.size(); ++i) {
        CHECK(approxEq(b.colors[i].x, a.colors[i].x));
    }
}

// AI28 (AC-31) -- INV-A5, over every fixture: `attributes` never claims a bit whose array is empty, and
// `positions`/`indices` are never empty on a primitive that survives.
TEST_CASE("AI28: attributes never claims a bit whose array is empty, on any fixture") {
    for (const Fixture& fixture : fixtureTable()) {
        const ImportResult result =
            importModel(fixture.name, "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
        INFO("fixture: ", fixture.name, " message: ", result.message);
        for (const engine::editor::ImportedMesh& mesh : result.model.meshes) {
            for (const ImportedPrimitive& prim : mesh.primitives) {
                CHECK_FALSE(prim.positions.empty());
                CHECK_FALSE(prim.indices.empty());
                CHECK(has(prim.attributes, VertexAttribute::Position));
                CHECK(has(prim.attributes, VertexAttribute::Normal) == !prim.normals.empty());
                CHECK(has(prim.attributes, VertexAttribute::Tangent) == !prim.tangents.empty());
                CHECK(has(prim.attributes, VertexAttribute::TexCoord0) == !prim.uv0.empty());
                CHECK(has(prim.attributes, VertexAttribute::TexCoord1) == !prim.uv1.empty());
                CHECK(has(prim.attributes, VertexAttribute::Color0) == !prim.colors.empty());
            }
        }
    }
}

// AI29 (AC-31) -- the ONE useful thing aiProcess_FindInvalidData would have done, taken over so the MESH
// SURVIVES it. An optional channel whose every element is exactly zero is dropped with a warning naming
// it; the mesh, its primitive and every count survive, which is exactly what that flag would destroy.
TEST_CASE("AI29: an all-zero normal channel is dropped with a warning and the mesh survives") {
    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "0 0 0 0 0 0\n1 0 0 0 0 0\n0 1 0 0 0 0\n3 0 1 2\n";
    const ImportResult result = importModel("z.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Normal));
    CHECK(prim.normals.empty());
    CHECK(prim.positions.size() == 3U);
    CHECK(prim.indices.size() == 3U);
    CHECK(result.model.summary.vertexCount == 3U);
    CHECK(result.model.summary.triangleCount == 1U);

    REQUIRE(result.warningTotal == 1U);
    REQUIRE(result.warnings.size() == 1U);
    CHECK(result.warnings[0].find("normals") != std::string::npos);
}

// AI30 (AC-31) -- and the EXCEPTION that makes AI29 safe: an all-zero POSITION array is legitimate
// geometry at the origin. Dropping it would delete a real, if degenerate, model with no warning at all.
TEST_CASE("AI30: an all-zero position array imports as geometry at the origin, with no warning") {
    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "0 0 0\n0 0 0\n0 0 0\n3 0 1 2\n";
    const ImportResult result = importModel("o.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    REQUIRE(prim.positions.size() == 3U);
    for (const engine::Vec3& position : prim.positions) {
        CHECK(approxEq(position, engine::Vec3{}));
    }
    CHECK(has(prim.attributes, VertexAttribute::Position));
    CHECK(prim.bounds.valid());  // a POINT box at the origin, never the Aabb::empty() sentinel
    CHECK(result.warnings.empty());
    CHECK(result.warningTotal == 0U);
}

// AI31 (AC-32, CORRECTED against the library) -- a face indexing past the vertex count is REFUSED WHOLE,
// as Malformed, and the run is clean under ASan and UBSan.
//
// The plan predicted "the face is dropped with one capped warning and the status stays Ok". MEASURED, it
// cannot be: aiProcess_ValidateDataStructure is ON (A-6b) and ValidateDSProcess runs FIRST -- before
// ScenePreprocessor and before every other post-process step -- where
// `if (face.mIndices[a] >= pMesh->mNumVertices) ReportError(...)` throws DeadlyImportError("Validation
// failed: ..."). Nothing downstream can introduce an out-of-range index either, so OUR OWN range check
// (INV-A4) is unreachable while the flag is on. It stays, as the defence in depth it was always
// described as, and AI34 is its cover -- there is no behavioural one to have.
TEST_CASE("AI31: a .ply face indexing past the vertex count is refused as Malformed") {
    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_index\nend_header\n"
        "0 0 0\n1 0 0\n0 1 0\n3 0 1 99\n";
    const ImportResult result = importModel("bad.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.status == ImportStatus::Malformed);
    CHECK_FALSE(result.message.empty());
    CHECK(result.message.find("alidation") != std::string::npos);
}

// AI32 (AC-56) -- a .ply header that LIES about its element counts is refused BEFORE the library sees
// it, by the one pre-allocation bound in this task. Without that check the loader works from the
// declared count and grinds at unbounded, climbing resident memory (MEASURED: over seventeen minutes on
// a 120-byte file). The case completing at all is part of the assertion.
TEST_CASE("AI32: a .ply declaring more elements than the file can hold is refused as Malformed") {
    const std::string ply =
        "ply\nformat ascii 1.0\nelement vertex 1000000000\nproperty float x\nproperty float y\n"
        "property float z\nend_header\n0 0 0\n";
    const ImportResult result = importModel("lie.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.status == ImportStatus::Malformed);
    CHECK_FALSE(result.message.empty());

    // And the same file at Structure depth is Ok with no URIs -- the library is never entered there, so
    // the pre-check is not what makes Structure safe; not entering is.
    const ImportResult structure =
        importModel("lie.ply", "", asBytes(ply), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(structure.status == ImportStatus::Ok);
    CHECK(structure.externalUris.empty());
}

// AI33 (E3) -- the .stl half of the same question, and it needs NO first-party check: STLLoader.cpp
// compares the declared facet count against the file size in 64-bit arithmetic BEFORE its own
// `new aiVector3D[mNumFaces * 3]`, and IsBinarySTL demands an EXACT size match, so an over-declaring
// file is not even recognised as binary. An added .stl pre-check would be dead code duplicating a
// guarantee the loader already makes -- this case is what proves the guarantee is real.
TEST_CASE("AI33: an .stl declaring more triangles than the file contains fails without a first-party check") {
    const std::vector<Tri> tris = {
        Tri{{0.0F, 0.0F, 1.0F}, {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}}}};
    std::string lying = buildBinaryStl("over-declared", tris, {});
    // Rewrite the facet count in place: 4 million facets, in a file holding one.
    const std::uint32_t absurd = 4000000U;
    for (unsigned int i = 0; i < 4U; ++i) {
        lying[80U + i] = static_cast<char>((absurd >> (i * 8U)) & 0xFFU);
    }
    const ImportResult result = importModel("lie.stl", "", asBytes(lying), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.status != ImportStatus::Ok);
    CHECK_FALSE(result.message.empty());
}

// AI34 -- THE GUARDS NO RUNTIME CASE CAN REACH, pinned in the source text, because their absence is not
// a failing CHECK.
//
//  * plyDeclaredCountsExceedBytes: deleting the CALL makes the suite HANG, not fail (AI32's fixture
//    grinds for minutes at climbing RSS), so no runtime case can cover it. The ORDER matters as much as
//    the call: it must run AFTER seedPlyExternalUris, so AC-19's depth equality still holds on a refused
//    file, and BEFORE runAssimp, or it protects nothing.
//  * the two INV-A4 range checks: aiProcess_ValidateDataStructure refuses an out-of-range face index or
//    weight vertex id FIRST (AI31), and nothing downstream can introduce one, so both checks are
//    unreachable while the flag is on. The flag is reversible in one token (R5), and these checks are
//    what stand behind it when it is -- so they are pinned rather than deleted as dead code.
//
// This is 3.2.4's blocking SDL_WaitProcess finding, one task on: where the failure mode is a HANG or a
// masked branch rather than a red case, the comment-stripped source IS the cover.
TEST_CASE("AI34: the ply pre-check and the two range checks are present, in the order that makes them work") {
    const std::string code = strippedSource("assimp_import.cpp");
    REQUIRE_FALSE(code.empty());

    const std::size_t precheck = code.find("plyDeclaredCountsExceedBytes(bytes)");
    const std::size_t run = code.find("runAssimp(bytes,");
    REQUIRE(precheck != std::string::npos);
    REQUIRE(run != std::string::npos);
    const std::size_t seed = code.rfind("seedPlyExternalUris(bytes", precheck);
    REQUIRE(seed != std::string::npos);
    CHECK(seed < precheck);
    CHECK(precheck < run);

    CHECK(code.find("face.mIndices[k] >= src.mNumVertices") != std::string::npos);
    // GAP-CLOSING (sabotage seed S16): the WEIGHT range check had no cover of any kind -- no runtime
    // case can reach it and nothing pinned it in the source, so deleting it was invisible.
    CHECK(code.find("vw.mVertexId >= src.mNumVertices") != std::string::npos);
    // And the defensive non-triangle check, unreachable after aiProcess_Triangulate (seed S17), which
    // is pinned here rather than left as a branch a refactor could delete without consequence.
    CHECK(code.find("face.mNumIndices != 3") != std::string::npos);
}

// AI35 -- summary.bounds is folded from surviving PRIMITIVES, never from mesh bounds. 3.2.3 confirmed by
// sabotage that these are two genuinely independent properties. The fixture's geometry deliberately
// EXCLUDES the origin: `outMesh.bounds` is still a default point box at (0,0,0) when the summary is
// folded, so a seed reading it there drags the model bounds back to the origin and this case goes red.
TEST_CASE("AI35: summary.bounds is folded from primitives and never from a mesh-level point box") {
    const std::string stl =
        "solid Offset\nfacet normal 0 0 1\nouter loop\nvertex 10 10 10\nvertex 11 10 10\nvertex 10 11 10\n"
        "endloop\nendfacet\nendsolid Offset\n";
    const ImportResult result = importModel("off.stl", "", asBytes(stl), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.summary.bounds.valid());
    CHECK(approxEq(result.model.summary.bounds.min, engine::Vec3{10.0F, 10.0F, 10.0F}));
    CHECK(approxEq(result.model.summary.bounds.max, engine::Vec3{11.0F, 11.0F, 10.0F}));
    CHECK(approxEq(result.model.meshes[0].bounds.min, engine::Vec3{10.0F, 10.0F, 10.0F}));
}

// ---- step 5: .dae hierarchy and its Structure/Full split ---------------------------------------------

// AI36 (AC-33) -- a three-level <node> chain. Assimp makes the <visual_scene> ITSELF the root node, so
// the document's three nodes arrive as four, and `parent`/`children` must agree in both directions.
TEST_CASE("AI36: a .dae node chain becomes a consistent four-node hierarchy with one root") {
    const std::string text = dae(DAE_CHAIN);
    const ImportResult result = importModel("chain.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 4U);
    CHECK(result.model.roots == std::vector<std::uint32_t>{0U});
    CHECK(result.model.summary.nodeCount == 4U);

    CHECK(result.model.nodes[0].parent == engine::editor::INVALID_SUBASSET);
    for (std::size_t i = 0; i < result.model.nodes.size(); ++i) {
        for (const std::uint32_t child : result.model.nodes[i].children) {
            REQUIRE(child < result.model.nodes.size());
            CHECK(result.model.nodes[child].parent == i);
        }
    }
    // The document's own order down the chain: S -> A -> B -> C.
    CHECK(result.model.nodes[1].name == "A");
    CHECK(result.model.nodes[2].name == "B");
    CHECK(result.model.nodes[3].name == "C");
    CHECK(approxEq(result.model.nodes[1].translation, engine::Vec3{1.0F, 2.0F, 3.0F}));
    CHECK(approxEq(result.model.nodes[2].scale, engine::Vec3{2.0F, 2.0F, 2.0F}));
    CHECK(result.model.nodes[3].meshIndex == 0U);
}

// AI37 (AC-34) -- nodes[i].localId == i, on EVERY fixture. The coincidence is real for glTF and OBJ too
// and FALSE for FBX, and 3.2.2's BLOCKING ASan heap-buffer-overflow was the panel leaning on it. Pinning
// it here means a future change that breaks it is CAUGHT rather than discovered in the panel.
TEST_CASE("AI37: every node's localId equals its index in ImportedModel::nodes") {
    std::vector<std::string> texts;
    for (const Fixture& fixture : fixtureTable()) {
        texts.emplace_back(fixture.text);
    }
    texts.push_back(dae(DAE_CHAIN));
    texts.push_back(dae(DAE_SIBLINGS));
    texts.emplace_back(DAE_TWO_MESHES_ONE_NODE);

    for (std::size_t t = 0; t < texts.size(); ++t) {
        const std::string_view name = t < fixtureTable().size() ? fixtureTable()[t].name : std::string_view("x.dae");
        const ImportResult result = importModel(name, "", asBytes(texts[t]), ImportSettings{}, ImportDepth::Full, {});
        INFO("index: ", t, " name: ", name);
        for (std::size_t i = 0; i < result.model.nodes.size(); ++i) {
            CHECK(result.model.nodes[i].localId == i);
        }
    }
}

// AI38 -- PRE-ORDER DOCUMENT ORDER. The walk pushes children in REVERSE so the first is popped first;
// getting that backwards is invisible in a one-child fixture and wrong in every real file.
TEST_CASE("AI38: sibling nodes arrive in document order") {
    const std::string text = dae(DAE_SIBLINGS);
    const ImportResult result = importModel("sib.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 4U);
    CHECK(result.model.nodes[1].name == "a");
    CHECK(result.model.nodes[2].name == "b");
    CHECK(result.model.nodes[3].name == "c");
}

// AI39 (AC-27) -- aiProcess_Triangulate's 3(n-2) split, asserted on a quad and a pentagon. The quad also
// proves the split REUSES vertices rather than duplicating them.
TEST_CASE("AI39: a quad becomes six indices over four vertices and a pentagon becomes nine") {
    const std::string text(DAE_POLYGONS);
    const ImportResult result = importModel("poly.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 2U);

    std::size_t quad = result.model.meshes.size();
    std::size_t pentagon = result.model.meshes.size();
    for (std::size_t m = 0; m < result.model.meshes.size(); ++m) {
        REQUIRE(result.model.meshes[m].primitives.size() == 1U);
        if (result.model.meshes[m].primitives[0].positions.size() == 4U) {
            quad = m;
        } else if (result.model.meshes[m].primitives[0].positions.size() == 5U) {
            pentagon = m;
        }
    }
    REQUIRE(quad < result.model.meshes.size());
    REQUIRE(pentagon < result.model.meshes.size());
    CHECK(result.model.meshes[quad].primitives[0].indices.size() == 6U);
    CHECK(result.model.meshes[pentagon].primitives[0].indices.size() == 9U);
    CHECK(result.model.summary.triangleCount == 5U);  // 2 + 3
}

// AI40 (AC-28, CORRECTED against the library) -- point and line primitives are REMOVED, and removing the
// last mesh in a document is a hard refusal, not an empty mesh.
//
// The plan predicted "a mesh with zero primitives and one warning naming the counts, and the mesh
// SURVIVES". MEASURED: aiProcess_SortByPType + AI_CONFIG_PP_SBP_REMOVE DELETES a mesh whose only
// primitive type is removed and updates the node graph around it, then throws
// DeadlyImportError("No meshes remaining") when nothing is left. So a lines-only .dae is refused, and a
// mixed one keeps exactly the triangle mesh. Our own "no triangles survived" arm stays for the same
// reason the range check does (it is what a validation-off build would need) and has no live cover.
TEST_CASE("AI40: a lines-only .dae is refused and a mixed one keeps only the triangle mesh") {
    constexpr std::string_view LINES_ONLY =
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries><geometry id="l1" name="Line"><mesh>
    <source id="lp"><float_array id="lpa" count="6">0 0 0 1 0 0</float_array>
      <technique_common><accessor source="#lpa" count="2" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="lv"><input semantic="POSITION" source="#lp"/></vertices>
    <lines count="1"><input semantic="VERTEX" source="#lv" offset="0"/><p>0 1</p></lines>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="L" name="L"><instance_geometry url="#l1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";
    const std::string linesOnly(LINES_ONLY);
    const ImportResult refused =
        importModel("lines.dae", "", asBytes(linesOnly), ImportSettings{}, ImportDepth::Full, {});
    INFO("lines-only message: ", refused.message);
    CHECK(refused.status != ImportStatus::Ok);
    CHECK_FALSE(refused.message.empty());

    // The SAME line geometry beside a triangle one, in ONE visual scene: the line mesh is removed and
    // the triangle mesh survives -- so the removal is a per-mesh decision, not a whole-document one.
    constexpr std::string_view MIXED =
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries>
    <geometry id="l1" name="Line"><mesh>
      <source id="lp"><float_array id="lpa" count="6">0 0 0 1 0 0</float_array>
        <technique_common><accessor source="#lpa" count="2" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="lv"><input semantic="POSITION" source="#lp"/></vertices>
      <lines count="1"><input semantic="VERTEX" source="#lv" offset="0"/><p>0 1</p></lines>
    </mesh></geometry>
    <geometry id="g1" name="Tri"><mesh>
      <source id="p"><float_array id="pa" count="9">0 0 0 1 0 0 0 1 0</float_array>
        <technique_common><accessor source="#pa" count="3" stride="3">
          <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
        </accessor></technique_common></source>
      <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
      <triangles count="1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 1 2</p></triangles>
    </mesh></geometry>
  </library_geometries>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="L" name="L"><instance_geometry url="#l1"/></node>
    <node id="T" name="T"><instance_geometry url="#g1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";
    const std::string mixed(MIXED);
    const ImportResult kept = importModel("mixed.dae", "", asBytes(mixed), ImportSettings{}, ImportDepth::Full, {});
    INFO("mixed message: ", kept.message);
    REQUIRE(kept.status == ImportStatus::Ok);
    REQUIRE(kept.model.meshes.size() == 1U);  // the line mesh is GONE, not empty
    REQUIRE(kept.model.meshes[0].primitives.size() == 1U);
    CHECK(kept.model.meshes[0].primitives[0].indices.size() == 3U);
}

// AI41 (AC-49) -- ImportedNode::meshIndex holds exactly ONE index, so a node instancing two geometries is
// split into a parent plus one child named `<parent>.1`, and the split is NAMED once, never per mesh.
TEST_CASE("AI41: a node referencing two meshes becomes two nodes and exactly one warning") {
    const std::string text(DAE_TWO_MESHES_ONE_NODE);
    const ImportResult result = importModel("both.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 3U);  // the visual scene, the node, and its split child
    CHECK(result.model.nodes[1].name == "Both");
    CHECK(result.model.nodes[2].name == "Both.1");
    CHECK(result.model.nodes[2].parent == 1U);
    CHECK(result.model.nodes[1].children == std::vector<std::uint32_t>{2U});
    CHECK(result.model.nodes[1].meshIndex == 0U);
    CHECK(result.model.nodes[2].meshIndex == 1U);

    std::size_t splitWarnings = 0;
    for (const std::string& warning : result.warnings) {
        if (warning.find("split into") != std::string::npos) {
            ++splitWarnings;
        }
    }
    CHECK(splitWarnings == 1U);
}

// AI42 (AC-53) -- MAX_NODE_DEPTH. Assimp has no node-depth limit of its own, and the walk is ITERATIVE,
// so this must redden a CHECK rather than crash: a crash here means the walk was made recursive.
TEST_CASE("AI42: a 300-deep .dae node chain truncates with a message naming the depth limit") {
    const std::string text = buildDeepDae(300U);
    const ImportResult result = importModel("deep.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("depth") != std::string::npos);
    // A COHERENT smaller model: every child index still resolves, and no node claims a parent it is not
    // a child of.
    CHECK(result.model.nodes.size() <= engine::editor::MAX_NODE_DEPTH + 2U);
    for (std::size_t i = 0; i < result.model.nodes.size(); ++i) {
        for (const std::uint32_t child : result.model.nodes[i].children) {
            REQUIRE(child < result.model.nodes.size());
            CHECK(result.model.nodes[child].parent == i);
        }
    }
}

// AI43 (AC-18) -- .dae at Structure depth: IDENTITY survives, CONTENT does not. glTF's own split, and
// the reason .dae parses at both depths while .stl/.ply skip the library entirely.
TEST_CASE("AI43: .dae at Structure depth keeps nodes and mesh identity and loses every sample") {
    const std::string text = dae(DAE_CHAIN);
    const ImportResult result =
        importModel("chain.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Structure, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    CHECK(result.model.nodes.size() == 4U);
    REQUIRE(result.model.meshes.size() == 1U);
    CHECK(result.model.meshes[0].primitives.empty());
    CHECK(result.model.meshes[0].bounds.valid());  // a POINT box, never the Aabb::empty() sentinel
    CHECK(result.model.summary.vertexCount == 0U);
    CHECK(result.model.summary.triangleCount == 0U);
    CHECK(result.model.summary.primitiveCount == 0U);
    CHECK(result.model.summary.animationDuration == 0.0F);
    CHECK_FALSE(result.model.summary.bounds.valid());
}

// AI44 (AC-35) -- A-9's BEHAVIOUR, asserted rather than described. Assimp's Collada loader bakes the
// declared unit and the up-axis correction into the ROOT NODE's transform, so a Z-up centimetre document
// and its Y-up metre twin have IDENTICAL mesh-local geometry and differ only in the hierarchy.
//
// The rotation is hand-computed: the loader post-multiplies the row-major matrix
// [[1,0,0],[0,0,1],[0,-1,0]], which maps (x,y,z) -> (x, z, -y) -- a -90 degree rotation about X, whose
// quaternion in engine::Quat's own {x,y,z,w} order is {-sin45, 0, 0, cos45}. A trap-2 reordering makes
// this read {0, 0, 0, ...} and the case goes red.
TEST_CASE("AI44: a Z-up centimetre .dae carries its conversion in the root transform, never in geometry") {
    const std::string centimetre = buildSpacedDae("0.01", "Z_UP");
    const std::string metre = buildSpacedDae("1", "Y_UP");
    const ImportResult cm = importModel("cm.dae", "", asBytes(centimetre), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult m = importModel("m.dae", "", asBytes(metre), ImportSettings{}, ImportDepth::Full, {});
    INFO("cm message: ", cm.message, " m message: ", m.message);
    REQUIRE(cm.status == ImportStatus::Ok);
    REQUIRE(m.status == ImportStatus::Ok);

    REQUIRE_FALSE(cm.model.nodes.empty());
    REQUIRE_FALSE(m.model.nodes.empty());
    CHECK(approxEq(cm.model.nodes[0].scale, engine::Vec3{0.01F, 0.01F, 0.01F}));
    CHECK(approxEq(cm.model.nodes[0].rotation.x, -0.70710678F, 1e-4F));
    CHECK(approxEq(cm.model.nodes[0].rotation.y, 0.0F, 1e-4F));
    CHECK(approxEq(cm.model.nodes[0].rotation.z, 0.0F, 1e-4F));
    CHECK(approxEq(cm.model.nodes[0].rotation.w, 0.70710678F, 1e-4F));

    CHECK(approxEq(m.model.nodes[0].scale, engine::Vec3{1.0F, 1.0F, 1.0F}));
    CHECK(approxEq(m.model.nodes[0].rotation.x, 0.0F, 1e-4F));
    CHECK(approxEq(m.model.nodes[0].rotation.w, 1.0F, 1e-4F));

    // The GEOMETRY is identical between the two -- the conversion lives in the hierarchy.
    CHECK(modelsMatchIgnoringNames(cm.model, m.model));
}

// AI45 (AC-36) -- the SourceSpace half of AI44, driven through importModel rather than through the pure
// helper, so importDae's own wiring of it is covered too.
TEST_CASE("AI45: .dae reports its declared unit and up-axis, and .ply/.stl declare nothing") {
    const std::string centimetre = buildSpacedDae("0.01", "Z_UP");
    const ImportResult cm = importModel("cm.dae", "", asBytes(centimetre), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(cm.status == ImportStatus::Ok);
    CHECK(cm.model.sourceSpace.declared);
    CHECK(approxEq(cm.model.sourceSpace.unitMeters, 0.01F));
    CHECK(cm.model.sourceSpace.upAxis == 'Z');

    const std::string metre = buildSpacedDae("1", "Y_UP");
    const ImportResult m = importModel("m.dae", "", asBytes(metre), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(m.status == ImportStatus::Ok);
    CHECK(m.model.sourceSpace.declared);
    CHECK(approxEq(m.model.sourceSpace.unitMeters, 1.0F));
    CHECK(m.model.sourceSpace.upAxis == 'Y');

    for (const std::string_view name : {std::string_view("t.ply"), std::string_view("t.stl")}) {
        const std::string_view text = name == "t.ply" ? TRIANGLE_PLY : TRIANGLE_STL;
        const ImportResult other = importModel(name, "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
        INFO("name: ", name);
        CHECK_FALSE(other.model.sourceSpace.declared);
    }
}

// AI46 (E8) -- a .dae with no <library_visual_scenes> is refused, and the LIBRARY'S OWN message is what
// the panel shows.
TEST_CASE("AI46: a .dae with no visual scene is refused with the library's own message") {
    constexpr std::string_view NO_SCENE =
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
</COLLADA>
)";
    const std::string text(NO_SCENE);
    const ImportResult result = importModel("empty.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.status != ImportStatus::Ok);
    CHECK(result.status != ImportStatus::Unsupported);
    CHECK_FALSE(result.message.empty());
}

// AI47 (E10) -- a CROSS-DOCUMENT <instance_geometry url="other.dae#g">. MEASURED: ColladaParser refuses
// it outright ("Unknown reference format", ColladaParser.cpp's `if (url[0] != '#') throw`), and the ONLY
// pIOHandler->Open call anywhere in the Collada sources is for the primary file. So no construct in any
// of this task's three formats can make the library request a second path at all.
//
// That is the reason sabotage seed S1 (deleting the SetIOHandler call) reddens only the source-text
// gate: RefusingIoSystem guards a door these three loaders never knock on, and it earns its place as
// defence for the primary open, for .zae, and for whatever a future Assimp bump changes. The sibling
// below carries a REAL geometry precisely so that "never read" is asserted behaviourally rather than
// assumed -- and the directory is listed before and after so "never written" is too.
TEST_CASE("AI47: a cross-document instance_geometry never opens the other file") {
    const TempDir temp;
    {
        // GAP-CLOSING (sabotage seed S1): the sibling carries a REAL geometry with a distinctive
        // coordinate. An empty <COLLADA/> proved nothing -- with the refusing handler removed, reading
        // it would still have produced no observable difference, so the seed reddened only AI3. This
        // vertex is what makes "the second document was never read" a BEHAVIOURAL assertion.
        std::ofstream sibling(temp.path() / "other.dae", std::ios::binary);
        sibling << dae(DAE_CHAIN);
    }
    const ScopedCwd cwd(temp.path());

    std::vector<std::string> before;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp.path())) {
        before.push_back(entry.path().filename().string());
    }

    std::string text = dae(DAE_CHAIN);
    const std::size_t at = text.find("<instance_geometry url=\"#g1\"/>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<instance_geometry url=\"#g1\"/>").size(),
                 "<instance_geometry url=\"other.dae#g1\"/>");
    const ImportResult result = importModel("cross.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    CHECK(result.externalUris.empty());
    // The sibling's geometry must appear NOWHERE: not a mesh, not a node named after one of its nodes.
    for (const engine::editor::ImportedMesh& mesh : result.model.meshes) {
        CHECK(mesh.primitives.empty());
    }
    for (const engine::editor::ImportedNode& node : result.model.nodes) {
        CHECK(node.meshIndex == engine::editor::INVALID_SUBASSET);
    }

    std::vector<std::string> after;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp.path())) {
        after.push_back(entry.path().filename().string());
    }
    CHECK(before == after);
}

// ---- step 6: materials and image references ----------------------------------------------------------

// The material fixture: three effects, deliberately different in the ways the cases care about.
//   e1 -- a diffuse TEXTURE (wood.png, clamped in U), an emission colour, a transparency, double-sided
//   e2 -- a BLACK diffuse colour and NO texture, which is a legitimately black material
//   e3 -- nothing declared at all, so Assimp's own Effect default (0.6 grey) is what arrives
//
// MEASURED, and it is why AI50 is driven from a .ply rather than from here: COLLADA CANNOT EXPRESS a
// black diffuse COLOUR together with a diffuse TEXTURE. <diffuse> holds either a <color> or a <texture>
// and ColladaLoader writes AI_MATKEY_COLOR_DIFFUSE unconditionally from Effect::mDiffuse, whose default
// is (0.6, 0.6, 0.6). So the zero-factor rule's paired arms have no Collada spelling at all.
constexpr std::string_view DAE_MATERIALS =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>
    <image id="i_diff"><init_from>wood.png</init_from></image>
  </library_images>
  <library_effects>
    <effect id="e1"><profile_COMMON>
      <newparam sid="s_diff"><surface type="2D"><init_from>i_diff</init_from></surface></newparam>
      <newparam sid="sam_diff"><sampler2D><source>s_diff</source></sampler2D></newparam>
      <technique sid="common"><lambert>
        <emission><color>0.1 0.2 0.3 1</color></emission>
        <diffuse><texture texture="sam_diff" texcoord="UV0">
          <extra><technique profile="MAYA"><wrapU>0</wrapU></technique></extra>
        </texture></diffuse>
        <transparency><float>0.5</float></transparency>
      </lambert>
      <extra><technique profile="GOOGLEEARTH"><double_sided>1</double_sided></technique></extra>
      </technique>
    </profile_COMMON></effect>
    <effect id="e2"><profile_COMMON>
      <technique sid="common"><lambert>
        <diffuse><color>0 0 0 1</color></diffuse>
      </lambert></technique>
    </profile_COMMON></effect>
    <effect id="e3"><profile_COMMON>
      <technique sid="common"><lambert/></technique>
    </profile_COMMON></effect>
  </library_effects>
  <library_materials>
    <material id="m1" name="MatOne"><instance_effect url="#e1"/></material>
    <material id="m2" name="MatTwo"><instance_effect url="#e2"/></material>
    <material id="m3" name="MatThree"><instance_effect url="#e3"/></material>
  </library_materials>
%GEOMETRY%
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="N" name="N"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="mat" target="#m1"/></technique_common></bind_material></instance_geometry></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// e2 names the SAME texture as e1, so a case can prove first-seen dedup by RESOLVED relativePath.
constexpr std::string_view DAE_MATERIALS_TWO_TEXTURED =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>
    <image id="i_a"><init_from>wood.png</init_from></image>
    <image id="i_b"><init_from>wood.png</init_from></image>
  </library_images>
  <library_effects>
    <effect id="e1"><profile_COMMON>
      <newparam sid="sa"><surface type="2D"><init_from>i_a</init_from></surface></newparam>
      <newparam sid="sama"><sampler2D><source>sa</source></sampler2D></newparam>
      <technique sid="common"><lambert>
        <diffuse><texture texture="sama" texcoord="UV0"/></diffuse>
      </lambert></technique>
    </profile_COMMON></effect>
    <effect id="e2"><profile_COMMON>
      <newparam sid="sb"><surface type="2D"><init_from>i_b</init_from></surface></newparam>
      <newparam sid="samb"><sampler2D><source>sb</source></sampler2D></newparam>
      <technique sid="common"><lambert>
        <diffuse><texture texture="samb" texcoord="UV0"/></diffuse>
      </lambert></technique>
    </profile_COMMON></effect>
  </library_effects>
  <library_materials>
    <material id="m1" name="A"><instance_effect url="#e1"/></material>
    <material id="m2" name="B"><instance_effect url="#e2"/></material>
  </library_materials>
%GEOMETRY%
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="N" name="N"><instance_geometry url="#g1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// AI48/AI49/AI50/AI51 -- A-11's mapping table, one CHECK per row, plus the two rules that are DEFAULTS
// this importer supplies rather than claims about the file.
TEST_CASE("AI48: a .dae material carries its name, colours, opacity, emissive and double-sidedness") {
    const std::string text = dae(DAE_MATERIALS);
    const ImportResult result = importModel("mat.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 3U);
    CHECK(result.model.summary.materialCount == 3U);

    const engine::editor::ImportedMaterial& one = result.model.materials[0];
    CHECK(one.name == "MatOne");
    CHECK(one.localId == 0U);
    CHECK(result.model.materials[1].localId == 1U);
    CHECK(result.model.materials[2].localId == 2U);
    CHECK(approxEq(one.emissiveFactor, engine::Vec3{0.1F, 0.2F, 0.3F}));
    CHECK(one.doubleSided);
    // <transparency>0.5</transparency> with the default A_ONE <transparent> alpha of 1 gives
    // AI_MATKEY_OPACITY == 0.5 (MEASURED against ColladaLoader.cpp's own arithmetic), which is what the
    // alpha channel carries and what makes the material Blend rather than Opaque.
    CHECK(approxEq(one.baseColorFactor.w, 0.5F));
    CHECK(one.alphaMode == engine::editor::AlphaMode::Blend);
    CHECK(result.model.materials[2].alphaMode == engine::editor::AlphaMode::Opaque);
    // NEVER Mask: none of the three formats has an alpha cutoff, so that enumerator is unreachable here.
    for (const engine::editor::ImportedMaterial& material : result.model.materials) {
        CHECK(material.alphaMode != engine::editor::AlphaMode::Mask);
    }
}

// AI49 (AC-37) -- THE STATED DEFAULTS. metallicFactor must be written EXPLICITLY because
// ImportedMaterial's own field default is 1.0F; leaving it would ship every DAE/PLY/STL material as a
// full metal. And there is no SHININESS -> roughness curve, on a material that declares shininess or one
// that does not: this is the case that reddens if anyone ever adds one.
TEST_CASE("AI49: metallic is 0 and roughness is 1 on every material, with no shininess curve") {
    const std::string text = dae(DAE_MATERIALS);
    const ImportResult result = importModel("mat.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE_FALSE(result.model.materials.empty());
    for (const engine::editor::ImportedMaterial& material : result.model.materials) {
        CHECK(approxEq(material.metallicFactor, 0.0F));
        CHECK(approxEq(material.roughnessFactor, 1.0F));
        CHECK_FALSE(material.metallicRoughness.has_value());
        CHECK_FALSE(material.occlusion.has_value());  // LIGHTMAP is an ambient map, never occlusion
    }

    // The .stl default material takes the same defaults, and it declares SHININESS.
    const ImportResult stl = importModel("t.stl", "", asBytes(TRIANGLE_STL), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(stl.status == ImportStatus::Ok);
    REQUIRE(stl.model.materials.size() == 1U);
    CHECK(approxEq(stl.model.materials[0].metallicFactor, 0.0F));
    CHECK(approxEq(stl.model.materials[0].roughnessFactor, 1.0F));
}

// AI50 (AC-37) -- THE ZERO-FACTOR RULE, BOTH HALVES IN ONE CASE, because the pairing is what is
// load-bearing: a black factor annihilates the texture the same material supplies, and no format here
// has a "was it set" flag. WITH a bound base-colour texture a black factor reads as neutral white;
// WITHOUT one it stays black, which is a legitimately black material.
TEST_CASE("AI50: a black diffuse factor reads as white with a texture and stays black without one") {
    // A .ply `element material` supplies COLOR_DIFFUSE independently of the `TextureFile` comment, which
    // is what makes the pairing expressible at all -- Collada cannot say both (see DAE_MATERIALS above).
    const std::string withTexture =
        "ply\nformat ascii 1.0\ncomment TextureFile scan.png\nelement vertex 3\nproperty float x\n"
        "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_index\n"
        "element material 1\nproperty float diffuse_red\nproperty float diffuse_green\n"
        "property float diffuse_blue\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n0 0 0\n";
    const ImportResult textured =
        importModel("black.ply", "", asBytes(withTexture), ImportSettings{}, ImportDepth::Full, {});
    INFO("textured message: ", textured.message);
    REQUIRE(textured.status == ImportStatus::Ok);
    REQUIRE(textured.model.materials.size() == 1U);
    REQUIRE(textured.model.materials[0].baseColor.has_value());
    CHECK(approxEq(textured.model.materials[0].baseColorFactor.x, 1.0F));
    CHECK(approxEq(textured.model.materials[0].baseColorFactor.y, 1.0F));
    CHECK(approxEq(textured.model.materials[0].baseColorFactor.z, 1.0F));

    std::string withoutTexture(withTexture);
    const std::size_t comment = withoutTexture.find("comment TextureFile scan.png\n");
    REQUIRE(comment != std::string::npos);
    withoutTexture.erase(comment, std::string_view("comment TextureFile scan.png\n").size());
    const ImportResult plain =
        importModel("black.ply", "", asBytes(withoutTexture), ImportSettings{}, ImportDepth::Full, {});
    INFO("plain message: ", plain.message);
    REQUIRE(plain.status == ImportStatus::Ok);
    REQUIRE(plain.model.materials.size() == 1U);
    CHECK_FALSE(plain.model.materials[0].baseColor.has_value());
    CHECK(approxEq(plain.model.materials[0].baseColorFactor.x, 0.0F));
    CHECK(approxEq(plain.model.materials[0].baseColorFactor.y, 0.0F));
    CHECK(approxEq(plain.model.materials[0].baseColorFactor.z, 0.0F));

    // And the Collada side of the same rule: a black <color> with no texture stays black.
    const std::string text = dae(DAE_MATERIALS);
    const ImportResult collada = importModel("mat.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(collada.status == ImportStatus::Ok);
    REQUIRE(collada.model.materials.size() == 3U);
    CHECK_FALSE(collada.model.materials[1].baseColor.has_value());
    CHECK(approxEq(collada.model.materials[1].baseColorFactor.x, 0.0F));
    CHECK(approxEq(collada.model.materials[1].baseColorFactor.z, 0.0F));
}

// AI51 (AC-37) -- a non-identity UV transform has NO field in ImportedTextureRef, so it produces exactly
// ONE aggregate warning per MATERIAL rather than silently wrong UVs -- and never one per slot.
TEST_CASE("AI51: a non-identity UV transform produces exactly one warning per material") {
    std::string text = dae(DAE_MATERIALS);
    const std::size_t at = text.find("<technique profile=\"MAYA\"><wrapU>0</wrapU></technique>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<technique profile=\"MAYA\"><wrapU>0</wrapU></technique>").size(),
                 "<technique profile=\"MAYA\"><repeatU>2</repeatU><repeatV>3</repeatV></technique>");
    const ImportResult result = importModel("uvx.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);

    std::size_t uvWarnings = 0;
    for (const std::string& warning : result.warnings) {
        if (warning.find("UV transform") != std::string::npos) {
            ++uvWarnings;
        }
    }
    CHECK(uvWarnings == 1U);
}

// AI52 (AC-38) -- the three wrap modes Collada can express. aiTextureMapMode_Decal has NO Collada
// spelling at all (ColladaLoader emits only Wrap/Clamp/Mirror, from mWrapU/mMirrorU), so its arm is
// defence in depth and is pinned in the SOURCE TEXT rather than claimed to be exercised.
TEST_CASE("AI52: sampler wrap modes map to Repeat, ClampToEdge and MirroredRepeat") {
    const auto wrapOf = [](std::string_view mayaExtra) {
        std::string text = dae(DAE_MATERIALS);
        const std::size_t at = text.find("<technique profile=\"MAYA\"><wrapU>0</wrapU></technique>");
        REQUIRE(at != std::string::npos);
        text.replace(at, std::string_view("<technique profile=\"MAYA\"><wrapU>0</wrapU></technique>").size(),
                     std::string(mayaExtra));
        const ImportResult result = importModel("w.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE(result.status == ImportStatus::Ok);
        REQUIRE_FALSE(result.model.materials.empty());
        REQUIRE(result.model.materials[0].baseColor.has_value());
        return result.model.materials[0].baseColor->wrapU;
    };
    CHECK(wrapOf("") == engine::editor::TextureWrap::Repeat);  // Collada's own default is wrap
    CHECK(wrapOf("<technique profile=\"MAYA\"><wrapU>0</wrapU></technique>") ==
          engine::editor::TextureWrap::ClampToEdge);
    CHECK(wrapOf("<technique profile=\"MAYA\"><wrapU>1</wrapU><mirrorU>1</mirrorU></technique>") ==
          engine::editor::TextureWrap::MirroredRepeat);

    const std::string code = strippedSource("assimp_import.cpp");
    const std::size_t decal = code.find("case aiTextureMapMode_Decal:");
    REQUIRE(decal != std::string::npos);
    CHECK(code.find("TextureWrap::ClampToEdge", decal) != std::string::npos);
    CHECK(code.find("addWarning", decal) < code.find("TextureWrap::ClampToEdge", decal));
}

// AI53/AI54 (AC-39/E15) -- each DISTINCT texture path becomes exactly one ImportedImage, deduplicated by
// resolved relativePath, in first-seen order; two materials naming the same texture share it.
TEST_CASE("AI53: two materials naming the same texture share one image and one dependency") {
    const std::string text = dae(DAE_MATERIALS_TWO_TEXTURED);
    const ImportResult result = importModel("dup.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.materials.size() == 2U);
    REQUIRE(result.model.images.size() == 1U);
    CHECK(result.model.summary.imageCount == 1U);
    CHECK(result.model.images[0].relativePath == "wood.png");
    CHECK(result.externalUris == std::vector<std::string>{"wood.png"});

    REQUIRE(result.model.materials[0].baseColor.has_value());
    REQUIRE(result.model.materials[1].baseColor.has_value());
    CHECK(result.model.materials[0].baseColor->imageIndex == 0U);
    CHECK(result.model.materials[1].baseColor->imageIndex == 0U);
}

// AI55 (AC-40) -- a '*'-prefixed path is Assimp's EMBEDDED-texture convention. Recorded as embedded,
// NEVER a dependency, NEVER read. For these three formats aiScene::mTextures should always be empty
// (Collada embeds only inside .zae, which the IO refusal forbids reaching), so this arm exists to be
// CORRECT rather than because it is expected to fire.
TEST_CASE("AI55: a '*'-prefixed texture path is recorded as embedded and is never a dependency") {
    std::string text = dae(DAE_MATERIALS);
    const std::size_t at = text.find("<init_from>wood.png</init_from>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<init_from>wood.png</init_from>").size(), "<init_from>*0</init_from>");
    const ImportResult result = importModel("emb.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1U);
    CHECK(result.model.images[0].embedded);
    CHECK(result.model.images[0].uri == "*0");
    CHECK(result.model.images[0].relativePath.empty());
    CHECK(result.externalUris.empty());
}

// AI56 (E16) -- a texture path resolving OUTSIDE the assets root is refused, the exact reason is carried
// on the ImportedImage so the panel can show it, and nothing reaches externalUris.
TEST_CASE("AI56: a texture path escaping the assets root is refused with its reason recorded") {
    std::string text = dae(DAE_MATERIALS);
    const std::size_t at = text.find("<init_from>wood.png</init_from>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<init_from>wood.png</init_from>").size(),
                 "<init_from>../../secret.png</init_from>");
    const ImportResult result = importModel("esc.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1U);
    CHECK_FALSE(result.model.images[0].refusal.empty());
    CHECK(result.model.images[0].relativePath.empty());
    CHECK(result.externalUris.empty());
    CHECK_FALSE(result.model.materials[0].baseColor.has_value());  // a refused path never binds
}

// AI57 (E9) -- a percent-encoded <init_from> is decoded BY THE LOADER and must not be decoded again.
// 3.2.1's A1, third application: decode twice and `100%2520.png` becomes `100 .png`.
TEST_CASE("AI57: a percent-encoded texture path is decoded exactly once") {
    std::string text = dae(DAE_MATERIALS);
    const std::size_t at = text.find("<init_from>wood.png</init_from>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<init_from>wood.png</init_from>").size(), "<init_from>wood%20a.png</init_from>");
    const ImportResult result = importModel("pct.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1U);
    CHECK(result.model.images[0].relativePath == "wood a.png");
    CHECK(result.externalUris == std::vector<std::string>{"wood a.png"});
}

// AI58 (AC-19) -- the .ply seeding step's ONLY direct cover: at Full depth the loader's own material
// carries the SAME TextureFile operand the header scan found, and the find-or-append dedup must collapse
// the two onto ONE entry rather than appending a second.
TEST_CASE("AI58: a .ply's Full-depth URI set is the header scan's, with the loader's own copy deduped") {
    const ImportResult full =
        importModel("scan.ply", "", asBytes(TEXTURED_PLY), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", full.message);
    REQUIRE(full.status == ImportStatus::Ok);
    CHECK(full.externalUris == std::vector<std::string>{"scan.png"});
    REQUIRE(full.model.images.size() == 1U);
    CHECK(full.model.images[0].relativePath == "scan.png");

    const ImportResult structure =
        importModel("scan.ply", "", asBytes(TEXTURED_PLY), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(structure.externalUris == full.externalUris);
}

// AI59 (AC-48) -- importMaterials == false empties materials AND images, sets every materialIndex to
// INVALID_SUBASSET, and changes nothing else. It is also the live proof that applyMaterialMap RUNS: with
// it skipped, the raw mMaterialIndex (0) would survive here instead of the sentinel.
TEST_CASE("AI59: importMaterials false empties materials and images and invalidates every material index") {
    const std::string text = dae(DAE_MATERIALS);
    ImportSettings off;
    off.importMaterials = false;
    const ImportResult with = importModel("mat.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult without = importModel("mat.dae", "", asBytes(text), off, ImportDepth::Full, {});
    REQUIRE(with.status == ImportStatus::Ok);
    REQUIRE(without.status == ImportStatus::Ok);

    CHECK(without.model.materials.empty());
    CHECK(without.model.images.empty());
    CHECK(without.externalUris.empty());
    CHECK(without.model.summary.materialCount == 0U);
    CHECK(without.model.summary.imageCount == 0U);
    for (const engine::editor::ImportedMesh& mesh : without.model.meshes) {
        for (const ImportedPrimitive& prim : mesh.primitives) {
            CHECK(prim.materialIndex == engine::editor::INVALID_SUBASSET);
        }
    }
    // Nothing else moved: the geometry and the hierarchy are identical either way.
    CHECK(modelsMatchIgnoringNames(with.model, without.model));
    CHECK(with.model.nodes.size() == without.model.nodes.size());
}

// AI61 (AC-53) -- MAX_EXTERNAL_URIS overflow reports Truncated ONCE, not once per overflowing texture
// (3.2.3's gap 7: one flag for the whole call, never one per slot).
TEST_CASE("AI61: overflowing MAX_EXTERNAL_URIS truncates once, not once per texture") {
    const std::size_t count = engine::editor::MAX_EXTERNAL_URIS + 8U;
    std::string images;
    std::string effects;
    std::string materials;
    for (std::size_t i = 0; i < count; ++i) {
        images += std::format(R"(<image id="i{}"><init_from>t{}.png</init_from></image>)", i, i);
        effects += std::format(
            R"(<effect id="e{}"><profile_COMMON><newparam sid="s{}"><surface type="2D"><init_from>i{}</init_from></surface></newparam>)"
            R"(<newparam sid="sam{}"><sampler2D><source>s{}</source></sampler2D></newparam>)"
            R"(<technique sid="common"><lambert><diffuse><texture texture="sam{}" texcoord="UV0"/></diffuse></lambert></technique>)"
            R"(</profile_COMMON></effect>)",
            i, i, i, i, i, i);
        materials += std::format(R"(<material id="m{}" name="M{}"><instance_effect url="#e{}"/></material>)", i, i, i);
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>{}</library_images>
  <library_effects>{}</library_effects>
  <library_materials>{}</library_materials>
{}  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="N" name="N"><instance_geometry url="#g1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        images, effects, materials, DAE_TRIANGLE_GEOMETRY);

    const ImportResult result = importModel("many.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.externalUris.size() == engine::editor::MAX_EXTERNAL_URIS);
    // ONE message about external references, however many textures overflowed.
    std::size_t occurrences = 0;
    std::size_t at = result.message.find("external reference");
    while (at != std::string::npos) {
        ++occurrences;
        at = result.message.find("external reference", at + 1U);
    }
    CHECK(occurrences == 1U);
}

// AI60/AI62 (AC-53, and 3.2.3's BLOCKING gap 1) -- MAX_MATERIALS_PER_MODEL trims the tail, which is the
// ONLY thing that makes the raw and the converted material index spaces diverge. A primitive whose raw
// index fell past the cap must read INVALID_SUBASSET, never a stale index into a shorter list.
TEST_CASE("AI60: overflowing MAX_MATERIALS_PER_MODEL truncates and never leaves a stale material index") {
    const std::size_t count = engine::editor::MAX_MATERIALS_PER_MODEL + 2U;
    std::string effects;
    std::string materials;
    effects.reserve(count * 128U);
    materials.reserve(count * 72U);
    for (std::size_t i = 0; i < count; ++i) {
        // ZERO-PADDED ids, deliberately: Assimp's Collada material library is a std::map keyed by id, so
        // the converted order is the ids' BYTE order. Padding makes that order the numeric one, which is
        // the only way a test can name the material that lands PAST the cap.
        effects += std::format(
            R"(<effect id="e{:05}"><profile_COMMON><technique sid="common"><lambert/></technique></profile_COMMON></effect>)",
            i);
        materials +=
            std::format(R"(<material id="m{:05}" name="M{}"><instance_effect url="#e{:05}"/></material>)", i, i, i);
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_effects>{}</library_effects>
  <library_materials>{}</library_materials>
{}  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="N" name="N"><instance_geometry url="#g1"><bind_material><technique_common>
      <instance_material symbol="mat" target="#m{:05}"/></technique_common></bind_material></instance_geometry></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        effects, materials, DAE_TRIANGLE_GEOMETRY, count - 1U);

    const ImportResult result = importModel("caps.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("material count") != std::string::npos);
    CHECK(result.model.materials.size() == engine::editor::MAX_MATERIALS_PER_MODEL);
    CHECK(result.model.summary.materialCount == engine::editor::MAX_MATERIALS_PER_MODEL);

    // AI62's half: the bound material's RAW index fell past the cap, so the primitive must carry the
    // sentinel. A raw copy would carry an index one past the end of a list that is now shorter.
    REQUIRE(result.model.meshes.size() == 1U);
    REQUIRE(result.model.meshes[0].primitives.size() == 1U);
    CHECK(result.model.meshes[0].primitives[0].materialIndex == engine::editor::INVALID_SUBASSET);
}

// ---- step 7: skins and animations --------------------------------------------------------------------

// A skinned triangle with TWO joints. Bone1's inverse bind matrix is deliberately NON-SYMMETRIC with a
// known translation of (1, 2, 3): Collada writes matrices ROW-MAJOR, so the translation is the last
// COLUMN, i.e. elements [3], [7], [11]. Assimp copies those into aiMatrix4x4::a4/b4/c4 verbatim (it is
// row-major too), and engine::Mat4 is COLUMN-major, so the conversion must TRANSPOSE. A transpose-free
// copy puts the translation at data()[3],[7],[11] instead of [12],[13],[14] and AI63 goes red.
namespace {

constexpr std::string_view SKIN_LIBRARIES =
    R"(  <library_controllers>
    <controller id="skin1" name="SkinCtrl">
      <skin source="#g1">
        <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
        <source id="jointNames">
          <Name_array id="jn" count="2">Bone1 Bone2</Name_array>
          <technique_common><accessor source="#jn" count="2" stride="1">
            <param name="JOINT" type="name"/></accessor></technique_common>
        </source>
        <source id="invBind">
          <float_array id="ib" count="32">1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</float_array>
          <technique_common><accessor source="#ib" count="2" stride="16">
            <param name="TRANSFORM" type="float4x4"/></accessor></technique_common>
        </source>
        <source id="skinWeights">
          <float_array id="wa" count="3">1 0.5 0.5</float_array>
          <technique_common><accessor source="#wa" count="3" stride="1">
            <param name="WEIGHT" type="float"/></accessor></technique_common>
        </source>
        <joints>
          <input semantic="JOINT" source="#jointNames"/>
          <input semantic="INV_BIND_MATRIX" source="#invBind"/>
        </joints>
        <vertex_weights count="3">
          <input semantic="JOINT" source="#jointNames" offset="0"/>
          <input semantic="WEIGHT" source="#skinWeights" offset="1"/>
          <vcount>1 2 1</vcount>
          <v>0 0 0 1 1 2 1 0</v>
        </vertex_weights>
      </skin>
    </controller>
  </library_controllers>
)";

// %ANIMATIONS% is replaced by a <library_animations> block, or by nothing.
constexpr std::string_view SKINNED_DAE =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
%GEOMETRY%
%CONTROLLERS%
%ANIMATIONS%
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="Bone1" sid="Bone1" name="Bone1" type="JOINT">
      <matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
      <node id="Bone2" sid="Bone2" name="Bone2" type="JOINT">
        <matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
      </node>
    </node>
    <node id="Mesh" name="Mesh">
      <instance_controller url="#skin1"><skeleton>#Bone1</skeleton></instance_controller>
    </node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

// A <library_animations> block driving Bone1/transform with three matrix keys at 0, 0.5 and 1 seconds.
// `matrixRow` is one key's 16 row-major floats; all three keys carry the same value, so a resampling
// change upstream cannot move what the case asserts.
[[nodiscard]] std::string animationBlock(std::string_view matrixRow, std::string_view times = "0 0.5 1") {
    std::string keys;
    for (int i = 0; i < 3; ++i) {
        keys += std::string(matrixRow) + " ";
    }
    return std::format(
        R"(  <library_animations><animation id="a1" name="Clip">
      <source id="a_in">
        <float_array id="a_in_a" count="3">{}</float_array>
        <technique_common><accessor source="#a_in_a" count="3" stride="1">
          <param name="TIME" type="float"/></accessor></technique_common>
      </source>
      <source id="a_out">
        <float_array id="a_out_a" count="48">{}</float_array>
        <technique_common><accessor source="#a_out_a" count="3" stride="16">
          <param name="TRANSFORM" type="float4x4"/></accessor></technique_common>
      </source>
      <source id="a_interp">
        <Name_array id="a_interp_a" count="3">LINEAR LINEAR LINEAR</Name_array>
        <technique_common><accessor source="#a_interp_a" count="3" stride="1">
          <param name="INTERPOLATION" type="name"/></accessor></technique_common>
      </source>
      <sampler id="samp">
        <input semantic="INPUT" source="#a_in"/>
        <input semantic="OUTPUT" source="#a_out"/>
        <input semantic="INTERPOLATION" source="#a_interp"/>
      </sampler>
      <channel source="#samp" target="Bone1/transform"/>
    </animation></library_animations>
)",
        times, keys);
}

// The skinned document, with the geometry, the controllers and (optionally) an animation spliced in.
[[nodiscard]] std::string skinnedDae(std::string_view controllers, std::string_view animations) {
    std::string out(SKINNED_DAE);
    const auto substitute = [&out](std::string_view token, std::string_view text) {
        const std::size_t at = out.find(token);
        REQUIRE(at != std::string::npos);
        out.replace(at, token.size(), text);
    };
    substitute("%GEOMETRY%\n", DAE_TRIANGLE_GEOMETRY);
    substitute("%CONTROLLERS%\n", controllers);
    substitute("%ANIMATIONS%\n", animations);
    return out;
}

}  // namespace

// AI63 (AC-43) -- D14 TRAP 1, HAND-COMPUTED. A transpose-free copy is a plausible WRONG model, never a
// failure, so this is the only thing standing between the two.
TEST_CASE("AI63: an inverse bind matrix is transposed on conversion, translation column and all") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, "");
    const ImportResult result = importModel("skin.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.skins.size() == 1U);
    const engine::editor::ImportedSkin& skin = result.model.skins[0];
    REQUIRE(skin.inverseBindMatrices.size() == skin.joints.size());
    REQUIRE_FALSE(skin.inverseBindMatrices.empty());

    const engine::Mat4& ibm = skin.inverseBindMatrices[0];
    CHECK(approxEq(ibm.columns[3].x, 1.0F));
    CHECK(approxEq(ibm.columns[3].y, 2.0F));
    CHECK(approxEq(ibm.columns[3].z, 3.0F));
    CHECK(approxEq(ibm.columns[3].w, 1.0F));
    // And the same fact through the contiguous, GPU-upload-order view: a transpose-free copy would put
    // the translation at [3], [7], [11] instead.
    CHECK(approxEq(ibm.data()[12], 1.0F));
    CHECK(approxEq(ibm.data()[13], 2.0F));
    CHECK(approxEq(ibm.data()[14], 3.0F));
    CHECK(approxEq(ibm.data()[3], 0.0F));
    CHECK(approxEq(ibm.data()[7], 0.0F));
    CHECK(approxEq(ibm.data()[11], 0.0F));
}

// AI64 (AC-44) -- D14 TRAP 2, HAND-COMPUTED, through convertAnimations' OWN conversion. A rotation of
// exactly +90 degrees about X is Quat{sin45, 0, 0, cos45} in engine's {x,y,z,w} order; a {w,x,y,z}
// reordering produces {cos45, sin45, 0, 0}, which is a different, perfectly renderable rotation.
TEST_CASE("AI64: an animated 90-degree rotation about X converts as a named field copy, not a reorder") {
    // Row-major Collada matrix for +90 about X: (x,y,z) -> (x, -z, y).
    const std::string text = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 0 -1 0 0 1 0 0 0 0 0 1"));
    const ImportResult result = importModel("anim.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1U);

    const engine::editor::ImportedAnimationChannel* rotation = nullptr;
    for (const engine::editor::ImportedAnimationChannel& channel : result.model.animations[0].channels) {
        if (channel.path == engine::editor::AnimationPath::Rotation) {
            rotation = &channel;
        }
    }
    REQUIRE(rotation != nullptr);
    REQUIRE_FALSE(rotation->values.empty());
    CHECK(approxEq(rotation->values[0].x, 0.70710678F, 1e-4F));
    CHECK(approxEq(rotation->values[0].y, 0.0F, 1e-4F));
    CHECK(approxEq(rotation->values[0].z, 0.0F, 1e-4F));
    CHECK(approxEq(rotation->values[0].w, 0.70710678F, 1e-4F));
}

// AI64b (AC-44) -- the SAME trap through convertNodes' toQuat, because the two call sites break
// independently. A static +90 about X on a node transform.
TEST_CASE("AI64b: a 90-degree node rotation about X converts as a named field copy, not a reorder") {
    std::string text = dae(DAE_CHAIN);
    const std::size_t at = text.find("<translate>1 2 3</translate>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<translate>1 2 3</translate>").size(), "<rotate>1 0 0 90</rotate>");
    const ImportResult result = importModel("rot.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 4U);
    const engine::Quat& q = result.model.nodes[1].rotation;
    CHECK(approxEq(q.x, 0.70710678F, 1e-4F));
    CHECK(approxEq(q.y, 0.0F, 1e-4F));
    CHECK(approxEq(q.z, 0.0F, 1e-4F));
    CHECK(approxEq(q.w, 0.70710678F, 1e-4F));
}

// AI65 (AC-41) -- the skin's shape: joints as NODE localIds in mBones order, inverseBindMatrices exactly
// joints.size() at Full and EMPTY at Structure, per-vertex weights summing to 1 on every weighted vertex.
TEST_CASE("AI65: a skinned .dae yields one skin per skinned mesh with joints, matrices and weights") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, "");
    const ImportResult full = importModel("skin.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", full.message);
    REQUIRE(full.status == ImportStatus::Ok);
    REQUIRE(full.model.skins.size() == 1U);
    const engine::editor::ImportedSkin& skin = full.model.skins[0];
    CHECK(skin.joints.size() == 2U);
    CHECK(skin.inverseBindMatrices.size() == skin.joints.size());
    CHECK(full.model.summary.skinCount == 1U);
    CHECK(full.model.summary.jointCount == 2U);
    for (const std::uint32_t joint : skin.joints) {
        REQUIRE(joint < full.model.nodes.size());
    }
    // Every joint resolves to a node that really is a joint node in the document.
    CHECK(full.model.nodes[skin.joints[0]].name == "Bone1");
    // MEASURED, answering the one open question this step carried: aiBone::mArmature DOES come back
    // engaged for a Collada skin -- aiProcess_PopulateArmatureData fills it -- and it resolves to the
    // armature node, which here is the <visual_scene> root itself (localId 0) because the joints hang
    // directly off it. That node is NOT itself a joint, and it is recorded AS-IS rather than replaced by
    // a computed common ancestor: 3.2.1's E24, arriving for real rather than as a hypothetical.
    CHECK(skin.skeletonRoot == 0U);
    CHECK(skin.skeletonRoot < full.model.nodes.size());
    CHECK(full.model.nodes[skin.joints[1]].name == "Bone2");

    const ImportedPrimitive& prim = onlyPrimitive(full.model);
    CHECK(has(prim.attributes, VertexAttribute::Joints0));
    CHECK(has(prim.attributes, VertexAttribute::Weights0));
    REQUIRE(prim.weights.size() == prim.positions.size());
    REQUIRE(prim.joints.size() == prim.positions.size());
    for (const engine::Vec4& weight : prim.weights) {
        const float sum = weight.x + weight.y + weight.z + weight.w;
        CHECK(approxEq(sum, 1.0F, 1e-4F));
    }
    // The node holding the skinned mesh points back at the skin.
    bool bound = false;
    for (const engine::editor::ImportedNode& node : full.model.nodes) {
        if (node.skinIndex == 0U) {
            bound = true;
        }
    }
    CHECK(bound);

    const ImportResult structure =
        importModel("skin.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(structure.status == ImportStatus::Ok);
    REQUIRE(structure.model.skins.size() == 1U);
    CHECK(structure.model.skins[0].joints.size() == 2U);          // IDENTITY survives
    CHECK(structure.model.skins[0].inverseBindMatrices.empty());  // SAMPLES do not
}

// AI66 (AC-42) -- a vertex with NO influences gets all-zero joints AND all-zero weights, never
// {1,0,0,0}, which would silently bind it to joint 0: a plausible wrong deformation, not a failure.
TEST_CASE("AI66: a vertex with no influences gets all-zero joints and all-zero weights") {
    std::string controllers(SKIN_LIBRARIES);
    const std::size_t at = controllers.find("<vcount>1 2 1</vcount>\n          <v>0 0 0 1 1 2 1 0</v>");
    REQUIRE(at != std::string::npos);
    controllers.replace(at, std::string_view("<vcount>1 2 1</vcount>\n          <v>0 0 0 1 1 2 1 0</v>").size(),
                        "<vcount>1 2 0</vcount>\n          <v>0 0 0 1 1 2</v>");
    const std::string text = skinnedDae(controllers, "");
    const ImportResult result = importModel("zero.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    REQUIRE(prim.weights.size() == 3U);
    REQUIRE(prim.joints.size() == 3U);
    CHECK(approxEq(prim.weights[2].x, 0.0F));
    CHECK(approxEq(prim.weights[2].y, 0.0F));
    CHECK(approxEq(prim.weights[2].z, 0.0F));
    CHECK(approxEq(prim.weights[2].w, 0.0F));
    CHECK(prim.joints[2] == std::array<std::uint16_t, 4>{0U, 0U, 0U, 0U});
}

// AI67 (E12) -- a bone naming a node the document does not contain: that JOINT is dropped with one
// warning, the SKIN survives with the rest, and `joints` stays contiguous -- no gap, no sentinel.
TEST_CASE("AI67: a bone resolving to no node is dropped and the skin survives with the rest") {
    std::string controllers(SKIN_LIBRARIES);
    const std::size_t at = controllers.find("Bone1 Bone2");
    REQUIRE(at != std::string::npos);
    controllers.replace(at, std::string_view("Bone1 Bone2").size(), "Bone1 Ghost");
    const std::string text = skinnedDae(controllers, "");
    const ImportResult result = importModel("ghost.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.skins.size() == 1U);
    CHECK(result.model.skins[0].joints.size() == 1U);
    CHECK(result.model.skins[0].inverseBindMatrices.size() == 1U);
    for (const std::uint32_t joint : result.model.skins[0].joints) {
        CHECK(joint != engine::editor::INVALID_SUBASSET);
        CHECK(joint < result.model.nodes.size());
    }
    std::size_t dropWarnings = 0;
    for (const std::string& warning : result.warnings) {
        if (warning.find("resolved to no node") != std::string::npos) {
            ++dropWarnings;
        }
    }
    CHECK(dropWarnings == 1U);
}

// AI68 (AC-32, CORRECTED TWICE against the library) -- a `vertex_weights` block declaring MORE entries
// than the mesh has vertices imports cleanly, with weights for the real vertices and nothing else, and
// the run is clean under ASan and UBSan.
//
// The plan asked for "a weight whose mVertexId is out of range is dropped with one capped warning". That
// input has NO Collada spelling: ColladaLoader builds its weight lists by walking the mesh's OWN
// vertices, so a surplus <vcount>/<v> entry is simply never read, and even if one reached the scene
// aiProcess_ValidateDataStructure refuses it outright (it checks mWeights[i].mVertexId against
// mNumVertices and throws, exactly as it does for a face index -- see AI31). So OUR range check is
// unreachable from both directions; it stays as defence in depth for a validation-off build and AI34
// pins it in the source text. What this case can and does assert is the ROBUSTNESS the AC was protecting:
// a lying weight block produces no over-read and no malformed arrays.
TEST_CASE("AI68: a vertex_weights block longer than the mesh imports cleanly with no over-read") {
    std::string controllers(SKIN_LIBRARIES);
    const std::size_t at = controllers.find("<vertex_weights count=\"3\">");
    REQUIRE(at != std::string::npos);
    controllers.replace(at, std::string_view("<vertex_weights count=\"3\">").size(), "<vertex_weights count=\"9\">");
    const std::size_t counts = controllers.find("<vcount>1 2 1</vcount>\n          <v>0 0 0 1 1 2 1 0</v>");
    REQUIRE(counts != std::string::npos);
    controllers.replace(counts, std::string_view("<vcount>1 2 1</vcount>\n          <v>0 0 0 1 1 2 1 0</v>").size(),
                        "<vcount>1 1 1 1 1 1 1 1 1</vcount>\n          <v>0 0 0 0 0 0 0 0 0 0 0 0 1 0 1 0 1 0</v>");
    const std::string text = skinnedDae(controllers, "");
    const ImportResult result = importModel("badw.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    const ImportedPrimitive& prim = onlyPrimitive(result.model);
    CHECK(prim.weights.size() == prim.positions.size());
    CHECK(prim.joints.size() == prim.positions.size());
    for (const std::array<std::uint16_t, 4>& joint : prim.joints) {
        for (const std::uint16_t slot : joint) {
            CHECK(slot <= result.model.skins[0].joints.size());
        }
    }
}

// AI69 (AC-45) -- one ImportedAnimation per aiAnimation and THREE channels per aiNodeAnim that has keys
// on all three paths; times strictly increasing, in SECONDS; values.size() == times.size() on every
// Linear channel (INV-M6).
TEST_CASE("AI69: an animated .dae yields three Linear channels per node with strictly increasing times") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"));
    const ImportResult result = importModel("anim.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1U);
    CHECK(result.model.summary.animationCount == 1U);

    const engine::editor::ImportedAnimation& clip = result.model.animations[0];
    CHECK(clip.localId == 0U);
    CHECK(clip.channels.size() == 3U);
    bool sawTranslation = false;
    bool sawRotation = false;
    bool sawScale = false;
    for (const engine::editor::ImportedAnimationChannel& channel : clip.channels) {
        CHECK(channel.interpolation == engine::editor::AnimationInterpolation::Linear);
        CHECK(channel.values.size() == channel.times.size());  // INV-M6 for Linear
        REQUIRE_FALSE(channel.times.empty());
        CHECK(channel.targetNode < result.model.nodes.size());
        for (std::size_t i = 1; i < channel.times.size(); ++i) {
            CHECK(channel.times[i] > channel.times[i - 1U]);
        }
        sawTranslation = sawTranslation || channel.path == engine::editor::AnimationPath::Translation;
        sawRotation = sawRotation || channel.path == engine::editor::AnimationPath::Rotation;
        sawScale = sawScale || channel.path == engine::editor::AnimationPath::Scale;
    }
    CHECK(sawTranslation);
    CHECK(sawRotation);
    CHECK(sawScale);
}

// AI70 (AC-46, half) -- the zero-ticks-per-second arm has NO reachable input in this task's three
// formats: ColladaLoader writes mTicksPerSecond = 1000 unconditionally, and .ply and .stl carry no
// animation at all. So the arm is pinned in the source text, with the one warning per clip it owes, and
// the reason is recorded rather than left as a silently untested branch.
TEST_CASE("AI70: a zero ticks-per-second falls back to seconds with one warning per clip") {
    const std::string code = strippedSource("assimp_import.cpp");
    const std::size_t at = code.find("double ticksPerSecond = src.mTicksPerSecond;");
    REQUIRE(at != std::string::npos);
    const std::size_t guard = code.find("if (ticksPerSecond == 0.0) {", at);
    REQUIRE(guard != std::string::npos);
    const std::size_t warn = code.find("addWarning(result", guard);
    const std::size_t fallback = code.find("ticksPerSecond = 1.0;", guard);
    REQUIRE(warn != std::string::npos);
    REQUIRE(fallback != std::string::npos);
    CHECK(warn < fallback);

    // GAP-CLOSING, and for the same reason (sabotage seed S27): the STRICTLY-INCREASING enforcement has
    // no reachable input either. ColladaLoader::CreateAnimation RESAMPLES onto a stepped timeline from
    // startTime to endTime, so a duplicate in the document's own <float_array> of input times cannot
    // survive into aiNodeAnim -- measured by seeding the enforcement away with a fixture declaring
    // "0 0.5 0.5" and watching nothing redden. It stays because INV-M6 and model_import.hpp's own
    // contract require it of every channel this tree produces, whatever a future loader emits.
    const std::size_t vec3Guard = code.find("appendVec3Channel");
    REQUIRE(vec3Guard != std::string::npos);
    CHECK(code.find("if (!first && t <= lastTime) {", vec3Guard) != std::string::npos);
    const std::size_t quatGuard = code.find("appendQuatChannel");
    REQUIRE(quatGuard != std::string::npos);
    CHECK(code.find("if (!first && t <= lastTime) {", quatGuard) != std::string::npos);
}

// AI71 (AC-46) -- ticks to SECONDS, hand-computed against the loader's own 1000 ticks per second: a
// document declaring keys at 0, 0.5 and 1 seconds must read back as 0, 0.5 and 1.
TEST_CASE("AI71: key times are converted from ticks to seconds") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"));
    const ImportResult result = importModel("anim.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1U);
    REQUIRE_FALSE(result.model.animations[0].channels.empty());
    const engine::editor::ImportedAnimationChannel& channel = result.model.animations[0].channels[0];
    REQUIRE(channel.times.size() == 3U);
    CHECK(approxEq(channel.times[0], 0.0F, 1e-3F));
    CHECK(approxEq(channel.times[1], 0.5F, 1e-3F));
    CHECK(approxEq(channel.times[2], 1.0F, 1e-3F));
}

// AI72 (AC-47) -- a channel targeting a node the model does not contain is dropped, never silently
// retargeted and never emitted with an invalid targetNode; and a DUPLICATE key time is dropped keeping
// the first, with one warning per channel.
//
// The duplicate half is an HONEST NEGATIVE RESULT, recorded rather than dressed up: a document
// declaring "0 0.5 0.5" still yields strictly increasing key times, because ColladaLoader::CreateAnimation
// RESAMPLES onto a stepped timeline rather than copying the document's own input source. So the
// enforcement in appendVec3Channel/appendQuatChannel has no reachable input from any of these three
// formats, seed S27 reddens nothing behaviourally, and AI70 carries its source-text pin instead. The
// arm stays because it asserts the OUTPUT contract (INV-M6's neighbour) on a fixture that tries hardest
// to break it.
TEST_CASE("AI72: an unknown channel target is dropped, and so is a duplicate key time") {
    const std::string good = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"));
    std::string text(good);
    const std::size_t at = text.find("target=\"Bone1/transform\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("target=\"Bone1/transform\"").size(), "target=\"NoSuchNode/transform\"");
    const ImportResult result = importModel("ghosta.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Ok);
    // Whether the drop happens in the library (no aiNodeAnim at all) or in our own resolve (one warning),
    // the OBSERVABLE is the same and is what matters: no channel claims a node that does not exist.
    for (const engine::editor::ImportedAnimation& clip : result.model.animations) {
        for (const engine::editor::ImportedAnimationChannel& channel : clip.channels) {
            CHECK(channel.targetNode < result.model.nodes.size());
        }
    }

    // A REPEATED timestamp: three keys declared at 0, 0.5 and 0.5 seconds.
    const std::string repeated =
        skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1", "0 0.5 0.5"));
    const ImportResult dup = importModel("dupt.dae", "", asBytes(repeated), ImportSettings{}, ImportDepth::Full, {});
    INFO("duplicate message: ", dup.message, " warnings: ", dup.warningTotal);
    REQUIRE(dup.status == ImportStatus::Ok);
    REQUIRE(dup.model.animations.size() == 1U);
    for (const engine::editor::ImportedAnimationChannel& channel : dup.model.animations[0].channels) {
        for (std::size_t i = 1; i < channel.times.size(); ++i) {
            CHECK(channel.times[i] > channel.times[i - 1U]);  // STRICTLY increasing, always
        }
    }
}

// AI73 (AC-45) -- duration is RECOMPUTED from the surviving channels, in SECONDS. Taking it from
// aiAnimation::mDuration instead would report the Collada clip's TICKS -- 1000x too large -- which is
// exactly the plausible-but-wrong number this case exists to catch.
TEST_CASE("AI73: animation duration is recomputed in seconds, never taken from the library's own field") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"));
    const ImportResult result = importModel("anim.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 1U);
    CHECK(approxEq(result.model.animations[0].duration, 1.0F, 1e-3F));
    CHECK(approxEq(result.model.summary.animationDuration, 1.0F, 1e-3F));
}

// AI74 (AC-48) -- importSkins == false empties skins and leaves no primitive claiming Joints0/Weights0;
// importAnimations == false empties animations. Each toggle changes ONLY its own collection.
TEST_CASE("AI74: importSkins and importAnimations each empty exactly their own collection") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, animationBlock("1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"));
    const ImportResult both = importModel("anim.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(both.status == ImportStatus::Ok);
    REQUIRE_FALSE(both.model.skins.empty());
    REQUIRE_FALSE(both.model.animations.empty());

    ImportSettings noSkins;
    noSkins.importSkins = false;
    const ImportResult withoutSkins = importModel("anim.dae", "", asBytes(text), noSkins, ImportDepth::Full, {});
    REQUIRE(withoutSkins.status == ImportStatus::Ok);
    CHECK(withoutSkins.model.skins.empty());
    CHECK(withoutSkins.model.summary.skinCount == 0U);
    CHECK(withoutSkins.model.summary.jointCount == 0U);
    CHECK(withoutSkins.model.animations.size() == both.model.animations.size());
    for (const engine::editor::ImportedMesh& mesh : withoutSkins.model.meshes) {
        for (const ImportedPrimitive& prim : mesh.primitives) {
            CHECK_FALSE(has(prim.attributes, VertexAttribute::Joints0));
            CHECK_FALSE(has(prim.attributes, VertexAttribute::Weights0));
            CHECK(prim.joints.empty());
            CHECK(prim.weights.empty());
        }
    }

    ImportSettings noAnims;
    noAnims.importAnimations = false;
    const ImportResult withoutAnims = importModel("anim.dae", "", asBytes(text), noAnims, ImportDepth::Full, {});
    REQUIRE(withoutAnims.status == ImportStatus::Ok);
    CHECK(withoutAnims.model.animations.empty());
    CHECK(withoutAnims.model.summary.animationCount == 0U);
    CHECK(approxEq(withoutAnims.model.summary.animationDuration, 0.0F));
    CHECK(withoutAnims.model.skins.size() == both.model.skins.size());
}

// AI75 (AC-53) -- MAX_JOINTS_PER_SKIN. The WHOLE SKIN is dropped, never truncated: a partial palette
// binds vertices to the WRONG bones, which renders and is wrong, where an unskinned mesh renders and is
// obviously unskinned.
TEST_CASE("AI75: a skin exceeding MAX_JOINTS_PER_SKIN is dropped whole, not truncated") {
    // MAX + 2, chosen because it is DIVISIBLE BY THREE: one vertex per joint, every vertex referenced by
    // a face. MEASURED the hard way -- ColladaLoader builds its submesh from the FACES, so a vertex no
    // face references is dropped and its bone becomes weightless, and Assimp then removes weightless
    // bones, which put the count back UNDER the cap and made the first draft of this case pass for
    // entirely the wrong reason.
    const std::size_t joints = engine::editor::MAX_JOINTS_PER_SKIN + 2U;
    REQUIRE(joints % 3U == 0U);
    std::string positions;
    std::string faces;
    for (std::size_t i = 0; i < joints; ++i) {
        positions += std::format("{} 0 0 ", i);
    }
    for (std::size_t i = 0; i + 2U < joints; i += 3U) {
        faces += std::format("{} {} {} ", i, i + 1U, i + 2U);
    }
    const std::size_t faceCount = joints / 3U;

    std::string names;
    std::string invBind;
    std::string weights;
    std::string vcount;
    std::string vlist;
    std::string nodes;
    for (std::size_t i = 0; i < joints; ++i) {
        names += std::format("B{} ", i);
        invBind += "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 ";
        weights += "1 ";
        vcount += "1 ";
        vlist += std::format("{} {} ", i, i);
        nodes += std::format(R"(<node id="B{}" sid="B{}" name="B{}" type="JOINT"/>)", i, i, i);
    }

    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries><geometry id="g1" name="Many"><mesh>
    <source id="p"><float_array id="pa" count="{}">{}</float_array>
      <technique_common><accessor source="#pa" count="{}" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
    <triangles count="{}"><input semantic="VERTEX" source="#v" offset="0"/><p>{}</p></triangles>
  </mesh></geometry></library_geometries>
  <library_controllers><controller id="skin1" name="Big"><skin source="#g1">
    <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
    <source id="jointNames"><Name_array id="jn" count="{}">{}</Name_array>
      <technique_common><accessor source="#jn" count="{}" stride="1">
        <param name="JOINT" type="name"/></accessor></technique_common></source>
    <source id="invBind"><float_array id="ib" count="{}">{}</float_array>
      <technique_common><accessor source="#ib" count="{}" stride="16">
        <param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
    <source id="skinWeights"><float_array id="wa" count="{}">{}</float_array>
      <technique_common><accessor source="#wa" count="{}" stride="1">
        <param name="WEIGHT" type="float"/></accessor></technique_common></source>
    <joints><input semantic="JOINT" source="#jointNames"/>
      <input semantic="INV_BIND_MATRIX" source="#invBind"/></joints>
    <vertex_weights count="{}"><input semantic="JOINT" source="#jointNames" offset="0"/>
      <input semantic="WEIGHT" source="#skinWeights" offset="1"/>
      <vcount>{}</vcount><v>{}</v></vertex_weights>
  </skin></controller></library_controllers>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="Root" name="Root">{}</node>
    <node id="Mesh" name="Mesh"><instance_controller url="#skin1"><skeleton>#B0</skeleton></instance_controller></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        joints * 3U, positions, joints, faceCount, faces, joints, names, joints, joints * 16U, invBind, joints, joints,
        weights, joints, joints, vcount, vlist, nodes);

    const ImportResult result = importModel("bigskin.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("joint count") != std::string::npos);
    CHECK(result.model.skins.empty());  // DROPPED WHOLE, never a partial palette
    for (const engine::editor::ImportedMesh& mesh : result.model.meshes) {
        for (const ImportedPrimitive& prim : mesh.primitives) {
            CHECK(prim.joints.empty());
            CHECK(prim.weights.empty());
        }
    }
}

// AI77 (AC-29) -- ImportSettings::scale reaches the inverse bind matrices' TRANSLATION COLUMN and the
// ROOT node's translation, and nothing else: not rotations, not weights, not a non-root translation.
TEST_CASE("AI77: scale reaches inverse bind translations and root translations only") {
    const std::string text = skinnedDae(SKIN_LIBRARIES, "");
    ImportSettings doubled;
    doubled.scale = 2.0F;
    const ImportResult plain = importModel("skin.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult scaled = importModel("skin.dae", "", asBytes(text), doubled, ImportDepth::Full, {});
    REQUIRE(plain.status == ImportStatus::Ok);
    REQUIRE(scaled.status == ImportStatus::Ok);
    REQUIRE(plain.model.skins.size() == 1U);
    REQUIRE(scaled.model.skins.size() == 1U);

    const engine::Mat4& before = plain.model.skins[0].inverseBindMatrices[0];
    const engine::Mat4& after = scaled.model.skins[0].inverseBindMatrices[0];
    CHECK(approxEq(after.columns[3].x, before.columns[3].x * 2.0F));
    CHECK(approxEq(after.columns[3].y, before.columns[3].y * 2.0F));
    CHECK(approxEq(after.columns[3].z, before.columns[3].z * 2.0F));
    // The basis columns are UNTOUCHED -- a rotation is not a length.
    for (std::size_t c = 0; c < 3U; ++c) {
        CHECK(approxEq(after.columns[c].x, before.columns[c].x));
        CHECK(approxEq(after.columns[c].y, before.columns[c].y));
        CHECK(approxEq(after.columns[c].z, before.columns[c].z));
    }

    const ImportedPrimitive& beforePrim = onlyPrimitive(plain.model);
    const ImportedPrimitive& afterPrim = onlyPrimitive(scaled.model);
    REQUIRE(beforePrim.weights.size() == afterPrim.weights.size());
    for (std::size_t i = 0; i < beforePrim.weights.size(); ++i) {
        CHECK(approxEq(afterPrim.weights[i].x, beforePrim.weights[i].x));
    }

    // Non-root translations are already expressed in their parent's scaled space, so they must NOT move.
    REQUIRE(plain.model.nodes.size() == scaled.model.nodes.size());
    for (std::size_t i = 1; i < plain.model.nodes.size(); ++i) {
        CHECK(approxEq(scaled.model.nodes[i].translation, plain.model.nodes[i].translation));
        CHECK(approxEq(scaled.model.nodes[i].rotation.w, plain.model.nodes[i].rotation.w));
    }

    // GAP-CLOSING (sabotage seed S24). Every node in the skinned fixture has a ZERO translation, so
    // scaling all of them instead of only the root is invisible there -- the seed reddened nothing at
    // all until this arm existed. DAE_CHAIN's node 1 carries <translate>1 2 3</translate>, and it must
    // read (1, 2, 3) whatever the scale is.
    const std::string chain = dae(DAE_CHAIN);
    const ImportResult chainPlain =
        importModel("chain.dae", "", asBytes(chain), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult chainScaled = importModel("chain.dae", "", asBytes(chain), doubled, ImportDepth::Full, {});
    REQUIRE(chainPlain.status == ImportStatus::Ok);
    REQUIRE(chainScaled.status == ImportStatus::Ok);
    REQUIRE(chainScaled.model.nodes.size() == 4U);
    CHECK(approxEq(chainPlain.model.nodes[1].translation, engine::Vec3{1.0F, 2.0F, 3.0F}));
    CHECK(approxEq(chainScaled.model.nodes[1].translation, engine::Vec3{1.0F, 2.0F, 3.0F}));
}

// ---- step 8: caps, failure mapping and the validation refusal ----------------------------------------

// AI78 (AC-52) -- THE DISTINCTION THE PANEL SHOWS THE USER. A scene that PARSED and then failed
// aiProcess_ValidateDataStructure is Malformed with the validation message carried through; a document
// that could not be parsed at all is ParseFailed. Assimp's own wording is "Validation failed: ..." from
// ValidateDSProcess::ReportError (MEASURED against the pinned 6.0.4 source), which is what the substring
// test keys on.
TEST_CASE("AI78: a validation failure is Malformed and a parse failure is ParseFailed") {
    // TWO IDENTICALLY-NAMED JOINTS. The document parses -- Collada permits it -- and
    // ValidateDSProcess::Validate(aiMesh) then refuses it: "aiMesh::mBones[i], name = ... has the same
    // name as aiMesh::mBones[a]". This is the arm the plan asked for, and finding an input that reaches
    // it took measurement: the obvious candidate (an out-of-range <p> index) never gets there, because
    // ColladaParser rejects it FIRST with "Invalid data index (99/3) in primitive specification", which
    // is a PARSE failure and is asserted as such below.
    std::string controllers(SKIN_LIBRARIES);
    const std::size_t names = controllers.find("Bone1 Bone2");
    REQUIRE(names != std::string::npos);
    controllers.replace(names, std::string_view("Bone1 Bone2").size(), "Bone1 Bone1");
    const std::string duplicate = skinnedDae(controllers, "");
    const ImportResult invalid =
        importModel("dup.dae", "", asBytes(duplicate), ImportSettings{}, ImportDepth::Full, {});
    INFO("validation message: ", invalid.message);
    CHECK(invalid.status == ImportStatus::Malformed);
    CHECK(invalid.status != ImportStatus::ParseFailed);
    CHECK(invalid.message.find("alidation") != std::string::npos);

    const std::string garbage = "<COLLADA><this is not xml at all";
    const ImportResult unparseable =
        importModel("junk.dae", "", asBytes(garbage), ImportSettings{}, ImportDepth::Full, {});
    INFO("parse message: ", unparseable.message);
    CHECK(unparseable.status == ImportStatus::ParseFailed);
    CHECK_FALSE(unparseable.message.empty());

    // And the measured third case, recorded so the two paths are never conflated: an out-of-range
    // primitive index in a .dae is a PARSER refusal, not a validation one, and reads ParseFailed.
    std::string text = dae(DAE_CHAIN);
    const std::size_t at = text.find("<p>0 1 2</p>");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string_view("<p>0 1 2</p>").size(), "<p>0 1 99</p>");
    const ImportResult parserRefusal =
        importModel("bad.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("parser message: ", parserRefusal.message);
    CHECK(parserRefusal.status == ImportStatus::ParseFailed);
    CHECK(parserRefusal.message.find("Invalid data index") != std::string::npos);
}

// AI79 (AC-53) -- TWO caps in ONE import produce ONE Truncated and TWO "; "-joined messages, which is
// ImportResult::message's own documented rule. A 300-deep node chain (past MAX_NODE_DEPTH) in a document
// that also names more textures than MAX_EXTERNAL_URIS.
TEST_CASE("AI79: two caps in one import produce one Truncated and two joined messages") {
    const std::size_t textures = engine::editor::MAX_EXTERNAL_URIS + 4U;
    std::string images;
    std::string effects;
    std::string materials;
    for (std::size_t i = 0; i < textures; ++i) {
        images += std::format(R"(<image id="i{}"><init_from>t{}.png</init_from></image>)", i, i);
        effects += std::format(
            R"(<effect id="e{}"><profile_COMMON><newparam sid="s{}"><surface type="2D"><init_from>i{}</init_from></surface></newparam>)"
            R"(<newparam sid="sam{}"><sampler2D><source>s{}</source></sampler2D></newparam>)"
            R"(<technique sid="common"><lambert><diffuse><texture texture="sam{}" texcoord="UV0"/></diffuse></lambert></technique>)"
            R"(</profile_COMMON></effect>)",
            i, i, i, i, i, i);
        materials += std::format(R"(<material id="m{}" name="M{}"><instance_effect url="#e{}"/></material>)", i, i, i);
    }
    std::string chain;
    std::string closing;
    for (unsigned int i = 0; i < 300U; ++i) {
        chain += std::format(R"(<node id="n{}" name="n{}">)", i, i);
        closing += "</node>";
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>{}</library_images>
  <library_effects>{}</library_effects>
  <library_materials>{}</library_materials>
{}  <library_visual_scenes><visual_scene id="S" name="S">
{}<instance_geometry url="#g1"/>{}
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        images, effects, materials, DAE_TRIANGLE_GEOMETRY, chain, closing);

    const ImportResult result = importModel("two.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("depth") != std::string::npos);
    CHECK(result.message.find("external reference") != std::string::npos);
    CHECK(result.message.find("; ") != std::string::npos);
}

// AI80 (AC-54) -- `warnings` caps at MAX_IMPORT_WARNINGS; `warningTotal` does NOT. Forty refused texture
// paths produce forty warnings and twenty entries -- the MAX_REPORTED_PER_CATEGORY shape, a fifth use.
TEST_CASE("AI80: warnings cap at MAX_IMPORT_WARNINGS while warningTotal stays uncapped") {
    constexpr std::size_t REFUSALS = 40;
    std::string images;
    std::string effects;
    std::string materials;
    for (std::size_t i = 0; i < REFUSALS; ++i) {
        images += std::format(R"(<image id="i{}"><init_from>../../escape{}.png</init_from></image>)", i, i);
        effects += std::format(
            R"(<effect id="e{}"><profile_COMMON><newparam sid="s{}"><surface type="2D"><init_from>i{}</init_from></surface></newparam>)"
            R"(<newparam sid="sam{}"><sampler2D><source>s{}</source></sampler2D></newparam>)"
            R"(<technique sid="common"><lambert><diffuse><texture texture="sam{}" texcoord="UV0"/></diffuse></lambert></technique>)"
            R"(</profile_COMMON></effect>)",
            i, i, i, i, i, i);
        materials += std::format(R"(<material id="m{}" name="M{}"><instance_effect url="#e{}"/></material>)", i, i, i);
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_images>{}</library_images>
  <library_effects>{}</library_effects>
  <library_materials>{}</library_materials>
{}  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="N" name="N"><instance_geometry url="#g1"/></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        images, effects, materials, DAE_TRIANGLE_GEOMETRY);

    const ImportResult result = importModel("warn.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message, " warningTotal: ", result.warningTotal);
    REQUIRE(result.status == ImportStatus::Ok);
    CHECK(result.warnings.size() == engine::editor::MAX_IMPORT_WARNINGS);
    CHECK(result.warningTotal == REFUSALS);
    CHECK(result.externalUris.empty());
}

// AI81 (AC-53) -- MAX_NODES_PER_MODEL. A COHERENT smaller model: no `children` entry may point past
// nodes.size(), and every child's `parent` must still name the node that lists it.
TEST_CASE("AI81: overflowing MAX_NODES_PER_MODEL truncates without leaving a dangling child index") {
    const std::size_t nodes = engine::editor::MAX_NODES_PER_MODEL + 4U;
    std::string siblings;
    siblings.reserve(nodes * 32U);
    for (std::size_t i = 0; i < nodes; ++i) {
        siblings += std::format(R"(<node id="n{}" name="n{}"/>)", i, i);
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
{}  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="G" name="G"><instance_geometry url="#g1"/></node>
{}
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        DAE_TRIANGLE_GEOMETRY, siblings);

    const ImportResult result = importModel("nodes.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("node count") != std::string::npos);
    CHECK(result.model.nodes.size() == engine::editor::MAX_NODES_PER_MODEL);
    CHECK(result.model.summary.nodeCount == engine::editor::MAX_NODES_PER_MODEL);
    for (std::size_t i = 0; i < result.model.nodes.size(); ++i) {
        for (const std::uint32_t child : result.model.nodes[i].children) {
            REQUIRE(child < result.model.nodes.size());
            CHECK(result.model.nodes[child].parent == i);
        }
    }
}

// AI82 (AC-53) -- MAX_PRIMITIVES_PER_MODEL, and the ONLY input in this suite that produces a SURVIVING
// mesh with ZERO primitives. That makes it the one place the point-box fallback is observable: a mesh
// the cap refused must carry Aabb{} (valid, a point at the origin), never the Aabb::empty() sentinel,
// whose NaN centre would leak into anything that folded it.
TEST_CASE("AI82: overflowing MAX_PRIMITIVES_PER_MODEL leaves the refused meshes a point box") {
    const std::size_t meshes = engine::editor::MAX_PRIMITIVES_PER_MODEL + 4U;
    std::string geometries;
    std::string nodes;
    geometries.reserve(meshes * 512U);
    nodes.reserve(meshes * 64U);
    for (std::size_t i = 0; i < meshes; ++i) {
        geometries += std::format(
            R"(<geometry id="g{}" name="G{}"><mesh><source id="p{}"><float_array id="pa{}" count="9">0 0 0 1 0 0 0 1 0</float_array>)"
            R"(<technique_common><accessor source="#pa{}" count="3" stride="3"><param name="X" type="float"/>)"
            R"(<param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>)"
            R"(<vertices id="v{}"><input semantic="POSITION" source="#p{}"/></vertices>)"
            R"(<triangles count="1"><input semantic="VERTEX" source="#v{}" offset="0"/><p>0 1 2</p></triangles>)"
            R"(</mesh></geometry>)",
            i, i, i, i, i, i, i, i);
        nodes += std::format(R"(<node id="n{}" name="n{}"><instance_geometry url="#g{}"/></node>)", i, i, i);
    }
    const std::string text = std::format(
        R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
  <library_geometries>{}</library_geometries>
  <library_visual_scenes><visual_scene id="S" name="S">
{}
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)",
        geometries, nodes);

    const ImportResult result = importModel("prims.dae", "", asBytes(text), ImportSettings{}, ImportDepth::Full, {});
    INFO("message: ", result.message);
    REQUIRE(result.status == ImportStatus::Truncated);
    CHECK(result.message.find("primitive count") != std::string::npos);
    CHECK(result.model.summary.primitiveCount == engine::editor::MAX_PRIMITIVES_PER_MODEL);

    std::size_t empties = 0;
    for (const engine::editor::ImportedMesh& mesh : result.model.meshes) {
        if (!mesh.primitives.empty()) {
            continue;
        }
        ++empties;
        CHECK(mesh.bounds.valid());  // a POINT box, never Aabb::empty()'s NaN-centred sentinel
        CHECK(approxEq(mesh.bounds.min, mesh.bounds.max));
    }
    CHECK(empties == 4U);
    // And the model bounds are folded from the SURVIVING primitives, so the refused meshes' point boxes
    // do not appear in them.
    REQUIRE(result.model.summary.bounds.valid());
    CHECK(approxEq(result.model.summary.bounds.max, engine::Vec3{1.0F, 1.0F, 0.0F}));
}

// AI83 -- every cap message names its cap in human-readable prose, so a future refactor cannot silently
// reduce them all to "limit exceeded". Read from the comment-stripped source, because there is no input
// that hits all eight caps in one import and a per-cap behavioural case would only re-assert what the
// eight cap cases already do.
TEST_CASE("AI83: every Truncated message names the cap it hit") {
    const std::string code = strippedSource("assimp_import.cpp");
    REQUIRE_FALSE(code.empty());
    for (const std::string_view phrase :
         {"the node depth exceeds", "the node count exceeds", "the primitive count exceeds", "the vertex count exceeds",
          "the index count exceeds", "the material count exceeds", "the external reference count exceeds",
          "the joint count exceeds", "the animation key count exceeds"}) {
        INFO("cap message: ", phrase);
        CHECK(code.find(phrase) != std::string::npos);
    }
    // And none of them is a bare, unhelpful sentence.
    CHECK(code.find("\"limit exceeded\"") == std::string::npos);
    CHECK(code.find("\"too many\"") == std::string::npos);
}
