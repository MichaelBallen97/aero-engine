// tests/scene_serialize_test.cpp — task 1.4.2: the World <-> SceneDocument bridge (spec §3.9).
// Standalone single-TU doctest target, gated inside AERO_REFLECT_TOOLS (own
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, like aero_reflect_json_test — no shared test_main.cpp).
// Tier-0 throughout: no GPU, no reflect-gen at test time, no randomness, no files besides the one
// committed samples/phase-1-scene/scene.json test 9 reads.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/core/guid.hpp>  // task 3.1.5 — MeshRenderer's two Guid fields
#include <aero/core/math.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/scene_serialize/scene_serialize.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
// <ostream> is required by MSVC, not by libc++ (the 0.4.1 trap): doctest stringifies a failing CHECK
// involving a std::string_view through operator<<, and MSVC's overload needs a COMPLETE std::ostream.
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::scene_serialize;

namespace {

// Mirrors spec §3.8's committed lit tableau: 6 entities, FLAT (D6), all 5 built-ins across the set.
// Also used to generate the committed samples/phase-1-scene/scene.json (via saveWorldText), so the
// numeric layout here and the committed file's numbers must stay in lockstep (see that file's header
// comment). Order is load-bearing: the camera is created first (lowest World index, 1.4.1's camera
// selection rule).
void buildTableau(World& world) {
    const Entity camera = world.create();
    world.add<Transform>(camera, Transform{.position = {0.0F, 2.5F, 6.0F},
                                           .rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, radians(-18.0F))});
    world.add<Camera>(camera, Camera{});  // defaults == the spec table's values

    const Entity ground = world.create();
    world.add<Transform>(ground, Transform{.position = {0.0F, 0.0F, 0.0F}, .scale = {8.0F, 1.0F, 8.0F}});
    world.add<MeshRenderer>(ground, MeshRenderer{.primitive = 2, .color = {0.5F, 0.5F, 0.55F}});

    const Entity cube = world.create();
    world.add<Transform>(cube, Transform{.position = {-1.3F, 0.5F, 0.0F},
                                         .rotation = fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, radians(20.0F))});
    world.add<MeshRenderer>(cube, MeshRenderer{.primitive = 0, .color = {0.85F, 0.35F, 0.30F}});

    const Entity sphere = world.create();
    world.add<Transform>(sphere, Transform{.position = {1.3F, 0.6F, 0.0F}, .scale = {0.6F, 0.6F, 0.6F}});
    world.add<MeshRenderer>(sphere, MeshRenderer{.primitive = 1, .color = {0.30F, 0.55F, 0.85F}});

    const Entity sun = world.create();
    world.add<Transform>(sun, Transform{.position = {0.0F, 0.0F, 0.0F},
                                        .rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, radians(-50.0F))});
    // Intensities tuned DOWN from the spec table's starting point (spec §3.8: "numbers are a starting
    // point — tune at implementation for a well-framed, clearly-lit frame"): scene.frag.hlsl has no
    // tonemapping, so the original 2.0/6.0 blew every surface out to flat white on a real GPU run
    // (verified visually, samples/phase-1-scene/VALIDATION.md). These values keep each primitive's
    // base color visibly distinct while the lamp still visibly lights the tableau.
    world.add<DirectionalLight>(sun, DirectionalLight{.color = {1.0F, 0.96F, 0.9F}, .intensity = 0.9F});

    const Entity lamp = world.create();
    world.add<Transform>(lamp, Transform{.position = {0.0F, 2.5F, 2.0F}});
    world.add<PointLight>(lamp, PointLight{.color = {0.6F, 0.8F, 1.0F}, .intensity = 1.0F, .range = 12.0F});
}

std::vector<Entity> collectEntities(const World& world) {
    std::vector<Entity> out;
    world.eachEntity([&out](Entity e) { out.push_back(e); });
    return out;
}

// ---- task 2.5.2: the golden fixtures ---------------------------------------------------------
// THESE THREE FILES ARE CONTENT PINS. Nothing in this repository regenerates them: no environment
// variable, no CMake option, no target, no script. If a deliberate format change makes one stale,
// the fix is to regenerate it BY HAND with the recipe recorded in docs/10's 2.5.2 entry, review the
// resulting diff AS a format change, and update docs/09 in the SAME commit. A fixture edit that
// arrives without a matching change to docs/09, engine/reflect/, engine/scene_serialize/ or
// tools/reflect-gen/ is a defect being made permanent (INV-2).
//
// Bytes are NECESSARY and NEVER SUFFICIENT. G5-G7 assert structure and semantics alongside them,
// because a load/save pair that BOTH stopped handling `parent` would pass a byte comparison the
// moment anyone regenerated the fixture. Removing those cases to "simplify" reopens that hole; S12
// in the task's sabotage matrix is the seed that proves it.
constexpr std::string_view GOLDEN_EMPTY = AERO_GOLDEN_SCENES_DIR "/empty.scene.json";
constexpr std::string_view GOLDEN_FULL = AERO_GOLDEN_SCENES_DIR "/full.scene.json";
constexpr std::string_view GOLDEN_EDGE = AERO_GOLDEN_SCENES_DIR "/edge.scene.json";
constexpr std::string_view PHASE1_SAMPLE = AERO_PHASE1_SCENE_DIR "/scene.json";

// The exact bytes the pre-existing "empty World" case already pins programmatically. Having BOTH
// means a writer change accompanied by a "helpfully" regenerated fixture still reddens here.
constexpr std::string_view EMPTY_DOCUMENT = "{\n  \"version\": 1,\n  \"entities\": []\n}\n";

// edge.scene.json's two exotic names, as BYTES. Written with \x escapes rather than glyphs because
// this task is a byte comparison and an escape is source-charset-independent by construction (the
// tree sets no /utf-8 flag anywhere, so MSVC reads this file in the active code page). They spell:
//   ESCAPED_NAME : quote" backslash\ tab<TAB> newline<LF> control<0x01> end          (44 bytes)
//   UTF8_NAME    : c a f e-acute SP check-mark SP rocket -- 2-, 3- and 4-byte UTF-8  (14 bytes)
// EVERY hex escape below is terminated by a backslash or a non-hex character. "a\x01b" would be ONE
// byte (0x1b, with 'b' swallowed as a hex digit) -- never write one without checking what follows.
constexpr std::string_view ESCAPED_NAME = "quote\" backslash\\ tab\t newline\n control\x01 end";
constexpr std::string_view UTF8_NAME = "caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x9A\x80";

