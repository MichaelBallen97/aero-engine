// tests/script_system_test.cpp -- task 4.4.1: engine::script::ScriptSystem over one World (LC1-LC25, LC27, LC31-LC38)
// and task 4.4.2's flush and entity.script (LC40-LC49). The system is written against the Vm's public API, so every
// case drives it through a VM over the built-in table, a World, and a resolver from a script Guid to a res:// path.
// Scripts observe through `globalThis.log`, instance fields and `this.entity.alive`, so none of these cases needs a
// bound component and all of them run with the reflect tools OFF. A TU of aero_tests, which supplies main() from
// test_main.cpp.
#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>

namespace {

using engine::Entity;
using engine::Guid;
using engine::LogLevel;
using engine::ScriptComponent;
using engine::Transform;
using engine::script::ScriptEntityState;
using engine::script::scriptEntityStateLabel;
using engine::script::ScriptHook;
using engine::script::ScriptInstance;
using engine::script::scriptStatusLabel;
using engine::script::ScriptSystem;
using engine::script::ScriptSystemReport;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::internal::VmAccess;

using script_test::classSource;
using script_test::MemoryBackend;

// A class whose three hooks each log one line, `<tag><hook>:<n>` (onUpdate adds `:<dt>`), n being the instance's own
// serial. Each extra statement runs AFTER the hook's own log line, so a throw still shows the call happened.
[[nodiscard]] std::string hooks(std::string_view tag = "", std::string_view initExtra = "",
                                std::string_view updateExtra = "", std::string_view destroyExtra = "") {
    return std::format(
        "  n = globalThis.serial++;\n  count = 0;\n"
        "  onInit() {{ log.push('{0}init:' + this.n); {1} }}\n"
        "  onUpdate(dt) {{ this.count += 1; log.push('{0}update:' + this.n + ':' + dt); {2} }}\n"
        "  onDestroy() {{ log.push('{0}destroy:' + this.n); {3} }}",
        tag, initExtra, updateExtra, destroyExtra);
}

[[nodiscard]] Guid guidOf(std::uint64_t id) { return Guid{0, id}; }

// A VM over a VFS holding one MemoryBackend, a World, and the ScriptSystem over both. Non-movable: the Vm borrows the
// VFS and the system borrows the Vm and the World; the system is declared last, so it is destroyed first.
class SystemFixture {
public:
    explicit SystemFixture(const VmConfig& config = script_test::patient()) {
        auto backend = std::make_unique<MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(config, vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
        const engine::script::EvalResult primed =
            vm.evaluateScript("globalThis.log = []; globalThis.serial = 0; globalThis.evals = 0;", "prime.js");
        REQUIRE(primed.status == engine::script::EvalStatus::Ok);
        system = std::make_unique<ScriptSystem>(vm, world, [this](const Guid& script) { return resolve(script); });
        REQUIRE(system->bound());
    }
    SystemFixture(const SystemFixture&) = delete;
    SystemFixture& operator=(const SystemFixture&) = delete;
    SystemFixture(SystemFixture&&) = delete;
    SystemFixture& operator=(SystemFixture&&) = delete;
    ~SystemFixture() = default;

    // Makes `guid` resolve to res://s/<file>, holding `source`.
    void publish(const Guid& guid, const std::string& file, const std::string& source) {
        files->set(std::format("s/{}", file), source);
        paths[guid] = std::format("res://s/{}", file);
    }

    // A live entity naming `guid` (the helper the cases call attachScript).
    [[nodiscard]] Entity attachScript(const Guid& guid) {
        const Entity entity = world.create();
        REQUIRE(world.add<ScriptComponent>(entity, ScriptComponent{guid}) != nullptr);
        return entity;
    }

    ScriptSystemReport tick(double dt = 0.5) { return system->update(dt); }
    [[nodiscard]] std::string reload(const Guid& guid) {
        return std::string{scriptStatusLabel(system->reloadScript(guid))};
    }
    // Runs `source` as global code; the completion value is discarded.
    void run(std::string_view source) { static_cast<void>(script_test::completion(vm, source)); }

    [[nodiscard]] std::string log() { return script_test::completion(vm, "log.join('|')"); }
    void clearLog() { script_test::completion(vm, "log.length = 0; ''"); }

    // Evaluates `expr` with the instance attached to `entity` bound as the global `i`.
    [[nodiscard]] std::string instanceValue(Entity entity, std::string_view expr) {
        const ScriptInstance handle = system->instanceOf(entity);
        REQUIRE(handle.valid());
        REQUIRE(VmAccess::defineInstanceGlobal(vm, handle, "i"));
        return script_test::completion(vm, expr);
    }

    // The system goes first, then the VM is shut down and checked.
    void finish() {
        system.reset();
        script_test::expectCleanShutdown(vm);
    }

    [[nodiscard]] std::size_t queriesFor(const Guid& guid) const {
        const auto found = queries.find(guid);
        return found == queries.end() ? 0U : found->second;
    }

    engine::VirtualFileSystem vfs;
    MemoryBackend* files = nullptr;
    engine::World world;
    Vm vm;
    std::map<Guid, std::string> paths;
    std::map<Guid, std::size_t> queries;
    std::unique_ptr<ScriptSystem> system;

private:
    std::string resolve(const Guid& script) {
        ++queries[script];
        const auto found = paths.find(script);
        return found == paths.end() ? std::string{} : found->second;
    }
};

struct Expected {
    std::size_t attached = 0;
    std::size_t detached = 0;
    std::size_t initialised = 0;
    std::size_t updated = 0;
    std::size_t running = 0;
};

void expectReport(const ScriptSystemReport& report, const Expected& expected) {
    CHECK(report.bound);
    CHECK(report.attached == expected.attached);
    CHECK(report.detached == expected.detached);
    CHECK(report.initialised == expected.initialised);
    CHECK(report.updated == expected.updated);
    CHECK(report.running == expected.running);
}

// True when every counter is zero and the report says nothing ran.
[[nodiscard]] bool inertReport(const ScriptSystemReport& r) {
    return !r.bound && r.attached == 0U && r.detached == 0U && r.initialised == 0U && r.updated == 0U &&
           r.faulted == 0U && r.running == 0U && r.unresolved == 0U && r.failedLoads == 0U && r.failedAttaches == 0U &&
           r.suspendedScripts == 0U;
}

[[nodiscard]] bool answersUnknown(Vm& vm, ScriptInstance handle) {
    return scriptStatusLabel(vm.callHook(handle, ScriptHook::Update, 0.0).status) == "UnknownInstance";
}

[[nodiscard]] std::string label(ScriptEntityState state) { return std::string{scriptEntityStateLabel(state)}; }

// A budget short enough for a case that interrupts a hook and long enough for an ordinary one.
[[nodiscard]] VmConfig briefBudget() {
    VmConfig config = script_test::patient();
    config.callTimeBudget = std::chrono::milliseconds{100};
    return config;
}

// The records at WARN and above whose message contains `needle`.
[[nodiscard]] std::size_t recordsNaming(const script_test::LogCapture& capture, std::string_view needle) {
    std::size_t found = 0;
    for (const script_test::CapturedRecord& record : capture.atLeast(LogLevel::Warn)) {
        found += record.message.find(needle) != std::string::npos ? 1U : 0U;
    }
    return found;
}

// A script counting its constructions in globalThis.ctor<tag>, logging `<tag>update:<c>` and `<tag>destroy:<c>`, and
// spinning in onUpdate once globalThis.spin<tag> is set. `async` makes the spin a promise executor that never ends.
[[nodiscard]] std::string spinSource(std::string_view tag, bool async = false) {
    const std::string_view spin = async ? "await new Promise(() => { for (;;) {} });" : "for (;;) {}";
    return classSource(
        std::format("  c = globalThis.ctor{0}++;\n"
                    "  {2}onUpdate() {{ log.push('{0}update:' + this.c); if (globalThis.spin{0}) {{ {1} }} }}\n"
                    "  onDestroy() {{ log.push('{0}destroy:' + this.c); }}",
                    tag, spin, async ? "async " : ""));
}

// A script counting its constructions in globalThis.ctor<tag>, logging `<tag>update:<c>` and `<tag>destroy:<c>`, and
// spinning in onDestroy, after its log line, in each instance whose c is at least globalThis.spin<tag> once it is set.
[[nodiscard]] std::string destroySpinSource(std::string_view tag) {
    return classSource(
        std::format("  c = globalThis.ctor{0}++;\n"
                    "  onUpdate() {{ log.push('{0}update:' + this.c); }}\n"
                    "  onDestroy() {{\n"
                    "    log.push('{0}destroy:' + this.c);\n"
                    "    if (globalThis.spin{0} !== undefined && this.c >= globalThis.spin{0}) {{ for (;;) {{}} }}\n"
                    "  }}",
                    tag));
}

// The line of the throw in faultySource(true): classSource puts the members from line 2.
constexpr int FAULTY_THROW_LINE = 8;

[[nodiscard]] std::string faultySource(bool throws) {
    return classSource(
        std::format("  bad = globalThis.flags.shift();\n"
                    "  inits = 0;\n"
                    "  updates = 0;\n"
                    "  onInit() {{ this.inits += 1; }}\n"
                    "  onUpdate() {{\n"
                    "    this.updates += 1;\n"
                    "{}"
                    "  }}",
                    throws ? "    if (this.bad) throw new Error('bad mover');\n" : "    // corrected\n"));
}

}  // namespace

TEST_CASE(
    "system: a bound system, an inert one over a refused bind, and one over a World without the component (LC1)") {
    script_test::LogCapture log;
    {
        SystemFixture fx;
        CHECK(fx.system->bound());
        const Entity probe = fx.world.create();
        CHECK(fx.vm.defineEntityGlobal("probe", probe) == engine::script::BindStatus::Ok);
        fx.finish();
    }

    {  // a VM over the probe table, a World without the probe types: the VM refuses the bind
        const auto table = script_test::probeTable();
        Vm probeVm = script_test::makeProbeVm(table);
        engine::World plain;
        const Entity named = plain.create();
        REQUIRE(plain.add<ScriptComponent>(named, ScriptComponent{guidOf(1)}) != nullptr);
        std::size_t asks = 0;
        log.clear();
        {
            ScriptSystem refused(probeVm, plain, [&asks](const Guid& /*script*/) {
                ++asks;
                return std::string{"res://s/none.js"};
            });
            CHECK_FALSE(refused.bound());
            const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
            REQUIRE(loud.size() == 1U);
            CHECK(loud[0].level == LogLevel::Error);
            CHECK(loud[0].message.starts_with("script: bindWorld refused"));
            CHECK(inertReport(refused.update(0.5)));
            CHECK(inertReport(refused.stop()));
            CHECK(asks == 0U);
            CHECK(log.countAtLeast(LogLevel::Warn) == 1U);
        }
        script_test::expectCleanShutdown(probeVm);
    }

    {  // a moved-from World registers nothing
        Vm vm = script_test::makeBuiltinVm();
        engine::World source;
        const engine::World taken{std::move(source)};
        std::size_t asks = 0;
        log.clear();
        {
            // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- deliberate
            ScriptSystem refused(vm, source, [&asks](const Guid& /*script*/) {
                ++asks;
                return std::string{};
            });
            CHECK_FALSE(refused.bound());
            const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
            REQUIRE(loud.size() == 1U);
            CHECK(loud[0].level == LogLevel::Error);
            CHECK(loud[0].message ==
                  "script: ScriptSystem refused: the World does not register engine::ScriptComponent");
            CHECK(inertReport(refused.update(0.5)));
            CHECK(asks == 0U);
        }
        script_test::expectCleanShutdown(vm);
    }
}

TEST_CASE("system: every onInit runs before any onUpdate, once, and each instance keeps its own fields (LC2)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    const Entity e0 = fx.attachScript(guidOf(1));
    const Entity e1 = fx.attachScript(guidOf(1));

    expectReport(fx.tick(), Expected{.attached = 2, .detached = 0, .initialised = 2, .updated = 2, .running = 2});
    CHECK(fx.log() == "init:0|init:1|update:0:0.5|update:1:0.5");
    expectReport(fx.tick(), Expected{.attached = 0, .detached = 0, .initialised = 0, .updated = 2, .running = 2});
    expectReport(fx.tick(), Expected{.attached = 0, .detached = 0, .initialised = 0, .updated = 2, .running = 2});
    CHECK(fx.log() == "init:0|init:1|update:0:0.5|update:1:0.5|update:0:0.5|update:1:0.5|update:0:0.5|update:1:0.5");
    CHECK(fx.instanceValue(e0, "i.n + ',' + i.count") == "0,3");
    CHECK(fx.instanceValue(e1, "i.n + ',' + i.count") == "1,3");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE(
    "system: a detach runs onDestroy then destroys, for a gone entity, a removed component, nil and a rename (LC3)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "a.js", classSource(hooks()));
    fx.publish(guidOf(2), "b.js", classSource(hooks("b:")));
    const Entity e1 = fx.attachScript(guidOf(1));
    const Entity e2 = fx.attachScript(guidOf(1));
    const Entity e3 = fx.attachScript(guidOf(1));
    const Entity e4 = fx.attachScript(guidOf(1));
    expectReport(fx.tick(), Expected{.attached = 4, .detached = 0, .initialised = 4, .updated = 4, .running = 4});
    const std::array<ScriptInstance, 4> held{fx.system->instanceOf(e1), fx.system->instanceOf(e2),
                                             fx.system->instanceOf(e3), fx.system->instanceOf(e4)};
    for (const ScriptInstance handle : held) {
        REQUIRE(handle.valid());
    }

    REQUIRE(fx.world.destroy(e1));
    REQUIRE(fx.world.remove<ScriptComponent>(e2));
    REQUIRE(fx.world.get<ScriptComponent>(e3) != nullptr);
    fx.world.get<ScriptComponent>(e3)->script = Guid{};
    REQUIRE(fx.world.get<ScriptComponent>(e4) != nullptr);
    fx.world.get<ScriptComponent>(e4)->script = guidOf(2);
    fx.clearLog();

    const ScriptSystemReport report = fx.tick();
    CHECK(report.detached == 4U);
    CHECK(report.attached == 1U);
    CHECK(fx.log() == "destroy:0|destroy:1|destroy:2|destroy:3|b:init:4|b:update:4:0.5");
    for (const ScriptInstance handle : held) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    CHECK(label(fx.system->stateOf(e4)) == "Running");
    CHECK(fx.system->instanceOf(e4).valid());
    CHECK(fx.system->instanceOf(e4) != held[3]);
    CHECK(label(fx.system->stateOf(e2)) == "None");
    CHECK(label(fx.system->stateOf(e3)) == "None");
    fx.finish();
}

TEST_CASE("system: the run order is the attach order, never the World's or the entity index's (LC4)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    const Entity e1 = fx.attachScript(guidOf(1));
    const Entity e2 = fx.attachScript(guidOf(1));
    const Entity e3 = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "init:0|init:1|init:2|update:0:0.5|update:1:0.5|update:2:0.5");
    REQUIRE(fx.world.destroy(e1));
    const Entity e4 = fx.attachScript(guidOf(1));
    CHECK(e4.index == e1.index);  // the slot is recycled, so e4 sorts BEFORE e2 and e3 by index
    CHECK(e2.index < e3.index);
    fx.clearLog();
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "destroy:0|init:3|update:1:0.5|update:2:0.5|update:3:0.5");
    fx.finish();
}

