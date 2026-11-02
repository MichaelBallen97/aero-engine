// tests/script_world_test.cpp -- task 4.4.2: the World side of the script API. WA1-WA6 are the entity table (one
// engine.Entity object per entity per binding); the later steps add the entity's name and parent, spawn, destroy, find,
// findAll and query, and the Vm's destroy queue (WA7-WA28). A TU of aero_tests, which supplies main() from
// test_main.cpp. Nothing here needs a reflected component, so the reflect-tools-OFF tree runs every case.
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
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