// Loads a fixture, asserts hygiene and a clean parse, and hands back the parsed document. REQUIREs
// on every failure -- a golden that can skip itself is the same failure mode as a regeneration flag.
[[nodiscard]] SceneDocument requireGolden(std::string_view path, std::string& bytesOut) {
    const scene_golden::FileBytes file = scene_golden::readBytes(path);
    REQUIRE_MESSAGE(file.ok, file.error);
    const std::string hygiene = scene_golden::hygieneComplaint(file.text);
    CHECK_MESSAGE(hygiene.empty(), hygiene);
    SceneParseResult parsed = parseScene(file.text);
    REQUIRE_MESSAGE(parsed.document.has_value(), parsed.error.message);
    bytesOut = file.text;
    return std::move(*parsed.document);
}

}  // namespace

TEST_CASE("scene_serialize: load basic (AC-1)") {
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::Transform": {
          "position": {"x": 1, "y": 2, "z": 3},
          "rotation": {"x": 0, "y": 0, "z": 0, "w": 1},
          "scale": {"x": 1, "y": 1, "z": 1}
        }
      }
    },
    {
      "id": 2,
      "components": {
        "engine::MeshRenderer": {
          "primitive": 1,
          "color": {"x": 0.25, "y": 0.5, "z": 0.75}
        }
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(world.entityCount() == 2);
    CHECK(result.report.entitiesCreated == 2);
    CHECK(result.report.componentsAttached == 2);
    CHECK(result.report.componentsSkipped == 0);
    CHECK(result.report.componentsFailed == 0);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 2);
    CHECK(world.has<Transform>(entities[0]));
    CHECK(!world.has<MeshRenderer>(entities[0]));
    const Transform* t = world.get<Transform>(entities[0]);
    REQUIRE(t != nullptr);
    CHECK(*t == Transform{.position = {1.0F, 2.0F, 3.0F}});

    CHECK(world.has<MeshRenderer>(entities[1]));
    CHECK(!world.has<Transform>(entities[1]));
    const MeshRenderer* mr = world.get<MeshRenderer>(entities[1]);
    REQUIRE(mr != nullptr);
    CHECK(*mr == MeshRenderer{.primitive = 1, .color = {0.25F, 0.5F, 0.75F}});
}

TEST_CASE("scene_serialize: unknown component name WARN+skip, load continues (AC-1)") {
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::Transform": {"position": {"x": 0, "y": 0, "z": 0}},
        "demo::Ghost": {}
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.entitiesCreated == 1);
    CHECK(result.report.componentsAttached == 1);
    CHECK(result.report.componentsSkipped == 1);
    CHECK(result.report.componentsFailed == 0);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    CHECK(world.has<Transform>(entities[0]));
}

TEST_CASE("scene_serialize: bad payload best-effort (AC-1)") {
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::Transform": {
          "position": 5,
          "scale": {"x": 2, "y": 2, "z": 2}
        }
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.entitiesCreated == 1);
    CHECK(result.report.componentsAttached == 1);
    CHECK(result.report.componentsFailed == 1);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    const Transform* t = world.get<Transform>(entities[0]);
    REQUIRE(t != nullptr);
    CHECK(t->position == Vec3{0.0F, 0.0F, 0.0F});  // left at default (the bad field)
    CHECK(t->scale == Vec3{2.0F, 2.0F, 2.0F});     // the good field still applied
}

// ---- task 3.1.5: docs/09 §2.3's tolerance table, on MeshRenderer's three new keys ------------------
//
// The three keys are APPENDED, so every scene written before this task omits all three. §2.3's
// missing-key rule makes that silent, not degraded; and a malformed Guid is an ordinary bad field --
// warned, counted, left at its prior value, with every other field still applied.

namespace {

// The canonical text of the two guids below, as LITERALS -- so a writer that ever stopped emitting
// lowercase would redden here as well as in the byte-pinned goldens.
constexpr std::string_view MESH_GUID_TEXT = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
constexpr std::string_view MATERIAL_GUID_TEXT = "0fedcba987654321fedcba9876543210";
constexpr Guid MESH_GUID{0xA1B2C3D4E5F60718ULL, 0x293A4B5C6D7E8F90ULL};
constexpr Guid MATERIAL_GUID{0x0FEDCBA987654321ULL, 0xFEDCBA9876543210ULL};

}  // namespace

TEST_CASE("scene_serialize: a PRE-3.1.5 MeshRenderer payload loads with all three new keys at defaults") {
    // Byte-for-byte what every scene in this tree looked like before task 3.1.5: no mesh, no
    // meshIndex, no material. §2.3's missing-key rule is SILENT -- not a warning, not a failure.
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::MeshRenderer": {
          "primitive": 2,
          "color": {"x": 0.5, "y": 0.5, "z": 0.55}
        }
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.componentsAttached == 1);
    CHECK(result.report.componentsSkipped == 0);
    CHECK(result.report.componentsFailed == 0);  // SILENT: a missing key is schema evolution, not an error

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    const MeshRenderer* mr = world.get<MeshRenderer>(entities[0]);
    REQUIRE(mr != nullptr);
    CHECK(mr->primitive == 2);                    // the old fields still read
    CHECK(mr->color == Vec3{0.5F, 0.5F, 0.55F});  //
    CHECK_FALSE(mr->mesh.valid());                // nil => draw `primitive`, exactly as before 3.1.5
    CHECK(mr->meshIndex == 0);
    CHECK_FALSE(mr->material.valid());
}

