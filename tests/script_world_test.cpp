// tests/script_world_test.cpp -- task 4.4.2: the World side of the script API. WA1-WA6 are the entity table (one
// engine.Entity object per entity per binding); WA7-WA14 are the entity's name, parent and children; the later steps
// add spawn, destroy, find, findAll and query, and the Vm's destroy queue (WA15-WA28). A TU of aero_tests, which
// supplies main() from test_main.cpp. Nothing here needs a reflected component, so the reflect-tools-OFF tree runs
// every case.
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
