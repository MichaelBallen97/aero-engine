// tests/script_attach_test.cpp -- task 4.4.1: engine.Script as the base of a script's class, and the instances the VM
// attaches to entities. VM66 is the base class through the VM's own load and reload; the later cases drive
// createInstance(path, entity) and callHook. A TU of aero_tests, which supplies main() from test_main.cpp.
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>

namespace {

using engine::Entity;
using engine::LogLevel;
using engine::script::BindStatus;
using engine::script::ScriptHook;
using engine::script::ScriptHookResult;
using engine::script::ScriptInstance;
using engine::script::ScriptInstanceResult;
using engine::script::ScriptLoadResult;
using engine::script::scriptStatusLabel;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::internal::VmAccess;

// A module source in memory.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    void set(std::string relPath, std::string text) { files[std::move(relPath)] = std::move(text); }

    [[nodiscard]] bool exists(std::string_view relPath) const override { return files.contains(relPath); }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(found->second.size());
    }

    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        engine::ByteBuffer bytes;
        bytes.reserve(found->second.size());
        for (const char c : found->second) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }

private:
    std::map<std::string, std::string, std::less<>> files;
};

// A VM over a VFS whose root is one MemoryBackend. Non-movable: the Vm borrows the VFS's address.
class Fixture {
public:
    explicit Fixture(const VmConfig& config = script_test::patient()) {
        auto backend = std::make_unique<MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(config, vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;

    engine::VirtualFileSystem vfs;
    MemoryBackend* files = nullptr;
    Vm vm;
};

// A script in esbuild 0.28.2's measured shape: `var <name> = class extends engine.Script {...}; export { <name> as
// default };`.
[[nodiscard]] std::string classSource(std::string_view members, std::string_view name = "Mover") {
    return std::format("var {0} = class extends engine.Script {{\n{1}\n}};\nexport {{\n  {0} as default\n}};\n", name,
                       members);
}

// A module whose class is written out whole, for the cases that need a second class beside the default export.
[[nodiscard]] std::string moduleSource(std::string_view body, std::string_view name = "Mover") {
    return std::format("{1}\nexport {{\n  {0} as default\n}};\n", name, body);
}

// The one record at WARN or above that a refusal logs, at ERROR.
[[nodiscard]] std::string theOneError(const script_test::LogCapture& log) {
    const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    return loud[0].message;
}

void expose(Vm& vm, ScriptInstance instance, std::string_view name) {
    REQUIRE(VmAccess::defineInstanceGlobal(vm, instance, name));
}

// Loads `source` at res://s/<file> and requires Ok.
void load(Fixture& fx, std::string_view file, const std::string& source) {
    fx.files->set(std::format("s/{}", file), source);
    const ScriptLoadResult loaded = fx.vm.loadScript(std::format("res://s/{}", file));
    INFO("load ", file, ": ", loaded.error.message);
    REQUIRE(scriptStatusLabel(loaded.status) == "Ok");
}

[[nodiscard]] ScriptInstance attached(Fixture& fx, std::string_view file, Entity entity) {
    const ScriptInstanceResult made = fx.vm.createInstance(std::format("res://s/{}", file), entity);
    INFO("attach ", file, ": ", made.error.message);
    REQUIRE(scriptStatusLabel(made.status) == "Ok");
    REQUIRE(made.instance.valid());
    return made.instance;
}

// An instance of the script at res://s/<file> with no entity, exposed as the global `name`; hooks may be called on one.
[[nodiscard]] ScriptInstance made(Fixture& fx, std::string_view file, std::string_view name = "i") {
    const ScriptInstanceResult result = fx.vm.createInstance(std::format("res://s/{}", file));
    INFO("create ", file, ": ", result.error.message);
    REQUIRE(scriptStatusLabel(result.status) == "Ok");
    expose(fx.vm, result.instance, name);
    return result.instance;
}

// Binds `world` to the fixture's VM.
void bind(Fixture& fx, engine::World& world) { REQUIRE(fx.vm.bindWorld(world) == BindStatus::Ok); }

}  // namespace

TEST_CASE(
    "vm: a class extending engine.Script loads, makes entity-less instances and is re-created by a reload (VM66)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/m.js", classSource("  speed = 2;"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/m.js");
    INFO("load: ", loaded.error.message);
    REQUIRE(scriptStatusLabel(loaded.status) == "Ok");

    const ScriptInstanceResult made = fx.vm.createInstance("res://s/m.js");
    REQUIRE(scriptStatusLabel(made.status) == "Ok");
    REQUIRE(made.instance.valid());
    REQUIRE(VmAccess::defineInstanceGlobal(fx.vm, made.instance, "i"));
    CHECK(script_test::completion(
              fx.vm, "[i instanceof engine.Script, Object.getOwnPropertyNames(i).join('|')].join()") == "true,speed");

    fx.files->set("s/m.js", classSource("  speed = 3;\n  extra = 1;"));
    const ScriptLoadResult reloaded = fx.vm.loadScript("res://s/m.js");
    INFO("reload: ", reloaded.error.message);
    REQUIRE(scriptStatusLabel(reloaded.status) == "Ok");
    CHECK(reloaded.instancesMoved == 1U);
    REQUIRE(VmAccess::defineInstanceGlobal(fx.vm, made.instance, "i"));
    CHECK(script_test::completion(fx.vm,
                                  "[i instanceof engine.Script, Object.getOwnPropertyNames(i).join('|')].join()") ==
          "true,speed|extra");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

// ---- VM67-VM73, VM82: createInstance(path, entity) ---------------------------------------------------------

TEST_CASE("vm: createInstance with an entity refuses in order, each once and before anything runs (VM67)") {
    script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity live = world.create();
    constexpr std::string_view KEY = "res://s/m.js";
    load(fx, "m.js", classSource("  constructor() {\n    super();\n    globalThis.made += 1;\n  }"));
    REQUIRE(script_test::completion(fx.vm, "globalThis.made = 0; String(made)") == "0");

    // InvalidVm: a default-constructed Vm is the moved-from state.
    Vm invalid;
    log.clear();
    CHECK(scriptStatusLabel(invalid.createInstance(KEY, live).status) == "InvalidVm");
    CHECK(log.countAtLeast(LogLevel::Warn) == 1U);

    // WrongThread.
    log.clear();
    ScriptInstanceResult fromOther;
    std::thread other{[&] { fromOther = fx.vm.createInstance(KEY, live); }};
    other.join();
    CHECK(scriptStatusLabel(fromOther.status) == "WrongThread");
    CHECK(log.countAtLeast(LogLevel::Warn) == 1U);

    // NotLoaded comes before the World checks: no World is bound yet either.
    log.clear();
    const ScriptInstanceResult unloaded = fx.vm.createInstance("res://s/none.js", live);
    CHECK(scriptStatusLabel(unloaded.status) == "NotLoaded");
    CHECK(theOneError(log) == "script: res://s/none.js: createInstance refused: no script class is loaded from it");

    // NoWorld.
    log.clear();
    const ScriptInstanceResult noWorld = fx.vm.createInstance(KEY, live);
    CHECK(scriptStatusLabel(noWorld.status) == "NoWorld");
    CHECK_FALSE(noWorld.instance.valid());
    CHECK(theOneError(log) == "script: res://s/m.js: createInstance refused: no World is bound");

    // DeadEntity: destroyed, and an entity of another World that is not alive in the bound one.
    bind(fx, world);
    const Entity doomed = world.create();
    REQUIRE(world.destroy(doomed));
    log.clear();
    const ScriptInstanceResult dead = fx.vm.createInstance(KEY, doomed);
    CHECK(scriptStatusLabel(dead.status) == "DeadEntity");
    CHECK_FALSE(dead.instance.valid());
    CHECK(theOneError(log) ==
          "script: res://s/m.js: createInstance refused: the entity is not alive in the bound World");
    engine::World elsewhere;
    Entity foreign;
    for (int i = 0; i < 6; ++i) {
        foreign = elsewhere.create();
    }
    REQUIRE_FALSE(world.alive(foreign));
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance(KEY, foreign).status) == "DeadEntity");
    CHECK(theOneError(log) ==
          "script: res://s/m.js: createInstance refused: the entity is not alive in the bound World");

    CHECK(script_test::completion(fx.vm, "String(made)") == "0");  // no constructor ran for any refusal
    CHECK(scriptStatusLabel(engine::script::ScriptStatus::NoWorld) == "NoWorld");
    CHECK(scriptStatusLabel(engine::script::ScriptStatus::DeadEntity) == "DeadEntity");
    log.clear();
    const ScriptInstanceResult ok = fx.vm.createInstance(KEY, live);  // control
    CHECK(scriptStatusLabel(ok.status) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(made)") == "1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: an attached instance holds its entity as read-only data its field initializers already see (VM68)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e = world.create();
    bind(fx, world);
    load(fx, "m.js",
         classSource("  captured = (globalThis.trace.push('field:' + (this.entity instanceof engine.Entity && "
                     "this.entity.alive)), this.entity);\n"
                     "  constructor() {\n    globalThis.trace = ['ctor-before'];\n    super();\n"
                     "    globalThis.trace.push('ctor-after:' + this.entity.alive);\n  }"));
    const ScriptInstance i = attached(fx, "m.js", e);
    expose(fx.vm, i, "i");
    CHECK(script_test::completion(fx.vm, "trace.join('|')") == "ctor-before|field:true|ctor-after:true");
    CHECK(script_test::completion(fx.vm, R"js(
        const d = Object.getOwnPropertyDescriptor(i, 'entity');
        [d.writable, d.enumerable, d.configurable, 'value' in d, d.value instanceof engine.Entity, d.value.alive,
         i.captured === i.entity, Object.keys(i).join('|'), JSON.stringify(i).includes('entity')].join();
    )js") == "false,false,true,true,true,true,true,captured,false");
    CHECK(
        script_test::completion(fx.vm, "try { i.entity = 1; 'no throw'; } catch (x) { x.name + ': ' + x.message; }") ==
        "TypeError: 'entity' is read-only");
    REQUIRE(world.destroy(e));
    CHECK(script_test::completion(fx.vm, "String(i.entity.alive)") == "false");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a class that does not extend engine.Script gets its entity after its constructor returns (VM69)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e = world.create();
    bind(fx, world);
    load(fx, "p.js",
         moduleSource("var Plain = class {\n  seen = typeof this.entity;\n"
                      "  constructor() {\n    globalThis.ctorSees = typeof this.entity;\n  }\n};",
                      "Plain"));
    load(fx, "q.js", moduleSource("var Clash = class {\n  entity = 5;\n};", "Clash"));
    const ScriptInstance plain = attached(fx, "p.js", e);
    const ScriptInstance clash = attached(fx, "q.js", e);
    expose(fx.vm, plain, "p");
    expose(fx.vm, clash, "c");
    CHECK(script_test::completion(fx.vm, "ctorSees + ',' + p.seen") == "undefined,undefined");
    for (const char* const name : {"p", "c"}) {
        CHECK(script_test::completion(fx.vm, std::format(R"js(
            (() => {{
                const d = Object.getOwnPropertyDescriptor({0}, 'entity');
                return [d.writable, d.enumerable, d.configurable, d.value instanceof engine.Entity,
                        d.value.alive].join();
            }})();
        )js",
                                                         name)) == "false,false,true,true,true");
    }
    CHECK(script_test::completion(fx.vm, "Object.keys(c).length + ',' + Object.keys(p).join('|')") == "0,seen");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: an instance that cannot hold its entity is refused NotAClass, once, and nothing is made (VM70)") {
    script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e = world.create();
    bind(fx, world);
    const std::string reason =
        "not a script: its instance cannot hold its entity (frozen, sealed, not extensible, "
        "or with a non-configurable own 'entity')";
    struct Shape {
        std::string_view file;
        std::string_view body;
    };
    const std::array<Shape, 6> shapes{{
        {"freeze", "constructor() {\n    Object.freeze(this);\n  }"},
        {"seal", "constructor() {\n    Object.seal(this);\n  }"},
        {"prevent", "constructor() {\n    Object.preventExtensions(this);\n  }"},
        {"writable", "constructor() {\n    Object.defineProperty(this, 'entity', {value: 1, writable: true});\n  }"},
        {"fixed", "constructor() {\n    Object.defineProperty(this, 'entity', {value: 1});\n  }"},
        {"derived", "constructor() {\n    super();\n    Object.freeze(this);\n  }"},
    }};
    for (const Shape& shape : shapes) {
        CAPTURE(shape.file);
        const bool derived = shape.file == "derived";
        const std::string source =
            derived ? classSource(shape.body)
                    : moduleSource(std::format("var Plain = class {{\n  {}\n}};", shape.body), "Plain");
        load(fx, std::format("{}.js", shape.file), source);
        log.clear();
        const ScriptInstanceResult made = fx.vm.createInstance(std::format("res://s/{}.js", shape.file), e);
        CHECK(scriptStatusLabel(made.status) == "NotAClass");
        CHECK_FALSE(made.instance.valid());
        CHECK(made.error.message == reason);
        CHECK(theOneError(log) == std::format("script: res://s/{}.js: {}", shape.file, reason));
    }
    // Nothing was made: a reload with new bytes moves no instance.
    fx.files->set("s/freeze.js", moduleSource("var Plain = class {\n  constructor() {\n  }\n};", "Plain"));
    const ScriptLoadResult reloaded = fx.vm.loadScript("res://s/freeze.js");
    CHECK(scriptStatusLabel(reloaded.status) == "Ok");
    CHECK(reloaded.instancesMoved == 0U);
    // The frozen class through the one-argument overload is still Ok.
    fx.files->set("s/freeze.js",
                  moduleSource("var Plain = class {\n  constructor() {\n    Object.freeze(this);\n  }\n};", "Plain"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/freeze.js").status) == "Ok");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance("res://s/freeze.js").status) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: two attaches of one script hold separate fields and separate entities (VM71)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity first = world.create();
    const Entity second = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  speed = 1;"));
    const ScriptInstance a = attached(fx, "m.js", first);
    const ScriptInstance b = attached(fx, "m.js", second);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "a.speed = 9; a.speed + ',' + b.speed") == "9,1");
    CHECK(script_test::completion(fx.vm, "[a.entity.alive, b.entity.alive].join()") == "true,true");
    REQUIRE(world.destroy(first));
    CHECK(script_test::completion(fx.vm, "[a.entity.alive, b.entity.alive].join()") == "false,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a constructor that throws or never ends makes nothing and leaves no pending entity (VM72)") {
    script_test::LogCapture log;
    engine::World world;
    engine::script::VmConfig config = script_test::patient();
    config.callTimeBudget = std::chrono::milliseconds{100};
    Fixture fx{config};
    const Entity e = world.create();
    bind(fx, world);
    load(fx, "m.js",
         classSource("  constructor() {\n    super();\n    if (globalThis.mode === 'throw') {\n"
                     "      throw new Error('boom');\n    }\n    if (globalThis.mode === 'spin') {\n"
                     "      for (;;) {\n      }\n    }\n  }"));
    REQUIRE(script_test::completion(fx.vm, "globalThis.mode = 'throw'; mode") == "throw");
    log.clear();
    const ScriptInstanceResult thrown = fx.vm.createInstance("res://s/m.js", e);
    CHECK(scriptStatusLabel(thrown.status) == "Exception");
    CHECK_FALSE(thrown.instance.valid());
    CHECK(thrown.error.message == "boom");
    static_cast<void>(theOneError(log));

    REQUIRE(script_test::completion(fx.vm, "globalThis.mode = 'spin'; mode") == "spin");
    log.clear();
    const ScriptInstanceResult spun = fx.vm.createInstance("res://s/m.js", e);
    CHECK(scriptStatusLabel(spun.status) == "Interrupted");
    CHECK_FALSE(spun.instance.valid());
    CHECK(theOneError(log).find("interrupted after exceeding") != std::string::npos);

    REQUIRE(script_test::completion(fx.vm, "globalThis.mode = ''; mode.length") == "0");
    const ScriptInstance plain = [&] {
        const ScriptInstanceResult made = fx.vm.createInstance("res://s/m.js");
        REQUIRE(scriptStatusLabel(made.status) == "Ok");
        return made.instance;
    }();
    expose(fx.vm, plain, "i");
    CHECK(script_test::completion(fx.vm, "Object.getOwnPropertyNames(i).length + ',' + ('entity' in i)") == "0,false");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: only the first base construction inside an attach takes the entity (VM73)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e = world.create();
    bind(fx, world);
    load(fx, "after.js",
         moduleSource("var Other = class extends engine.Script {\n};\n"
                      "var Mover = class extends engine.Script {\n  constructor() {\n    super();\n"
                      "    globalThis.inner = new Other();\n  }\n};"));
    load(fx, "before.js",
         moduleSource("var Other = class extends engine.Script {\n};\n"
                      "var Mover = class extends engine.Script {\n  constructor() {\n"
                      "    globalThis.inner = new Other();\n    super();\n  }\n};"));
    const char* const names = "Object.getOwnPropertyNames";

    const ScriptInstance a = attached(fx, "after.js", e);
    expose(fx.vm, a, "i");
    CHECK(script_test::completion(fx.vm, std::format("{0}(i).join() + '/' + {0}(inner).length", names)) == "entity/0");

    const ScriptInstance b = attached(fx, "before.js", e);
    expose(fx.vm, b, "i");
    CHECK(script_test::completion(fx.vm, std::format("{0}(i).join() + '/' + {0}(inner).join()", names)) ==
          "entity/entity");
    CHECK(script_test::completion(fx.vm, "[i.entity.alive, inner.entity.alive, i.entity === inner.entity].join()") ==
          "true,true,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a job the construction queues cannot take the entity its constructor left pending (VM82)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e = world.create();
    bind(fx, world);
    // A plain class never reaches engine.Script's constructor, so the pending entity is still set when the
    // construction ends; the job it queued runs in the call's own drain and must find nothing to take.
    load(fx, "p.js",
         moduleSource("var D = class extends engine.Script {\n};\n"
                      "var Plain = class {\n  constructor() {\n    Promise.resolve().then(() => {\n"
                      "      try { globalThis.made = new D(); } catch (e) { globalThis.refused = String(e); }\n"
                      "    });\n  }\n};",
                      "Plain"));
    const ScriptInstance plain = attached(fx, "p.js", e);
    expose(fx.vm, plain, "p");
    CHECK(script_test::completion(fx.vm, "p.entity.alive") == "true");
    // The job ran (made exists), took no entity, and was neither refused nor rejected.
    CHECK(script_test::completion(fx.vm,
                                  "typeof made + '/' + Object.getOwnPropertyNames(made).length + '/' + "
                                  "('entity' in made) + '/' + typeof globalThis.refused") ==
          "object/0/false/undefined");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

// ---- VM74-VM81: callHook -------------------------------------------------------------------------------------

TEST_CASE("vm: a hook runs as a method, an own arrow field or a getter's function, with this and dt (VM74)") {
    const script_test::LogCapture log;
    Fixture fx;
    load(fx, "proto.js",
         classSource(
             "  onInit() {\n    log.push('init:' + arguments.length);\n    return 5;\n  }\n"
             "  onUpdate(dt) {\n    log.push('upd:' + (this === globalThis.i) + ':' + dt + ':' + arguments.length);\n"
             "    return 7;\n  }\n"
             "  onDestroy() {\n    log.push('destroy:' + arguments.length);\n    return 9;\n  }"));
    load(fx, "arrow.js",
         classSource("  onUpdate = (dt) => {\n    log.push('arrow:' + (this === globalThis.i) + ':' + dt);\n  };"));
    load(fx, "getter.js",
         classSource("  get onUpdate() {\n    const self = this;\n    return function (dt) {\n"
                     "      log.push('get:' + (this === globalThis.i) + ':' + (self === globalThis.i) + ':' + dt);\n"
                     "    };\n  }"));
    REQUIRE(script_test::completion(fx.vm, "globalThis.log = []; log.length") == "0");

    const ScriptInstance proto = made(fx, "proto.js");
    for (const ScriptHook hook : {ScriptHook::Init, ScriptHook::Update, ScriptHook::Destroy}) {
        const ScriptHookResult result = fx.vm.callHook(proto, hook, 0.5);
        CHECK(scriptStatusLabel(result.status) == "Ok");
        CHECK(result.ran);
        CHECK_FALSE(result.rejected);
    }
    CHECK(script_test::completion(fx.vm, "log.join('|')") == "init:0|upd:true:0.5:1|destroy:0");

    for (const char* const file : {"arrow.js", "getter.js"}) {
        CAPTURE(file);
        const ScriptInstance other = made(fx, file);
        const ScriptHookResult result = fx.vm.callHook(other, ScriptHook::Update, 0.5);
        CHECK(scriptStatusLabel(result.status) == "Ok");
        CHECK(result.ran);
    }
    CHECK(script_test::completion(fx.vm, "log.slice(3).join('|')") == "arrow:true:0.5|get:true:true:0.5");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a hook that is not a function is absent, and nothing is logged for it (VM75)") {
    script_test::LogCapture log;
    Fixture fx;
    const std::array<std::pair<std::string_view, std::string_view>, 5> shapes{{
        {"undef", "  onUpdate = undefined;"},
        {"null", "  onUpdate = null;"},
        {"number", "  onUpdate = 5;"},
        {"object", "  onUpdate = {};"},
        {"none", "  other = 1;"},
    }};
    for (const auto& [file, body] : shapes) {
        CAPTURE(file);
        load(fx, std::format("{}.js", file), classSource(body));
        const ScriptInstance instance = made(fx, std::format("{}.js", file));
        const std::uint64_t before = fx.vm.stats().unhandledRejections;
        log.clear();
        for (const ScriptHook hook : {ScriptHook::Init, ScriptHook::Update, ScriptHook::Destroy}) {
            const ScriptHookResult result = fx.vm.callHook(instance, hook, 0.5);
            CHECK(scriptStatusLabel(result.status) == "Ok");
            CHECK_FALSE(result.ran);
            CHECK_FALSE(result.rejected);
            CHECK(result.error.message.empty());
        }
        CHECK(log.countAtLeast(LogLevel::Info) == 0U);
        CHECK(fx.vm.stats().unhandledRejections == before);
    }
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a hook that throws is one Exception and one record (VM76)") {
    script_test::LogCapture log;
    Fixture fx;
    load(fx, "method.js", classSource("  onUpdate() {\n    throw new Error('boom');\n  }"));
    load(fx, "getter.js", classSource("  get onUpdate() {\n    throw new Error('from the getter');\n  }"));
    load(fx, "klass.js", classSource("  onUpdate = class {};"));

    const ScriptInstance method = made(fx, "method.js");
    log.clear();
    const ScriptHookResult thrown = fx.vm.callHook(method, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(thrown.status) == "Exception");
    CHECK(thrown.ran);
    CHECK(thrown.error.message == "boom");
    const std::string methodRecord = theOneError(log);
    CHECK(methodRecord.find("uncaught Error: boom") != std::string::npos);
    CHECK(methodRecord.find("at onUpdate") != std::string::npos);

    const ScriptInstance getter = made(fx, "getter.js");
    log.clear();
    const ScriptHookResult read = fx.vm.callHook(getter, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(read.status) == "Exception");
    CHECK_FALSE(read.ran);
    CHECK(read.error.message == "from the getter");
    CHECK(theOneError(log).find("get onUpdate") != std::string::npos);

    const ScriptInstance klass = made(fx, "klass.js");
    log.clear();
    const ScriptHookResult called = fx.vm.callHook(klass, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(called.status) == "Exception");
    CHECK(called.error.message == "class constructors must be invoked with 'new'");
    static_cast<void>(theOneError(log));
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: an endless hook is Interrupted for its own call alone (VM77)") {
    script_test::LogCapture log;
    VmConfig config = script_test::patient();
    config.callTimeBudget = std::chrono::milliseconds{100};
    SUBCASE("a loop") {
        Fixture fx{config};
        load(fx, "spin.js", classSource("  onUpdate() {\n    for (;;) {\n    }\n  }"));
        load(fx, "calm.js", classSource("  onUpdate() {\n    globalThis.calm = true;\n  }"));
        const ScriptInstance spin = made(fx, "spin.js", "a");
        const ScriptInstance calm = made(fx, "calm.js", "b");
        log.clear();
        const ScriptHookResult spun = fx.vm.callHook(spin, ScriptHook::Update, 0.5);
        CHECK(scriptStatusLabel(spun.status) == "Interrupted");
        CHECK(spun.ran);
        CHECK(theOneError(log).find("interrupted after exceeding") != std::string::npos);
        log.clear();
        const ScriptHookResult next = fx.vm.callHook(calm, ScriptHook::Update, 0.5);
        CHECK(scriptStatusLabel(next.status) == "Ok");
        CHECK(script_test::completion(fx.vm, "String(globalThis.calm)") == "true");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        script_test::expectCleanShutdown(fx.vm);
    }
    SUBCASE("a promise executor that never ends (R34)") {
        Fixture fx{config};
        load(fx, "async.js",
             classSource(
                 "  async onUpdate() {\n    await new Promise(() => {\n      for (;;) {\n      }\n    });\n  }"));
        load(fx, "calm.js", classSource("  onUpdate() {\n    globalThis.calm = true;\n  }"));
        const ScriptInstance stuck = made(fx, "async.js", "a");
        const ScriptInstance calm = made(fx, "calm.js", "b");
        log.clear();
        const ScriptHookResult spun = fx.vm.callHook(stuck, ScriptHook::Update, 0.5);
        CHECK(scriptStatusLabel(spun.status) == "Interrupted");
        static_cast<void>(theOneError(log));
        // R34 as it stands: the engine's refusal after the fire reads as an Exception on the next call.
        log.clear();
        const ScriptHookResult next = fx.vm.callHook(calm, ScriptHook::Update, 0.5);
        INFO("next: ", next.error.name, ": ", next.error.message);
        CHECK(scriptStatusLabel(next.status) == "Exception");
        CHECK(next.error.name == "InternalError");
        CHECK(next.error.message == "interrupted");
        static_cast<void>(theOneError(log));
        script_test::expectCleanShutdown(fx.vm);
    }
}

TEST_CASE("vm: an async hook's rejection is the call's own report, and a generator's body never runs (VM78)") {
    script_test::LogCapture log;
    Fixture fx;
    load(fx, "reject.js", classSource("  async onUpdate() {\n    throw new Error('x');\n  }"));
    load(fx, "settle.js", classSource("  async onUpdate() {\n    await 1;\n    globalThis.settled = true;\n  }"));
    load(fx, "gen.js", classSource("  *onUpdate() {\n    globalThis.body = true;\n  }"));
    REQUIRE(script_test::completion(fx.vm, "globalThis.settled = false; globalThis.body = false; 0") == "0");

    const ScriptInstance reject = made(fx, "reject.js");
    for (int round = 0; round < 2; ++round) {
        CAPTURE(round);
        const std::uint64_t before = fx.vm.stats().unhandledRejections;
        log.clear();
        const ScriptHookResult result = fx.vm.callHook(reject, ScriptHook::Update, 0.5);
        CHECK(scriptStatusLabel(result.status) == "Ok");
        CHECK(result.ran);
        CHECK(result.rejected);
        CHECK(theOneError(log).find("unhandled promise rejection: Error: x") != std::string::npos);
        CHECK(fx.vm.stats().unhandledRejections == before + 1U);
    }

    const ScriptInstance settle = made(fx, "settle.js", "s");
    log.clear();
    const ScriptHookResult awaited = fx.vm.callHook(settle, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(awaited.status) == "Ok");
    CHECK(awaited.ran);
    CHECK_FALSE(awaited.rejected);  // the lifetime count is already 2: only this call's report counts
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(script_test::completion(fx.vm, "String(globalThis.settled)") == "true");

    const ScriptInstance gen = made(fx, "gen.js", "g");
    const ScriptHookResult generated = fx.vm.callHook(gen, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(generated.status) == "Ok");
    CHECK(generated.ran);
    CHECK(script_test::completion(fx.vm, "String(globalThis.body)") == "false");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a handle that names no live instance is refused once and nothing runs (VM79)") {
    script_test::LogCapture log;
    Fixture fx;
    load(fx, "m.js", classSource("  onUpdate() {\n    globalThis.hits += 1;\n  }"));
    REQUIRE(script_test::completion(fx.vm, "globalThis.hits = 0; String(hits)") == "0");
    const std::string expected = "script: callHook refused: the handle names no live instance";

    log.clear();
    const ScriptHookResult none = fx.vm.callHook(ScriptInstance{}, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(none.status) == "UnknownInstance");
    CHECK_FALSE(none.ran);
    CHECK(theOneError(log) == expected);

    const ScriptInstance stale = made(fx, "m.js", "a");
    REQUIRE(fx.vm.destroyInstance(stale) == engine::script::ScriptStatus::Ok);
    const ScriptInstance reused = made(fx, "m.js", "b");  // the freed slot may be recycled; the generation differs
    CHECK_FALSE(stale == reused);
    log.clear();
    const ScriptHookResult gone = fx.vm.callHook(stale, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(gone.status) == "UnknownInstance");
    CHECK(theOneError(log) == expected);
    CHECK(script_test::completion(fx.vm, "String(hits)") == "0");

    Vm invalid;
    log.clear();
    CHECK(scriptStatusLabel(invalid.callHook(reused, ScriptHook::Update, 0.5).status) == "InvalidVm");
    CHECK(log.countAtLeast(LogLevel::Warn) == 1U);

    log.clear();
    ScriptHookResult fromOther;
    std::thread other{[&] { fromOther = fx.vm.callHook(reused, ScriptHook::Update, 0.5); }};
    other.join();
    CHECK(scriptStatusLabel(fromOther.status) == "WrongThread");
    CHECK(log.countAtLeast(LogLevel::Warn) == 1U);
    CHECK(script_test::completion(fx.vm, "String(hits)") == "0");

    const ScriptHookResult control = fx.vm.callHook(reused, ScriptHook::Update, 0.5);  // the live handle still works
    CHECK(scriptStatusLabel(control.status) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(hits)") == "1");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: a hook the heap refuses is OutOfMemory, once, and the next call is Ok (VM80)") {
    script_test::LogCapture log;
    Fixture fx{script_test::patient(VmConfig{.heapLimitBytes = std::size_t{2} * 1024U * 1024U})};
    load(fx, "hog.js", classSource("  onUpdate() {\n    return 'x'.repeat(1e9);\n  }"));
    load(fx, "calm.js", classSource("  onUpdate() {\n  }"));
    const ScriptInstance hog = made(fx, "hog.js", "a");
    const ScriptInstance calm = made(fx, "calm.js", "b");
    log.clear();
    const ScriptHookResult full = fx.vm.callHook(hog, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(full.status) == "OutOfMemory");
    CHECK(full.ran);
    CHECK(theOneError(log).find("out of memory (heap limit 2097152 bytes, ") != std::string::npos);
    log.clear();
    const ScriptHookResult next = fx.vm.callHook(calm, ScriptHook::Update, 0.5);
    CHECK(scriptStatusLabel(next.status) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: callTimeBudget reads the configured budget and the hook names are the three a script defines (VM81)") {
    const script_test::LogCapture log;
    {
        auto created = Vm::create(VmConfig{.callTimeBudget = std::chrono::milliseconds{250}});
        REQUIRE(created.vm.valid());
        CHECK(created.vm.callTimeBudget() == std::chrono::milliseconds{250});
        script_test::expectCleanShutdown(created.vm);
    }
    {
        auto created = Vm::create();
        REQUIRE(created.vm.valid());
        CHECK(created.vm.callTimeBudget() == engine::script::DEFAULT_CALL_TIME_BUDGET);
        Vm moved = std::move(created.vm);
        CHECK(created.vm.callTimeBudget() == std::chrono::milliseconds{0});  // moved-from
        CHECK(moved.callTimeBudget() == engine::script::DEFAULT_CALL_TIME_BUDGET);
        script_test::expectCleanShutdown(moved);
    }
    {
        Vm patientVm = script_test::makeBuiltinVm();
        CHECK(patientVm.callTimeBudget() == engine::script::MAX_CALL_TIME_BUDGET);
        script_test::expectCleanShutdown(patientVm);
    }
    CHECK(engine::script::SCRIPT_HOOK_COUNT == 3U);
    CHECK(engine::script::scriptHookName(ScriptHook::Init) == "onInit");
    CHECK(engine::script::scriptHookName(ScriptHook::Update) == "onUpdate");
    CHECK(engine::script::scriptHookName(ScriptHook::Destroy) == "onDestroy");
}

// ---- VM83-VM87: entity.script, the instance attached to an entity (task 4.4.2)
// ---------------------------------------

namespace {

void entityGlobal(Fixture& fx, std::string_view name, Entity entity) {
    REQUIRE(fx.vm.defineEntityGlobal(name, entity) == BindStatus::Ok);
}

// `expr` inside a function that throws nothing: its result, or "<name>: <message>" of what it threw.
[[nodiscard]] std::string caught(Fixture& fx, std::string_view expr) {
    return script_test::completion(
        fx.vm, std::format("(function () {{ try {{ return String({}); }} catch (x) {{ return x.name + ': ' + "
                           "x.message; }} }})()",
                           expr));
}

}  // namespace

TEST_CASE("vm: entity.script is the instance attached to the entity, and null for an entity with none (VM83)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e1 = world.create();
    const Entity e2 = world.create();
    const Entity bare = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  n = 0;\n  bump() { this.n += 1; return this.n; }"));
    const ScriptInstance a = attached(fx, "m.js", e1);
    const ScriptInstance b = attached(fx, "m.js", e2);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    entityGlobal(fx, "e1", e1);
    entityGlobal(fx, "e2", e2);
    entityGlobal(fx, "bare", bare);

    CHECK(script_test::completion(fx.vm,
                                  "[e1.script === a, e2.script === b, e1.script !== e2.script, bare.script]"
                                  ".map(String).join()") == "true,true,true,null");
    CHECK(script_test::completion(fx.vm, "e1.script !== Object.getPrototypeOf(a)") == "true");
    // The object is the instance itself: its own field moves, and no other instance's does.
    CHECK(script_test::completion(fx.vm, "e1.script.bump(); e1.script.bump(); [a.n, b.n, e2.script.bump()].join()") ==
          "2,0,1");
    CHECK(script_test::completion(fx.vm, "e1.script.entity === e1") == "true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: after a reload entity.script is the new object, the handle's current one (VM84)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e1 = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  keep = 4;\n  version() { return 1; }"));
    const ScriptInstance a = attached(fx, "m.js", e1);
    entityGlobal(fx, "e1", e1);
    REQUIRE(script_test::completion(fx.vm, "globalThis.before = e1.script; String(before.version())") == "1");

    load(fx, "m.js", classSource("  keep = 4;\n  version() { return 2; }"));
    CHECK(script_test::completion(fx.vm,
                                  "[e1.script !== before, e1.script.version(), before.version(), e1.script.keep]"
                                  ".join()") == "true,2,1,4");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "String(e1.script === a)") == "true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: entity.script is the earliest live attach, and null once the last is destroyed (VM85)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e1 = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  id = 'm';"));
    load(fx, "n.js", classSource("  id = 'n';", "Other"));
    const ScriptInstance first = attached(fx, "m.js", e1);
    const ScriptInstance second = attached(fx, "n.js", e1);
    entityGlobal(fx, "e1", e1);
    expose(fx.vm, first, "first");
    expose(fx.vm, second, "second");
    CHECK(script_test::completion(fx.vm, "[e1.script === first, e1.script === second, e1.script.id].join()") ==
          "true,false,m");

    CHECK(scriptStatusLabel(fx.vm.destroyInstance(first)) == "Ok");
    CHECK(script_test::completion(fx.vm, "[e1.script === second, e1.script.id].join()") == "true,n");
    // A later attach under the same entity queues behind the one that is left.
    const ScriptInstance third = attached(fx, "m.js", e1);
    expose(fx.vm, third, "third");
    CHECK(script_test::completion(fx.vm, "String(e1.script === second)") == "true");
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(second)) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(e1.script === third)") == "true");
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(third)) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(e1.script)") == "null");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: an instance attached under an earlier binding is not answered under a later one (VM86)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e1 = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  id = 'm';"));
    const ScriptInstance a = attached(fx, "m.js", e1);
    entityGlobal(fx, "old", e1);
    CHECK(script_test::completion(fx.vm, "old.script.id") == "m");

    bind(fx, world);  // a new binding, the same World
    entityGlobal(fx, "fresh", e1);
    CHECK(script_test::completion(fx.vm, "String(fresh.script)") == "null");
    CHECK(caught(fx, "old.script") ==
          "TypeError: engine.Entity.script: the entity belongs to an earlier World binding");
    // The instance is still live: a new attach under the new binding is answered, and the old one still is not.
    const ScriptInstance b = attached(fx, "m.js", e1);
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "String(fresh.script === b)") == "true");
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(a)) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(fresh.script === b)") == "true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("vm: script is a read-only accessor that throws for a dead entity and for no World (VM87)") {
    const script_test::LogCapture log;
    engine::World world;
    Fixture fx;
    const Entity e1 = world.create();
    const Entity doomed = world.create();
    bind(fx, world);
    load(fx, "m.js", classSource("  id = 'm';"));
    const ScriptInstance a = attached(fx, "m.js", e1);
    expose(fx.vm, a, "a");
    entityGlobal(fx, "e1", e1);
    entityGlobal(fx, "doomed", doomed);

    // Sloppy code ignores an assignment to a getter-only accessor (evaluateScript compiles strict, so the sloppy
    // function is made by Function); strict code throws.
    CHECK(caught(fx, "new Function(\"e1.script = null; return 'assigned'\")()") == "assigned");
    CHECK(script_test::completion(fx.vm, "String(e1.script === a)") == "true");
    CHECK(caught(fx, "(function () { 'use strict'; e1.script = null; return 'assigned'; })()") ==
          "TypeError: no setter for property");
    CHECK(script_test::completion(fx.vm, "String(e1.script === a)") == "true");
    CHECK(script_test::completion(fx.vm,
                                  "const d = Object.getOwnPropertyDescriptor(engine.Entity.prototype, 'script');"
                                  "const p = Object.getOwnPropertyDescriptor(engine.Entity.prototype, 'parent');"
                                  "[typeof d.get, d.set, d.enumerable === p.enumerable, d.configurable]"
                                  ".map(String).join()") == "function,undefined,true,false");

    REQUIRE(world.destroy(doomed));
    CHECK(caught(fx, "doomed.script") == "TypeError: engine.Entity.script: the entity is not alive");
    fx.vm.unbindWorld();
    CHECK(caught(fx, "e1.script") == "TypeError: engine.Entity.script: no World is bound");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}