TEST_CASE("scene_serialize: a NUMERIC mesh value is a bad field -- mesh stays nil, every other field applies") {
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::MeshRenderer": {
          "primitive": 1,
          "color": {"x": 0.25, "y": 0.5, "z": 0.75},
          "mesh": 7,
          "meshIndex": 4,
          "material": "0fedcba987654321fedcba9876543210"
        }
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.componentsAttached == 1);
    CHECK(result.report.componentsFailed == 1);  // the WARN's observable

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    const MeshRenderer* mr = world.get<MeshRenderer>(entities[0]);
    REQUIRE(mr != nullptr);
    CHECK_FALSE(mr->mesh.valid());  // left at its prior value -- never "helpfully" anything else
    CHECK(mr->primitive == 1);      // best-effort: every OTHER field still applied,
    CHECK(mr->color == Vec3{0.25F, 0.5F, 0.75F});
    CHECK(mr->meshIndex == 4);             // including the two that come AFTER the bad one
    CHECK(mr->material == MATERIAL_GUID);  //
}

TEST_CASE("scene_serialize: a 32-character NON-HEX mesh value behaves identically to a numeric one") {
    // Exactly GUID_TEXT_LENGTH characters, so only the alphabet is wrong -- the arm a length check
    // alone would let through.
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::MeshRenderer": {
          "primitive": 1,
          "color": {"x": 0.25, "y": 0.5, "z": 0.75},
          "mesh": "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
          "meshIndex": 4,
          "material": "0fedcba987654321fedcba9876543210"
        }
      }
    }
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.componentsAttached == 1);
    CHECK(result.report.componentsFailed == 1);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    const MeshRenderer* mr = world.get<MeshRenderer>(entities[0]);
    REQUIRE(mr != nullptr);
    CHECK_FALSE(mr->mesh.valid());
    CHECK(mr->primitive == 1);
    CHECK(mr->meshIndex == 4);
    CHECK(mr->material == MATERIAL_GUID);
}

TEST_CASE("scene_serialize: a VALID non-nil mesh reference round-trips, lowercase, byte for byte") {
    // The two guids appear here as LITERALS and are cross-checked against MESH_GUID/MATERIAL_GUID
    // below, so a formatGuid/parseGuid disagreement about the text form reddens rather than cancelling
    // itself out.
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {
      "id": 1,
      "components": {
        "engine::MeshRenderer": {
          "primitive": 0,
          "color": {"x": 1, "y": 1, "z": 1},
          "mesh": "a1b2c3d4e5f60718293a4b5c6d7e8f90",
          "meshIndex": 3,
          "material": "0fedcba987654321fedcba9876543210"
        }
      }
    }
  ]
}
)";
    CHECK(TEXT.find(MESH_GUID_TEXT) != std::string_view::npos);      // the literals above and the
    CHECK(TEXT.find(MATERIAL_GUID_TEXT) != std::string_view::npos);  // constants below are the same text

    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.componentsFailed == 0);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 1);
    const MeshRenderer* mr = world.get<MeshRenderer>(entities[0]);
    REQUIRE(mr != nullptr);
    CHECK(*mr ==
          MeshRenderer{
              .primitive = 0, .color = Vec3::one(), .mesh = MESH_GUID, .meshIndex = 3, .material = MATERIAL_GUID});

    // Save: the canonical spelling is lowercase, and both guids survive distinct.
    const std::string saved = saveWorldText(world);
    CHECK(saved.find(std::string(MESH_GUID_TEXT)) != std::string::npos);
    CHECK(saved.find(std::string(MATERIAL_GUID_TEXT)) != std::string::npos);

    // Re-load the saved text: the component is bit-identical, and re-saving is a fixpoint.
    World reloaded;
    const SceneLoadResult second = loadSceneText(reloaded, saved);
    REQUIRE(!second.error.has_value());
    const std::vector<Entity> reloadedEntities = collectEntities(reloaded);
    REQUIRE(reloadedEntities.size() == 1);
    const MeshRenderer* back = reloaded.get<MeshRenderer>(reloadedEntities[0]);
    REQUIRE(back != nullptr);
    CHECK(*back == *mr);
    CHECK(saveWorldText(reloaded) == saved);
}

TEST_CASE("scene_serialize: parent + forward reference (AC-1)") {
    // Child (file id 2) listed BEFORE its parent (file id 1) — forward references are legal
    // (docs/09 §2.2); a third root entity (id 3) proves parent()==Entity{} for a non-parented one.
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {"id": 2, "parent": 1},
    {"id": 1},
    {"id": 3}
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.entitiesCreated == 3);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 3);
    const Entity child = entities[0];   // file id 2, created first
    const Entity parent = entities[1];  // file id 1, created second
    const Entity root = entities[2];    // file id 3, never parented
    CHECK(world.parent(child) == parent);
    CHECK(world.parent(parent) == Entity{});
    CHECK(world.parent(root) == Entity{});
}

TEST_CASE("scene_serialize: parse error leaves the World untouched (AC-2)") {
    World world;
    {
        const SceneLoadResult result = loadSceneText(world, "{ not json");
        CHECK(result.error.has_value());
        CHECK(world.entityCount() == 0);
    }
    {
        const SceneLoadResult result = loadSceneText(world, R"({"version": 2, "entities": []})");
        CHECK(result.error.has_value());
        CHECK(world.entityCount() == 0);
    }
}

TEST_CASE("scene_serialize: save shape (AC-3)") {
    World world;
    const Entity a = world.create();
    world.add<Transform>(a, Transform{.position = {1.0F, 0.0F, 0.0F}});
    world.add<MeshRenderer>(a, MeshRenderer{.primitive = 0});
    const Entity b = world.create();
    world.add<Transform>(b, Transform{.position = {2.0F, 0.0F, 0.0F}});
    CHECK(world.setParent(b, a));

    const SceneDocument doc = saveWorld(world);
    REQUIRE(doc.entities.size() == 2);
    CHECK(doc.entities[0].id == 1);
    CHECK(doc.entities[0].parent == 0);
    CHECK(doc.entities[1].id == 2);
    CHECK(doc.entities[1].parent == 1);

    REQUIRE(doc.entities[0].components.size() == 2);
    CHECK(doc.entities[0].components[0].type == "engine::Transform");  // registry order
    CHECK(doc.entities[0].components[1].type == "engine::MeshRenderer");
    REQUIRE(doc.entities[1].components.size() == 1);
    CHECK(doc.entities[1].components[0].type == "engine::Transform");
}