TEST_CASE("system: a Guid is resolved once and a path loaded once, however many entities and updates (LC5)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    // Two Guids, one path: the module's body must still be evaluated once.
    const std::string source = "globalThis.evals += 1;\n" + classSource(hooks());
    fx.publish(guidOf(1), "m.js", source);
    fx.paths[guidOf(2)] = "res://s/m.js";
    static_cast<void>(fx.attachScript(guidOf(1)));
    static_cast<void>(fx.attachScript(guidOf(1)));
    static_cast<void>(fx.attachScript(guidOf(2)));

    static_cast<void>(fx.tick());
    const std::size_t readsAfterFirst = fx.files->readCount();
    CHECK(readsAfterFirst >= 1U);
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(fx.tick());
    }
    CHECK(fx.queriesFor(guidOf(1)) == 1U);
    CHECK(fx.queriesFor(guidOf(2)) == 1U);
    CHECK(script_test::completion(fx.vm, "String(evals)") == "1");
    CHECK(fx.files->readCount() == readsAfterFirst);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: a Guid the resolver does not know is counted, not logged, and not queried again (LC6)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    const Entity lost = fx.attachScript(guidOf(9));
    const Entity fine = fx.attachScript(guidOf(1));
    for (int i = 0; i < 5; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.unresolved == 1U);
        CHECK(report.running == 1U);
    }
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(label(fx.system->stateOf(lost)) == "Unresolved");
    CHECK(fx.queriesFor(guidOf(9)) == 1U);
    CHECK(fx.instanceValue(fine, "String(i.count)") == "5");
    fx.finish();
}

TEST_CASE("system: a script that does not compile is one ERROR and is never loaded again (LC7)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "bad.js", "var = ;\n");
    const Entity a = fx.attachScript(guidOf(1));
    const Entity b = fx.attachScript(guidOf(1));
    const ScriptSystemReport first = fx.tick();
    CHECK(first.failedLoads == 1U);
    CHECK(first.running == 0U);
    const std::size_t readsAfterFirst = fx.files->readCount();
    for (int i = 0; i < 4; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.failedLoads == 1U);
    }
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(label(fx.system->stateOf(a)) == "LoadFailed");
    CHECK(label(fx.system->stateOf(b)) == "LoadFailed");
    CHECK(fx.files->readCount() == readsAfterFirst);
    fx.finish();
}

