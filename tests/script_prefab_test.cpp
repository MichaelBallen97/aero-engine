// tests/script_prefab_test.cpp -- task 4.4.4: engine.spawn(asset, ...), the three forms that copy a .aeroprefab into
// the bound World (PR1-PR13 and PR15; the cache cases follow). A TU of aero_tests, which supplies main() from
// test_main.cpp. A prefab is parsed through the generated scene serializer, so a case that needs a parse opens with
// `if (!prefabsParse()) { return; }` (the reflect-tools-OFF build has no component table and refuses every parse); the
// argument-order cases run in both configurations.
#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using engine::Entity;
using engine::Guid;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::script::BindStatus;
using engine::script::ScriptSystem;
using engine::script::ScriptSystemReport;
using engine::script::Vm;

constexpr float HALF_SQRT2 = 0.70710678F;
constexpr std::string_view THREE_PATH = "res://assets/prefabs/three.aeroprefab";
constexpr std::string_view THREE_FILE = "assets/prefabs/three.aeroprefab";

// `thrown(() => ...)` reads "<name>: <message>" of what a function throws, or "none".
constexpr std::string_view THROWN_HELPER =
    "globalThis.thrown = (f) => { try { f(); return 'none'; } catch (x) { return x.name + ': ' + x.message; } };";

[[nodiscard]] bool prefabsParse() { return !engine::script::internal::builtinComponentBindings().components().empty(); }

[[nodiscard]] std::string transformJson(Vec3 position, Quat rotation, Vec3 scale) {
    return std::format(
        R"("engine::Transform": {{"position": {{"x": {}, "y": {}, "z": {}}}, )"
        R"("rotation": {{"x": {}, "y": {}, "z": {}, "w": {}}}, "scale": {{"x": {}, "y": {}, "z": {}}}}})",
        position.x, position.y, position.z, rotation.x, rotation.y, rotation.z, rotation.w, scale.x, scale.y, scale.z);
}

[[nodiscard]] std::string entityJson(int id, std::string_view name, int parent, const std::string& components) {
    return std::format(R"({{"id": {}, "name": "{}", {}"components": {{{}}}}})", id, name,
                       parent == 0 ? std::string{} : std::format(R"("parent": {}, )", parent), components);
}

[[nodiscard]] std::string sceneJson(const std::vector<std::string>& entities) {
    std::string text = R"({"version": 1, "entities": [)";
    for (std::size_t i = 0; i < entities.size(); ++i) {
        text += (i == 0 ? "" : ",") + entities[i];
    }
    return text + "]}";
}

// Root (90 degrees about +Y, scale 0.5, at (rootX, 0, 0)) with the children Left and Right, each at (1, 0, 0).
// `leftExtra` is appended to Left's components.
[[nodiscard]] std::string prefabText(float rootX = 0.0F, const std::string& leftExtra = {}) {
    const Quat turn{0.0F, HALF_SQRT2, 0.0F, HALF_SQRT2};
    const std::string child = transformJson(Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one());
    return sceneJson({entityJson(1, "Root", 0, transformJson(Vec3{rootX, 0.0F, 0.0F}, turn, Vec3{0.5F, 0.5F, 0.5F})),
                      entityJson(2, "Left", 1, child + leftExtra), entityJson(3, "Right", 1, child)});
}

// A World, a VFS holding one in-memory backend with three.aeroprefab, and a VM over the built-in table bound to that
// World. Members are destroyed in reverse, so the VM goes before the VFS it borrows and the World it is bound to.
struct PrefabScene {
    engine::World world;
    engine::VirtualFileSystem vfs;
    script_test::MemoryBackend* files = nullptr;
    Vm vm;