TEST_CASE("scene_serialize: round-trip idempotence (AC-4, the gate proof)") {
    World a;
    buildTableau(a);
    const std::string t1 = saveWorldText(a);

    World b;
    const SceneLoadResult result = loadSceneText(b, t1);
    REQUIRE(!result.error.has_value());
    CHECK(result.report.entitiesCreated == 6);
    CHECK(result.report.componentsFailed == 0);
    CHECK(result.report.componentsSkipped == 0);

    const std::string t2 = saveWorldText(b);
    CHECK(t1 == t2);

    // Spot-check B bit-exact against the builder: cube's MeshRenderer.color, camera's fovYRadians,
    // lamp's PointLight.range, sun's DirectionalLight.intensity.
    const std::vector<Entity> entities = collectEntities(b);
    REQUIRE(entities.size() == 6);

    const MeshRenderer* cubeMr = nullptr;
    const Camera* cam = nullptr;
    const PointLight* lamp = nullptr;
    const DirectionalLight* sun = nullptr;
    for (const Entity e : entities) {
        if (const MeshRenderer* mr = b.get<MeshRenderer>(e); mr != nullptr && mr->primitive == 0) {
            cubeMr = mr;
        }
        if (const Camera* c = b.get<Camera>(e)) {
            cam = c;
        }
        if (const PointLight* p = b.get<PointLight>(e)) {
            lamp = p;
        }
        if (const DirectionalLight* d = b.get<DirectionalLight>(e)) {
            sun = d;
        }
    }
    REQUIRE(cubeMr != nullptr);
    CHECK(cubeMr->color == Vec3{0.85F, 0.35F, 0.30F});
    REQUIRE(cam != nullptr);
    CHECK(cam->fovYRadians == radians(60.0F));
    REQUIRE(lamp != nullptr);
    CHECK(lamp->range == 12.0F);
    REQUIRE(sun != nullptr);
    CHECK(sun->intensity == 0.9F);
}

TEST_CASE("scene_serialize: empty World (AC-3/AC-8)") {
    const World world;
    const std::string text = saveWorldText(world);
    CHECK(text == "{\n  \"version\": 1,\n  \"entities\": []\n}\n");

    World other;
    const SceneLoadResult result = loadSceneText(other, text);
    REQUIRE(!result.error.has_value());
    CHECK(other.entityCount() == 0);
}

TEST_CASE("scene_serialize: the committed samples/phase-1-scene/scene.json (AC-6)") {
#ifdef AERO_PHASE1_SCENE_DIR
    const std::ifstream file(std::string(AERO_PHASE1_SCENE_DIR) + "/scene.json", std::ios::binary);
    REQUIRE(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string bytes = buffer.str();

    const SceneParseResult parsed = parseScene(bytes);
    REQUIRE(parsed.ok());

    World world;
    const SceneLoadReport report = loadScene(world, *parsed.document);
    CHECK(report.entitiesCreated == 6);
    CHECK(report.componentsSkipped == 0);
    CHECK(report.componentsFailed == 0);

    std::size_t cameraCount = 0;
    std::size_t dirCount = 0;
    std::size_t pointCount = 0;
    std::vector<std::uint32_t> primitives;
    world.eachEntity([&](Entity e) {
        if (world.has<Camera>(e)) {
            ++cameraCount;
        }
        if (world.has<DirectionalLight>(e)) {
            ++dirCount;
        }
        if (world.has<PointLight>(e)) {
            ++pointCount;
        }
        if (const MeshRenderer* mr = world.get<MeshRenderer>(e)) {
            primitives.push_back(mr->primitive);
        }
    });
    CHECK(cameraCount == 1);
    CHECK(dirCount == 1);
    CHECK(pointCount == 1);
    REQUIRE(primitives.size() == 3);
    CHECK(primitives[0] == 2);  // ground (Plane)
    CHECK(primitives[1] == 0);  // cube
    CHECK(primitives[2] == 1);  // sphere

    // task 2.2.1 / F15: the six committed names now survive into the World and back out.
    std::vector<std::string> names;
    world.eachEntity([&](Entity e) { names.emplace_back(world.name(e)); });
    REQUIRE(names.size() == 6);
    CHECK(names[0] == "camera");
    CHECK(names[1] == "ground");
    CHECK(names[2] == "cube");
    CHECK(names[3] == "sphere");
    CHECK(names[4] == "sun");
    CHECK(names[5] == "lamp");
    CHECK(saveWorldText(world).find(R"("name": "lamp")") != std::string::npos);

    // Canonical-form idempotence (docs/09 guarantee 2): write(parse(bytes)) re-parses+re-emits to itself.
    const std::string reemitted = writeSceneText(*parsed.document);
    const SceneParseResult reparsed = parseScene(reemitted);
    REQUIRE(reparsed.ok());
    CHECK(writeSceneText(*reparsed.document) == reemitted);
#else
    MESSAGE("AERO_PHASE1_SCENE_DIR not defined — skipping committed-scene test");
#endif
}

TEST_CASE("scene_serialize: dispatch/registration parity (AC-3/D8)") {
    const World world;
    const std::span<const std::string_view> names = builtinComponentNames();
    CHECK(names.size() == 5);
    CHECK(names.size() == world.componentTypeCount());
    for (const std::string_view name : names) {
        CHECK(world.findComponentType(name).valid());
    }
}

TEST_CASE("scene_serialize: names round-trip (2.2.1 D5, AC-7)") {
    // The LOAD direction, from a hand-written document. This literal is deliberately NOT canonical
    // (compact, and an empty Transform payload that save expands), so it is used only to prove the
    // name is applied and re-emitted -- byte-exact idempotence is proven programmatically below.
    constexpr std::string_view TEXT = R"({
  "version": 1,
  "entities": [
    {"id": 1, "name": "camera", "components": {"engine::Transform": {}}},
    {"id": 2, "components": {"engine::Transform": {}}}
  ]
}
)";
    World world;
    const SceneLoadResult result = loadSceneText(world, TEXT);
    REQUIRE(!result.error.has_value());
    REQUIRE(world.entityCount() == 2);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 2);
    CHECK(world.name(entities[0]) == std::string_view{"camera"});
    CHECK(world.name(entities[1]).empty());  // absent name stays absent

    const SceneDocument doc = saveWorld(world);
    REQUIRE(doc.entities.size() == 2);
    CHECK(doc.entities[0].name == "camera");
    CHECK(doc.entities[1].name.empty());  // omitted, not ""-emitted -- see the text below

    const std::string text = saveWorldText(world);
    CHECK(text.find(R"("name": "camera")") != std::string::npos);
    CHECK(text.find(R"("id": 2,)") != std::string::npos);  // entity 2 has no name key at all
}