TEST_CASE("system: a constructor that throws is one ERROR per entity, then that entity stays unattached (LC8)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "boom.js", classSource("  constructor() {\n    super();\n    throw new Error('boom');\n  }"));
    fx.publish(guidOf(2), "ok.js", classSource(hooks()));
    const Entity a = fx.attachScript(guidOf(1));
    const Entity b = fx.attachScript(guidOf(1));
    const Entity healthy = fx.attachScript(guidOf(2));
    capture.clear();
    const ScriptSystemReport first = fx.tick();
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);
    CHECK(first.failedAttaches == 2U);
    CHECK(first.running == 1U);
    capture.clear();
    for (int i = 0; i < 4; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.failedAttaches == 2U);
    }
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(label(fx.system->stateOf(a)) == "AttachFailed");
    CHECK(label(fx.system->stateOf(b)) == "AttachFailed");
    CHECK(fx.instanceValue(healthy, "String(i.count)") == "5");
    fx.finish();
}

TEST_CASE("system: a hook that throws faults its instance after one record, and onDestroy still runs (LC9)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js",
               classSource(hooks("", "", "if (this.n === 0 && this.count === 2) { throw new Error('bad'); }")));
    const Entity bad = fx.attachScript(guidOf(1));
    const Entity good = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    capture.clear();
    const ScriptSystemReport second = fx.tick();
    CHECK(second.faulted == 1U);
    CHECK(second.running == 1U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    for (int i = 0; i < 8; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.faulted == 0U);
        CHECK(report.running == 1U);
    }
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    CHECK(capture.atLeast(LogLevel::Warn)[0].level == LogLevel::Error);
    CHECK(label(fx.system->stateOf(bad)) == "Faulted");
    CHECK(label(fx.system->stateOf(good)) == "Running");
    CHECK(fx.instanceValue(bad, "String(i.count)") == "2");
    CHECK(fx.instanceValue(good, "String(i.count)") == "10");
    fx.clearLog();
    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 2U);
    CHECK(fx.log() == "destroy:0|destroy:1");
    fx.finish();
}

TEST_CASE("system: an onInit that throws faults its instance with no onUpdate and no onDestroy (LC10)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks("", "throw new Error('no');")));
    const Entity e = fx.attachScript(guidOf(1));
    capture.clear();
    const ScriptSystemReport first = fx.tick();
    CHECK(first.initialised == 0U);
    CHECK(first.faulted == 1U);
    CHECK(first.updated == 0U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    static_cast<void>(fx.tick());
    static_cast<void>(fx.tick());
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    CHECK(label(fx.system->stateOf(e)) == "Faulted");
    CHECK(fx.log() == "init:0");
    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 1U);
    CHECK(fx.log() == "init:0");
    fx.finish();
}

TEST_CASE("system: an onDestroy that throws is one ERROR and the instance is destroyed anyway (LC11)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks("", "", "", "throw new Error('d');")));
    const Entity e0 = fx.attachScript(guidOf(1));
    const Entity e1 = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    const ScriptInstance first = fx.system->instanceOf(e0);
    const ScriptInstance second = fx.system->instanceOf(e1);
    REQUIRE(first.valid());
    REQUIRE(second.valid());

    REQUIRE(fx.world.get<ScriptComponent>(e0) != nullptr);
    fx.world.get<ScriptComponent>(e0)->script = Guid{};
    capture.clear();
    const ScriptSystemReport detach = fx.tick();
    CHECK(detach.detached == 1U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    capture.clear();
    CHECK(answersUnknown(fx.vm, first));
    CHECK(fx.system->instanceOf(e0) == ScriptInstance{});

    capture.clear();
    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 1U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 1U);
    capture.clear();
    CHECK(answersUnknown(fx.vm, second));
    fx.finish();
}

TEST_CASE("system: stop runs onDestroy once per initialised instance and leaves the World and the VM unbound (LC12)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    // n 0 faults in onUpdate, n 1 fails its onInit, n 2 runs.
    fx.publish(guidOf(1), "m.js",
               classSource(hooks("", "if (this.n === 1) { throw new Error('init'); }",
                                 "if (this.n === 0) { throw new Error('update'); }")));
    const Entity faulted = fx.attachScript(guidOf(1));
    const Entity failedInit = fx.attachScript(guidOf(1));
    const Entity running = fx.attachScript(guidOf(1));
    REQUIRE(fx.world.add<Transform>(running) != nullptr);
    fx.world.get<Transform>(running)->position = engine::Vec3{1.0F, 2.0F, 3.0F};
    const std::size_t entityCount = fx.world.entityCount();
    const Transform transform = *fx.world.get<Transform>(running);
    const ScriptComponent component = *fx.world.get<ScriptComponent>(running);

    static_cast<void>(fx.tick());
    static_cast<void>(fx.tick());
    const std::array<ScriptInstance, 3> held{fx.system->instanceOf(faulted), fx.system->instanceOf(failedInit),
                                             fx.system->instanceOf(running)};
    for (const ScriptInstance handle : held) {
        REQUIRE(handle.valid());
    }
    fx.clearLog();
    capture.clear();

    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.bound);
    CHECK(stopped.detached == 3U);
    CHECK(fx.log() == "destroy:0|destroy:2");
    for (const ScriptInstance handle : held) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    CHECK_FALSE(fx.system->bound());
    CHECK(fx.vm.defineEntityGlobal("probe", running) == engine::script::BindStatus::NoWorld);

    capture.clear();
    CHECK(inertReport(fx.system->stop()));
    CHECK(inertReport(fx.tick()));
    CHECK(fx.log() == "destroy:0|destroy:2");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);

    CHECK(fx.world.entityCount() == entityCount);
    CHECK(fx.world.has<ScriptComponent>(faulted));
    CHECK(fx.world.has<ScriptComponent>(failedInit));
    CHECK(fx.world.has<ScriptComponent>(running));
    CHECK_FALSE(fx.world.has<Transform>(faulted));
    CHECK(*fx.world.get<Transform>(running) == transform);
    CHECK(*fx.world.get<ScriptComponent>(running) == component);

    fx.system.reset();
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(fx.log() == "destroy:0|destroy:2");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("system: a time step that is not finite and non-negative reaches onUpdate as 0 (LC13)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    static_cast<void>(fx.attachScript(guidOf(1)));
    static_cast<void>(fx.tick(0.25));
    static_cast<void>(fx.tick(std::numeric_limits<double>::quiet_NaN()));
    static_cast<void>(fx.tick(std::numeric_limits<double>::infinity()));
    static_cast<void>(fx.tick(-1.0));
    static_cast<void>(fx.tick(0.0));
    CHECK(fx.log() == "init:0|update:0:0.25|update:0:0|update:0:0|update:0:0|update:0:0");
    fx.finish();
}

TEST_CASE("system: stateOf names every state it reaches and instanceOf is the attached handle (LC14)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "ok.js", classSource(hooks("", "", "if (this.n === 1) { throw new Error('u'); }")));
    fx.publish(guidOf(2), "bad.js", "var = ;\n");
    fx.publish(guidOf(3), "boom.js", classSource("  constructor() {\n    super();\n    throw new Error('boom');\n  }"));
    const Entity none = fx.world.create();
    const Entity nil = fx.attachScript(Guid{});
    const Entity running = fx.attachScript(guidOf(1));
    const Entity faulted = fx.attachScript(guidOf(1));
    const Entity unresolved = fx.attachScript(guidOf(9));
    const Entity loadFailed = fx.attachScript(guidOf(2));
    const Entity attachFailed = fx.attachScript(guidOf(3));
    static_cast<void>(fx.tick());

    CHECK(label(fx.system->stateOf(none)) == "None");
    CHECK(label(fx.system->stateOf(nil)) == "None");
    CHECK(label(fx.system->stateOf(running)) == "Running");
    CHECK(label(fx.system->stateOf(faulted)) == "Faulted");
    CHECK(label(fx.system->stateOf(unresolved)) == "Unresolved");
    CHECK(label(fx.system->stateOf(loadFailed)) == "LoadFailed");
    CHECK(label(fx.system->stateOf(attachFailed)) == "AttachFailed");

    CHECK_FALSE(fx.system->instanceOf(none).valid());
    CHECK_FALSE(fx.system->instanceOf(unresolved).valid());
    CHECK_FALSE(fx.system->instanceOf(attachFailed).valid());
    const ScriptInstance handle = fx.system->instanceOf(running);
    REQUIRE(handle.valid());
    capture.clear();
    CHECK(scriptStatusLabel(fx.vm.callHook(handle, ScriptHook::Update, 0.0).status) == "Ok");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);

    REQUIRE(fx.world.destroy(running));
    static_cast<void>(fx.tick());
    CHECK_FALSE(fx.system->instanceOf(running).valid());
    CHECK(label(fx.system->stateOf(running)) == "None");

    // Eight states, eight distinct non-empty labels.
    std::set<std::string> labels;
    const std::array<ScriptEntityState, 8> all{ScriptEntityState::None,        ScriptEntityState::Unresolved,
                                               ScriptEntityState::LoadFailed,  ScriptEntityState::AttachFailed,
                                               ScriptEntityState::PendingInit, ScriptEntityState::Running,
                                               ScriptEntityState::Faulted,     ScriptEntityState::Suspended};
    for (const ScriptEntityState state : all) {
        CHECK_FALSE(label(state).empty());
        labels.insert(label(state));
    }
    CHECK(labels.size() == 8U);
    fx.finish();
}

