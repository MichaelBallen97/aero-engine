// tests/script_world_test.cpp -- task 4.4.2: the World side of the script API. WA1-WA6 are the entity table (one
// engine.Entity object per entity per binding); WA7-WA14 are the entity's name, parent and children; WA15-WA28 are
// spawn, destroy, find, findAll and query and the Vm's destroy queue. A TU of aero_tests, which supplies main() from
// test_main.cpp. Nothing here needs a reflected component (the cases that name a component class use the fixture
// table), so the reflect-tools-OFF tree runs every case.
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using engine::Entity;
using engine::script::BindStatus;
using engine::script::EvalResult;
using engine::script::EvalStatus;
using engine::script::ScriptInstance;
using engine::script::ScriptInstanceResult;
using engine::script::ScriptLoadResult;
using engine::script::scriptStatusLabel;
using engine::script::Vm;
using engine::script::internal::VmAccess;

// A World, a VFS holding one in-memory backend, and a VM over the built-in table bound to that World. Members are
// destroyed in reverse, so the VM goes before the VFS it borrows and the World it is bound to.
struct WorldScene {
    engine::World world;
    engine::VirtualFileSystem vfs;
    script_test::MemoryBackend* files = nullptr;
    Vm vm;

    WorldScene() {
        auto backend = std::make_unique<script_test::MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(script_test::patient(), vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
        REQUIRE(vm.bindWorld(world) == BindStatus::Ok);
    }
    WorldScene(const WorldScene&) = delete;
    WorldScene& operator=(const WorldScene&) = delete;
    WorldScene(WorldScene&&) = delete;
    WorldScene& operator=(WorldScene&&) = delete;
    ~WorldScene() = default;

    void define(std::string_view name, Entity entity) {
        REQUIRE(vm.defineEntityGlobal(name, entity) == BindStatus::Ok);
    }

    // Writes `source` to res://s/<file> and loads it as a script; requires Ok.
    void load(std::string_view file, const std::string& source) {
        files->set(std::format("s/{}", file), source);
        const ScriptLoadResult loaded = vm.loadScript(std::format("res://s/{}", file));
        INFO("load ", file, ": ", loaded.error.message);
        REQUIRE(scriptStatusLabel(loaded.status) == "Ok");
    }

    [[nodiscard]] ScriptInstance attach(std::string_view file, Entity entity, std::string_view global) {
        const ScriptInstanceResult made = vm.createInstance(std::format("res://s/{}", file), entity);
        INFO("attach ", file, ": ", made.error.message);
        REQUIRE(scriptStatusLabel(made.status) == "Ok");
        REQUIRE(VmAccess::defineInstanceGlobal(vm, made.instance, global));
        return made.instance;
    }
};

[[nodiscard]] std::string moverSource(std::string_view members) {
    return std::format("var Mover = class extends engine.Script {{\n{}\n}};\nexport {{\n  Mover as default\n}};\n",
                       members);
}

[[nodiscard]] std::string plainSource() {
    return "var Plain = class {\n  speed = 1;\n};\nexport {\n  Plain as default\n};\n";
}

}  // namespace

// ---- WA1-WA6: the entity table ----------------------------------------------------------------------------------