TEST_CASE("scene_serialize: a named World is byte-exactly idempotent (2.2.1 AC-7)") {
    // Built in code, so there is no hand-written canonical literal to drift (C3).
    World a;
    const Entity camera = a.create();
    REQUIRE(a.setName(camera, "Main Camera"));
    a.add<Transform>(camera, Transform{.position = {0.0F, 1.0F, 2.0F}});
    a.add<Camera>(camera, Camera{});
    const Entity child = a.create();
    REQUIRE(a.setName(child, "Child With A Long Name"));
    a.add<Transform>(child, Transform{});
    REQUIRE(a.setParent(child, camera));
    const Entity anonymous = a.create();
    a.add<MeshRenderer>(anonymous, MeshRenderer{});  // deliberately unnamed

    const std::string t1 = saveWorldText(a);
    World b;
    REQUIRE(!loadSceneText(b, t1).error.has_value());
    const std::string t2 = saveWorldText(b);
    CHECK(t1 == t2);  // byte-exact -- names included

    const std::vector<Entity> es = collectEntities(b);
    REQUIRE(es.size() == 3);
    CHECK(b.name(es[0]) == std::string_view{"Main Camera"});
    CHECK(b.name(es[1]) == std::string_view{"Child With A Long Name"});
    CHECK(b.name(es[2]).empty());
    CHECK(b.parent(es[1]) == es[0]);  // hierarchy still round-trips
}

// ================================================================================================
// task 2.5.2 -- the golden battery. G1-G4: the byte comparison itself.
// ================================================================================================

TEST_CASE("scene_golden: empty.scene.json is a byte-exact fixpoint (G1/AC-1/AC-2/AC-3/AC-6/AC-8)") {
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_EMPTY, bytes);
    CHECK(doc.entities.empty());

    World world;
    const SceneLoadReport report = loadScene(world, doc);
    CHECK(report.entitiesCreated == 0);
    CHECK(report.componentsAttached == 0);
    CHECK(report.componentsSkipped == 0);
    CHECK(report.componentsFailed == 0);

    const std::string actual = saveWorldText(world);
    INFO(scene_golden::describeMismatch(bytes, actual));
    if (actual != bytes) {
        scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, "empty", actual);
    }
    CHECK(actual == bytes);
    CHECK(bytes == EMPTY_DOCUMENT);
}

TEST_CASE("scene_golden: full.scene.json is a byte-exact fixpoint (G2/AC-1/AC-2/AC-3/AC-8)") {
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_FULL, bytes);

    World world;
    const SceneLoadReport report = loadScene(world, doc);
    CHECK(report.entitiesCreated == 8);
    CHECK(report.componentsAttached == 10);
    CHECK(report.componentsSkipped == 0);  // non-zero here means the fixture named a type this build
    CHECK(report.componentsFailed == 0);   // cannot resolve -- i.e. the fixture degraded (E2)

    const std::string actual = saveWorldText(world);
    INFO(scene_golden::describeMismatch(bytes, actual));
    if (actual != bytes) {
        scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, "full", actual);
    }
    CHECK(actual == bytes);
}

TEST_CASE("scene_golden: edge.scene.json is a byte-exact fixpoint (G3/AC-1/AC-2/AC-3/AC-8)") {
    // If exactly THIS case reddens and G2 does not, suspect a std::to_chars divergence between
    // standard libraries -- every exponent-form lexeme in the tree's fixtures lives in this one file
    // by construction (spec D2/D4). The fix goes into engine/reflect/src/json_writer.cpp's
    // value(float), NEVER into the fixture: a scene file whose bytes depend on which OS saved it
    // would make every cross-platform diff of a scene noise, which is the property docs/09 promises
    // and this battery exists to lock.
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_EDGE, bytes);

    World world;
    const SceneLoadReport report = loadScene(world, doc);
    CHECK(report.entitiesCreated == 4);
    CHECK(report.componentsAttached == 3);
    CHECK(report.componentsSkipped == 0);
    CHECK(report.componentsFailed == 0);

    const std::string actual = saveWorldText(world);
    INFO(scene_golden::describeMismatch(bytes, actual));
    if (actual != bytes) {
        scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, "edge", actual);
    }
    CHECK(actual == bytes);
}

TEST_CASE("scene_golden: every fixture converges on a second cycle (G4/AC-2)") {
    // A first cycle proving equality is not convergence: a writer that alternated between two forms
    // would pass G1-G3 on one of them and fail here.
    for (const std::string_view path : {GOLDEN_EMPTY, GOLDEN_FULL, GOLDEN_EDGE}) {
        INFO(std::string{path});
        const scene_golden::FileBytes file = scene_golden::readBytes(path);
        REQUIRE_MESSAGE(file.ok, file.error);

        World first;
        REQUIRE_FALSE(loadSceneText(first, file.text).error.has_value());
        const std::string cycle1 = saveWorldText(first);

        World second;
        REQUIRE_FALSE(loadSceneText(second, cycle1).error.has_value());
        const std::string cycle2 = saveWorldText(second);

        // ONE INFO PER COMPARISON. A single describeMismatch(cycle1, cycle2) would print an empty
        // string when it is the fixture-vs-cycle-1 check that fails, which is the failure a remote
        // lane is most likely to hit and the one D5 exists to make readable.
        INFO(scene_golden::describeMismatch(file.text, cycle1));
        CHECK(cycle1 == file.text);
        INFO(scene_golden::describeMismatch(cycle1, cycle2));
        CHECK(cycle2 == cycle1);
    }
}