TEST_CASE("system: one throwing instance logs once, the other runs on, and a reload runs the failed one again (LC15)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.run("globalThis.flags = [true, false];");
    fx.publish(guidOf(1), "faulty.js", faultySource(true));
    const Entity bad = fx.attachScript(guidOf(1));
    const Entity good = fx.attachScript(guidOf(1));
    capture.clear();
    for (int i = 0; i < 60; ++i) {
        static_cast<void>(fx.tick());
    }
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.starts_with("script: res://s/faulty.js: uncaught Error: bad mover"));
    CHECK(loud[0].message.find(std::format("at onUpdate (res://s/faulty.js:{}:", FAULTY_THROW_LINE)) !=
          std::string::npos);
    CHECK(fx.instanceValue(good, "String(i.updates)") == "60");
    CHECK(fx.instanceValue(bad, "String(i.updates)") == "1");
    CHECK(label(fx.system->stateOf(bad)) == "Faulted");

    fx.publish(guidOf(1), "faulty.js", faultySource(false));
    CHECK(fx.reload(guidOf(1)) == "Ok");
    capture.clear();
    static_cast<void>(fx.tick());
    CHECK(fx.instanceValue(bad, "i.inits + ',' + i.updates") == "1,2");
    CHECK(fx.instanceValue(good, "i.inits + ',' + i.updates") == "1,61");
    CHECK(label(fx.system->stateOf(bad)) == "Running");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE(
    "system: a hook past its budget suspends its script, one notice, and no sibling burns another budget (LC16)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorA = 0; globalThis.ctorB = 0; globalThis.spinA = true;");
    fx.publish(guidOf(1), "a.js", spinSource("A"));
    fx.publish(guidOf(2), "b.js", spinSource("B"));
    const Entity a0 = fx.attachScript(guidOf(1));
    const Entity a1 = fx.attachScript(guidOf(1));
    const Entity b0 = fx.attachScript(guidOf(2));
    capture.clear();

    const ScriptSystemReport first = fx.tick();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.find("interrupted after exceeding") != std::string::npos);
    CHECK(loud[1].level == LogLevel::Warn);
    CHECK(loud[1].message ==
          "script: res://s/a.js: suspended: a hook ran past its 100 ms time budget; its 2 instances run no hook "
          "until the script reloads");
    CHECK(first.suspendedScripts == 1U);
    CHECK(first.running == 1U);
    CHECK(label(fx.system->stateOf(a0)) == "Suspended");
    CHECK(label(fx.system->stateOf(a1)) == "Suspended");
    CHECK(label(fx.system->stateOf(b0)) == "Running");

    static_cast<void>(fx.tick());
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "Aupdate:0|Bupdate:0|Bupdate:0|Bupdate:0");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);

    const Entity a2 = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    CHECK(label(fx.system->stateOf(a2)) == "Suspended");
    CHECK_FALSE(fx.system->instanceOf(a2).valid());
    CHECK(fx.instanceValue(b0, "String(globalThis.ctorA)") == "2");

    fx.clearLog();
    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 3U);
    CHECK(fx.log() == "Bdestroy:0");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);
    fx.finish();
}

TEST_CASE("system: the settle call keeps an interrupted promise job from faulting the next instance (LC17)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorA = 0; globalThis.ctorB = 0; globalThis.spinA = true;");
    fx.publish(guidOf(1), "a.js", spinSource("A", true));
    fx.publish(guidOf(2), "b.js", spinSource("B"));
    const Entity a0 = fx.attachScript(guidOf(1));
    const Entity b0 = fx.attachScript(guidOf(2));
    capture.clear();

    const ScriptSystemReport first = fx.tick();
    CHECK(first.faulted == 0U);
    CHECK(first.suspendedScripts == 1U);
    CHECK(fx.log() == "Aupdate:0|Bupdate:0");
    CHECK(label(fx.system->stateOf(a0)) == "Suspended");
    CHECK(label(fx.system->stateOf(b0)) == "Running");
    CHECK(recordsNaming(capture, "res://s/b.js") == 0U);
    CHECK(recordsNaming(capture, "res://s/a.js") >= 2U);

    static_cast<void>(fx.tick());
    CHECK(fx.log() == "Aupdate:0|Bupdate:0|Bupdate:0");
    CHECK(recordsNaming(capture, "res://s/b.js") == 0U);
    fx.finish();
}

TEST_CASE("system: a constructor past its budget suspends the script and no other entity burns another budget (LC18)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorA = 0; globalThis.ctorB = 0;");
    fx.publish(guidOf(1), "a.js", classSource("  c = (() => { globalThis.ctorA += 1; for (;;) {} })();"));
    fx.publish(guidOf(2), "b.js", spinSource("B"));
    const Entity a0 = fx.attachScript(guidOf(1));
    const Entity a1 = fx.attachScript(guidOf(1));
    const Entity a2 = fx.attachScript(guidOf(1));
    const Entity b0 = fx.attachScript(guidOf(2));
    capture.clear();

    const ScriptSystemReport first = fx.tick();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[1].level == LogLevel::Warn);
    CHECK(loud[1].message.find("its 0 instances run no hook until the script reloads") != std::string::npos);
    CHECK(first.attached == 1U);
    CHECK(first.failedAttaches == 0U);
    CHECK(first.suspendedScripts == 1U);
    CHECK(fx.instanceValue(b0, "String(globalThis.ctorA)") == "1");
    CHECK(label(fx.system->stateOf(a0)) == "Suspended");
    CHECK(label(fx.system->stateOf(a1)) == "Suspended");
    CHECK(label(fx.system->stateOf(a2)) == "Suspended");
    CHECK(fx.log() == "Bupdate:0");
    static_cast<void>(fx.tick());
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);
    fx.finish();
}

// N counts every instance of the path in the run order as the detach began: the released one and the two survivors
// that precede it, 3 -- never only the survivors (2) nor the entries a half-compacted run order still names (1).
TEST_CASE("system: an onDestroy past its budget at a detach counts every instance of its script (LC32)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorP = 0; globalThis.ctorQ = 0;");
    fx.publish(guidOf(1), "p.js", destroySpinSource("P"));
    fx.publish(guidOf(2), "q.js", destroySpinSource("Q"));
    const Entity p0 = fx.attachScript(guidOf(1));
    const Entity p1 = fx.attachScript(guidOf(1));
    const Entity p2 = fx.attachScript(guidOf(1));
    const Entity q0 = fx.attachScript(guidOf(2));
    expectReport(fx.tick(), Expected{.attached = 4, .detached = 0, .initialised = 4, .updated = 4, .running = 4});
    const std::array<ScriptInstance, 4> held{fx.system->instanceOf(p0), fx.system->instanceOf(p1),
                                             fx.system->instanceOf(p2), fx.system->instanceOf(q0)};
    for (const ScriptInstance handle : held) {
        REQUIRE(handle.valid());
    }

    fx.run("globalThis.spinP = 0;");
    REQUIRE(fx.world.get<ScriptComponent>(p2) != nullptr);
    fx.world.get<ScriptComponent>(p2)->script = Guid{};
    fx.clearLog();
    capture.clear();
    const ScriptSystemReport detach = fx.tick();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.find("interrupted after exceeding") != std::string::npos);
    CHECK(loud[1].level == LogLevel::Warn);
    CHECK(loud[1].message ==
          "script: res://s/p.js: suspended: a hook ran past its 100 ms time budget; its 3 instances run no hook "
          "until the script reloads");
    CHECK(detach.detached == 1U);
    CHECK(detach.faulted == 0U);
    CHECK(detach.updated == 1U);
    CHECK(detach.running == 1U);
    CHECK(detach.suspendedScripts == 1U);
    CHECK(fx.log() == "Pdestroy:2|Qupdate:0");
    CHECK_FALSE(fx.system->instanceOf(p2).valid());
    CHECK(label(fx.system->stateOf(p2)) == "None");
    CHECK(fx.system->instanceOf(p0) == held[0]);
    CHECK(fx.system->instanceOf(p1) == held[1]);
    CHECK(fx.system->instanceOf(q0) == held[3]);
    CHECK(label(fx.system->stateOf(p0)) == "Suspended");
    CHECK(label(fx.system->stateOf(p1)) == "Suspended");
    CHECK(label(fx.system->stateOf(q0)) == "Running");

    static_cast<void>(fx.tick());
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "Pdestroy:2|Qupdate:0|Qupdate:0|Qupdate:0");
    CHECK(script_test::completion(fx.vm, "String(globalThis.ctorP)") == "3");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);

    fx.clearLog();
    capture.clear();
    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 3U);
    CHECK(fx.log() == "Qdestroy:0");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    for (const ScriptInstance handle : held) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    fx.finish();
}