TEST_CASE("world: every path that hands out an entity hands out the same object (WA1)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("a", e);
    scene.define("b", e);
    scene.load("m.js", moverSource("  speed = 2;"));
    scene.load("p.js", plainSource());
    static_cast<void>(scene.attach("m.js", e, "m"));
    static_cast<void>(scene.attach("p.js", e, "p"));

    CHECK(
        script_test::completion(scene.vm, "[a === b, a === m.entity, a === p.entity, m.entity === p.entity].join()") ==
        "true,true,true,true");
    // A second entity gets its own object, so the table is not one shared slot.
    const Entity other = scene.world.create();
    scene.define("c", other);
    CHECK(script_test::completion(scene.vm, "[c !== a, c.alive, a.alive].join()") == "true,true,true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: an entity object is not extensible, whichever path made it (WA2)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("a", e);
    scene.load("m.js", moverSource("  speed = 2;"));
    scene.load("p.js", plainSource());
    static_cast<void>(scene.attach("m.js", e, "m"));
    static_cast<void>(scene.attach("p.js", e, "p"));

    CHECK(script_test::completion(scene.vm, "[a, m.entity, p.entity].map((x) => Object.isExtensible(x)).join()") ==
          "false,false,false");
    // Sloppy code ignores the write (evaluateScript compiles strict, so the sloppy function is made by Function).
    CHECK(script_test::completion(scene.vm, "new Function('a.hp = 3; return String(a.hp)')()") == "undefined");
    // A script and a module are strict, so the same write throws.
    const EvalResult strictScript = scene.vm.evaluateScript("a.hp = 3;", "strict.js");
    CHECK(strictScript.status == EvalStatus::Exception);
    CHECK(strictScript.error.name == "TypeError");
    scene.files->set("strict.js", "a.hp = 3;\n");
    const EvalResult strict = scene.vm.evaluateModule("res://strict.js");
    CHECK(strict.status == EvalStatus::Exception);
    CHECK(strict.error.name == "TypeError");
    CHECK(script_test::completion(scene.vm, "String(a.hp)") == "undefined");
    CHECK(log.countAtLeast(engine::LogLevel::Error) == 2U);  // the two uncaught TypeErrors, and nothing else
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: a second binding starts a new table, and the old object reads dead (WA3)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("old", e);
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    scene.define("fresh", e);

    CHECK(script_test::completion(scene.vm, "[old !== fresh, old.alive, fresh.alive].join()") == "true,false,true");
    scene.define("again", e);
    CHECK(script_test::completion(scene.vm, "String(again === fresh)") == "true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: unbindWorld frees every held entity object, and a later bind makes new ones (WA4)") {
    const script_test::LogCapture log;
    WorldScene scene;
    constexpr std::size_t COUNT = 64;
    for (std::size_t i = 0; i < COUNT; ++i) {
        scene.define(std::format("g{}", i), scene.world.create());
    }
    // Only the table holds an object once its global is gone.
    REQUIRE(script_test::completion(scene.vm, "for (let i = 0; i < 64; i++) delete globalThis['g' + i]; 'done'") ==
            "done");
    scene.vm.collectGarbage();
    const std::size_t held = scene.vm.stats().liveAllocations;

    scene.vm.unbindWorld();
    scene.vm.collectGarbage();
    const std::size_t released = scene.vm.stats().liveAllocations;
    MESSAGE("WA4: ", held, " live allocations held, ", released, " after the unbind");
    CHECK(held >= released + (2 * COUNT));  // each object and its opaque block

    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    scene.define("z", scene.world.create());
    CHECK(script_test::completion(scene.vm, "z.alive") == "true");
    CHECK(scene.vm.stats().liveAllocations > released);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: a VM shut down with a populated table, still bound, leaks nothing (WA5)") {
    const script_test::LogCapture log;
    WorldScene scene;
    for (std::size_t i = 0; i < 3; ++i) {
        scene.define(std::format("g{}", i), scene.world.create());
    }
    REQUIRE(script_test::completion(scene.vm,
                                    "delete globalThis.g0; delete globalThis.g1; delete globalThis.g2; 'ok'") == "ok");
    // The three objects are now held by the table alone, and the binding is still live.
    script_test::expectCleanShutdown(scene.vm);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
}

TEST_CASE("world: a reload after a rebind re-creates the instance with a stale, unheld entity object (WA6)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.load("m.js", moverSource("  speed = 2;"));
    const ScriptInstance instance = scene.attach("m.js", e, "i");
    REQUIRE(instance.valid());

    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    scene.files->set("s/m.js", moverSource("  speed = 3;\n  extra = 1;"));
    const ScriptLoadResult reloaded = scene.vm.loadScript("res://s/m.js");
    INFO("reload: ", reloaded.error.message);
    REQUIRE(scriptStatusLabel(reloaded.status) == "Ok");
    CHECK(reloaded.instancesMoved == 1U);
    REQUIRE(VmAccess::defineInstanceGlobal(scene.vm, instance, "i"));

    CHECK(script_test::completion(scene.vm, "[i.entity.alive, Object.isExtensible(i.entity)].join()") == "false,false");
    scene.define("g", e);
    CHECK(script_test::completion(scene.vm, "[g !== i.entity, g.alive].join()") == "true,true");
    // The live object is the table's: a second lookup returns it again.
    scene.define("h", e);
    CHECK(script_test::completion(scene.vm, "String(g === h)") == "true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

// ---- WA7-WA14: an entity's name, parent and children ------------------------------------------------------------

namespace {

// `thrown(() => ...)` reads "<name>: <message>" of what a function throws, or "none".
constexpr std::string_view THROWN_HELPER =
    "globalThis.thrown = (f) => { try { f(); return 'none'; } catch (x) { return x.name + ': ' + x.message; } };";

}  // namespace

TEST_CASE("world: an entity's name reads and writes the World's name, and the empty string clears it (WA7)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("a", e);

    CHECK(script_test::completion(scene.vm, "JSON.stringify(a.name)") == "\"\"");
    CHECK(script_test::completion(scene.vm, "a.name = 'Player'; a.name") == "Player");
    CHECK(scene.world.name(e) == "Player");
    // A getter that cached would still read Player here.
    REQUIRE(scene.world.setName(e, "Renamed"));
    CHECK(script_test::completion(scene.vm, "a.name") == "Renamed");
    CHECK(script_test::completion(scene.vm, "a.name = ''; JSON.stringify(a.name)") == "\"\"");
    CHECK(scene.world.name(e).empty());
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE(
    "world: the name setter refuses a non-string without converting it, and a dead receiver without a record "
    "(WA8)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    REQUIRE(scene.world.setName(e, "Keep"));
    scene.define("a", e);
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    REQUIRE(script_test::completion(scene.vm, "globalThis.flag = false; 'ok'") == "ok");

    CHECK(script_test::completion(scene.vm, "thrown(() => { a.name = 5; })") ==
          "TypeError: engine.Entity.name: expected a string, got a number");
    CHECK(
        script_test::completion(scene.vm, "thrown(() => { a.name = { toString() { flag = true; return 'x'; } }; })") ==
        "TypeError: engine.Entity.name: expected a string, got an object");
    CHECK(script_test::completion(scene.vm, "String(flag)") == "false");
    CHECK(scene.world.name(e) == "Keep");

    const Entity doomed = scene.world.create();
    scene.define("gone", doomed);
    scene.world.destroy(doomed);
    CHECK(script_test::completion(scene.vm, "thrown(() => { gone.name = 'x'; })") ==
          "TypeError: engine.Entity.name: the entity is not alive");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: a name holding U+0000 and a non-ASCII character round-trips byte for byte (WA9)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("a", e);

    REQUIRE(script_test::completion(scene.vm, "a.name = 'a\\u0000b\\u00e9c'; 'set'") == "set");
    constexpr std::string_view EXPECTED{
        "a\0b\xC3\xA9"
        "c",
        6U};
    CHECK(scene.world.name(e) == EXPECTED);
    CHECK(script_test::completion(scene.vm, "String(a.name === 'a\\u0000b\\u00e9c') + ',' + a.name.length") ==
          "true,5");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: an entity's parent is null for a root and the table's object for a child (WA10)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity parent = scene.world.create();
    const Entity child = scene.world.create();
    REQUIRE(scene.world.setParent(child, parent));
    scene.define("p", parent);
    scene.define("c", child);

    CHECK(script_test::completion(scene.vm, "String(p.parent)") == "null");
    CHECK(script_test::completion(scene.vm, "String(c.parent === p) + ',' + String(c.parent === c.parent)") ==
          "true,true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: assigning a parent links it and keeps the local Transform; null detaches (WA11)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity parent = scene.world.create();
    const Entity first = scene.world.create();
    const Entity second = scene.world.create();
    const engine::Transform childLocal{engine::Vec3{1.0F, 2.0F, 3.0F}, engine::Quat::identity(), engine::Vec3::one()};
    const engine::Transform parentLocal{engine::Vec3{10.0F, 0.0F, 0.0F}, engine::Quat::identity(), engine::Vec3::one()};
    REQUIRE(scene.world.add<engine::Transform>(parent, parentLocal) != nullptr);
    REQUIRE(scene.world.add<engine::Transform>(first, childLocal) != nullptr);
    REQUIRE(scene.world.add<engine::Transform>(second, childLocal) != nullptr);
    scene.define("p", parent);
    scene.define("a", first);
    scene.define("b", second);
    const engine::Mat4 before = engine::worldMatrix(scene.world, first);

    REQUIRE(script_test::completion(scene.vm, "a.parent = p; b.parent = p; 'ok'") == "ok");
    CHECK(scene.world.parent(first) == parent);
    // The local Transform is bit-identical; the world one now includes the parent's translation.
    CHECK(*scene.world.get<engine::Transform>(first) == childLocal);
    CHECK(engine::worldMatrix(scene.world, first) != before);
    CHECK(engine::worldMatrix(scene.world, first) ==
          engine::worldMatrix(scene.world, parent) * engine::localMatrix(childLocal));

    // Re-assigning the current parent is a no-op that keeps the sibling order.
    REQUIRE(script_test::completion(scene.vm, "a.parent = p; 'ok'") == "ok");
    std::string order;
    scene.world.eachChild(parent, [&](Entity c) { order += c == first ? 'a' : (c == second ? 'b' : '?'); });
    CHECK(order == "ab");

    REQUIRE(script_test::completion(scene.vm, "a.parent = null; 'ok'") == "ok");
    CHECK_FALSE(scene.world.parent(first).valid());
    CHECK(scene.world.childCount(parent) == 1U);
    CHECK(*scene.world.get<engine::Transform>(first) == childLocal);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: every refused parent is one TypeError with no record and nothing changed (WA12)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    const Entity child = scene.world.create();
    const Entity grandchild = scene.world.create();
    const Entity doomed = scene.world.create();
    const Entity receiver = scene.world.create();
    REQUIRE(scene.world.setParent(child, e));
    REQUIRE(scene.world.setParent(grandchild, child));
    scene.define("e", e);
    scene.define("c", child);
    scene.define("g", grandchild);
    scene.define("d", doomed);
    scene.define("r", receiver);
    const Entity gone = scene.world.create();
    scene.define("z", gone);
    scene.world.destroy(gone);
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    scene.world.destroy(doomed);

    CHECK(script_test::completion(scene.vm, "thrown(() => { e.parent = 5; })") ==
          "TypeError: engine.Entity.parent: expected an engine.Entity or null, got a number");
    CHECK(script_test::completion(scene.vm, "thrown(() => { e.parent = {}; })") ==
          "TypeError: engine.Entity.parent: expected an engine.Entity or null, got an object");
    CHECK(script_test::completion(scene.vm, "thrown(() => { e.parent = e; })") ==
          "TypeError: engine.Entity.parent: the new parent is the entity itself");
    CHECK(script_test::completion(scene.vm, "thrown(() => { e.parent = c; })") ==
          "TypeError: engine.Entity.parent: the new parent is in the entity's subtree");
    CHECK(script_test::completion(scene.vm, "thrown(() => { e.parent = g; })") ==
          "TypeError: engine.Entity.parent: the new parent is in the entity's subtree");
    CHECK(script_test::completion(scene.vm, "thrown(() => { r.parent = d; })") ==
          "TypeError: engine.Entity.parent: the new parent is not alive");
    // The value's error wins over a dead receiver.
    CHECK(script_test::completion(scene.vm, "thrown(() => { z.parent = 5; })") ==
          "TypeError: engine.Entity.parent: expected an engine.Entity or null, got a number");
    CHECK(script_test::completion(scene.vm, "thrown(() => { z.parent = r; })") ==
          "TypeError: engine.Entity.parent: the entity is not alive");
    CHECK_FALSE(scene.world.parent(e).valid());
    CHECK(scene.world.parent(child) == e);
    CHECK(scene.world.parent(grandchild) == child);
    CHECK_FALSE(scene.world.parent(receiver).valid());

    // An entity of an earlier binding is refused as a parent.
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    scene.define("fresh", receiver);
    CHECK(script_test::completion(scene.vm, "thrown(() => { fresh.parent = e; })") ==
          "TypeError: engine.Entity.parent: the new parent belongs to an earlier World binding");
    CHECK_FALSE(scene.world.parent(receiver).valid());
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: children() lists the direct children in attach order, as a new array each call (WA13)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity parent = scene.world.create();
    const Entity c1 = scene.world.create();
    const Entity c2 = scene.world.create();
    const Entity c3 = scene.world.create();
    const Entity grandchild = scene.world.create();
    REQUIRE(scene.world.setParent(c3, parent));
    REQUIRE(scene.world.setParent(c1, parent));
    REQUIRE(scene.world.setParent(c2, parent));
    REQUIRE(scene.world.setParent(grandchild, c1));
    scene.define("p", parent);
    scene.define("c1", c1);
    scene.define("c2", c2);
    scene.define("c3", c3);

    CHECK(script_test::completion(scene.vm,
                                  "p.children().map((x) => x === c1 ? 'c1' : x === c2 ? 'c2' : x === "
                                  "c3 ? 'c3' : '?').join()") == "c3,c1,c2");
    CHECK(script_test::completion(scene.vm,
                                  "const a = p.children(), b = p.children(); "
                                  "String(a !== b) + ',' + String(a.every((x, i) => x === b[i]))") == "true,true");
    CHECK(script_test::completion(scene.vm, "JSON.stringify(c2.children()) + ',' + Array.isArray(c2.children())") ==
          "[],true");
    CHECK(script_test::completion(scene.vm, "String(c1.children().length)") == "1");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: name, parent and children throw for a dead entity or no World, while alive reads false (WA14)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    const Entity other = scene.world.create();
    scene.define("a", e);
    scene.define("o", other);
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    scene.world.destroy(e);

    const auto dead = [](std::string_view member) {
        return std::format("TypeError: engine.Entity.{}: the entity is not alive", member);
    };
    const auto unbound = [](std::string_view member) {
        return std::format("TypeError: engine.Entity.{}: no World is bound", member);
    };
    CHECK(script_test::completion(scene.vm, "thrown(() => a.name)") == dead("name"));
    CHECK(script_test::completion(scene.vm, "thrown(() => a.parent)") == dead("parent"));
    CHECK(script_test::completion(scene.vm, "thrown(() => { a.parent = null; })") == dead("parent"));
    CHECK(script_test::completion(scene.vm, "thrown(() => a.children())") == dead("children"));
    CHECK(script_test::completion(scene.vm, "thrown(() => a.alive) + ',' + a.alive") == "none,false");

    scene.vm.unbindWorld();
    CHECK(script_test::completion(scene.vm, "thrown(() => o.name)") == unbound("name"));
    CHECK(script_test::completion(scene.vm, "thrown(() => o.parent)") == unbound("parent"));
    CHECK(script_test::completion(scene.vm, "thrown(() => { o.parent = null; })") == unbound("parent"));
    CHECK(script_test::completion(scene.vm, "thrown(() => o.children())") == unbound("children"));
    CHECK(script_test::completion(scene.vm, "thrown(() => o.alive) + ',' + o.alive") == "none,false");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

// ---- WA15-WA28: spawn, destroy, find, findAll and query, and the destroy queue ------------------------------------

namespace {

// Every live entity, in the World's packed order.
[[nodiscard]] std::vector<Entity> packedOrder(const engine::World& world) {
    std::vector<Entity> out;
    world.eachEntity([&](Entity e) { out.push_back(e); });
    return out;
}

// The entities of `all` satisfying `keep`, ascending by index.
template <typename Keep>
[[nodiscard]] std::vector<Entity> ascending(const std::vector<Entity>& all, Keep&& keep) {
    std::vector<Entity> out;
    for (const Entity e : all) {
        if (keep(e)) {
            out.push_back(e);
        }
    }
    std::ranges::sort(out, {}, &Entity::index);
    return out;
}

// Defines g<prefix>0, g<prefix>1... for `expected` and returns the script expression that is true exactly when
// `expression` (an array of entities) holds them, in that order.
template <typename Define>
[[nodiscard]] std::string sameAs(Define&& define, std::string_view prefix, std::string_view expression,
                                 const std::vector<Entity>& expected) {
    std::string list;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::string name = std::format("{}{}", prefix, i);
        define(name, expected[i]);
        list += (i == 0 ? "" : ",") + name;
    }
    return std::format(
        "(() => {{ const got = {}; const want = [{}]; return got.length === want.length && "
        "got.every((x, i) => x === want[i]); }})()",
        expression, list);
}

}  // namespace

TEST_CASE("world: engine.spawn returns a live root entity with an identity Transform and no name (WA15)") {
    const script_test::LogCapture log;
    WorldScene scene;
    REQUIRE(scene.world.entityCount() == 0U);
    CHECK(script_test::completion(scene.vm, "globalThis.s = engine.spawn(); String(s instanceof engine.Entity)") ==
          "true");
    REQUIRE(scene.world.entityCount() == 1U);
    const Entity made = packedOrder(scene.world).front();
    CHECK_FALSE(scene.world.parent(made).valid());
    CHECK(scene.world.name(made).empty());
    REQUIRE(scene.world.get<engine::Transform>(made) != nullptr);
    CHECK(*scene.world.get<engine::Transform>(made) == engine::Transform{});
    // The object the call returned is the entity's table object, and the entity is alive at once.
    scene.define("same", made);
    CHECK(script_test::completion(scene.vm, "[s === same, s.alive, s.name === '', s.parent === null].join()") ==
          "true,true,true,true");
    CHECK(script_test::completion(scene.vm, "engine.spawn() !== engine.spawn() ? 'two' : 'one'") == "two");
    CHECK(scene.world.entityCount() == 3U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: with no World bound every namespace function throws, destroy after its value check (WA16)") {
    const script_test::LogCapture log;
    script_test::ProbeScene scene;
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    scene.vm.unbindWorld();
    const auto unbound = [](std::string_view fn) { return std::format("TypeError: engine.{}: no World is bound", fn); };
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.spawn())") == unbound("spawn"));
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.find('a'))") == unbound("find"));
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.findAll('a'))") == unbound("findAll"));
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query(engine.ScriptProbe))") == unbound("query"));
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.destroy(e))") == unbound("destroy"));
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.destroy(1))") ==
          "TypeError: engine.destroy: expected an engine.Entity, got a number");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: engine.spawn takes no argument and creates nothing when it throws (WA17)") {
    const script_test::LogCapture log;
    WorldScene scene;
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.spawn(1))") ==
          "TypeError: engine.spawn: expected no arguments, got 1");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.spawn('bullet'))") ==
          "TypeError: engine.spawn: expected no arguments, got 1");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.spawn(undefined, undefined))") ==
          "TypeError: engine.spawn: expected no arguments, got 2");
    CHECK(scene.world.entityCount() == 0U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: a spawn the heap refuses leaves no entity behind, at every limit swept (WA18)") {
    const script_test::LogCapture log;
    constexpr std::size_t MIB = std::size_t{1024} * 1024;
    constexpr std::size_t LIMITS = 24;
    std::size_t refusing = 0;
    for (std::size_t step = 0; step < LIMITS; ++step) {
        engine::World world;
        engine::script::VmConfig config;
        config.heapLimitBytes = MIB + step * 389U;
        Vm vm = script_test::makeBuiltinVm(script_test::patient(config));
        REQUIRE(vm.bindWorld(world) == BindStatus::Ok);
        // The counter is a typed array made before the loop, so counting allocates nothing; the table holds each
        // spawned object, so the heap fills with them.
        const EvalResult filled = vm.evaluateScript(
            "globalThis.count = new Int32Array(1); for (;;) { engine.spawn(); count[0]++; }", "wa18.js");
        REQUIRE(filled.status == EvalStatus::OutOfMemory);
        refusing += vm.stats().refusedAllocations > 0U ? 1U : 0U;
        vm.unbindWorld();  // frees the table's objects, so there is room to read the counter
        INFO("limit " << config.heapLimitBytes);
        CHECK(script_test::completion(vm, "String(count[0])") == std::to_string(world.entityCount()));
        script_test::expectCleanShutdown(vm);
    }
    CHECK(refusing >= 1U);
}