// ================================================================================================
// G5-G10: the defences. A load/save pair that BOTH stopped handling a key would pass G1-G4 the
// moment anyone regenerated a fixture; these cases are why that cannot happen quietly.
// ================================================================================================

TEST_CASE("scene_golden: full.scene.json still contains everything it is for (G5/AC-4/AC-10)") {
    // Asserted against the PARSED DOCUMENT, never by substring search: a substring search cannot
    // tell `-0` from `-0.1`, and cannot tell an entity that LOST its `parent` key from one that
    // never had it. Every count is an equality (spec D0's closing rule).
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_FULL, bytes);
    REQUIRE(doc.entities.size() == 8);

    std::size_t emptyName = 0;
    std::size_t emptyComponents = 0;
    std::size_t forwardParent = 0;
    std::size_t twoComponents = 0;
    std::size_t grandParented = 0;
    std::size_t namedProp = 0;
    std::size_t totalComponents = 0;
    std::vector<std::string> typeNames;

    for (std::size_t i = 0; i < doc.entities.size(); ++i) {
        const SceneEntityRecord& rec = doc.entities[i];
        // E1: saveWorld renumbers to 1..N in emission order (scene_serialize.cpp:159), so a
        // hand-edited fixture whose ids are not contiguous can never be a fixpoint. Pinning the
        // property turns that from an opaque byte diff into a named failure -- and it is what makes
        // the parent lookup below a direct index rather than a search.
        // REQUIRE, not CHECK: the grandparent walk below indexes doc.entities BY rec.parent - 1,
        // which is only in range because ids are contiguous 1..N. parseScene guarantees only that a
        // parent matches SOME record's id (scene_format.cpp:202-205), not that it is <= size(). A
        // CHECK here would report and continue straight into a read past the end of the vector --
        // an ASan heap-buffer-overflow abort on both Debug lanes, silent UB in Release.
        REQUIRE(rec.id == static_cast<std::uint64_t>(i) + 1U);
        if (rec.name.empty()) {
            ++emptyName;
        }
        if (rec.components.empty()) {
            ++emptyComponents;
        }
        if (rec.parent > rec.id) {
            ++forwardParent;  // an entity emitted BEFORE its parent (F4)
        }
        if (rec.components.size() == 2) {
            ++twoComponents;
        }
        if (rec.name == "prop") {
            ++namedProp;
        }
        totalComponents += rec.components.size();
        for (const SceneComponentRecord& comp : rec.components) {
            typeNames.push_back(comp.type);
        }
    }
    // A three-level chain, found without recursing: with ids pinned to 1..N, a record has a
    // GRANDparent iff its parent's own parent is non-zero.
    for (const SceneEntityRecord& rec : doc.entities) {
        if (rec.parent == 0) {
            continue;
        }
        if (doc.entities[rec.parent - 1U].parent != 0) {
            ++grandParented;
        }
    }

    CHECK(emptyName == 1);        // id 5, the bare entity -- emits exactly `{ "id": 5 }`
    CHECK(emptyComponents == 2);  // id 5, and id 7 (a name-and-parent-only record)
    CHECK(forwardParent == 1);    // id 7 -> 8, the only forward reference in the tree's fixtures
    CHECK(twoComponents == 4);    // ids 1, 2, 3, 6
    CHECK(grandParented == 1);    // id 4 -> 3 -> 2, the three-level chain
    CHECK(namedProp == 2);        // duplicate names are legal, unvalidated and preserved (E4)
    CHECK(totalComponents == 10);

    // All five built-in type names appear somewhere in the file. A sixth built-in arriving later
    // reddens G8, not this -- deliberately: this asks "did the fixture lose one?", G8 asks "did the
    // registry change?".
    const std::span<const std::string_view> builtins = builtinComponentNames();
    REQUIRE(builtins.size() == 5);
    for (const std::string_view name : builtins) {
        INFO(std::string{name});
        CHECK(std::find(typeNames.begin(), typeNames.end(), std::string{name}) != typeNames.end());
    }
}

TEST_CASE("scene_golden: edge.scene.json still contains every lexical corner (G5b/AC-5/AC-10)") {
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_EDGE, bytes);
    REQUIRE(doc.entities.size() == 4);

    // Walk the COMPONENT PAYLOADS only -- an ITERATIVE worklist, never a recursive visitor
    // (misc-no-recursion is --warnings-as-errors on the Linux lane). Payload-only matters: the
    // envelope's own `id` and `version` numbers would otherwise pollute every count below.
    std::vector<const JsonValue*> pending;
    for (const SceneEntityRecord& rec : doc.entities) {
        for (const SceneComponentRecord& comp : rec.components) {
            pending.push_back(&comp.value);
        }
    }
    REQUIRE(pending.size() == 3);

    std::size_t nullLeaves = 0;
    std::size_t negativeZero = 0;
    std::size_t plusExponent = 0;
    std::size_t minusExponent = 0;
    std::size_t fullPrecision = 0;
    while (!pending.empty()) {
        const JsonValue* node = pending.back();
        pending.pop_back();
        if (node->isNull()) {
            ++nullLeaves;
            continue;
        }
        if (node->isNumber()) {
            // The VERBATIM validated token (json_value.hpp:68). This is what distinguishes `-0` from
            // `-0.1`; a substring search over the file counts two and proves nothing.
            const std::string_view lexeme = node->numberLexeme();
            if (lexeme == "-0") {
                ++negativeZero;
            }
            if (lexeme.find("e+") != std::string_view::npos) {
                ++plusExponent;
            }
            if (lexeme.find("e-") != std::string_view::npos) {
                ++minusExponent;
            }
            if (lexeme == "0.33333334") {
                ++fullPrecision;
            }
            continue;
        }
        for (const JsonValue& child : node->elements()) {
            pending.push_back(&child);
        }
        for (const JsonMember& member : node->members()) {
            pending.push_back(&member.value);
        }
    }

    CHECK(nullLeaves == 2);     // one from NaN, one from +inf (F5) -- indistinguishable on disk
    CHECK(negativeZero == 1);   // the sign survives; -0 is NOT canonicalized to 0
    CHECK(plusExponent == 1);   // 3.4028235e+38
    CHECK(minusExponent == 3);  // 1e-07 (twice) and 1.1754944e-38
    CHECK(fullPrecision == 1);  // 0.33333334 -- shortest round-trip of 1.0F/3.0F

    // The two exotic names, at the DOCUMENT layer (parseScene stores already-unescaped bytes).
    CHECK(doc.entities[2].name == std::string{ESCAPED_NAME});
    CHECK(doc.entities[3].name == std::string{UTF8_NAME});
}