// P's second instance spins: its first already ran onDestroy, so N = 3 tells a count over the intact run order from
// one over what is left of it (2); the third never runs onDestroy, and Q's instance after the spin still does.
TEST_CASE("system: an onDestroy past its budget at stop() runs no later onDestroy of its script (LC33)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorP = 0; globalThis.ctorQ = 0;");
    fx.publish(guidOf(1), "p.js", destroySpinSource("P"));
    fx.publish(guidOf(2), "q.js", destroySpinSource("Q"));
    const Entity p0 = fx.attachScript(guidOf(1));
    const Entity q0 = fx.attachScript(guidOf(2));
    const Entity p1 = fx.attachScript(guidOf(1));
    const Entity p2 = fx.attachScript(guidOf(1));
    const Entity q1 = fx.attachScript(guidOf(2));
    expectReport(fx.tick(), Expected{.attached = 5, .detached = 0, .initialised = 5, .updated = 5, .running = 5});
    const std::array<ScriptInstance, 5> held{fx.system->instanceOf(p0), fx.system->instanceOf(q0),
                                             fx.system->instanceOf(p1), fx.system->instanceOf(p2),
                                             fx.system->instanceOf(q1)};
    for (const ScriptInstance handle : held) {
        REQUIRE(handle.valid());
    }

    fx.run("globalThis.spinP = 1;");
    fx.clearLog();
    capture.clear();
    const ScriptSystemReport stopped = fx.system->stop();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.find("interrupted after exceeding") != std::string::npos);
    CHECK(loud[1].level == LogLevel::Warn);
    CHECK(loud[1].message ==
          "script: res://s/p.js: suspended: a hook ran past its 100 ms time budget; its 3 instances run no hook "
          "until the script reloads");
    CHECK(stopped.bound);
    CHECK(stopped.detached == 5U);
    CHECK(fx.log() == "Pdestroy:0|Qdestroy:0|Pdestroy:1|Qdestroy:1");
    CHECK_FALSE(fx.system->bound());
    capture.clear();
    for (const ScriptInstance handle : held) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    CHECK(capture.countAtLeast(LogLevel::Warn) == held.size());  // one refusal ERROR per probe, nothing else
    fx.finish();
}

TEST_CASE("system: a rejecting async onUpdate is one record and a fault, a resolving one runs every update (LC19)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.run("globalThis.rejCalls = 0; globalThis.okCalls = 0;");
    fx.publish(guidOf(1), "rej.js",
               classSource("  async onUpdate() {\n    globalThis.rejCalls += 1;\n    throw new Error('x');\n  }"));
    fx.publish(guidOf(2), "ok.js",
               classSource("  async onUpdate() {\n    await 1;\n    globalThis.okCalls += 1;\n  }"));
    const Entity rejecting = fx.attachScript(guidOf(1));
    const Entity resolving = fx.attachScript(guidOf(2));
    capture.clear();
    const ScriptSystemReport first = fx.tick();
    CHECK(first.faulted == 1U);
    for (int i = 0; i < 4; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.faulted == 0U);
    }
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.find("unhandled promise rejection") != std::string::npos);
    CHECK(fx.instanceValue(rejecting, "String(globalThis.rejCalls)") == "1");
    CHECK(fx.instanceValue(resolving, "String(globalThis.okCalls)") == "5");
    CHECK(label(fx.system->stateOf(rejecting)) == "Faulted");
    CHECK(label(fx.system->stateOf(resolving)) == "Running");
    fx.finish();
}

TEST_CASE("system: reloadScript of a script the system has not loaded is NotLoaded and runs nothing (LC20)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    static_cast<void>(fx.attachScript(guidOf(1)));
    static_cast<void>(fx.tick());
    const std::size_t reads = fx.files->readCount();
    capture.clear();
    CHECK(fx.reload(guidOf(77)) == "NotLoaded");
    CHECK(fx.files->readCount() == reads);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);

    {  // an inert system: a moved-from World registers nothing
        Vm vm = script_test::makeBuiltinVm();
        engine::World source;
        const engine::World taken{std::move(source)};
        capture.clear();
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- deliberate
        ScriptSystem inert(vm, source, [](const Guid& /*script*/) { return std::string{"res://s/m.js"}; });
        CHECK_FALSE(inert.bound());
        const std::size_t refusals = capture.countAtLeast(LogLevel::Warn);
        CHECK(scriptStatusLabel(inert.reloadScript(guidOf(1))) == "NotLoaded");
        CHECK(capture.countAtLeast(LogLevel::Warn) == refusals);
    }

    static_cast<void>(fx.system->stop());
    CHECK(fx.reload(guidOf(1)) == "NotLoaded");
    CHECK(fx.files->readCount() == reads);
    fx.finish();
}

TEST_CASE("system: a script that did not compile attaches after it is corrected and reloaded (LC21)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", "var = ;\n");
    const Entity e0 = fx.attachScript(guidOf(1));
    const Entity e1 = fx.attachScript(guidOf(1));
    const ScriptSystemReport first = fx.tick();
    CHECK(first.failedLoads == 1U);
    CHECK(label(fx.system->stateOf(e0)) == "LoadFailed");
    CHECK(label(fx.system->stateOf(e1)) == "LoadFailed");

    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    CHECK(fx.reload(guidOf(1)) == "Ok");
    capture.clear();
    const ScriptSystemReport second = fx.tick();
    CHECK(second.failedLoads == 0U);
    expectReport(second, Expected{.attached = 2, .detached = 0, .initialised = 2, .updated = 2, .running = 2});
    CHECK(fx.log() == "init:0|init:1|update:0:0.5|update:1:0.5");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE(
    "system: one reload clears an onInit failure, an onUpdate failure, an attach failure and a suspension (LC22)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    const std::string body =
        "  who = globalThis.serial++;\n  guard = (() => { if (globalThis.ctorFail) throw new Error('c'); })();\n"
        "  inits = 0;\n  updates = 0;\n"
        "  onInit() { this.inits += 1; if (globalThis.initFail && this.who === 0) throw new Error('i'); }\n"
        "  onUpdate() { this.updates += 1; if (globalThis.updFail && this.who === 1) throw new Error('u'); }";
    fx.run("globalThis.initFail = true; globalThis.updFail = true; globalThis.ctorFail = false;");
    fx.publish(guidOf(1), "x.js", classSource(body));
    const Entity x = fx.attachScript(guidOf(1));
    const Entity y = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    fx.run("globalThis.ctorFail = true;");
    const Entity z = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    CHECK(label(fx.system->stateOf(x)) == "Faulted");
    CHECK(label(fx.system->stateOf(y)) == "Faulted");
    CHECK(label(fx.system->stateOf(z)) == "AttachFailed");

    fx.run("globalThis.initFail = false; globalThis.updFail = false; globalThis.ctorFail = false;");
    fx.publish(guidOf(1), "x.js", classSource(body) + "// v2\n");
    CHECK(fx.reload(guidOf(1)) == "Ok");
    CHECK(label(fx.system->stateOf(x)) == "PendingInit");
    CHECK(label(fx.system->stateOf(y)) == "Running");
    capture.clear();
    const ScriptSystemReport next = fx.tick();
    CHECK(next.attached == 1U);
    CHECK(next.failedAttaches == 0U);
    CHECK(fx.instanceValue(x, "i.inits + ',' + i.updates") == "2,1");
    CHECK(fx.instanceValue(y, "i.inits + ',' + i.updates") == "1,2");
    CHECK(fx.instanceValue(z, "i.inits + ',' + i.updates") == "1,1");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);

    // A suspension clears too: a Running instance and one whose onInit was interrupted resume by their initDone.
    fx.run("globalThis.ctorA = 0; globalThis.spinOnInit = false;");
    const std::string spinning =
        "  c = globalThis.ctorA++;\n  inits = 0;\n"
        "  onInit() { this.inits += 1; if (globalThis.spinOnInit && this.c === 1) { for (;;) {} } }\n"
        "  onUpdate() { log.push('A:' + this.c); }";
    fx.publish(guidOf(2), "a.js", classSource(spinning));
    const Entity a0 = fx.attachScript(guidOf(2));
    static_cast<void>(fx.tick());
    fx.run("globalThis.spinOnInit = true;");
    const Entity a1 = fx.attachScript(guidOf(2));
    static_cast<void>(fx.tick());
    CHECK(label(fx.system->stateOf(a0)) == "Suspended");
    CHECK(label(fx.system->stateOf(a1)) == "Suspended");

    fx.run("globalThis.spinOnInit = false;");
    fx.publish(guidOf(2), "a.js", classSource(spinning) + "// v2\n");
    CHECK(fx.reload(guidOf(2)) == "Ok");
    CHECK(label(fx.system->stateOf(a0)) == "Running");
    CHECK(label(fx.system->stateOf(a1)) == "PendingInit");
    fx.clearLog();
    static_cast<void>(fx.tick());
    CHECK(fx.instanceValue(a0, "String(i.inits)") == "1");
    CHECK(fx.instanceValue(a1, "String(i.inits)") == "2");
    CHECK(label(fx.system->stateOf(a1)) == "Running");
    fx.finish();
}

