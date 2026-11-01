#pragma once
// Aero Engine -- the shared helpers of the binding tests (task 4.2.1): script_binding_test.cpp and, from the
// later steps, script_component_test.cpp. Everything is inline in namespace script_test, so including this header
// from two translation units defines nothing twice.
//
// The fixture component ScriptProbe holds one field of every bindable kind, so a case exercises every FieldKind
// without reflect-gen; probeTable() binds it with the same templates the generated code calls.

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/component.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace script_test {

struct CapturedRecord {
    engine::LogLevel level = engine::LogLevel::Info;
    std::string message;
};

// Every record, from any thread, copied under a lock.
class LogCapture {
public:
    LogCapture() {
        engine::setLogCallback([this](const engine::LogRecord& record) {
            const std::lock_guard<std::mutex> lock{mutex};
            records.push_back(CapturedRecord{record.level, std::string{record.message}});
        });
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;
    ~LogCapture() { engine::setLogCallback(nullptr); }

    [[nodiscard]] std::vector<CapturedRecord> atLeast(engine::LogLevel floor) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::vector<CapturedRecord> out;
        for (const CapturedRecord& record : records) {
            if (record.level >= floor) {
                out.push_back(record);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t countAtLeast(engine::LogLevel floor) const { return atLeast(floor).size(); }

    void clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        records.clear();
    }

private:
    mutable std::mutex mutex;
    std::vector<CapturedRecord> records;
};

// For a case bounded by memory or stack depth: it must never be cut short by the clock.
[[nodiscard]] inline engine::script::VmConfig patient(engine::script::VmConfig config = {}) {
    config.callTimeBudget = engine::script::MAX_CALL_TIME_BUDGET;
    return config;
}

// A VM whose `engine` namespace binds `table` instead of the built-ins (VmAccess: the runtime's test seam).
[[nodiscard]] inline engine::script::Vm makeProbeVm(const engine::script::internal::ComponentBindingTable& table,
                                                    const engine::script::VmConfig& config = patient()) {
    engine::script::VmCreateResult created = engine::script::internal::VmAccess::create(config, table);
    REQUIRE(created.status == engine::script::VmCreateStatus::Ok);
    REQUIRE(created.vm.valid());
    return std::move(created.vm);
}

// A VM over the built-in table (Vm::create), the shape a game gets.
[[nodiscard]] inline engine::script::Vm makeBuiltinVm(const engine::script::VmConfig& config = patient()) {
    engine::script::VmCreateResult created = engine::script::Vm::create(config);
    REQUIRE(created.status == engine::script::VmCreateStatus::Ok);
    REQUIRE(created.vm.valid());
    return std::move(created.vm);
}

// The tail of EVERY VM case: a leak check and the teardown-write check (R23).
inline void expectCleanShutdown(engine::script::Vm& vm) {
    const engine::script::VmTeardownReport report = vm.shutdown();
    CHECK(report.leakedAllocations == 0U);
    CHECK(report.leakedBytes == 0U);
    CHECK(report.writtenAfterRelease == 0U);
    CHECK(report.repeatedReleases == 0U);
    CHECK_FALSE(vm.valid());
}

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
[[nodiscard]] inline std::string classSource(std::string_view members) {
    return std::format("var Mover = class extends engine.Script {{\n{}\n}};\nexport {{\n  Mover as default\n}};\n",
                       members);
}

// One field of every bindable kind, with non-default values.
struct ScriptProbe {
    float f = 0.5F;
    bool b = true;
    std::uint32_t u = 7;
    engine::Vec3 v{1.0F, 2.0F, 3.0F};
    engine::Quat q{0.0F, 0.0F, 0.0F, 1.0F};
    engine::Guid g{};
    std::string s{"probe"};

    bool operator==(const ScriptProbe&) const = default;
};

// A component with no field.
struct ScriptTag {};

[[nodiscard]] inline engine::script::internal::ComponentBindingTable probeTable() {
    using engine::script::internal::bindComponent;
    using engine::script::internal::bindField;
    engine::script::internal::ComponentBindingTable table;
    table.add(bindComponent<ScriptProbe>("ScriptProbe", bindField<&ScriptProbe::f>("f"),
                                         bindField<&ScriptProbe::b>("b"), bindField<&ScriptProbe::u>("u"),
                                         bindField<&ScriptProbe::v>("v"), bindField<&ScriptProbe::q>("q"),
                                         bindField<&ScriptProbe::g>("g"), bindField<&ScriptProbe::s>("s")));
    table.add(bindComponent<ScriptTag>("ScriptTag"));
    return table;
}

// Register the two fixture components in a World (a constructed World already holds the built-ins).
inline void registerProbe(engine::World& world) {
    engine::scene::internal::registerComponent<ScriptProbe>(world, "test::ScriptProbe");
    engine::scene::internal::registerComponent<ScriptTag>(world, "test::ScriptTag");
}

// Runs `source`, requires the call to complete, and returns String(completion value).
// Not [[nodiscard]]: a case often runs a script for its effect on the World.
inline std::string completion(engine::script::Vm& vm, std::string_view source) {
    const engine::script::EvalResult result = vm.evaluateScript(source, "bn.js");
    INFO("script: " << source);
    INFO("error: " << result.error.name << ": " << result.error.message);
    REQUIRE(result.status == engine::script::EvalStatus::Ok);
    return result.completion;
}

[[nodiscard]] inline engine::World makeProbeWorld() {
    engine::World world;
    registerProbe(world);
    return world;
}

// A live entity holding a default ScriptProbe.
[[nodiscard]] inline engine::Entity makeProbeEntity(engine::World& world) {
    const engine::Entity entity = world.create();
    REQUIRE(world.add<ScriptProbe>(entity) != nullptr);
    return entity;
}

// The usual shape of a binding case: a probe table, a World that registers the fixture components, one entity with a
// default ScriptProbe, and a VM bound to the World with that entity defined as the global `e`. Members are destroyed
// in reverse, so the VM goes before the World it borrows and the table it borrows.
struct ProbeScene {
    engine::script::internal::ComponentBindingTable table = probeTable();
    engine::World world = makeProbeWorld();
    engine::Entity entity = makeProbeEntity(world);
    engine::script::Vm vm = makeBoundVm(table, world, entity);

    ProbeScene() = default;
    ProbeScene(const ProbeScene&) = delete;
    ProbeScene& operator=(const ProbeScene&) = delete;
    ProbeScene(ProbeScene&&) = delete;
    ProbeScene& operator=(ProbeScene&&) = delete;
    ~ProbeScene() = default;

    [[nodiscard]] ScriptProbe& probe() {
        auto* const held = world.get<ScriptProbe>(entity);
        REQUIRE(held != nullptr);
        return *held;
    }

    static engine::script::Vm makeBoundVm(const engine::script::internal::ComponentBindingTable& bindings,
                                          engine::World& boundWorld, engine::Entity boundEntity) {
        engine::script::Vm bound = makeProbeVm(bindings);
        REQUIRE(bound.bindWorld(boundWorld) == engine::script::BindStatus::Ok);
        REQUIRE(bound.defineEntityGlobal("e", boundEntity) == engine::script::BindStatus::Ok);
        return bound;
    }
};

}  // namespace script_test