TEST_CASE("scene_golden: full.scene.json means what it says (G6/AC-11)") {
    // Semantics over the LOADED WORLD, not the document. Bytes-in == bytes-out is necessary and
    // never sufficient; this is the half that survives a mutual load/save bug (spec D0).
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_FULL, bytes);
    World world;
    const SceneLoadReport report = loadScene(world, doc);
    REQUIRE(report.entitiesCreated == 8);

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 8);

    // The whole emission order as ONE assertion. For a freshly loaded World this is file order, and
    // G10 is the case that shows how fragile that becomes after an edit.
    std::vector<std::string> names;
    names.reserve(entities.size());
    for (const Entity e : entities) {
        names.emplace_back(world.name(e));
    }
    const std::vector<std::string> expectedNames{"camera", "ground", "prop",          "prop",
                                                 "",       "lights", "forward child", "late parent"};
    CHECK(names == expectedNames);

    // The parent CHAIN, entity by entity: ground -> prop -> prop, three levels deep.
    CHECK(world.parent(entities[1]) == Entity{});     // ground is a root
    CHECK(world.parent(entities[2]) == entities[1]);  // prop's parent is ground
    CHECK(world.parent(entities[3]) == entities[2]);  // the grandchild prop's parent is prop
    // And the forward reference resolved BACKWARDS through the file: entity 7 names entity 8.
    CHECK(world.parent(entities[6]) == entities[7]);
    CHECK(world.parent(entities[7]) == Entity{});
    CHECK(entities[2] != entities[3]);  // two DISTINCT entities happen to share the name "prop" (E4)

    // Named float fields against exact literals -- not approxEquals. A writer that rounded would
    // redden G2 on the bytes and this on the value, which is the point of asserting both.
    const Camera* camera = world.get<Camera>(entities[0]);
    REQUIRE(camera != nullptr);
    CHECK(camera->fovYRadians == radians(60.0F));
    CHECK(camera->nearPlane == 0.1F);
    CHECK(camera->farPlane == 100.0F);

    const MeshRenderer* groundMesh = world.get<MeshRenderer>(entities[1]);
    REQUIRE(groundMesh != nullptr);
    CHECK(groundMesh->primitive == 2U);
    const MeshRenderer* propMesh = world.get<MeshRenderer>(entities[2]);
    REQUIRE(propMesh != nullptr);
    CHECK(propMesh->primitive == 0U);

    const PointLight* point = world.get<PointLight>(entities[5]);
    REQUIRE(point != nullptr);
    CHECK(point->range == 12.0F);
    const DirectionalLight* directional = world.get<DirectionalLight>(entities[5]);
    REQUIRE(directional != nullptr);
    CHECK(directional->intensity == 0.9F);

    // The bare entity really is bare, and the forward child really has no components.
    CHECK(!world.has<Transform>(entities[4]));
    CHECK(!world.has<Transform>(entities[6]));
}

TEST_CASE("scene_golden: edge.scene.json means what it says (G7/AC-12/E5/E6)") {
    // NEVER `==` on a NaN, and never a Transform-wide equality anywhere near this fixture: NaN != NaN
    // makes both silently vacuous (E6). std::isnan / std::signbit, field by field.
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_EDGE, bytes);
    World world;
    const SceneLoadReport report = loadScene(world, doc);
    REQUIRE(report.entitiesCreated == 4);
    CHECK(report.componentsFailed == 0);  // `null` is a VALID float payload, not a bad field

    const std::vector<Entity> entities = collectEntities(world);
    REQUIRE(entities.size() == 4);

    const Transform* nonFinite = world.get<Transform>(entities[0]);
    REQUIRE(nonFinite != nullptr);
    CHECK(nonFinite->position.x == 0.0F);            // -0 compares EQUAL to 0...
    CHECK(std::signbit(nonFinite->position.x));      // ...so the sign is the only thing that proves it
    CHECK(std::isnan(nonFinite->position.y));        // NaN in, `null` on disk, NaN back
    CHECK(std::isnan(nonFinite->position.z));        // +inf in, `null` on disk, NaN back (F5)
    CHECK_FALSE(std::isinf(nonFinite->position.z));  // the half of F5 that is easy to forget:
                                                     // infinity is LOSSY EXACTLY ONCE and never returns

    const Transform* extremes = world.get<Transform>(entities[1]);
    REQUIRE(extremes != nullptr);
    CHECK(extremes->position.x == 1.0e-7F);
    CHECK(extremes->position.y == 1.0F / 3.0F);
    CHECK(extremes->position.z == -0.1F);
    CHECK(extremes->scale.x == 3.4028235e38F);   // FLT_MAX, written as 3.4028235e+38
    CHECK(extremes->scale.y == 1.1754944e-38F);  // FLT_MIN normal, written as 1.1754944e-38
    CHECK(extremes->scale.z == 0.1F);

    const PointLight* light = world.get<PointLight>(entities[2]);
    REQUIRE(light != nullptr);
    CHECK(light->range == 1.0e-7F);
    CHECK(light->intensity == 0.0F);

    // Both exotic names, against C++ string literals rather than against themselves.
    CHECK(world.name(entities[2]) == ESCAPED_NAME);
    CHECK(world.name(entities[3]) == UTF8_NAME);
    CHECK(world.name(entities[3]).size() == 14);  // 3 ASCII + 2 + 1 + 3 + 1 + 4 -- the UTF-8 widths
}