TEST_CASE("world: find gives the lowest-index entity of that exact name, and null for none or empty (WA19)") {
    const script_test::LogCapture log;
    WorldScene scene;
    std::vector<Entity> made;
    made.reserve(6);
    for (int i = 0; i < 6; ++i) {
        made.push_back(scene.world.create());
    }
    // Destroying the first swaps the last into its packed slot; the recreated one goes to the packed end.
    REQUIRE(scene.world.destroy(made[0]));
    const Entity reused = scene.world.create();
    for (const Entity e : packedOrder(scene.world)) {
        if (e.index == 0 || e.index == 3 || e.index == 5) {
            REQUIRE(scene.world.setName(e, "P"));
        } else if (e.index == 4) {
            REQUIRE(scene.world.setName(e, "p"));
        } else if (e.index == 2) {
            REQUIRE(scene.world.setName(e, "P "));
        }
    }
    const std::vector<Entity> named =
        ascending(packedOrder(scene.world), [&](Entity e) { return scene.world.name(e) == "P"; });
    REQUIRE(named.size() == 3U);
    std::vector<Entity> packedNamed;
    for (const Entity e : packedOrder(scene.world)) {
        if (scene.world.name(e) == "P") {
            packedNamed.push_back(e);
        }
    }
    REQUIRE(packedNamed != named);  // the setup reorders the packed storage, or this case would test nothing
    CHECK(reused.index == 0U);
    scene.define("hit", named.front());
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");

    CHECK(script_test::completion(scene.vm, "String(engine.find('P') === hit)") == "true");
    CHECK(script_test::completion(scene.vm, "String(engine.find('nothing'))") == "null");
    CHECK(script_test::completion(scene.vm, "String(engine.find(''))") == "null");
    CHECK(script_test::completion(scene.vm, "engine.find('P ').name + '|' + engine.find('p').name") == "P |p");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.find(5))") ==
          "TypeError: engine.find: expected a string, got a number");
    CHECK(script_test::completion(scene.vm,
                                  "globalThis.hit2 = false; const o = { toString() { hit2 = true; return 'P'; } }; "
                                  "thrown(() => engine.find(o)) + ',' + hit2") ==
          "TypeError: engine.find: expected a string, got an object,false");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: findAll gives every exact match in ascending index, as a new array each call (WA20)") {
    const script_test::LogCapture log;
    WorldScene scene;
    std::vector<Entity> made;
    made.reserve(8);
    for (int i = 0; i < 8; ++i) {
        made.push_back(scene.world.create());
    }
    REQUIRE(scene.world.destroy(made[1]));
    REQUIRE(scene.world.destroy(made[4]));
    static_cast<void>(scene.world.create());
    static_cast<void>(scene.world.create());
    std::size_t counter = 0;
    for (const Entity e : packedOrder(scene.world)) {
        REQUIRE(scene.world.setName(e, counter++ % 2U == 0U ? "M" : "N"));
    }
    const std::vector<Entity> all = packedOrder(scene.world);
    const std::vector<Entity> wanted = ascending(all, [&](Entity e) { return scene.world.name(e) == "M"; });
    REQUIRE(wanted.size() == 4U);

    const auto define = [&](const std::string& name, Entity e) { scene.define(name, e); };
    CHECK(script_test::completion(scene.vm, sameAs(define, "m", "engine.findAll('M')", wanted)) == "true");
    CHECK(script_test::completion(scene.vm, "String(engine.findAll('M') !== engine.findAll('M'))") == "true");
    CHECK(script_test::completion(scene.vm, "JSON.stringify(engine.findAll(''))") == "[]");
    CHECK(script_test::completion(scene.vm, "JSON.stringify(engine.findAll('absent'))") == "[]");
    CHECK(script_test::completion(scene.vm, "String(Array.isArray(engine.findAll('M')))") == "true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: query gives the holders of every class given, in ascending index (WA21)") {
    using script_test::ScriptProbe;
    using script_test::ScriptTag;
    const script_test::LogCapture log;
    script_test::ProbeScene scene;
    std::vector<Entity> made;
    made.reserve(9);
    for (int i = 0; i < 9; ++i) {
        made.push_back(scene.world.create());
    }
    REQUIRE(scene.world.destroy(made[2]));  // reorders the packed storage
    const Entity again = scene.world.create();
    for (std::size_t i = 0; i < made.size(); ++i) {
        const Entity e = made[i] == made[2] ? again : made[i];
        if (i % 2U == 0U) {
            REQUIRE(scene.world.add<ScriptProbe>(e) != nullptr);
        }
        if (i % 3U == 0U) {
            static_cast<void>(scene.world.add<ScriptTag>(e));  // a tag stores nothing, so the answer is null
            REQUIRE(scene.world.has<ScriptTag>(e));
        }
    }
    const std::vector<Entity> all = packedOrder(scene.world);
    const std::vector<Entity> probes = ascending(all, [&](Entity e) { return scene.world.has<ScriptProbe>(e); });
    const std::vector<Entity> both =
        ascending(all, [&](Entity e) { return scene.world.has<ScriptProbe>(e) && scene.world.has<ScriptTag>(e); });
    REQUIRE(probes.size() > both.size());
    REQUIRE_FALSE(both.empty());
    const auto define = [&](const std::string& name, Entity e) {
        REQUIRE(scene.vm.defineEntityGlobal(name, e) == BindStatus::Ok);
    };
    CHECK(script_test::completion(scene.vm, sameAs(define, "p", "engine.query(engine.ScriptProbe)", probes)) == "true");
    CHECK(script_test::completion(
              scene.vm, sameAs(define, "b", "engine.query(engine.ScriptProbe, engine.ScriptTag)", both)) == "true");
    CHECK(script_test::completion(
              scene.vm, sameAs(define, "r", "engine.query(engine.ScriptTag, engine.ScriptProbe)", both)) == "true");
    // A repeat counts once.
    CHECK(script_test::completion(
              scene.vm, sameAs(define, "d", "engine.query(engine.ScriptProbe, engine.ScriptProbe)", probes)) == "true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: query refuses a wrong class count or a non-class, before reading anything else (WA22)") {
    const script_test::LogCapture log;
    script_test::ProbeScene scene;
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query())") ==
          "RangeError: engine.query: expected 1 to 8 component classes, got 0");
    CHECK(script_test::completion(
              scene.vm, "const P = engine.ScriptProbe; thrown(() => engine.query(P, P, P, P, P, P, P, P, P))") ==
          "RangeError: engine.query: expected 1 to 8 component classes, got 9");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query(engine.ScriptProbe, 5))") ==
          "TypeError: engine.query: argument 2 is not an engine component class, got a number");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query(engine.ScriptProbe, engine.Vec3))") ==
          "TypeError: engine.query: argument 2 is not an engine component class, got a function");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query(engine.ScriptProbe, function plain() {}))") ==
          "TypeError: engine.query: argument 2 is not an engine component class, got a function");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.query(undefined))") ==
          "TypeError: engine.query: argument 1 is not an engine component class, got undefined");
    CHECK(script_test::completion(scene.vm, "String(engine.query.length)") == "8");
    // Eight classes are accepted.
    CHECK(script_test::completion(
              scene.vm, "const T = engine.ScriptTag; String(engine.query(T, T, T, T, T, T, T, T).length)") == "0");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: engine.destroy queues an entity once and changes nothing else (WA23)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity target = scene.world.create();
    REQUIRE(scene.world.setName(target, "A"));
    const Entity gone = scene.world.create();
    const Entity stale = scene.world.create();
    scene.define("a", target);
    scene.define("gone", gone);
    scene.define("stale", stale);
    REQUIRE(script_test::completion(scene.vm, std::string{THROWN_HELPER} + " 'ok'") == "ok");
    REQUIRE(scene.world.destroy(gone));

    CHECK(script_test::completion(scene.vm,
                                  "[engine.destroy(a), a.alive, a.name, engine.find('A') === a, "
                                  "engine.destroy(a)].join()") == "true,true,A,true,false");
    CHECK(script_test::completion(scene.vm, "[a.alive, a.name, engine.find('A') === a, engine.destroy(a)].join()") ==
          "true,A,true,false");
    CHECK(scene.world.alive(target));
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(gone))") == "false");
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);  // `stale` is now an earlier binding's object
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(stale))") == "false");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.destroy(5))") ==
          "TypeError: engine.destroy: expected an engine.Entity, got a number");
    CHECK(script_test::completion(scene.vm, "thrown(() => engine.destroy())") ==
          "TypeError: engine.destroy: expected an engine.Entity, got undefined");
    CHECK(scene.vm.takeDestroyQueue().empty());  // the rebind emptied the queue; the refusals added nothing
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: takeDestroyQueue and destroyQueued hand the flush to the caller, and evict the table (WA24)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity a = scene.world.create();
    const Entity b = scene.world.create();
    const Entity c = scene.world.create();
    const Entity a1 = scene.world.create();
    REQUIRE(scene.world.setParent(a1, a));
    scene.define("a", a);
    scene.define("b", b);
    scene.define("c", c);
    scene.define("a1", a1);
    CHECK(script_test::completion(scene.vm, "[engine.destroy(b), engine.destroy(a), engine.destroy(c)].join()") ==
          "true,true,true");
    const std::vector<Entity> taken = scene.vm.takeDestroyQueue();
    CHECK(taken == std::vector<Entity>{b, a, c});
    CHECK(scene.vm.takeDestroyQueue().empty());
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(b))") == "false");  // a taken queue keeps its marks

    CHECK(scene.vm.destroyQueued(a));
    CHECK_FALSE(scene.world.alive(a));
    CHECK_FALSE(scene.world.alive(a1));
    CHECK(scene.world.alive(b));
    CHECK(script_test::completion(scene.vm, "[a.alive, a1.alive].join()") == "false,false");
    // The same handle made alive again gets a new object: the old one was evicted from the table.
    REQUIRE(scene.world.recreate(a) == a);
    scene.define("x", a);
    CHECK(script_test::completion(scene.vm, "[x !== a, a.alive, x.alive].join()") == "true,true,true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: destroyQueued destroys only what was queued, and only a live entity (WA25)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity never = scene.world.create();
    const Entity queued = scene.world.create();
    scene.define("q", queued);
    CHECK_FALSE(scene.vm.destroyQueued(never));
    CHECK(scene.world.alive(never));
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(q))") == "true");
    REQUIRE(scene.vm.takeDestroyQueue() == std::vector<Entity>{queued});
    // C++ destroyed it first: the call only drops the mark.
    REQUIRE(scene.world.destroy(queued));
    CHECK_FALSE(scene.vm.destroyQueued(queued));
    CHECK_FALSE(scene.vm.destroyQueued(queued));  // the mark is gone
    REQUIRE(scene.world.recreate(queued) == queued);
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(q))") == "true");
    CHECK(scene.world.alive(queued));
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: a binding ends the queue and its marks (WA26)") {
    const script_test::LogCapture log;
    WorldScene scene;
    const Entity first = scene.world.create();
    scene.define("a", first);
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(a))") == "true");

    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    CHECK(scene.vm.takeDestroyQueue().empty());
    CHECK(scene.world.alive(first));
    scene.define("b", first);
    CHECK(script_test::completion(scene.vm, "[engine.destroy(a), engine.destroy(b)].join()") == "false,true");

    // unbind + bind does the same.
    scene.vm.unbindWorld();
    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);
    CHECK(scene.vm.takeDestroyQueue().empty());
    CHECK_FALSE(scene.vm.destroyQueued(first));
    scene.define("c", first);
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(c))") == "true");
    CHECK(scene.vm.takeDestroyQueue() == std::vector<Entity>{first});
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("world: find, findAll and query scan 10 000 entities and return the right ones (WA27)") {
    using script_test::ScriptProbe;
    const script_test::LogCapture log;
    script_test::ProbeScene scene;
    std::vector<Entity> named;
    std::vector<Entity> probes{scene.entity};
    for (int i = 0; i < 10000; ++i) {
        const Entity e = scene.world.create();
        if (i % 100 == 0) {
            REQUIRE(scene.world.setName(e, "E"));
            named.push_back(e);
        }
        if (i % 10 == 0) {
            REQUIRE(scene.world.add<ScriptProbe>(e) != nullptr);
            probes.push_back(e);
        }
    }
    REQUIRE(named.size() == 100U);
    REQUIRE(probes.size() == 1001U);
    REQUIRE(scene.vm.defineEntityGlobal("firstE", named.front()) == BindStatus::Ok);

    CHECK(script_test::completion(scene.vm, "String(engine.find('E') === firstE)") == "true");
    CHECK(script_test::completion(scene.vm, "String(engine.findAll('E').length)") == "100");
    CHECK(script_test::completion(scene.vm, "String(engine.query(engine.ScriptProbe).length)") == "1001");
    CHECK(script_test::completion(scene.vm, "String(engine.query(engine.ScriptProbe)[0] === e)") == "true");

    // The cost, for the manual pass to read in a Release tree: no duration is asserted.
    const auto median = [&](std::string_view source) {
        std::vector<double> micros;
        for (int i = 0; i < 20; ++i) {
            const auto start = std::chrono::steady_clock::now();
            static_cast<void>(script_test::completion(scene.vm, source));
            micros.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        }
        std::ranges::sort(micros);
        return micros[micros.size() / 2U];
    };
    MESSAGE("find: ", median("engine.find('E') === null"), " us; findAll: ", median("engine.findAll('E').length"),
            " us; query: ", median("engine.query(engine.ScriptProbe).length"), " us (median of 20, 10 000 entities)");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE(
    "world: takeDestroyQueue and destroyQueued on an invalid Vm or another thread log once and do nothing (WA28)") {
    script_test::LogCapture log;
    WorldScene scene;
    const Entity e = scene.world.create();
    scene.define("a", e);
    CHECK(script_test::completion(scene.vm, "String(engine.destroy(a))") == "true");

    std::vector<Entity> foreignTake{e};
    bool foreignDestroy = true;
    log.clear();
    std::thread{[&] {
        foreignTake = scene.vm.takeDestroyQueue();
        foreignDestroy = scene.vm.destroyQueued(e);
    }}.join();
    CHECK(foreignTake.empty());
    CHECK_FALSE(foreignDestroy);
    const auto loud = log.atLeast(engine::LogLevel::Error);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].message == "script: takeDestroyQueue called from a thread that is not the VM's; refused");
    CHECK(loud[1].message == "script: destroyQueued called from a thread that is not the VM's; refused");
    CHECK(scene.world.alive(e));  // nothing changed: the queue and the mark are still there
    CHECK(scene.vm.takeDestroyQueue() == std::vector<Entity>{e});

    Vm moved = std::move(scene.vm);
    log.clear();
    CHECK(scene.vm.takeDestroyQueue().empty());
    CHECK_FALSE(scene.vm.destroyQueued(e));
    const auto invalid = log.atLeast(engine::LogLevel::Error);
    REQUIRE(invalid.size() == 2U);
    CHECK(invalid[0].message == "script: takeDestroyQueue on an invalid Vm (a failed create or a moved-from Vm)");
    CHECK(invalid[1].message == "script: destroyQueued on an invalid Vm (a failed create or a moved-from Vm)");
    script_test::expectCleanShutdown(moved);
}