TEST_CASE("system: Unchanged, CompileError and AlreadyRefused are returned and clear nothing (LC23)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js",
               classSource(hooks("", "", "if (this.n === 0 && this.count === 1) { throw new Error('bad'); }")));
    fx.publish(guidOf(2), "bad.js", "var = ;\n");
    const Entity faulted = fx.attachScript(guidOf(1));
    const Entity healthy = fx.attachScript(guidOf(1));
    const Entity broken = fx.attachScript(guidOf(2));
    static_cast<void>(fx.tick());
    REQUIRE(label(fx.system->stateOf(faulted)) == "Faulted");
    REQUIRE(label(fx.system->stateOf(broken)) == "LoadFailed");
    fx.clearLog();
    capture.clear();

    CHECK(fx.reload(guidOf(1)) == "Unchanged");
    CHECK(label(fx.system->stateOf(faulted)) == "Faulted");
    CHECK(fx.reload(guidOf(2)) == "AlreadyRefused");
    fx.publish(guidOf(2), "bad.js", "var = = ;\n");
    CHECK(fx.reload(guidOf(2)) == "CompileError");
    CHECK(fx.reload(guidOf(2)) == "AlreadyRefused");
    CHECK(label(fx.system->stateOf(broken)) == "LoadFailed");
    CHECK(label(fx.system->stateOf(faulted)) == "Faulted");

    const ScriptSystemReport next = fx.tick();
    CHECK(next.failedLoads == 1U);
    CHECK(next.faulted == 0U);
    CHECK(next.updated == 1U);
    CHECK(fx.log() == "update:1:0.5");
    CHECK(label(fx.system->stateOf(healthy)) == "Running");
    fx.finish();
}

TEST_CASE("system: a loadScript made around the system moves its instances and clears none of its failures (LC24)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js",
               classSource(hooks("v1:", "", "if (this.n === 0 && this.count === 1) { throw new Error('bad'); }")));
    const Entity faulted = fx.attachScript(guidOf(1));
    const Entity running = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    REQUIRE(label(fx.system->stateOf(faulted)) == "Faulted");
    fx.publish(guidOf(1), "m.js", classSource(hooks("v2:")));
    CHECK(fx.vm.loadScript("res://s/m.js").status == engine::script::ScriptStatus::Ok);
    fx.clearLog();
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "v2:update:1:0.5");
    CHECK(label(fx.system->stateOf(faulted)) == "Faulted");
    CHECK(label(fx.system->stateOf(running)) == "Running");
    fx.finish();
}

TEST_CASE("system: an unresolved Guid is resolved again after a reloadScript, with no VM call (LC25)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    const Entity e = fx.attachScript(guidOf(5));
    static_cast<void>(fx.tick());
    static_cast<void>(fx.tick());
    CHECK(fx.queriesFor(guidOf(5)) == 1U);
    CHECK(label(fx.system->stateOf(e)) == "Unresolved");

    const std::size_t reads = fx.files->readCount();
    capture.clear();
    CHECK(fx.reload(guidOf(5)) == "NotLoaded");
    CHECK(fx.files->readCount() == reads);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(fx.queriesFor(guidOf(5)) == 1U);

    fx.publish(guidOf(5), "m.js", classSource(hooks()));
    const ScriptSystemReport next = fx.tick();
    CHECK(fx.queriesFor(guidOf(5)) == 2U);
    CHECK(next.attached == 1U);
    CHECK(label(fx.system->stateOf(e)) == "Running");
    fx.finish();
}

TEST_CASE("system: reloadScript refuses a Guid the resolver now sends to another path (LC27)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "one.js", classSource(hooks("one:")));
    fx.publish(guidOf(2), "two.js", classSource(hooks("two:")));
    const Entity e = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    fx.paths[guidOf(1)] = "res://s/two.js";
    const std::size_t reads = fx.files->readCount();
    capture.clear();
    CHECK(fx.reload(guidOf(1)) == "LoadError");
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message ==
          "script: res://s/one.js: reloadScript refused: the resolver now names 'res://s/two.js' for this script; "
          "a new ScriptSystem resolves it again");
    CHECK(fx.files->readCount() == reads);
    fx.clearLog();
    static_cast<void>(fx.tick());
    CHECK(fx.log() == "one:update:0:0.5");
    CHECK(label(fx.system->stateOf(e)) == "Running");
    fx.finish();
}

TEST_CASE("system: ten thousand instances of a hook-less script are all detached by stop() (LC31)") {
    const script_test::LogCapture capture;
    constexpr std::size_t ENTITY_COUNT = 10000;
    constexpr std::size_t SPREAD_STEP = 997;
    SystemFixture fx;
    fx.publish(guidOf(1), "bare.js", classSource(""));
    std::vector<Entity> entities;
    entities.reserve(ENTITY_COUNT);
    for (std::size_t i = 0; i < ENTITY_COUNT; ++i) {
        entities.push_back(fx.attachScript(guidOf(1)));
    }

    const ScriptSystemReport updated = fx.tick();
    CHECK(updated.attached == ENTITY_COUNT);
    CHECK(updated.updated == ENTITY_COUNT);
    CHECK(updated.running == ENTITY_COUNT);

    std::vector<ScriptInstance> spread;
    for (std::size_t i = 0; i < ENTITY_COUNT; i += SPREAD_STEP) {
        const ScriptInstance handle = fx.system->instanceOf(entities[i]);
        REQUIRE(handle.valid());
        spread.push_back(handle);
    }
    REQUIRE(spread.size() > 1U);

    const auto start = std::chrono::steady_clock::now();
    const ScriptSystemReport stopped = fx.system->stop();
    const auto stop = std::chrono::steady_clock::now();
    MESSAGE("LC31: stop() over ", ENTITY_COUNT,
            " instances = ", std::chrono::duration<double, std::milli>(stop - start).count(), " ms");

    CHECK(stopped.detached == ENTITY_COUNT);
    CHECK_FALSE(fx.system->bound());
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);  // nothing failed before the probes below
    for (const ScriptInstance handle : spread) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    CHECK(capture.countAtLeast(LogLevel::Warn) == spread.size());  // one refusal ERROR per probe, nothing else
    fx.finish();
}

TEST_CASE("system: a system destroyed without stop() runs onDestroy once per initialised instance and unbinds (LC34)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    const Entity e0 = fx.attachScript(guidOf(1));
    const Entity e1 = fx.attachScript(guidOf(1));
    expectReport(fx.tick(), Expected{.attached = 2, .detached = 0, .initialised = 2, .updated = 2, .running = 2});
    const std::array<ScriptInstance, 2> held{fx.system->instanceOf(e0), fx.system->instanceOf(e1)};
    for (const ScriptInstance handle : held) {
        REQUIRE(handle.valid());
    }
    fx.clearLog();
    capture.clear();

    fx.system.reset();  // never stop()ped: the destructor is the only teardown
    CHECK(fx.log() == "destroy:0|destroy:1");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(fx.vm.defineEntityGlobal("probe", e0) == engine::script::BindStatus::NoWorld);
    for (const ScriptInstance handle : held) {
        CHECK(answersUnknown(fx.vm, handle));
    }
    CHECK(fx.world.has<ScriptComponent>(e0));
    CHECK(fx.world.has<ScriptComponent>(e1));
    fx.finish();
}

TEST_CASE("system: an entity whose ScriptComponent is nil is neither resolved nor counted unresolved (LC35)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", classSource(hooks()));
    const Entity nil = fx.attachScript(Guid{});
    const Entity named = fx.attachScript(guidOf(1));
    REQUIRE(nil.index < named.index);

    for (int i = 0; i < 3; ++i) {
        const ScriptSystemReport report = fx.tick();
        CHECK(report.unresolved == 0U);
        CHECK(report.attached == (i == 0 ? 1U : 0U));
        CHECK(report.running == 1U);
    }
    CHECK(fx.queriesFor(Guid{}) == 0U);
    CHECK(fx.queriesFor(guidOf(1)) == 1U);
    CHECK(label(fx.system->stateOf(nil)) == "None");
    CHECK_FALSE(fx.system->instanceOf(nil).valid());
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: an entity renamed to another script forgets the attach that failed under the old one (LC36)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "boom.js", classSource("  constructor() {\n    super();\n    throw new Error('boom');\n  }"));
    fx.publish(guidOf(2), "ok.js", classSource(hooks()));
    const Entity entity = fx.attachScript(guidOf(1));

    const ScriptSystemReport failed = fx.tick();
    CHECK(failed.failedAttaches == 1U);
    CHECK(label(fx.system->stateOf(entity)) == "AttachFailed");

    REQUIRE(fx.world.get<ScriptComponent>(entity) != nullptr);
    fx.world.get<ScriptComponent>(entity)->script = guidOf(2);
    capture.clear();
    const ScriptSystemReport renamed = fx.tick();
    CHECK(renamed.attached == 1U);
    CHECK(renamed.running == 1U);
    CHECK(renamed.failedAttaches == 0U);  // the failure belonged to the old name
    CHECK(label(fx.system->stateOf(entity)) == "Running");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(fx.log() == "init:0|update:0:0.5");
    fx.finish();
}