TEST_CASE("scene_golden: registry order is pinned, and the fixture obeys it (G8/AC-10/E12)") {
    // Stated as a PROPERTY of the fixture rather than by eyeballing it: every entity's component key
    // order must be a SUBSEQUENCE of the registry order. A writer that sorted alphabetically, or a
    // BUILTINS table reordered, breaks one or both halves.
    const std::span<const std::string_view> builtins = builtinComponentNames();
    REQUIRE(builtins.size() == 5);
    CHECK(builtins[0] == "engine::Transform");
    CHECK(builtins[1] == "engine::Camera");
    CHECK(builtins[2] == "engine::DirectionalLight");
    CHECK(builtins[3] == "engine::PointLight");
    CHECK(builtins[4] == "engine::MeshRenderer");
    // A sixth built-in reddens exactly here, by design (E12). The correct response is to regenerate
    // full.scene.json to exercise the new type and update this list in the SAME pull request -- not
    // to relax the assertion.
    const World fresh;
    CHECK(builtins.size() == fresh.componentTypeCount());

    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_FULL, bytes);
    std::string offenders;
    std::size_t seen = 0;
    for (const SceneEntityRecord& rec : doc.entities) {
        std::size_t cursor = 0;
        for (const SceneComponentRecord& comp : rec.components) {
            ++seen;
            while (cursor < builtins.size() && builtins[cursor] != comp.type) {
                ++cursor;
            }
            if (cursor >= builtins.size()) {
                offenders += " id=" + std::to_string(rec.id) + " type=" + comp.type;
                break;
            }
            ++cursor;
        }
    }
    CHECK_MESSAGE(offenders.empty(), offenders);
    CHECK(seen == 10);  // ANTI-VACUITY: the loop above must actually have inspected ten components
}

TEST_CASE("scene_golden: the committed sample scene is still canonical (G9/AC-13/D9)") {
    // A PROPERTY assertion, not a content pin (D9): samples/phase-1-scene/scene.json is shipped
    // content that may legitimately change. What must never change is that it stays canonical, so a
    // human editing it by hand finds out immediately instead of at the next save.
    //
    // No #ifdef here, deliberately (D8). AERO_PHASE1_SCENE_DIR is defined unconditionally for this
    // target, inside the same if(AERO_REFLECT_TOOLS) block that declares it, so the guard the
    // pre-existing case at :314 carries would buy nothing here. That case is left untouched.
    const scene_golden::FileBytes file = scene_golden::readBytes(PHASE1_SAMPLE);
    REQUIRE_MESSAGE(file.ok, file.error);
    const std::string hygiene = scene_golden::hygieneComplaint(file.text);
    CHECK_MESSAGE(hygiene.empty(), hygiene);

    World world;
    const SceneLoadResult result = loadSceneText(world, file.text);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.report.entitiesCreated == 6);
    CHECK(result.report.componentsSkipped == 0);
    CHECK(result.report.componentsFailed == 0);

    const std::string actual = saveWorldText(world);
    INFO(
        "the sample scene is no longer canonical -- re-save it (open it in the editor and save) "
        "and commit the result; do NOT relax this assertion");
    INFO(scene_golden::describeMismatch(file.text, actual));
    if (actual != file.text) {
        scene_golden::dumpActual(AERO_GOLDEN_OUT_DIR, "phase-1-sample", actual);
    }
    CHECK(actual == file.text);
}

TEST_CASE("scene_golden: deleting one entity reorders and renumbers the whole file (G10/AC-14/F8)") {
    // THIS CASE PINS A LIMITATION, NOT A GUARANTEE, and the behaviour it describes is real today.
    //
    // World::nextEntity walks the entity storage's PACKED array (engine/scene/src/world.cpp:303-317),
    // and destroy() is swap-and-pop, so the LAST live entity moves into the destroyed entity's slot.
    // saveWorld assigns file ids 1..N in that order. The consequence, measured below: deleting ONE
    // entity in the editor and saving renumbers every later id, reorders the entities, and can turn
    // a FORWARD parent reference into a backward one -- so a one-entity edit produces a whole-file
    // diff.
    //
    // Fixing that is a format-level decision with a migration question attached (a stable sort key? a
    // hierarchy walk? persistent per-entity ids?) and it is UNOWNED -- it is not this task's, and it
    // interacts with the future .meta GUID system. If you are here because this case reddened, you
    // have either changed entity ordering deliberately (update this case and docs/09 section 2.7 in
    // the same commit) or by accident (do not update it).
    std::string bytes;
    const SceneDocument doc = requireGolden(GOLDEN_FULL, bytes);
    World world;
    REQUIRE(loadScene(world, doc).entitiesCreated == 8);

    const std::vector<Entity> before = collectEntities(world);
    REQUIRE(before.size() == 8);
    const Entity victim = before[3];
    // Discriminate it from the OTHER entity named "prop": this one is the grandchild, and it is a
    // LEAF, so destroy() takes nothing else with it and no re-parenting muddies the result.
    REQUIRE(world.name(victim) == std::string_view{"prop"});
    REQUIRE(world.parent(victim) == before[2]);
    REQUIRE(world.childCount(victim) == 0);
    REQUIRE(world.destroy(victim));

    std::vector<std::string> names;
    world.eachEntity([&names, &world](Entity e) { names.emplace_back(world.name(e)); });
    // MEASURED, not predicted: "late parent" was LAST and moved into the destroyed entity's slot.
    const std::vector<std::string> afterNames{"camera", "ground", "prop", "late parent", "", "lights", "forward child"};
    CHECK(names == afterNames);

    const SceneDocument after = saveWorld(world);
    REQUIRE(after.entities.size() == 7);
    CHECK(after.entities[3].name == "late parent");
    CHECK(after.entities[3].id == 4);  // was 8
    CHECK(after.entities[6].name == "forward child");
    CHECK(after.entities[6].parent == 4);  // the FORWARD reference (7 -> 8) is now BACKWARD (7 -> 4)
    CHECK(after.entities[6].parent < after.entities[6].id);

    // The reordered document is still canonical: the damage is to the DIFF, never to the format.
    const std::string reordered = saveWorldText(world);
    World reloaded;
    REQUIRE_FALSE(loadSceneText(reloaded, reordered).error.has_value());
    const std::string again = saveWorldText(reloaded);
    INFO(scene_golden::describeMismatch(reordered, again));
    CHECK(again == reordered);
}
