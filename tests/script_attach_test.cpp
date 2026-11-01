// tests/script_attach_test.cpp -- task 4.4.1: engine.Script as the base of a script's class, and the instances the VM
// attaches to entities. VM66 is the base class through the VM's own load and reload; the later cases drive
// createInstance(path, entity) and callHook. A TU of aero_tests, which supplies main() from test_main.cpp.
#include <aero/core/vfs.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>

namespace {

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