TEST_CASE("system: a script the VM already loaded attaches, its module is not evaluated again (LC37)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "m.js", "globalThis.evals += 1;\n" + classSource(hooks()));
    const engine::script::ScriptLoadResult loaded = fx.vm.loadScript("res://s/m.js");
    REQUIRE(loaded.status == engine::script::ScriptStatus::Ok);
    const Entity entity = fx.attachScript(guidOf(1));

    const ScriptSystemReport report = fx.tick();
    CHECK(report.failedLoads == 0U);
    expectReport(report, Expected{.attached = 1, .detached = 0, .initialised = 1, .updated = 1, .running = 1});
    CHECK(label(fx.system->stateOf(entity)) == "Running");
    CHECK(script_test::completion(fx.vm, "String(evals)") == "1");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

// A module whose body interrupts inside a promise executor leaves the rejection's reaction job queued (R34). The
// system's settle call runs it under the script's own path, so its record names a.js and B's load, the next VM call,
// finds nothing to run first.
TEST_CASE("system: a module load past its budget is settled under the script's path and spares the next load (LC38)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.publish(guidOf(1), "a.js",
               classSource("  onUpdate() {}") + "(async () => { await new Promise(() => { for (;;) {} }); })();\n");
    fx.publish(guidOf(2), "b.js", classSource(hooks("b:")));
    const Entity a = fx.attachScript(guidOf(1));
    const Entity b = fx.attachScript(guidOf(2));
    REQUIRE(a.index < b.index);  // A's load is the one that is interrupted, and B's the next VM call
    capture.clear();

    const ScriptSystemReport first = fx.tick();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.starts_with("script: res://s/a.js: interrupted after exceeding"));
    CHECK(loud[1].level == LogLevel::Error);
    CHECK(loud[1].message.starts_with("script: res://s/a.js: uncaught InternalError: interrupted"));
    CHECK(recordsNaming(capture, "res://s/b.js") == 0U);
    CHECK(first.failedLoads == 1U);
    CHECK(first.attached == 1U);
    CHECK(first.running == 1U);
    CHECK(label(fx.system->stateOf(a)) == "LoadFailed");
    CHECK(label(fx.system->stateOf(b)) == "Running");
    CHECK(fx.log() == "b:init:0|b:update:0:0.5");

    static_cast<void>(fx.tick());
    CHECK(capture.countAtLeast(LogLevel::Warn) == 2U);
    CHECK(fx.log() == "b:init:0|b:update:0:0.5|b:update:0:0.5");
    fx.finish();
}

// ---- LC40-LC48: the sixth phase, the flush of engine.destroy's queue (task 4.4.2) -----------------------------------

namespace {

// A script whose onUpdate runs `body`; it has no other hook.
[[nodiscard]] std::string updateSource(std::string_view body) {
    return classSource(std::format("  onUpdate() {{ {} }}", body));
}

// A script whose onDestroy logs its entity's name, liveness and parent's name; it has no other hook.
[[nodiscard]] std::string destroyLogSource() {
    return classSource(
        "  onDestroy() {\n"
        "    log.push('destroy:' + this.entity.name + ':' + this.entity.alive + ':' +\n"
        "             (this.entity.parent ? this.entity.parent.name : '-'));\n"
        "  }");
}

[[nodiscard]] Entity namedEntity(SystemFixture& fx, std::string_view name) {
    const Entity entity = fx.world.create();
    REQUIRE(fx.world.setName(entity, name));
    return entity;
}

void defineGlobal(SystemFixture& fx, std::string_view name, Entity entity) {
    REQUIRE(fx.vm.defineEntityGlobal(name, entity) == engine::script::BindStatus::Ok);
}

}  // namespace

TEST_CASE("system: engine.destroy leaves the entity alive for every script until the update ends (LC40)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "b.js", updateSource("engine.destroy(globalThis.target);"));
    fx.publish(guidOf(2), "c.js",
               updateSource("log.push([target.alive, target.name, engine.find(target.name) === target].join());"));
    const Entity target = namedEntity(fx, "victim");
    defineGlobal(fx, "target", target);
    const Entity b = fx.attachScript(guidOf(1));
    const Entity c = fx.attachScript(guidOf(2));
    REQUIRE(b.index < c.index);

    const ScriptSystemReport report = fx.tick();
    CHECK(fx.log() == "true,victim,true");
    CHECK(report.destroyed == 1U);
    CHECK_FALSE(fx.world.alive(target));
    CHECK(fx.world.alive(b));
    CHECK(fx.world.alive(c));
    CHECK(script_test::completion(fx.vm, "String(target.alive)") == "false");
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: a queued subtree's onDestroy hooks run in run order, every entity alive and parented (LC41)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "d.js", updateSource("engine.destroy(globalThis.R);"));
    fx.publish(guidOf(2), "n.js", destroyLogSource());
    const Entity killer = fx.attachScript(guidOf(1));
    const Entity root = namedEntity(fx, "R");
    const Entity k1 = namedEntity(fx, "K1");
    const Entity k2 = namedEntity(fx, "K2");
    const Entity grand = namedEntity(fx, "G");
    REQUIRE(fx.world.setParent(k1, root));
    REQUIRE(fx.world.setParent(k2, root));
    REQUIRE(fx.world.setParent(grand, k1));
    defineGlobal(fx, "R", root);
    for (const Entity entity : {root, k1, k2, grand}) {
        REQUIRE(fx.world.add<ScriptComponent>(entity, ScriptComponent{guidOf(2)}) != nullptr);
    }

    const ScriptSystemReport first = fx.tick();
    CHECK(first.attached == 5U);
    CHECK(first.detached == 4U);
    CHECK(first.destroyed == 1U);
    CHECK(fx.log() == "destroy:R:true:-|destroy:K1:true:R|destroy:K2:true:R|destroy:G:true:K1");
    for (const Entity entity : {root, k1, k2, grand}) {
        CHECK_FALSE(fx.world.alive(entity));
    }
    CHECK(fx.world.alive(killer));
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE(
    "system: the flush gives onDestroy to an instance whose onUpdate threw, not to one whose onInit threw (LC42)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "f.js",
               classSource("  n = globalThis.serial++;\n"
                           "  onInit() { if (this.n === 0) throw new Error('init'); }\n"
                           "  onUpdate() { if (this.n === 1) throw new Error('update'); }\n"
                           "  onDestroy() { log.push('destroy:' + this.n); }"));
    fx.publish(guidOf(2), "k.js", updateSource("if (globalThis.kill) { engine.destroy(a); engine.destroy(b); }"));
    const Entity a = fx.attachScript(guidOf(1));
    const Entity b = fx.attachScript(guidOf(1));
    const Entity killer = fx.attachScript(guidOf(2));
    defineGlobal(fx, "a", a);
    defineGlobal(fx, "b", b);

    const ScriptSystemReport first = fx.tick();
    CHECK(first.faulted == 2U);
    CHECK(first.destroyed == 0U);
    CHECK(label(fx.system->stateOf(a)) == "Faulted");
    CHECK(label(fx.system->stateOf(b)) == "Faulted");
    fx.run("globalThis.kill = true;");
    const ScriptSystemReport second = fx.tick();
    CHECK(second.detached == 2U);
    CHECK(second.destroyed == 2U);
    CHECK(fx.log() == "destroy:1");
    CHECK_FALSE(fx.world.alive(a));
    CHECK_FALSE(fx.world.alive(b));
    CHECK(fx.world.alive(killer));
    fx.finish();
}

