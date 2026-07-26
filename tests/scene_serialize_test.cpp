// tests/scene_serialize_test.cpp — task 1.4.2: the World <-> SceneDocument bridge (spec §3.9).
// Standalone single-TU doctest target, gated inside AERO_REFLECT_TOOLS (own
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, like aero_reflect_json_test — no shared test_main.cpp).
// Tier-0 throughout: no GPU, no reflect-gen at test time, no randomness, no files besides the one
// committed samples/phase-1-scene/scene.json test 9 reads.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
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
