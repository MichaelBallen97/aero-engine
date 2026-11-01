// tests/script_system_test.cpp -- task 4.4.1: engine::script::ScriptSystem over one World (LC1-LC14). The system is
// written against the Vm's public API, so every case drives it through a VM over the built-in table, a World, and a
// resolver from a script Guid to a res:// path. Scripts observe through `globalThis.log`, instance fields and
// `this.entity.alive`, so none of these cases needs a bound component and all of them run with the reflect tools OFF.
// A TU of aero_tests, which supplies main() from test_main.cpp.
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

#include <array>
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
using engine::script::internal::VmAccess;

// A module source in memory; it counts its reads, so a case can tell that nothing re-read a file.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    void set(std::string relPath, std::string text) { files[std::move(relPath)] = std::move(text); }

    [[nodiscard]] std::size_t readCount() const noexcept { return reads; }

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
        ++reads;
        engine::ByteBuffer bytes;
        bytes.reserve(found->second.size());
        for (const char c : found->second) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }

private:
    std::map<std::string, std::string, std::less<>> files;
    mutable std::size_t reads = 0;
};

// A script in esbuild 0.28.2's measured shape.
[[nodiscard]] std::string classSource(std::string_view members) {
    return std::format("var Mover = class extends engine.Script {{\n{}\n}};\nexport {{\n  Mover as default\n}};\n",
                       members);
}

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
    SystemFixture() {
        auto backend = std::make_unique<MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(script_test::patient(), vfs);
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