TEST_CASE(
    "system: an onDestroy in the flush may queue another entity, and the flush takes it in the same update (LC43)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "k.js", updateSource("engine.destroy(globalThis.X);"));
    fx.publish(guidOf(2), "x.js",
               classSource("  onDestroy() { log.push('X:' + engine.destroy(globalThis.Y) + ',' +\n"
                           "                         engine.destroy(this.entity)); }"));
    fx.publish(guidOf(3), "y.js", classSource("  onDestroy() { log.push('Ydestroy'); }"));
    const Entity killer = fx.attachScript(guidOf(1));
    const Entity x = fx.attachScript(guidOf(2));
    const Entity y = fx.attachScript(guidOf(3));
    defineGlobal(fx, "X", x);
    defineGlobal(fx, "Y", y);

    const ScriptSystemReport report = fx.tick();
    CHECK(fx.log() == "X:true,false|Ydestroy");
    CHECK(report.destroyed == 2U);
    CHECK(report.detached == 2U);
    CHECK_FALSE(fx.world.alive(x));
    CHECK_FALSE(fx.world.alive(y));
    CHECK(fx.world.alive(killer));
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: an entity spawned and destroyed in one call leaves no entry for the next update to release (LC44)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "s.js", updateSource("globalThis.made = engine.spawn(); engine.destroy(made);"));
    const Entity spawner = fx.attachScript(guidOf(1));
    REQUIRE(fx.world.entityCount() == 1U);

    const ScriptSystemReport first = fx.tick();
    CHECK(first.destroyed == 1U);
    CHECK(first.detached == 0U);
    CHECK(fx.world.entityCount() == 1U);
    CHECK(fx.world.alive(spawner));
    CHECK(script_test::completion(fx.vm, "String(made.alive)") == "false");
    const ScriptSystemReport second = fx.tick();
    CHECK(second.attached == 0U);
    CHECK(second.detached == 0U);
    CHECK(second.destroyed == 1U);
    CHECK(fx.world.entityCount() == 1U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: an onDestroy that moves entities in or out changes the subtree the flush destroys (LC45)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "d.js",
               updateSource("if (!globalThis.done) { globalThis.done = true; engine.destroy(globalThis.R); }"));
    fx.publish(guidOf(2), "r.js",
               classSource("  onDestroy() { log.push('Rdestroy'); X.parent = R; K.parent = null; }"));
    fx.publish(guidOf(3), "k.js",
               classSource("  onUpdate() { log.push('Kupdate'); }\n  onDestroy() { log.push('Kdestroy'); }"));
    fx.publish(guidOf(4), "x.js", classSource("  onDestroy() { log.push('Xdestroy'); }"));
    const Entity killer = fx.attachScript(guidOf(1));
    const Entity r = fx.attachScript(guidOf(2));
    const Entity k = fx.attachScript(guidOf(3));
    const Entity x = fx.attachScript(guidOf(4));
    REQUIRE(fx.world.setParent(k, r));
    defineGlobal(fx, "R", r);
    defineGlobal(fx, "K", k);
    defineGlobal(fx, "X", x);

    const ScriptSystemReport first = fx.tick();
    CHECK(fx.log() == "Kupdate|Rdestroy|Xdestroy");
    CHECK(first.detached == 2U);
    CHECK(first.destroyed == 1U);
    CHECK_FALSE(fx.world.alive(r));
    CHECK_FALSE(fx.world.alive(x));
    REQUIRE(fx.world.alive(k));
    CHECK_FALSE(fx.world.parent(k).valid());
    CHECK(label(fx.system->stateOf(k)) == "Running");
    CHECK(fx.system->instanceOf(k).valid());
    CHECK(fx.world.alive(killer));

    fx.clearLog();
    const ScriptSystemReport second = fx.tick();
    CHECK(fx.log() == "Kupdate");
    CHECK(second.updated == 2U);
    CHECK(second.detached == 0U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: stop() runs every onDestroy, then destroys what is still queued, then unbinds (LC46)") {
    const script_test::LogCapture capture;
    SystemFixture fx;
    fx.publish(guidOf(1), "h.js", classSource("  onDestroy() { log.push('destroy:' + target.alive); }"));
    const Entity target = namedEntity(fx, "victim");
    defineGlobal(fx, "target", target);
    const Entity holder = fx.attachScript(guidOf(1));
    static_cast<void>(fx.tick());
    fx.run("engine.destroy(globalThis.target);");
    CHECK(fx.world.alive(target));

    const ScriptSystemReport stopped = fx.system->stop();
    CHECK(stopped.detached == 1U);
    CHECK(stopped.destroyed == 1U);
    CHECK(fx.log() == "destroy:true");
    CHECK_FALSE(fx.world.alive(target));
    CHECK(fx.world.alive(holder));
    CHECK(fx.vm.takeDestroyQueue().empty());
    CHECK(script_test::completion(
              fx.vm, "try { engine.destroy(globalThis.target); 'none'; } catch (x) { x.name + ': ' + x.message; }") ==
          "TypeError: engine.destroy: no World is bound");
    fx.finish();
}

TEST_CASE(
    "system: an onDestroy past its budget in the flush leaves the rest of the subtree destroyed unhooked (LC47)") {
    script_test::LogCapture capture;
    SystemFixture fx{briefBudget()};
    fx.run("globalThis.ctorP = 0; globalThis.spinP = 0;");
    fx.publish(guidOf(1), "d.js", updateSource("engine.destroy(globalThis.R);"));
    fx.publish(guidOf(2), "p.js", destroySpinSource("P"));
    const Entity killer = fx.attachScript(guidOf(1));
    const Entity root = fx.attachScript(guidOf(2));
    const Entity k1 = fx.attachScript(guidOf(2));
    const Entity k2 = fx.attachScript(guidOf(2));
    REQUIRE(fx.world.setParent(k1, root));
    REQUIRE(fx.world.setParent(k2, root));
    defineGlobal(fx, "R", root);
    capture.clear();

    const ScriptSystemReport report = fx.tick();
    const std::vector<script_test::CapturedRecord> loud = capture.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 2U);
    CHECK(loud[0].level == LogLevel::Error);
    CHECK(loud[0].message.find("interrupted after exceeding") != std::string::npos);
    CHECK(loud[1].level == LogLevel::Warn);
    CHECK(loud[1].message ==
          "script: res://s/p.js: suspended: a hook ran past its 100 ms time budget; its 3 instances run no hook "
          "until the script reloads");
    CHECK(report.detached == 3U);
    CHECK(report.destroyed == 1U);
    CHECK(report.suspendedScripts == 1U);
    CHECK(fx.log() == "Pupdate:0|Pupdate:1|Pupdate:2|Pdestroy:0");
    CHECK_FALSE(fx.world.alive(root));
    CHECK_FALSE(fx.world.alive(k1));
    CHECK_FALSE(fx.world.alive(k2));
    CHECK(fx.world.alive(killer));
    fx.finish();
}

TEST_CASE("system: spawning and destroying a thousand entities per update leaves the heap and the World flat (LC48)") {
    const script_test::LogCapture capture;
    constexpr int PER_UPDATE = 1000;
    constexpr int UPDATE_COUNT = 50;
    constexpr int SETTLED = 3;  // the first update that has a predecessor to destroy, and the allocator's warm-up
    SystemFixture fx;
    fx.publish(guidOf(1), "churn.js",
               classSource("  prev = [];\n"
                           "  onUpdate() {\n"
                           "    for (const old of this.prev) engine.destroy(old);\n"
                           "    const next = [];\n"
                           "    for (let i = 0; i < 1000; ++i) {\n"
                           "      const e = engine.spawn();\n"
                           "      e.name = 'n' + i;\n"
                           "      next.push(e);\n"
                           "    }\n"
                           "    this.prev = next;\n"
                           "  }"));
    static_cast<void>(fx.attachScript(guidOf(1)));

    std::vector<double> millis;
    millis.reserve(UPDATE_COUNT);
    std::size_t settledHeap = 0;
    std::size_t settledAllocations = 0;
    std::size_t settledEntities = 0;
    std::size_t heapDrift = 0;
    std::size_t allocationDrift = 0;
    std::size_t entityDrift = 0;
    std::size_t destroyedTotal = 0;
    for (int update = 1; update <= UPDATE_COUNT; ++update) {
        const auto start = std::chrono::steady_clock::now();
        const ScriptSystemReport report = fx.tick();
        const auto stop = std::chrono::steady_clock::now();
        millis.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        destroyedTotal += report.destroyed;
        const engine::script::VmStats stats = fx.vm.stats();
        if (update == SETTLED) {
            settledHeap = stats.heapBytesInUse;
            settledAllocations = stats.liveAllocations;
            settledEntities = fx.world.entityCount();
        } else if (update > SETTLED) {
            heapDrift += stats.heapBytesInUse == settledHeap ? 0U : 1U;
            allocationDrift += stats.liveAllocations == settledAllocations ? 0U : 1U;
            entityDrift += fx.world.entityCount() == settledEntities ? 0U : 1U;
        }
    }
    std::sort(millis.begin(), millis.end());
    MESSAGE("LC48: update spawning and destroying ", PER_UPDATE, " entities, median = ", millis[millis.size() / 2],
            " ms, p95 = ", millis[(millis.size() * 95U) / 100U], " ms");

    CHECK(settledHeap > 0U);  // anti-vacuity: the figures are measurements
    CHECK(settledAllocations > 0U);
    CHECK(settledEntities == static_cast<std::size_t>(PER_UPDATE) + 1U);
    CHECK(destroyedTotal == static_cast<std::size_t>(PER_UPDATE) * (UPDATE_COUNT - 1));
    CHECK(heapDrift == 0U);
    CHECK(allocationDrift == 0U);
    CHECK(entityDrift == 0U);
    CHECK(capture.countAtLeast(LogLevel::Warn) == 0U);
    fx.finish();
}

TEST_CASE("system: entity.script is answered from the VM's attaches, not from the system's states (LC49)") {
    script_test::LogCapture capture;
    SystemFixture fx;
    // `a` reads the script of an entity attached in the same update, before that entity's own onInit has run.
    fx.publish(guidOf(1), "a.js",
               classSource("  onInit() { log.push('a:' + (other.script !== null) + ':' + other.script.n); }"));
    fx.publish(guidOf(2), "b.js",
               classSource("  n = 7;\n  onInit() { log.push('b:init'); }\n"
                           "  onUpdate() { throw new Error('boom'); }"));
    const Entity first = fx.attachScript(guidOf(1));
    const Entity other = fx.attachScript(guidOf(2));
    REQUIRE(first.index < other.index);
    defineGlobal(fx, "other", other);

    static_cast<void>(fx.tick());
    CHECK(fx.log() == "a:true:7|b:init");
    CHECK(recordsNaming(capture, "boom") >= 1U);  // b's onUpdate threw: its instance is faulted, not detached
    CHECK(label(fx.system->stateOf(other)) == "Faulted");
    CHECK(script_test::completion(fx.vm, "String(other.script.n)") == "7");
    CHECK(script_test::completion(fx.vm, "String(other.script.entity === other)") == "true");

    fx.clearLog();
    static_cast<void>(fx.tick());
    CHECK(script_test::completion(fx.vm, "String(other.script !== null)") == "true");

    REQUIRE(fx.world.remove<ScriptComponent>(other));
    const ScriptSystemReport report = fx.tick();
    CHECK(report.detached >= 1U);
    CHECK(script_test::completion(fx.vm, "String(other.script)") == "null");
    fx.finish();
}