    explicit PrefabScene(const engine::script::VmConfig& config = script_test::patient()) {
        auto backend = std::make_unique<script_test::MemoryBackend>();
        files = backend.get();
        backend->set(std::string{THREE_FILE}, prefabText());
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(config, vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
        REQUIRE(vm.bindWorld(world) == BindStatus::Ok);
        REQUIRE(script_test::completion(vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    }
    PrefabScene(const PrefabScene&) = delete;
    PrefabScene& operator=(const PrefabScene&) = delete;
    PrefabScene(PrefabScene&&) = delete;
    PrefabScene& operator=(PrefabScene&&) = delete;
    ~PrefabScene() = default;

    void define(std::string_view name, Entity entity) {
        REQUIRE(vm.defineEntityGlobal(name, entity) == BindStatus::Ok);
    }

    // A live entity with a Transform.
    [[nodiscard]] Entity place(Vec3 position, Quat rotation = Quat::identity(), Vec3 scale = Vec3::one()) {
        const Entity entity = world.create();
        REQUIRE(world.add<Transform>(entity, Transform{position, rotation, scale}) != nullptr);
        return entity;
    }

    // Every live entity named `name`, ascending by index (creation order while nothing is destroyed).
    [[nodiscard]] std::vector<Entity> named(std::string_view name) const {
        std::vector<Entity> found;
        world.eachEntity([&](Entity entity) {
            if (world.name(entity) == name) {
                found.push_back(entity);
            }
        });
        std::ranges::sort(found, {}, &Entity::index);
        return found;
    }

    [[nodiscard]] Transform transformOf(Entity entity) const {
        const Transform* const held = world.get<Transform>(entity);
        REQUIRE(held != nullptr);
        return *held;
    }

    [[nodiscard]] std::string thrown(std::string_view call) {
        return script_test::completion(vm, std::format("thrown(() => {})", call));
    }
};

void checkVec(Vec3 got, Vec3 want) {
    CHECK(got.x == doctest::Approx(want.x).epsilon(1e-5));
    CHECK(got.y == doctest::Approx(want.y).epsilon(1e-5));
    CHECK(got.z == doctest::Approx(want.z).epsilon(1e-5));
}

void checkQuat(Quat got, Quat want) {
    CHECK(got.x == doctest::Approx(want.x).epsilon(1e-5));
    CHECK(got.y == doctest::Approx(want.y).epsilon(1e-5));
    CHECK(got.z == doctest::Approx(want.z).epsilon(1e-5));
    CHECK(got.w == doctest::Approx(want.w).epsilon(1e-5));
}

}  // namespace

TEST_CASE("prefab: spawn(asset) copies the tree, returns the root's one entity object and keeps the order (PR1)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    CHECK(script_test::completion(scene.vm,
                                  "globalThis.r = engine.spawn('res://assets/prefabs/three.aeroprefab'); "
                                  "[r === engine.find('Root'), r.children().map((c) => c.name).join('+'), "
                                  "r.parent === null].join()") == "true,Left+Right,true");
    CHECK(scene.world.entityCount() == 3U);
    const std::vector<Entity> roots = scene.named("Root");
    REQUIRE(roots.size() == 1U);
    CHECK_FALSE(scene.world.parent(roots.front()).valid());
    const Transform root = scene.transformOf(roots.front());
    checkVec(root.position, Vec3{});
    CHECK(scene.world.childCount(roots.front()) == 2U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: spawn(asset, position) places the root there and keeps the saved rotation and scale (PR2)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    CHECK(script_test::completion(scene.vm,
                                  "String(engine.spawn('res://assets/prefabs/three.aeroprefab', "
                                  "new engine.Vec3(1, 2, 3)).name)") == "Root");
    const Transform root = scene.transformOf(scene.named("Root").front());
    checkVec(root.position, Vec3{1.0F, 2.0F, 3.0F});
    checkQuat(root.rotation, Quat{0.0F, HALF_SQRT2, 0.0F, HALF_SQRT2});
    checkVec(root.scale, Vec3{0.5F, 0.5F, 0.5F});
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: spawn(asset, position, rotation, scale) composes all three and leaves the children alone (PR3)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    scene.files->set("assets/prefabs/offset.aeroprefab", prefabText(2.0F));
    const Quat spin{HALF_SQRT2, 0.0F, 0.0F, HALF_SQRT2};  // 90 degrees about +X
    CHECK(script_test::completion(scene.vm,
                                  "String(engine.spawn('res://assets/prefabs/offset.aeroprefab', "
                                  "new engine.Vec3(1, 2, 3), new engine.Quat(0.70710678, 0, 0, 0.70710678), "
                                  "new engine.Vec3(2, 2, 2)).name)") == "Root");
    const Entity rootEntity = scene.named("Root").front();
    const Transform root = scene.transformOf(rootEntity);
    // position = P + R * (S * stored), rotation = R * stored, scale = S * stored.
    const Vec3 scaledOffset{4.0F, 0.0F, 0.0F};  // (2, 0, 0) * 2
    checkVec(root.position, Vec3{1.0F, 2.0F, 3.0F} + spin * scaledOffset);
    checkQuat(root.rotation, spin * Quat{0.0F, HALF_SQRT2, 0.0F, HALF_SQRT2});
    checkVec(root.scale, Vec3{1.0F, 1.0F, 1.0F});
    for (const std::string_view child : {"Left", "Right"}) {
        CAPTURE(child);
        const Transform local = scene.transformOf(scene.named(child).front());
        CHECK(local == Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()});
    }
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: spawn(asset, transform) reads the Transform's local fields, not its world ones (PR4)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    const Entity parent = scene.place(Vec3{10.0F, 0.0F, 0.0F});
    const Entity child = scene.place(Vec3{1.0F, 2.0F, 3.0F});
    REQUIRE(scene.world.setParent(child, parent));
    scene.define("c", child);
    CHECK(script_test::completion(scene.vm, "String(c.get(engine.Transform).worldPosition().x)") == "11");
    CHECK(script_test::completion(scene.vm,
                                  "String(engine.spawn('res://assets/prefabs/three.aeroprefab', "
                                  "c.get(engine.Transform)).name)") == "Root");
    const Transform root = scene.transformOf(scene.named("Root").front());
    checkVec(root.position, Vec3{1.0F, 2.0F, 3.0F});  // the local position; the world one would read x = 11
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE(
    "prefab: the Transform form equals the position-rotation-scale form, views and detached vectors alike (PR5)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    scene.files->set("assets/prefabs/offset.aeroprefab", prefabText(2.0F));
    const Entity source = scene.place(Vec3{1.0F, 2.0F, 3.0F}, Quat{0.0F, HALF_SQRT2, 0.0F, HALF_SQRT2}, Vec3{2, 2, 2});
    scene.define("s", source);
    CHECK(script_test::completion(
              scene.vm,
              "const a = 'res://assets/prefabs/offset.aeroprefab'; const t = s.get(engine.Transform); "
              "engine.spawn(a, t); engine.spawn(a, t.position, t.rotation, t.scale); "
              "engine.spawn(a, t.position.clone(), t.rotation.clone(), t.scale.clone()); "
              "String(engine.findAll('Root').length)") == "3");
    const std::vector<Entity> roots = scene.named("Root");
    REQUIRE(roots.size() == 3U);
    const Transform first = scene.transformOf(roots[0]);
    CHECK(scene.transformOf(roots[1]) == first);
    CHECK(scene.transformOf(roots[2]) == first);
    CHECK(first.scale.x == doctest::Approx(1.0F));  // 2 * 0.5
    CHECK(first.position.y == doctest::Approx(2.0F));
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: undefined stands for an omitted argument in every slot (PR6)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    const Entity source = scene.place(Vec3{4.0F, 0.0F, 0.0F});
    scene.define("s", source);
    CHECK(script_test::completion(
              scene.vm,
              "const a = 'res://assets/prefabs/three.aeroprefab'; const v = new engine.Vec3(1, 2, 3); "
              "const q = new engine.Quat(0.70710678, 0, 0, 0.70710678); const sc = new engine.Vec3(2, 2, 2); "
              "const t = s.get(engine.Transform); engine.spawn(a, undefined); engine.spawn(a, undefined, q); "
              "engine.spawn(a, v, undefined, sc); engine.spawn(a, t, undefined, undefined); "
              "String(engine.findAll('Root').length)") == "4");
    const std::vector<Entity> roots = scene.named("Root");
    REQUIRE(roots.size() == 4U);
    const Quat saved{0.0F, HALF_SQRT2, 0.0F, HALF_SQRT2};
    const Transform bare = scene.transformOf(roots[0]);
    checkVec(bare.position, Vec3{});
    checkQuat(bare.rotation, saved);
    const Transform turned = scene.transformOf(roots[1]);
    checkVec(turned.position, Vec3{});
    checkQuat(turned.rotation, Quat{HALF_SQRT2, 0.0F, 0.0F, HALF_SQRT2} * saved);
    const Transform scaled = scene.transformOf(roots[2]);
    checkVec(scaled.position, Vec3{1.0F, 2.0F, 3.0F});
    checkVec(scaled.scale, Vec3{1.0F, 1.0F, 1.0F});  // 2 * 0.5
    const Transform viewed = scene.transformOf(roots[3]);
    checkVec(viewed.position, Vec3{4.0F, 0.0F, 0.0F});
    checkVec(viewed.scale, Vec3{0.5F, 0.5F, 0.5F});
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: two hundred spawns from a Transform view land right while the Transform storage grows (PR7)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    const Entity source = scene.place(Vec3{1.0F, 2.0F, 3.0F});
    scene.define("s", source);
    CHECK(script_test::completion(scene.vm,
                                  "const t = s.get(engine.Transform); for (let i = 0; i < 200; i++) { "
                                  "engine.spawn('res://assets/prefabs/three.aeroprefab', t); } "
                                  "String(engine.findAll('Root').length)") == "200");
    CHECK(scene.world.entityCount() == 601U);
    for (const Entity root : scene.named("Root")) {
        checkVec(scene.transformOf(root).position, Vec3{1.0F, 2.0F, 3.0F});
    }
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: every wrong argument is refused in the contract's order, with nothing created (PR8)") {
    const script_test::LogCapture log;
    PrefabScene scene;
    // The values the placement slots receive. t is a Transform view, which exists only when the table binds one.
    REQUIRE(script_test::completion(scene.vm,
                                    "globalThis.v = new engine.Vec3(); globalThis.q = new engine.Quat(); "
                                    "globalThis.t = engine.Transform === undefined ? null : "
                                    "engine.spawn().get(engine.Transform); 'ok'") == "ok");
    const std::size_t before = scene.world.entityCount();
    CHECK(scene.thrown("engine.spawn('a', 1, 2, 3, 4)") ==
          "TypeError: engine.spawn: expected at most 4 arguments, got 5");
    CHECK(scene.thrown("engine.spawn(3)") == "TypeError: engine.spawn: expected a string, got a number");
    CHECK(scene.thrown("engine.spawn(undefined, undefined)") ==
          "TypeError: engine.spawn: expected a string, got undefined");
    const std::string vectorKind = "TypeError: engine.spawn: expected an engine.Transform or an engine.Vec3, got ";
    const std::string bare = "TypeError: engine.spawn: expected an engine.Vec3, got ";
    const std::string firstKind = prefabsParse() ? vectorKind : bare;
    CHECK(scene.thrown("engine.spawn('a', 1)") == firstKind + "a number");
    CHECK(scene.thrown("engine.spawn('a', q)") == firstKind + "an engine.Quat");
    CHECK(scene.thrown("engine.spawn('a', { position: v })") == firstKind + "an object");
    CHECK(scene.thrown("engine.spawn('a', v, v)") ==
          "TypeError: engine.spawn: expected an engine.Quat, got an engine.Vec3");
    CHECK(scene.thrown("engine.spawn('a', v, q, q)") ==
          "TypeError: engine.spawn: expected an engine.Vec3, got an engine.Quat");
    if (prefabsParse()) {
        const std::string after = "TypeError: engine.spawn: expected no argument after an engine.Transform, got ";
        CHECK(scene.thrown("engine.spawn('a', t, q)") == after + "an engine.Quat");
        CHECK(scene.thrown("engine.spawn('a', t, undefined, v)") == after + "an engine.Vec3");
    }
    // The one entity above is the only one made; no refusal added another.
    CHECK(scene.world.entityCount() == before);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: no argument kind is coerced, so no script code runs inside a refusal (PR9)") {
    const script_test::LogCapture log;
    PrefabScene scene;
    const std::string outcome = script_test::completion(
        scene.vm,
        "globalThis.hits = 0; const traps = {}; "
        "for (const k of ['get', 'set', 'has', 'ownKeys', 'getPrototypeOf', 'getOwnPropertyDescriptor', "
        "'defineProperty', 'deleteProperty', 'isExtensible', 'preventExtensions', 'setPrototypeOf']) { "
        "traps[k] = () => { hits++; return undefined; }; } "
        "const bads = [new Proxy({}, traps), "
        "{ get position() { hits++; return 0; }, get rotation() { hits++; return 0; }, "
        "valueOf() { hits++; return 1; }, toString() { hits++; return 'res://x'; } }, "
        "{ [Symbol.toPrimitive]() { hits++; return 'res://x'; } }, new String('res://x')]; "
        "const v = new engine.Vec3(); const q = new engine.Quat(); const out = []; "
        "for (const bad of bads) { out.push(thrown(() => engine.spawn(bad))); "
        "out.push(thrown(() => engine.spawn('res://x', bad))); out.push(thrown(() => engine.spawn('res://x', v, "
        "bad))); "
        "out.push(thrown(() => engine.spawn('res://x', v, q, bad))); } "
        "hits + ':' + out.length + ':' + out.every((s) => s.startsWith('TypeError: engine.spawn: expected'))");
    CHECK(outcome == "0:16:true");
    CHECK(scene.world.entityCount() == 0U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: the World, the path and the file source are consulted in that order, after every value (PR10)") {
    const script_test::LogCapture log;
    PrefabScene scene;
    scene.vm.unbindWorld();
    // A bad kind beats an unbound World; good arguments then meet the unbound World.
    CHECK(scene.thrown("engine.spawn(1)") == "TypeError: engine.spawn: expected a string, got a number");
    CHECK(scene.thrown("engine.spawn('res://assets/prefabs/three.aeroprefab')") ==
          (prefabsParse() ? "TypeError: engine.spawn: no World is bound"
                          : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    // A bad path beats a missing file source (the path is checked first).
    CHECK(scene.thrown("engine.spawn('bullet')") ==
          (prefabsParse() ? "TypeError: engine.spawn: 'bullet' is not a res:// file path"
                          : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));

    // A VM made with no module source refuses a good path in its own words.
    Vm bare = script_test::makeBuiltinVm();
    engine::World other;
    REQUIRE(bare.bindWorld(other) == BindStatus::Ok);
    REQUIRE(script_test::completion(bare, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    CHECK(script_test::completion(bare, "thrown(() => engine.spawn('bullet'))") ==
          (prefabsParse() ? "TypeError: engine.spawn: 'bullet' is not a res:// file path"
                          : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));
    CHECK(script_test::completion(bare, "thrown(() => engine.spawn('res://x.aeroprefab'))") ==
          (prefabsParse() ? "TypeError: engine.spawn: 'res://x.aeroprefab': this VM has no file source"
                          : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));
    CHECK(scene.world.entityCount() == 0U);
    CHECK(other.entityCount() == 0U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(bare);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: a path that is not a res:// file, or names no file, is refused with its own text (PR11)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    PrefabScene scene;
    const auto notAPath = [](std::string_view text) {
        return std::format("TypeError: engine.spawn: '{}' is not a res:// file path", text);
    };
    CHECK(scene.thrown("engine.spawn('res://')") == notAPath("res://"));
    CHECK(scene.thrown("engine.spawn('res://../x')") == notAPath("res://../x"));
    CHECK(scene.thrown("engine.spawn('assets/x')") == notAPath("assets/x"));
    CHECK(scene.thrown("engine.spawn('res://a:b')") == notAPath("res://a:b"));
    CHECK(scene.thrown("engine.spawn('')") == notAPath(""));
    CHECK(scene.thrown("engine.spawn('res://none.aeroprefab')") ==
          "TypeError: engine.spawn: 'res://none.aeroprefab': no such file");
    // A '%' in the script's text is inert: it reaches the message as characters, never as a format string.
    CHECK(scene.thrown("engine.spawn('%s%n')") == notAPath("%s%n"));
    CHECK(scene.thrown("engine.spawn('res://%s%n.aeroprefab')") ==
          "TypeError: engine.spawn: 'res://%s%n.aeroprefab': no such file");
    CHECK(scene.world.entityCount() == 0U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: a file the VM cannot hold or parse, a wrong root count and a stale view each refuse (PR12)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    constexpr std::size_t MIB = std::size_t{1024} * 1024;
    engine::script::VmConfig config;
    config.heapLimitBytes = MIB;
    PrefabScene scene{script_test::patient(config)};
    const auto refusal = [&](std::string_view path) {
        return scene.thrown(std::format("engine.spawn('res://{}')", path));
    };
    // Over the heap limit: refused on the size alone, before a byte is read.
    scene.files->set("big.aeroprefab", std::string(MIB + 1U, ' '));
    const std::size_t readsBefore = scene.files->readCount();
    CHECK(refusal("big.aeroprefab") == std::format("TypeError: engine.spawn: 'res://big.aeroprefab': {} bytes is more "
                                                   "than this VM's heap limit of {}",
                                                   MIB + 1U, MIB));
    CHECK(scene.files->readCount() == readsBefore);
    // A parse error names its line and column.
    scene.files->set("bad.aeroprefab", "{\n  \"version\": 1,\n  \"entities\": [ x ]\n}\n");
    const std::string bad = refusal("bad.aeroprefab");
    CHECK(bad.starts_with("TypeError: engine.spawn: 'res://bad.aeroprefab': line 3, column "));
    // Exactly one root.
    scene.files->set("none.aeroprefab", sceneJson({}));
    scene.files->set("two.aeroprefab", sceneJson({entityJson(1, "A", 0, ""), entityJson(2, "B", 0, "")}));
    CHECK(refusal("none.aeroprefab") ==
          "TypeError: engine.spawn: 'res://none.aeroprefab': a prefab has exactly one root");
    CHECK(refusal("two.aeroprefab") ==
          "TypeError: engine.spawn: 'res://two.aeroprefab': a prefab has exactly one root");
    // A view of a destroyed entity draws its own error.
    const Entity doomed = scene.place(Vec3{1.0F, 0.0F, 0.0F});
    scene.define("d", doomed);
    REQUIRE(script_test::completion(
                scene.vm, "globalThis.t = d.get(engine.Transform); globalThis.pv = t.position; 'ok'") == "ok");
    REQUIRE(scene.world.destroy(doomed));
    const std::size_t before = scene.world.entityCount();
    CHECK(scene.thrown("engine.spawn('res://assets/prefabs/three.aeroprefab', t)") ==
          "TypeError: engine.spawn: the entity is not alive");
    CHECK(scene.thrown("engine.spawn('res://assets/prefabs/three.aeroprefab', pv)")
              .starts_with("TypeError: engine.Transform.position"));
    CHECK(scene.world.entityCount() == before);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

namespace {

constexpr std::string_view SPAWNER_PATH = "res://s/spawner.js";
constexpr std::string_view CHILD_PATH = "res://s/child.js";

[[nodiscard]] Guid scriptGuid(std::uint64_t id) { return Guid{0, id}; }

// A VM over a VFS holding one backend, a World with the built-ins and the ScriptSystem over both. Non-movable: the Vm
// borrows the VFS and the system borrows the Vm and the World; the system is declared last, so it goes first.
class SystemScene {
public:
    SystemScene() {
        auto backend = std::make_unique<script_test::MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(script_test::patient(), vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
        system = std::make_unique<ScriptSystem>(vm, world, [this](const Guid& script) { return resolve(script); });
        REQUIRE(system->bound());
    }
    SystemScene(const SystemScene&) = delete;
    SystemScene& operator=(const SystemScene&) = delete;
    SystemScene(SystemScene&&) = delete;
    SystemScene& operator=(SystemScene&&) = delete;
    ~SystemScene() = default;

    engine::VirtualFileSystem vfs;
    script_test::MemoryBackend* files = nullptr;
    engine::World world;
    Vm vm;
    std::unique_ptr<ScriptSystem> system;

private:
    [[nodiscard]] std::string resolve(const Guid& script) const {
        if (script == scriptGuid(1)) {
            return std::string{SPAWNER_PATH};
        }
        return script == scriptGuid(2) ? std::string{CHILD_PATH} : std::string{};
    }
};

}  // namespace

TEST_CASE(
    "prefab: a script on a spawned entity attaches at the next update, not during the one that spawned it (PR13)") {
    if (!prefabsParse()) {
        return;
    }
    const script_test::LogCapture log;
    SystemScene scene;
    const std::string leftScript =
        std::format(R"(, "engine::ScriptComponent": {{"script": "{}"}})", engine::formatGuid(scriptGuid(2)));
    scene.files->set(std::string{THREE_FILE}, prefabText(0.0F, leftScript));
    scene.files->set("s/spawner.js",
                     script_test::classSource("  onUpdate() { if (globalThis.made === undefined) { globalThis.made = "
                                              "engine.spawn('res://assets/prefabs/three.aeroprefab'); } }"));
    scene.files->set("s/child.js",
                     script_test::classSource("  onInit() { globalThis.inits = (globalThis.inits || 0) + 1; }"));
    REQUIRE(script_test::completion(scene.vm, "globalThis.inits = 0; 'ok'") == "ok");
    const Entity spawner = scene.world.create();
    REQUIRE(scene.world.add<engine::ScriptComponent>(spawner, engine::ScriptComponent{scriptGuid(1)}) != nullptr);

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.attached == 1U);  // the spawner alone: the snapshot of the update was taken before the spawn
    CHECK(scene.world.entityCount() == 4U);
    CHECK(script_test::completion(scene.vm, "String(inits)") == "0");
    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.attached == 1U);  // the spawned Left
    CHECK(script_test::completion(scene.vm, "String(inits)") == "1");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    scene.system.reset();
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("prefab: the tools decide only what happens after the argument checks (PR15)") {
    const script_test::LogCapture log;
    PrefabScene scene;
    const bool parses = prefabsParse();
    CHECK(scene.thrown("engine.spawn('res://none.aeroprefab')") ==
          (parses ? "TypeError: engine.spawn: 'res://none.aeroprefab': no such file"
                  : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));
    scene.vm.unbindWorld();
    CHECK(scene.thrown("engine.spawn('res://none.aeroprefab')") ==
          (parses ? "TypeError: engine.spawn: no World is bound"
                  : "TypeError: engine.spawn: built without AERO_REFLECT_TOOLS"));
    CHECK(scene.thrown("engine.spawn(1)") == "TypeError: engine.spawn: expected a string, got a number");
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    CHECK(script_test::completion(scene.vm, "String(engine.spawn().alive)") == "true");  // spawn() is unchanged
    CHECK(scene.world.entityCount() == 1U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}
