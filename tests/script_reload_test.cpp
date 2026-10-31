// tests/script_reload_test.cpp -- task 4.3.3: script classes, instances and hot reload. HR1-HR3 are tier 0 through
// aero::script_internal (the carry rule and a module's state accessor); the later cases drive the public API and
// internal::VmAccess::defineInstanceGlobal. A TU of aero_tests, which supplies main() from test_main.cpp.
#include <aero/core/vfs.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/internal/script_carry.hpp>
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
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>
#include <vector>

namespace {

using engine::script::evalStatusLabel;
using engine::script::ModuleRecord;
using engine::script::ModuleState;
using engine::script::moduleStateLabel;
using engine::script::internal::carriesValue;
using engine::script::internal::VALUE_KIND_COUNT;
using engine::script::internal::ValueKind;
using engine::script::internal::valueKindLabel;

// One record of a table, read back off records() -- never off the table's own lookups.
[[nodiscard]] std::optional<ModuleRecord> recordIn(const engine::script::internal::ModuleTable& table,
                                                   std::string_view path) {
    for (ModuleRecord& record : table.records()) {
        if (record.path == path) {
            return std::move(record);
        }
    }
    return std::nullopt;
}

}  // namespace

// ---- HR1-HR3: the pure half, tier 0 ----------------------------------------------------------------------------

TEST_CASE("script: the carry rule matches the written-out table for all 169 kind pairs (HR1)") {
    // Rows: the running value's kind. Columns: the new default's kind. Both in enumerator order:
    // undefined, null, boolean, number, bigint, string, symbol, array, Vec3, Quat, Entity, object, function.
    // Typed by hand from the rule, not computed: a function never carries; an undefined default takes every other
    // kind; a null default takes null and the reference kinds; every other column takes its own kind only.
    constexpr std::array<std::string_view, 13> EXPECTED{
        "1000000000000",  // undefined
        "1100000000000",  // null
        "1010000000000",  // boolean
        "1001000000000",  // number
        "1000100000000",  // bigint
        "1000010000000",  // string
        "1000001000000",  // symbol
        "1100000100000",  // array
        "1100000010000",  // engine.Vec3
        "1100000001000",  // engine.Quat
        "1100000000100",  // engine.Entity
        "1100000000010",  // object
        "0000000000000",  // function
    };
    REQUIRE(VALUE_KIND_COUNT == EXPECTED.size());
    std::size_t wrong = 0;
    for (std::size_t running = 0; running < VALUE_KIND_COUNT; ++running) {
        REQUIRE(EXPECTED[running].size() == VALUE_KIND_COUNT);
        for (std::size_t fresh = 0; fresh < VALUE_KIND_COUNT; ++fresh) {
            const auto runningKind = static_cast<ValueKind>(running);
            const auto freshKind = static_cast<ValueKind>(fresh);
            const bool expected = EXPECTED[running][fresh] == '1';
            const bool got = carriesValue(runningKind, freshKind);
            if (got != expected) {
                ++wrong;
                INFO("running ", valueKindLabel(runningKind), " new default ", valueKindLabel(freshKind), ": expected ",
                     expected, " got ", got);
                CHECK(got == expected);
            }
        }
    }
    CHECK(wrong == 0U);
}

TEST_CASE("script: the value kinds are numbered 0-12 and labelled apart (HR2)") {
    CHECK(VALUE_KIND_COUNT == 13U);
    const std::array<std::pair<ValueKind, std::uint8_t>, 13> numbered{{
        {ValueKind::Undefined, 0},
        {ValueKind::Null, 1},
        {ValueKind::Boolean, 2},
        {ValueKind::Number, 3},
        {ValueKind::BigInt, 4},
        {ValueKind::String, 5},
        {ValueKind::Symbol, 6},
        {ValueKind::Array, 7},
        {ValueKind::Vec3, 8},
        {ValueKind::Quat, 9},
        {ValueKind::Entity, 10},
        {ValueKind::Object, 11},
        {ValueKind::Function, 12},
    }};
    std::vector<std::string> labels;
    for (const auto& [kind, index] : numbered) {
        CHECK(static_cast<std::uint8_t>(kind) == index);
        const std::string_view label = valueKindLabel(kind);
        CHECK_FALSE(label.empty());
        CHECK(label != "unknown");
        for (const std::string& seen : labels) {
            CHECK(seen != label);
        }
        labels.emplace_back(label);
    }
    CHECK(labels.size() == 13U);
    CHECK(labels[8] == "engine.Vec3");
    CHECK(labels[9] == "engine.Quat");
    CHECK(labels[10] == "engine.Entity");
}

TEST_CASE("script: a module's state follows its record through a failed and a redone load (HR3)") {
    using engine::script::internal::ModuleTable;
    constexpr std::string_view KEY = "res://hot.js";
    constexpr std::string_view OTHER = "res://other.js";
    constexpr std::string_view NEVER = "res://never.js";
    int token = 0;
    ModuleTable table;

    std::vector<ModuleState> seen;
    const auto observe = [&] {
        seen.push_back(table.state(KEY));
        const std::optional<ModuleRecord> record = recordIn(table, KEY);
        // The accessor and the record list agree; a key with no record reads Unloaded.
        CHECK(table.state(KEY) == (record ? record->state : ModuleState::Unloaded));
        CHECK(table.state(OTHER) == ModuleState::Unloaded);
        CHECK(table.state(NEVER) == ModuleState::Unloaded);
    };

    table.beginCompile(OTHER);  // a second record, Unloaded throughout
    observe();                  // unknown key
    table.beginCompile(KEY);
    observe();
    table.markLoaded(KEY, &token);
    observe();
    table.markEvaluating(KEY);
    observe();
    table.markFailed(KEY);
    observe();
    static_cast<void>(table.invalidate(KEY));
    observe();
    table.beginCompile(KEY);
    observe();
    table.markLoaded(KEY, &token);
    observe();
    table.markEvaluatedGraph(KEY);
    observe();
    table.retire({std::string{KEY}});
    observe();

    const std::vector<ModuleState> expected{
        ModuleState::Unloaded,  ModuleState::Unloaded, ModuleState::Loaded,   ModuleState::Evaluating,
        ModuleState::Failed,    ModuleState::Unloaded, ModuleState::Unloaded, ModuleState::Loaded,
        ModuleState::Evaluated, ModuleState::Unloaded,
    };
    REQUIRE(seen.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("step ", i, ": expected ", moduleStateLabel(expected[i]), " got ", moduleStateLabel(seen[i]));
        CHECK(seen[i] == expected[i]);
    }
}

// ---- HR4-HR17: script classes and their instances through the public API ---------------------------------------

namespace {

using engine::LogLevel;
using engine::script::ScriptInstance;
using engine::script::ScriptInstanceResult;
using engine::script::ScriptLoadResult;
using engine::script::ScriptStatus;
using engine::script::scriptStatusLabel;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::internal::VmAccess;

// A module source in memory, with per-path read counts and a one-shot replacement armed after a read.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    void set(std::string relPath, std::string text) { files[std::move(relPath)] = std::move(text); }

    // The file is gone: exists, fileSize and readFile all answer as for a path never written.
    void remove(std::string_view relPath) {
        if (const auto found = files.find(relPath); found != files.end()) {
            files.erase(found);
        }
    }

    // After the next readFile of `relPath` returns, the file holds `text`.
    void replaceAfterNextRead(std::string relPath, std::string text) {
        replacements[std::move(relPath)] = std::move(text);
    }

    [[nodiscard]] bool exists(std::string_view relPath) const override { return files.contains(relPath); }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(found->second.size());
    }

    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        ++readCounts[std::string{relPath}];
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        engine::ByteBuffer bytes;
        bytes.reserve(found->second.size());
        for (const char c : found->second) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        if (const auto armed = replacements.find(relPath); armed != replacements.end()) {
            found->second = armed->second;
            replacements.erase(armed);
        }
        return bytes;
    }

    [[nodiscard]] std::size_t reads(std::string_view relPath) const {
        const auto found = readCounts.find(relPath);
        return found == readCounts.end() ? 0U : found->second;
    }

private:
    mutable std::map<std::string, std::string, std::less<>> files;
    mutable std::map<std::string, std::string, std::less<>> replacements;
    mutable std::map<std::string, std::size_t, std::less<>> readCounts;
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

// A script in esbuild 0.28.2's measured shape: `var <name> = class {...}; export { <name> as default };`.
[[nodiscard]] std::string classSource(std::string_view members, std::string_view name = "Mover") {
    return std::format("var {0} = class {{\n{1}\n}};\nexport {{\n  {0} as default\n}};\n", name, members);
}

// A class whose hello() answers `version` and whose `tag` field starts at "x".
[[nodiscard]] std::string versioned(std::string_view version) {
    return classSource(std::format("  tag = \"x\";\n  hello() {{\n    return \"{}\";\n  }}", version));
}

// A module's record, read back off moduleRecords().
[[nodiscard]] ModuleRecord recordOf(const Vm& vm, std::string_view path) {
    for (ModuleRecord& record : vm.moduleRecords()) {
        if (record.path == path) {
            return std::move(record);
        }
    }
    FAIL("no module record for " << path);
    return {};
}

// `instance` becomes globalThis[name]; the case defines the name again after a reload moved the object.
void expose(Vm& vm, ScriptInstance instance, std::string_view name) {
    REQUIRE(VmAccess::defineInstanceGlobal(vm, instance, name));
}

// The one record at WARN or above that a refusal logs, at ERROR.
[[nodiscard]] std::string theOneError(const script_test::LogCapture& log) {
    const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Error);
    return loud[0].message;
}

[[nodiscard]] ScriptInstance instanceOf(Vm& vm, std::string_view path) {
    const ScriptInstanceResult made = vm.createInstance(path);
    REQUIRE(scriptStatusLabel(made.status) == "Ok");
    REQUIRE(made.instance.valid());
    return made.instance;
}

}  // namespace

TEST_CASE("script: a class loads in esbuild's shape and as a native default export and makes instances (HR4)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", classSource("  speed = 2;\n  hello() {\n    return \"hi\";\n  }"));
    fx.files->set("s/n.js", "export default class {\n  speed = 2;\n  hello() {\n    return \"native\";\n  }\n}\n");
    for (const std::string_view key : {"res://s/a.js", "res://s/n.js"}) {
        const ScriptLoadResult loaded = fx.vm.loadScript(key);
        INFO("path ", key, ": ", loaded.error.message);
        CHECK(scriptStatusLabel(loaded.status) == "Ok");
        CHECK(loaded.instancesMoved == 0U);
        const engine::script::VmStats stats = fx.vm.stats();
        CHECK(loaded.heapBytesInUse == stats.heapBytesInUse);
        CHECK(loaded.heapLimitBytes == stats.heapLimitBytes);
        const ModuleRecord record = recordOf(fx.vm, key);
        CHECK(moduleStateLabel(record.state) == "Evaluated");
        CHECK(record.generation == 1U);
    }
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/a.js");
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a != b);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "a.speed = 7; String(b.speed)") == "2");
    CHECK(script_test::completion(fx.vm, "String(a.speed)") == "7");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "hi");
    const ScriptInstance n = instanceOf(fx.vm, "res://s/n.js");
    expose(fx.vm, n, "n");
    CHECK(script_test::completion(fx.vm, "n.hello()") == "native");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a default export that is not a class is NotAClass and no script code runs to find out (HR5)") {
    script_test::LogCapture log;
    Fixture fx;
    struct Form {
        std::string_view name;
        std::string source;
        std::string_view what;
    };
    const std::string klass = "var K = class {};\n";
    const std::vector<Form> forms{
        {"plain", "export default function update(dt) {}\n", "a function"},
        {"bound", klass + "var B = K.bind(null);\nexport { B as default };\n", "a function"},
        {"arrow", "var f = (dt) => dt;\nexport { f as default };\n", "a function"},
        {"async", "async function f() {}\nexport { f as default };\n", "a function"},
        {"generator", "function* g() {}\nexport { g as default };\n", "a function"},
        {"literal", "var o = { speed: 1 };\nexport { o as default };\n", "an object"},
        {"instance", klass + "var i = new K();\nexport { i as default };\n", "an object"},
        {"number", "export default 42;\n", "a number"},
        {"missing", "export const x = 1;\n", "undefined"},
        {"vec3", "var v = new engine.Vec3();\nexport { v as default };\n", "an engine.Vec3"},
        {"proxy",
         "globalThis.trapped = 0;\n" + klass +
             "var P = new Proxy(K, {\n  get(t, k, r) { globalThis.trapped++; return Reflect.get(t, k, r); },\n"
             "  getOwnPropertyDescriptor(t, k) { globalThis.trapped++; return Reflect.getOwnPropertyDescriptor(t, k); "
             "}\n"
             "});\nexport { P as default };\n",
         "a function"},
    };
    REQUIRE(forms.size() == 11U);
    for (const Form& form : forms) {
        const std::string path = std::format("s/{}.js", form.name);
        const std::string key = "res://" + path;
        fx.files->set(path, form.source);
        log.clear();
        const ScriptLoadResult loaded = fx.vm.loadScript(key);
        INFO("form ", form.name);
        CHECK(scriptStatusLabel(loaded.status) == "NotAClass");
        const std::string text = std::format("not a script: its default export is {}, not a class", form.what);
        CHECK(loaded.error.message == text);
        CHECK(theOneError(log) == std::format("script: {}: {}", key, text));
        log.clear();
        const ScriptInstanceResult made = fx.vm.createInstance(key);
        CHECK(scriptStatusLabel(made.status) == "NotLoaded");
        CHECK_FALSE(made.instance.valid());
    }
    CHECK(script_test::completion(fx.vm, "String(globalThis.trapped)") == "0");
    fx.files->set("s/ok.js", versioned("ok"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/ok.js").status) == "Ok");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module that threw after its class line is refused and holds no class (HR6)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/t.js";
    fx.files->set(
        "s/t.js",
        "var A = class {\n  v = 1;\n};\nthrow new Error(\"after the class\");\nexport {\n  A as default\n};\n");
    const ScriptLoadResult loaded = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(loaded.status) == "Exception");
    CHECK(loaded.error.message == "after the class");
    CHECK(theOneError(log).starts_with("script: res://s/t.js: uncaught Error: after the class"));
    CHECK(moduleStateLabel(recordOf(fx.vm, KEY).state) == "Failed");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance(KEY).status) == "NotLoaded");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module whose top-level await has not settled is StillEvaluating, not a class (HR7)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/p.js";
    fx.files->set("s/p.js",
                  "var A = class {\n  v = 1;\n};\nawait new Promise(() => {});\nexport {\n  A as default\n};\n");
    const ScriptLoadResult loaded = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(loaded.status) == "StillEvaluating");
    const std::string text = "not a script: its top-level await had not settled when the load ended";
    CHECK(loaded.error.message == text);
    CHECK(theOneError(log) == "script: res://s/p.js: " + text);
    CHECK(moduleStateLabel(recordOf(fx.vm, KEY).state) == "Evaluating");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance(KEY).status) == "NotLoaded");
    log.clear();
    // The pending WARN is suppressed inside a load only: evaluateModule keeps it.
    fx.files->set("s/q.js", "await new Promise(() => {});\n");
    CHECK(evalStatusLabel(fx.vm.evaluateModule("res://s/q.js").status) == "Ok");
    const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 1U);
    CHECK(loud[0].level == LogLevel::Warn);
    CHECK(loud[0].message.starts_with("script: res://s/q.js: module evaluation is still pending"));
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a load the loader refuses logs the loader's one record and holds no class (HR8)") {
    script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/bad.js", "var = ;\n");
    struct Refused {
        std::string_view path;
        std::string_view status;
        std::string_view prefix;
    };
    const std::vector<Refused> cases{
        {"res://s/bad.js", "CompileError", "script: res://s/bad.js: compile error: SyntaxError:"},
        {"res://s/missing.js", "LoadError",
         "script: res://s/missing.js: load error: TypeError: cannot load 'res://s/missing.js': not found or "
         "unreadable through the VFS"},
        {"scripts/a.js", "LoadError", "script: scripts/a.js: load error: TypeError: cannot resolve 'scripts/a.js'"},
    };
    for (const Refused& refused : cases) {
        log.clear();
        const ScriptLoadResult loaded = fx.vm.loadScript(refused.path);
        INFO("path ", refused.path);
        CHECK(scriptStatusLabel(loaded.status) == refused.status);
        CHECK(theOneError(log).starts_with(refused.prefix));
        const engine::script::VmStats stats = fx.vm.stats();
        CHECK(loaded.heapBytesInUse == stats.heapBytesInUse);
        CHECK(loaded.heapLimitBytes == stats.heapLimitBytes);
        log.clear();
        CHECK(scriptStatusLabel(fx.vm.createInstance(refused.path).status) == "NotLoaded");
    }
    script_test::expectCleanShutdown(fx.vm);

    engine::script::VmCreateResult bare = Vm::create(script_test::patient());
    REQUIRE(bare.vm.valid());
    log.clear();
    const ScriptLoadResult loaded = bare.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "LoadError");
    CHECK(theOneError(log).find("the VM has no module source") != std::string::npos);
    CHECK(loaded.heapBytesInUse == bare.vm.stats().heapBytesInUse);
    log.clear();
    CHECK(scriptStatusLabel(bare.vm.createInstance("res://s/a.js").status) == "NotLoaded");
    script_test::expectCleanShutdown(bare.vm);
}

TEST_CASE("script: a rewrite with the same bytes is Unchanged and costs one read (HR9)") {
    script_test::LogCapture log;
    Fixture fx;
    const std::string source = classSource("  constructor() {\n    globalThis.made = (globalThis.made ?? 0) + 1;\n  }");
    fx.files->set("s/a.js", source);
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    static_cast<void>(instanceOf(fx.vm, "res://s/a.js"));
    static_cast<void>(instanceOf(fx.vm, "res://s/a.js"));
    const engine::script::VmStats before = fx.vm.stats();
    const std::size_t readsBefore = fx.files->reads("s/a.js");
    log.clear();

    fx.files->set("s/a.js", source);
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Unchanged");
    const engine::script::VmStats after = fx.vm.stats();
    CHECK(after.heapBytesInUse == before.heapBytesInUse);
    CHECK(after.moduleInstances == before.moduleInstances);
    CHECK(loaded.heapBytesInUse == after.heapBytesInUse);
    CHECK(fx.files->reads("s/a.js") == readsBefore + 1U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(recordOf(fx.vm, "res://s/a.js").generation == 1U);
    CHECK(script_test::completion(fx.vm, "String(globalThis.made)") == "2");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the bytes a generation runs are the bytes the loader compiled (HR10)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", versioned("v0"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");

    // The comparison reads v1; the file then holds v2 for the loader's own read.
    fx.files->set("s/a.js", versioned("v1"));
    fx.files->replaceAfterNextRead("s/a.js", versioned("v2"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v2");

    fx.files->set("s/a.js", versioned("v2"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Unchanged");
    fx.files->set("s/a.js", versioned("v1"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: createInstance refuses an unloaded script and a constructor that fails (HR11)") {
    script_test::LogCapture log;
    Fixture fx;
    ScriptInstanceResult made = fx.vm.createInstance("res://s/never.js");
    CHECK(scriptStatusLabel(made.status) == "NotLoaded");
    CHECK(theOneError(log) == "script: res://s/never.js: createInstance refused: no script class is loaded from it");
    log.clear();
    made = fx.vm.createInstance("never.js");
    CHECK(scriptStatusLabel(made.status) == "NotLoaded");
    CHECK(theOneError(log) == "script: never.js: createInstance refused: no script class is loaded from it");

    constexpr std::string_view KEY = "res://s/c.js";
    fx.files->set(
        "s/c.js",
        classSource(
            "  constructor() {\n    if (globalThis.fail) {\n      throw new Error(\"constructor refused\");\n    }\n"
            "    if (globalThis.proxy) {\n      return new Proxy({}, {\n"
            "        ownKeys(t) { globalThis.traps++; return Reflect.ownKeys(t); },\n"
            "        get(t, k, r) { globalThis.traps++; return Reflect.get(t, k, r); },\n"
            "        getOwnPropertyDescriptor(t, k) { globalThis.traps++; return Reflect.getOwnPropertyDescriptor(t, "
            "k); }\n"
            "      });\n    }\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.fail = true; globalThis.traps = 0; 0"));
    log.clear();
    made = fx.vm.createInstance(KEY);
    CHECK(scriptStatusLabel(made.status) == "Exception");
    CHECK_FALSE(made.instance.valid());
    CHECK(theOneError(log).starts_with("script: res://s/c.js: uncaught Error: constructor refused"));
    static_cast<void>(script_test::completion(fx.vm, "globalThis.fail = false; 0"));
    made = fx.vm.createInstance(KEY);
    CHECK(scriptStatusLabel(made.status) == "Ok");
    CHECK(made.instance.valid());

    static_cast<void>(script_test::completion(fx.vm, "globalThis.proxy = true; 0"));
    log.clear();
    made = fx.vm.createInstance(KEY);
    CHECK(scriptStatusLabel(made.status) == "NotAClass");
    CHECK_FALSE(made.instance.valid());
    CHECK(made.error.message == "not a script: its constructor returned a Proxy");
    CHECK(theOneError(log) == "script: res://s/c.js: not a script: its constructor returned a Proxy");
    CHECK(script_test::completion(fx.vm, "String(globalThis.traps)") == "0");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: destroyInstance frees an instance and refuses a stale or null handle (HR12)") {
    script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/a.js");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(a)) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(a)) == "UnknownInstance");
    CHECK(theOneError(log) == "script: destroyInstance refused: the handle names no live instance");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(ScriptInstance{})) == "UnknownInstance");
    CHECK(theOneError(log) == "script: destroyInstance refused: the handle names no live instance");
    CHECK_FALSE(VmAccess::defineInstanceGlobal(fx.vm, a, "gone"));

    const ScriptInstance c = instanceOf(fx.vm, "res://s/a.js");
    CHECK(c != a);
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(a)) ==
          "UnknownInstance");  // a reused slot never answers a stale handle
    CHECK(log.countAtLeast(LogLevel::Error) == 1U);
    CHECK(b != c);

    fx.files->set("s/a.js", versioned("v2"));
    const ScriptLoadResult moved = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(moved.status) == "Ok");
    CHECK(moved.instancesMoved == 2U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a changed file re-creates every instance and an old object keeps the old code (HR13)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.oldA = a; 0"));
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v1");

    fx.files->set("s/a.js", versioned("v2"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(loaded.instancesMoved == 2U);
    const ModuleRecord record = recordOf(fx.vm, "res://s/a.js");
    CHECK(moduleStateLabel(record.state) == "Evaluated");
    CHECK(record.generation == 2U);

    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v2");
    CHECK(script_test::completion(fx.vm, "b.hello()") == "v2");
    CHECK(script_test::completion(fx.vm, "oldA.hello()") == "v1");
    CHECK(script_test::completion(fx.vm, "String(a !== oldA)") == "true");
    CHECK(script_test::completion(fx.vm, "String(Object.getPrototypeOf(a) !== Object.getPrototypeOf(oldA))") == "true");
    const ScriptInstance c = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "c.hello()") == "v2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a broken reload changes nothing and a later working one succeeds (HR14)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(fx.vm, R"(a.tag = "a"; b.tag = "b"; 0)"));
    const auto readsAll = [&] { return script_test::completion(fx.vm, "[a.hello(), a.tag, b.hello(), b.tag].join()"); };
    REQUIRE(readsAll() == "v1,a,v1,b");

    struct Broken {
        std::string_view why;
        std::string source;
        std::string_view status;
        std::string_view prefix;
    };
    const std::vector<Broken> broken{
        {"compile", "var = ;\n", "CompileError", "script: res://s/a.js: compile error: SyntaxError:"},
        {"throw",
         "var K = class {\n  hello() { return \"v3\"; }\n};\nthrow new Error(\"boom\");\nexport { K as default };\n",
         "Exception", "script: res://s/a.js: uncaught Error: boom"},
        {"function", "export default function f() {}\n", "NotAClass",
         "script: res://s/a.js: not a script: its default export is a function, not a class"},
        {"await", "var K = class {};\nawait new Promise(() => {});\nexport { K as default };\n", "StillEvaluating",
         "script: res://s/a.js: not a script: its top-level await had not settled"},
        {"constructor",
         classSource("  constructor() {\n    globalThis.built = (globalThis.built ?? 0) + 1;\n    if (globalThis.built "
                     "=== 2) {\n"
                     "      throw new Error(\"constructor refused\");\n    }\n  }\n  hello() { return \"v5\"; }"),
         "Exception", "script: res://s/a.js: uncaught Error: constructor refused"},
    };
    for (const Broken& step : broken) {
        INFO("broken load: ", step.why);
        fx.files->set("s/a.js", step.source);
        log.clear();
        const ScriptLoadResult last = fx.vm.loadScript(KEY);
        // Read before any probe script runs; what the failed load built (the fifth builds one object before its second
        // constructor throws) is already freed.
        CHECK(last.heapBytesInUse == fx.vm.stats().heapBytesInUse);
        CHECK(scriptStatusLabel(last.status) == step.status);
        CHECK(theOneError(log).starts_with(step.prefix));
        CHECK(last.instancesMoved == 0U);
        CHECK(readsAll() == "v1,a,v1,b");
    }
    const ModuleRecord record = recordOf(fx.vm, KEY);
    CHECK(moduleStateLabel(record.state) == "Evaluated");
    CHECK(record.generation > 1U);

    const ScriptInstance c = instanceOf(fx.vm, KEY);  // from the last good class, v1's
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "c.hello()") == "v1");

    fx.files->set("s/a.js", versioned("v6"));
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 3U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello(), c.hello()].join()") == "v6,v6,v6");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a reload re-creates by construction, so constructors run again and private fields restart (HR15)") {
    const script_test::LogCapture log;
    Fixture fx;
    const auto source = [](std::string_view extra) {
        return classSource(
            std::format("  #n = 0;\n  constructor() {{\n    globalThis.built = (globalThis.built ?? 0) + 1;\n  }}\n"
                        "  bump() {{\n    return ++this.#n;\n  }}{}",
                        extra));
    };
    fx.files->set("s/a.js", source(""));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    static_cast<void>(instanceOf(fx.vm, "res://s/a.js"));
    CHECK(script_test::completion(fx.vm, "String(globalThis.built)") == "2");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.bump(); String(a.bump())") == "2");

    fx.files->set("s/a.js", source("\n  extra() {\n    return 1;\n  }"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    CHECK(script_test::completion(fx.vm, "String(globalThis.built)") == "4");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "String(a.bump())") == "1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the three calls refuse an invalid Vm and another thread with one ERROR each (HR16)") {
    script_test::LogCapture log;
    Vm invalid;
    const ScriptLoadResult loaded = invalid.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "InvalidVm");
    CHECK(loaded.heapBytesInUse == 0U);
    CHECK(loaded.heapLimitBytes == 0U);
    CHECK(theOneError(log) == "script: loadScript on an invalid Vm (a failed create or a moved-from Vm)");
    log.clear();
    CHECK(scriptStatusLabel(invalid.createInstance("res://s/a.js").status) == "InvalidVm");
    CHECK(theOneError(log) == "script: createInstance on an invalid Vm (a failed create or a moved-from Vm)");
    log.clear();
    CHECK(scriptStatusLabel(invalid.destroyInstance(ScriptInstance{})) == "InvalidVm");
    CHECK(theOneError(log) == "script: destroyInstance on an invalid Vm (a failed create or a moved-from Vm)");
    CHECK_FALSE(VmAccess::defineInstanceGlobal(invalid, ScriptInstance{}, "x"));

    Fixture fx;
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    fx.files->set("s/a.js", versioned("v2"));
    log.clear();
    ScriptLoadResult otherLoad;
    ScriptInstanceResult otherCreate;
    ScriptStatus otherDestroy = ScriptStatus::Ok;
    bool otherDefine = true;
    std::thread other{[&] {
        otherLoad = fx.vm.loadScript("res://s/a.js");
        otherCreate = fx.vm.createInstance("res://s/a.js");
        otherDestroy = fx.vm.destroyInstance(a);
        otherDefine = VmAccess::defineInstanceGlobal(fx.vm, a, "x");
    }};
    other.join();
    CHECK(scriptStatusLabel(otherLoad.status) == "WrongThread");
    CHECK(otherLoad.heapBytesInUse == 0U);
    CHECK(scriptStatusLabel(otherCreate.status) == "WrongThread");
    CHECK(scriptStatusLabel(otherDestroy) == "WrongThread");
    CHECK_FALSE(otherDefine);
    const std::vector<script_test::CapturedRecord> loud = log.atLeast(LogLevel::Warn);
    REQUIRE(loud.size() == 3U);
    CHECK(loud[0].message == "script: loadScript called from a thread that is not the VM's; refused");
    CHECK(loud[1].message == "script: createInstance called from a thread that is not the VM's; refused");
    CHECK(loud[2].message == "script: destroyInstance called from a thread that is not the VM's; refused");

    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v1");
    CHECK(recordOf(fx.vm, "res://s/a.js").generation == 1U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a VM holding classes, instances and refused and old generations shuts down clean (HR17)") {
    const script_test::LogCapture log;
    Fixture fx;
    const auto linked = [](std::string_view version) {
        return classSource(
            std::format("  self = () => this;\n  link = null;\n  hello() {{\n    return \"{}\";\n  }}", version));
    };
    fx.files->set("s/a.js", linked("A1"));
    fx.files->set("s/b.js", versioned("B1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/b.js").status) == "Ok");
    const ScriptInstance a1 = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance a2 = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b1 = instanceOf(fx.vm, "res://s/b.js");
    const ScriptInstance b2 = instanceOf(fx.vm, "res://s/b.js");
    expose(fx.vm, a1, "a1");
    expose(fx.vm, a2, "a2");
    expose(fx.vm, b1, "b1");
    static_cast<void>(
        script_test::completion(fx.vm, "a2.link = a1; globalThis.oldA = a1; globalThis.keep = a1.self(); 0"));
    CHECK(scriptStatusLabel(fx.vm.destroyInstance(b2)) == "Ok");

    fx.files->set("s/a.js", linked("A2"));
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    fx.files->set("s/b.js", "var = ;\n");
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/b.js").status) == "CompileError");
    expose(fx.vm, a1, "a1");
    CHECK(script_test::completion(fx.vm, "a1.hello() + oldA.hello()") == "A2A1");
    script_test::expectCleanShutdown(fx.vm);
    CHECK(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "InvalidVm");
}

// ---- HR18-HR26: the carry --------------------------------------------------------------------------------------

namespace {

using engine::Entity;
using engine::World;
using engine::script::BindStatus;

// "<kept> <reset> <added> <dropped>" of a load, for one CHECK per case.
[[nodiscard]] std::string countsOf(const ScriptLoadResult& result) {
    return std::format("{} {} {} {}", result.fieldsKept, result.fieldsReset, result.fieldsAdded, result.fieldsDropped);
}

// The World a case's entities live in, bound to its VM. Declared before the Fixture so it is destroyed after it.
void bindWorld(Fixture& fx, World& world) { REQUIRE(fx.vm.bindWorld(world) == BindStatus::Ok); }

void defineEntity(Fixture& fx, std::string_view name, Entity entity) {
    REQUIRE(fx.vm.defineEntityGlobal(name, entity) == BindStatus::Ok);
}

}  // namespace

TEST_CASE("script: a reload keeps each instance's own field values, drops removed fields and adds new ones (HR18)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js",
                  classSource("  speed = 2;\n  label = \"x\";\n  old = 1;\n  hello() {\n    return \"v1\";\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(
        fx.vm, "a.speed = 7; a.label = 'a'; a.old = 10; b.speed = 9; b.label = 'b'; b.old = 20; 0"));

    fx.files->set("s/a.js", classSource("  speed = 2;\n  label = \"x\";\n  added = 5;\n  hello() {\n"
                                        "    return \"v2:\" + this.speed + \":\" + this.label;\n  }"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(loaded.instancesMoved == 2U);
    CHECK(countsOf(loaded) == "4 0 2 2");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hello(), a.added, 'old' in a, b.hello(), b.added, 'old' in b].join()") ==
          "v2:7:a,5,false,v2:9:b,5,false");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a changed default does not reset a field, and an unmodified field is carried too (HR19)") {
    const script_test::LogCapture log;
    Fixture fx;
    const auto source = [](int initial, std::string_view version) {
        return classSource(std::format("  speed = {};\n  hello() {{\n    return \"{}\";\n  }}", initial, version));
    };
    fx.files->set("s/a.js", source(2, "v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(fx.vm, "a.speed = 7; 0"));

    fx.files->set("s/a.js", source(5, "v2"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "2 0 0 0");
    const ScriptInstance fresh = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    expose(fx.vm, fresh, "c");
    CHECK(script_test::completion(fx.vm, "[a.speed, b.speed, c.speed].join()") == "7,2,5");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: every value kind carries when its kind is unchanged (HR20)") {
    const script_test::LogCapture log;
    World world;
    Fixture fx;
    bindWorld(fx, world);
    defineEntity(fx, "ent", world.create());
    defineEntity(fx, "other", world.create());
    const auto source = [](std::string_view version) {
        return classSource(std::format(
            "  flag = true;\n  num = 1;\n  big = 1n;\n  str = \"s\";\n  sym = Symbol(\"d\");\n  arr = [0];\n"
            "  vec = new engine.Vec3(1, 2, 3);\n  quat = new engine.Quat(0, 0, 0, 1);\n  ent = globalThis.ent;\n"
            "  obj = {{ k: 1 }};\n  nul = null;\n  hello() {{\n    return \"{}\";\n  }}",
            version));
    };
    fx.files->set("s/a.js", source("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(
        fx.vm,
        "globalThis.ks = Symbol('kept'); globalThis.ka = [4, 5]; globalThis.kv = new engine.Vec3(7, 8, 9); "
        "globalThis.kq = new engine.Quat(1, 0, 0, 0); globalThis.ko = { z: 2 }; globalThis.kn = { n: 3 }; "
        "a.flag = false; a.num = 3.5; a.big = 7n; a.str = 'hi'; a.sym = ks; a.arr = ka; a.vec = kv; a.quat = kq; "
        "a.ent = other; a.obj = ko; a.nul = kn; 0"));

    fx.files->set("s/a.js", source("v2"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "11 0 0 0");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm,
                                  "[a.flag === false, a.num === 3.5, a.big === 7n, a.str === 'hi', a.sym === ks, "
                                  "a.arr === ka, a.vec === kv, a.quat === kq, a.ent === other, a.obj === ko, "
                                  "a.nul === kn].join()") == "true,true,true,true,true,true,true,true,true,true,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a field whose default changes kind takes the new default, by kind and not by typeof (HR21)") {
    const script_test::LogCapture log;
    World world;
    Fixture fx;
    bindWorld(fx, world);
    defineEntity(fx, "ent", world.create());
    const auto source = [](std::string_view fields) {
        return classSource(std::format("{}\n  hello() {{\n    return {};\n  }}", fields, fields.size()));
    };
    fx.files->set("s/a.js", source("  n = 1;\n  v = new engine.Vec3(1, 2, 3);\n  ar = [1];\n  ob = { k: 1 };\n"
                                   "  en = globalThis.ent;\n  bo = true;\n  bi = 5n;\n  sy = Symbol(\"s\");\n"
                                   "  st = \"s\";"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");

    fx.files->set("s/a.js",
                  source("  n = \"text\";\n  v = new engine.Quat(0, 0, 0, 1);\n  ar = { k: 2 };\n  ob = [9];\n"
                         "  en = { e: 1 };\n  bo = 4;\n  bi = 6;\n  sy = \"sym\";\n  st = null;"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "0 9 0 0");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(
              fx.vm,
              "[a.n, a.v instanceof engine.Quat, Array.isArray(a.ar), a.ar.k, Array.isArray(a.ob), a.ob[0], a.en.e, "
              "a.bo, a.bi, a.sy, a.st].join()") == "text,true,false,2,true,9,1,4,6,sym,");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE(
    "script: a field declared without a value and a null default take what a null or missing default may (HR22)") {
    const script_test::LogCapture log;
    World world;
    Fixture fx;
    bindWorld(fx, world);
    defineEntity(fx, "other", world.create());
    fx.files->set("s/a.js", classSource("  f1 = 0;\n  f2 = null;\n  f3 = null;\n  f4 = 0;\n  f5 = \"\";\n  f6 = 0;\n"
                                        "  hello() {\n    return \"v1\";\n  }"));
    fx.files->set("s/b.js", classSource("  g1 = {};\n  g2 = [];\n  g3 = new engine.Vec3();\n  g4 = null;\n"
                                        "  g5 = null;\n  g6 = 0;\n  g7 = \"\";\n  g8 = undefined;\n"
                                        "  hello() {\n    return \"v1\";\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/b.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/b.js");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(
        fx.vm,
        "globalThis.k1 = { q: 1 }; globalThis.k2 = [1]; globalThis.k3 = new engine.Vec3(1, 2, 3); "
        "a.f1 = 4; a.f2 = k1; a.f3 = other; a.f4 = null; a.f5 = 's'; a.f6 = k2; "
        "b.g1 = k1; b.g2 = k2; b.g3 = k3; b.g4 = other; b.g5 = null; b.g6 = 5; b.g7 = 's'; 0"));

    fx.files->set("s/a.js",
                  classSource("  f1;\n  f2;\n  f3;\n  f4;\n  f5;\n  f6;\n  hello() {\n    return \"v2\";\n  }"));
    const ScriptLoadResult first = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(first.status) == "Ok");
    CHECK(countsOf(first) == "6 0 0 0");
    fx.files->set("s/b.js",
                  classSource("  g1 = null;\n  g2 = null;\n  g3 = null;\n  g4 = null;\n  g5 = null;\n"
                              "  g6 = null;\n  g7 = null;\n  g8 = null;\n  hello() {\n    return \"v2\";\n  }"));
    const ScriptLoadResult second = fx.vm.loadScript("res://s/b.js");
    CHECK(scriptStatusLabel(second.status) == "Ok");
    CHECK(countsOf(second) == "5 3 0 0");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.f1, a.f2 === k1, a.f3 === other, a.f4, a.f5, a.f6 === k2].join()") ==
          "4,true,true,,s,true");
    CHECK(script_test::completion(fx.vm,
                                  "[b.g1 === k1, b.g2 === k2, b.g3 === k3, b.g4 === other, b.g5 === null, "
                                  "b.g6 === null, b.g7 === null, b.g8 === null].join()") ==
          "true,true,true,true,true,true,true,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a function never carries, so a closure over the old instance is replaced (HR23)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js",
                  classSource("  speed = 3;\n  cb = () => \"v1:\" + this.speed;\n  g = () => 1;\n  num = 1;"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.keptCb = a.cb; a.speed = 6; 0"));
    CHECK(script_test::completion(fx.vm, "keptCb()") == "v1:6");

    fx.files->set("s/a.js",
                  classSource("  speed = 3;\n  cb = () => \"v2:\" + this.speed;\n  g;\n  num = () => \"fn\";"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "1 3 0 0");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "[a.cb(), a.cb !== keptCb, String(a.g), typeof a.num, a.num()].join()") ==
          "v2:6,true,undefined,function,fn");
    CHECK(script_test::completion(fx.vm, "keptCb()") == "v1:6");  // the old closure still reads the old instance
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a carried object is carried by reference and keeps the code it was made with (HR24)") {
    const script_test::LogCapture log;
    Fixture fx;
    const auto source = [](std::string_view helper) {
        return std::format(
            "var Helper = class {{\n  describe() {{\n    return \"{}\";\n  }}\n}};\n"
            "var Mover = class {{\n  helper = new Helper();\n  list = [1, 2];\n}};\nexport {{\n  Mover as "
            "default\n}};\n",
            helper);
    };
    fx.files->set("s/a.js", source("helper-v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(
        script_test::completion(fx.vm, "a.list.push(3); globalThis.kl = a.list; globalThis.kh = a.helper; 0"));

    fx.files->set("s/a.js", source("helper-v2"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "2 0 0 0");
    const ScriptInstance fresh = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    expose(fx.vm, fresh, "c");
    CHECK(script_test::completion(fx.vm,
                                  "[a.list === kl, a.list.join('-'), a.helper === kh, a.helper.describe(), "
                                  "c.helper.describe(), c.list.join('-')].join()") ==
          "true,1-2-3,true,helper-v1,helper-v2,1-2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: an accessor is not a field, and the carry runs neither a getter nor a setter (HR25)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", classSource("  speed = 3;\n  constructor() {\n"
                                        "    Object.defineProperty(this, \"peek\", {\n"
                                        "      get() {\n        globalThis.calls++;\n        return 1;\n      },\n"
                                        "      enumerable: true,\n      configurable: true\n    });\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.calls = 0; a.speed = 8; 0"));

    fx.files->set("s/a.js",
                  classSource("  _s = 0;\n  get speed() {\n    globalThis.calls++;\n    return this._s;\n  }\n"
                              "  set speed(value) {\n    globalThis.calls++;\n    this._s = value;\n  }"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "0 0 1 1");  // `speed` dropped; `peek` was never a field; `_s` is new
    CHECK(script_test::completion(fx.vm, "String(calls)") == "0");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "String(a.speed)") == "0");
    CHECK(script_test::completion(fx.vm, "String(calls)") == "1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a field made non-enumerable is dropped and a frozen instance keeps its defaults (HR26)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", classSource("  hidden = 1;"));
    fx.files->set("s/b.js", classSource("  speed = 2;"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/b.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    const ScriptInstance b = instanceOf(fx.vm, "res://s/b.js");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(fx.vm, "a.hidden = 5; b.speed = 7; 0"));

    fx.files->set("s/a.js", classSource("  hidden = 0;\n  constructor() {\n"
                                        "    Object.defineProperty(this, \"hidden\", { enumerable: false });\n  }"));
    const ScriptLoadResult hidden = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(hidden.status) == "Ok");
    CHECK(countsOf(hidden) == "0 0 0 1");
    fx.files->set("s/b.js", classSource("  speed = 2;\n  constructor() {\n    Object.freeze(this);\n  }"));
    const ScriptLoadResult frozen = fx.vm.loadScript("res://s/b.js");
    CHECK(scriptStatusLabel(frozen.status) == "Ok");
    CHECK(countsOf(frozen) == "0 1 0 0");
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hidden, b.speed].join()") == "0,2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a field the new class makes an own accessor on its instance takes the accessor (HR38)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", classSource("  speed = 3;\n  hello() {\n    return \"v1\";\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.calls = 0; globalThis.stored = 0; a.speed = 8; 0"));

    // The accessor is defined by the constructor, on the instance itself and not on the prototype.
    fx.files->set("s/a.js", classSource("  constructor() {\n    Object.defineProperty(this, \"speed\", {\n"
                                        "      get() {\n        globalThis.calls++;\n        return 42;\n      },\n"
                                        "      set(value) {\n        globalThis.calls++;\n"
                                        "        globalThis.stored = value;\n      },\n"
                                        "      enumerable: true,\n      configurable: true\n    });\n  }\n"
                                        "  hello() {\n    return \"v2\";\n  }"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "0 0 0 1");  // dropped, not kept: the running 8 is not written over the accessor
    CHECK(script_test::completion(fx.vm, "[calls, stored].join()") == "0,0");  // neither half ran during the carry
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "String(a.speed)") == "42");  // the accessor, not the running 8
    CHECK(script_test::completion(fx.vm, "[calls, stored].join()") == "1,0");
    CHECK(script_test::completion(fx.vm, "a.speed = 5; [calls, stored].join()") == "2,5");  // still an accessor
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE(
    "script: a boolean, a bigint and a symbol are not references, so a null or object default resets them (HR39)") {
    const script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/a.js", classSource("  b1 = true;\n  n1 = 1n;\n  s1 = Symbol(\"a\");\n"
                                        "  b2 = true;\n  n2 = 1n;\n  s2 = Symbol(\"b\");\n"
                                        "  hello() {\n    return \"v1\";\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript("res://s/a.js").status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, "res://s/a.js");
    expose(fx.vm, a, "a");
    static_cast<void>(script_test::completion(
        fx.vm, "a.b1 = false; a.n1 = 7n; a.s1 = Symbol('x'); a.b2 = false; a.n2 = 9n; a.s2 = Symbol('y'); 0"));

    // The first three fields are `null` in the new class (the relaxation carries reference kinds only); the last
    // three are an object (an object default carries objects only).
    fx.files->set("s/a.js", classSource("  b1 = null;\n  n1 = null;\n  s1 = null;\n"
                                        "  b2 = { d: 1 };\n  n2 = { d: 2 };\n  s2 = { d: 3 };\n"
                                        "  hello() {\n    return \"v2\";\n  }"));
    const ScriptLoadResult loaded = fx.vm.loadScript("res://s/a.js");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(countsOf(loaded) == "0 6 0 0");
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "[a.b1 === null, a.n1 === null, a.s1 === null].join()") == "true,true,true");
    CHECK(script_test::completion(fx.vm, "[a.b2.d, a.n2.d, a.s2.d].join()") == "1,2,3");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

// ---- HR27-HR32: refused bytes report once, the heap's own words, the budget, and scale -------------------------

namespace {

constexpr std::size_t MIB = std::size_t{1024} * 1024U;

// Trailing assignments to a name the source already declares: a refused bytecode growth anywhere before them meets
// an assignment's lvalue check, the backend's one check of the buffer, and the compile returns (R24 in docs/08).
// Sixteen leaves three statements after the buffer's last growth in every padded source here (measured).
// Copied from script_module_test.cpp, where the measurement lives.
[[nodiscard]] std::string assignmentPad(std::string_view name) {
    std::string pad;
    for (int i = 1; i <= 16; ++i) {
        pad += " " + std::string{name} + " = " + std::to_string(i) + ";";
    }
    return pad;
}

// `source` followed by the pad: cases that compile after the heap has been full end every module in it (R24).
[[nodiscard]] std::string padded(std::string_view source) {
    return "let pad = 0;\n" + std::string{source} + "\n" + assignmentPad("pad") + "\n";
}

// The first line of a record: the stack, when there is one, follows after a newline.
[[nodiscard]] std::string_view firstLine(std::string_view record) { return record.substr(0, record.find('\n')); }

[[nodiscard]] bool errorIsEmpty(const engine::script::ScriptError& error) {
    return error.name.empty() && error.message.empty() && error.stack.empty();
}

// A function that fills the heap with arrays until the allocator refuses, then throws the backend's out-of-memory
// error. What it held is freed when the throw unwinds.
constexpr std::string_view HOG =
    "function hog() {\n  const held = [];\n  for (;;) {\n    held.push(new Array(1024).fill(0));\n  }\n}\n";

}  // namespace

TEST_CASE("script: a refused load is reported once, then remembered until the bytes change or a load succeeds (HR27)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/a.js";
    const std::string v1 = classSource(
        "  constructor() {\n    globalThis.built = (globalThis.built ?? 0) + 1;\n  }\n"
        "  hello() {\n    return \"v1\";\n  }");
    const std::string v2 =
        "var K = class {\n  hello() { return \"v2\"; }\n};\nthrow new Error(\"boom\");\n"
        "export { K as default };\n";
    const std::string v3 = "var = ;\n";
    fx.files->set("s/a.js", v1);
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    REQUIRE(script_test::completion(fx.vm, "String(built)") == "2");

    fx.files->set("s/a.js", v2);
    log.clear();
    const ScriptLoadResult thrown = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(thrown.status) == "Exception");
    CHECK(theOneError(log).starts_with("script: res://s/a.js: uncaught Error: boom"));
    const std::uint32_t generation = recordOf(fx.vm, KEY).generation;
    const engine::script::VmStats before = fx.vm.stats();

    // The same bytes again, as a watcher's next trigger delivers them: nothing runs, nothing is logged.
    fx.files->set("s/a.js", v2);
    log.clear();
    const ScriptLoadResult again = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(again.status) == "AlreadyRefused");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(errorIsEmpty(again.error));
    CHECK(again.instancesMoved == 0U);
    CHECK(recordOf(fx.vm, KEY).generation == generation);
    const engine::script::VmStats after = fx.vm.stats();
    CHECK(after.heapBytesInUse == before.heapBytesInUse);
    CHECK(after.moduleInstances == before.moduleInstances);
    CHECK(again.heapBytesInUse == after.heapBytesInUse);
    CHECK(script_test::completion(fx.vm, "String(built)") == "2");

    fx.files->set("s/a.js", v3);
    log.clear();
    const ScriptLoadResult compile = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(compile.status) == "CompileError");
    CHECK(theOneError(log).starts_with("script: res://s/a.js: compile error: SyntaxError:"));

    fx.files->set("s/a.js", v1);
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Unchanged");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    fx.files->set("s/a.js", versioned("v4"));
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");

    // A success forgets the refused bytes: v3 is attempted anew.
    fx.files->set("s/a.js", v3);
    log.clear();
    const ScriptLoadResult anew = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(anew.status) == "CompileError");
    CHECK(theOneError(log).starts_with("script: res://s/a.js: compile error: SyntaxError:"));
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v4");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a path never loaded remembers its refused bytes too (HR28)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/new.js";
    fx.files->set("s/new.js", "var = ;\n");
    const ScriptLoadResult first = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(first.status) == "CompileError");
    CHECK(theOneError(log).starts_with("script: res://s/new.js: compile error: SyntaxError:"));

    log.clear();
    const ScriptLoadResult second = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(second.status) == "AlreadyRefused");
    CHECK(errorIsEmpty(second.error));
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(scriptStatusLabel(fx.vm.createInstance(KEY).status) == "NotLoaded");

    log.clear();
    fx.files->set("s/new.js", versioned("v1"));
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance made = instanceOf(fx.vm, KEY);
    expose(fx.vm, made, "made");
    CHECK(script_test::completion(fx.vm, "made.hello()") == "v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a load that fills the heap says so in the engine's words and commits nothing (HR29)") {
    script_test::LogCapture log;
    Fixture fx{script_test::patient(VmConfig{.heapLimitBytes = 2 * MIB})};
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", padded(versioned("v1")));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);

    fx.files->set("s/a.js", padded(std::string{HOG} + "hog();\n" + versioned("v2")));
    log.clear();
    const ScriptLoadResult full = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(full.status) == "OutOfMemory");
    const std::string record = theOneError(log);
    const std::string_view head = firstLine(record);
    CHECK(head.starts_with("script: res://s/a.js: out of memory (heap limit 2097152 bytes, "));
    CHECK(
        head.ends_with("bytes in use): the script and its instances are unchanged, and only a new VM resets the "
                       "heap"));
    CHECK(head.find("null") == std::string_view::npos);
    CHECK(head.find("InternalError") == std::string_view::npos);
    CHECK(full.heapLimitBytes == 2 * MIB);
    CHECK(full.heapBytesInUse == fx.vm.stats().heapBytesInUse);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello()].join()") == "v1,v1");

    // The hog was a function's local: the collector freed it, so the next load has room.
    fx.files->set("s/a.js", padded(versioned("v3")));
    log.clear();
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 2U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v3");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a constructor that fills the heap leaves the evaluated class uncommitted (HR30)") {
    script_test::LogCapture log;
    Fixture fx{script_test::patient(VmConfig{.heapLimitBytes = 2 * MIB})};
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", padded(versioned("v1")));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);

    const std::string hogging = std::string{HOG} + classSource(
                                                       "  constructor() {\n    hog();\n  }\n"
                                                       "  hello() {\n    return \"v2\";\n  }");
    fx.files->set("s/a.js", padded(hogging));
    log.clear();
    const ScriptLoadResult full = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(full.status) == "OutOfMemory");
    const std::string record = theOneError(log);
    const std::string_view head = firstLine(record);
    CHECK(head.starts_with("script: res://s/a.js: out of memory (heap limit 2097152 bytes, "));
    CHECK(
        head.ends_with("bytes in use): the script and its instances are unchanged, and only a new VM resets the "
                       "heap"));
    const ModuleRecord module = recordOf(fx.vm, KEY);
    CHECK(moduleStateLabel(module.state) == "Evaluated");
    CHECK(module.generation > 1U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello()].join()") == "v1,v1");

    // Made from the last good class: the hogging constructor of the evaluated v2 does not run.
    log.clear();
    const ScriptInstance c = instanceOf(fx.vm, KEY);
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "c.hello()") == "v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    fx.files->set("s/a.js", padded(versioned("v3")));
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 3U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello(), c.hello()].join()") == "v3,v3,v3");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a constructor that runs past the time budget interrupts the load and commits nothing (HR31)") {
    script_test::LogCapture log;
    Fixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);
    REQUIRE(script_test::completion(fx.vm, "globalThis.built = 0; String(built)") == "0");

    fx.files->set("s/a.js", classSource("  constructor() {\n    globalThis.built += 1;\n    if (built === 2) {\n"
                                        "      for (;;) {}\n    }\n  }\n  hello() {\n    return \"v2\";\n  }"));
    log.clear();
    const ScriptLoadResult stuck = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(stuck.status) == "Interrupted");
    CHECK(theOneError(log).starts_with("script: res://s/a.js: interrupted after exceeding the 100 ms time budget"));
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello(), built].join()") == "v1,v1,2");

    fx.files->set("s/a.js", versioned("v3"));
    log.clear();
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 2U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello()].join()") == "v3,v3");
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a reload of 10000 instances pairs each with its own running object (HR32)") {
    const script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/a.js";
    constexpr std::size_t COUNT = 10000;
    fx.files->set("s/a.js", classSource("  index = globalThis.next++;\n  hello() {\n    return \"v1\";\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    REQUIRE(script_test::completion(fx.vm, "globalThis.next = 0; String(next)") == "0");
    std::vector<ScriptInstance> instances;
    instances.reserve(COUNT);
    for (std::size_t i = 0; i < COUNT; ++i) {
        instances.push_back(instanceOf(fx.vm, KEY));
    }

    fx.files->set("s/a.js", classSource("  index = -1;\n  hello() {\n    return \"v2\";\n  }"));
    const auto start = std::chrono::steady_clock::now();
    const ScriptLoadResult loaded = fx.vm.loadScript(KEY);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    MESSAGE("reload of 10000 instances: " << elapsed.count() << " ms");
    CHECK(scriptStatusLabel(loaded.status) == "Ok");
    CHECK(loaded.instancesMoved == COUNT);
    CHECK(loaded.fieldsKept == COUNT);

    std::vector<std::size_t> sampled{0, COUNT - 1};
    for (std::size_t i = 997; i < COUNT; i += 997) {
        sampled.push_back(i);
    }
    for (const std::size_t i : sampled) {
        expose(fx.vm, instances[i], "s");
        CHECK(script_test::completion(fx.vm, "s.index + ':' + s.hello()") == std::format("{}:v2", i));
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a load or a construction its time budget ended is Interrupted with one record (HR33)") {
    const std::array<std::string_view, 2> bodies{
        "var K = class {};\nawait new Promise(() => { for (;;) {} });\nexport { K as default };\n",
        "var K = class {};\nPromise.resolve({ then() { for (;;) {} } }); await new Promise(() => {});\n"
        "export { K as default };\n"};
    // The first line of the one record the VM's next call logs after the interrupted reload.
    const std::array<std::string_view, 2> nextRecords{
        "script: next.js: uncaught InternalError: interrupted",  // the executor's leftover job
        ""};                                                     // none: the thenable's next call is clean
    const std::string interrupted = "script: res://s/p.js: interrupted after exceeding the 100 ms time budget";
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const std::string_view body = bodies[i];
        INFO(body);
        script_test::LogCapture log;
        Fixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
        constexpr std::string_view KEY = "res://s/p.js";
        // A first load: Interrupted, one record, no class.
        fx.files->set("s/p.js", std::string{body});
        const ScriptLoadResult first = fx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(first.status) == "Interrupted");
        CHECK(firstLine(theOneError(log)) == interrupted);
        log.clear();
        CHECK(scriptStatusLabel(fx.vm.createInstance(KEY).status) == "NotLoaded");

        // A reload of a loaded script with a live instance: nothing moves, the last good class stays.
        script_test::LogCapture reloadLog;
        Fixture rx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
        rx.files->set("s/p.js", versioned("v1"));
        REQUIRE(scriptStatusLabel(rx.vm.loadScript(KEY).status) == "Ok");
        const ScriptInstance live = instanceOf(rx.vm, KEY);
        reloadLog.clear();
        rx.files->set("s/p.js", std::string{body});
        const ScriptLoadResult reloaded = rx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(reloaded.status) == "Interrupted");
        CHECK(firstLine(theOneError(reloadLog)) == interrupted);
        // The interrupted reload skips its drain, so what its budget left queued runs in the VM's next
        // call. After the promise executor's interrupt, that call runs the leftover rejection job and
        // answers Exception with its one record; after the thenable's, it is clean. Pinned as measured:
        // a later VM-core change that settles the leftover inside the interrupted call changes this case
        // on purpose.
        reloadLog.clear();
        const engine::script::EvalResult next = rx.vm.evaluateScript("0", "next.js");
        if (nextRecords[i].empty()) {
            CHECK(evalStatusLabel(next.status) == "Ok");
            CHECK(reloadLog.countAtLeast(LogLevel::Warn) == 0U);
        } else {
            CHECK(evalStatusLabel(next.status) == "Exception");
            CHECK(firstLine(theOneError(reloadLog)) == nextRecords[i]);
        }
        reloadLog.clear();
        expose(rx.vm, live, "a");
        CHECK(script_test::completion(rx.vm, "a.hello() + a.tag") == "v1x");
        reloadLog.clear();
        const ScriptInstanceResult again = rx.vm.createInstance(KEY);
        CHECK(scriptStatusLabel(again.status) == "Ok");
        CHECK(reloadLog.countAtLeast(LogLevel::Warn) == 0U);
        rx.files->set("s/p.js", versioned("v2"));
        const ScriptLoadResult working = rx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(working.status) == "Ok");
        CHECK(working.instancesMoved == 2U);
        expose(rx.vm, live, "a");
        CHECK(script_test::completion(rx.vm, "a.hello()") == "v2");
        script_test::expectCleanShutdown(rx.vm);
        script_test::expectCleanShutdown(fx.vm);
    }

    // createInstance: a constructor whose promise executor runs past the budget and which then returns a Proxy.
    script_test::LogCapture log;
    Fixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
    constexpr std::string_view KEY = "res://s/p.js";
    fx.files->set(
        "s/p.js",
        classSource("  constructor() {\n    if (globalThis.slow) {\n"
                    "      new Promise(() => { for (;;) {} });\n      return new Proxy({}, {});\n    }\n  }"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    static_cast<void>(script_test::completion(fx.vm, "globalThis.slow = true; 0"));
    log.clear();
    const ScriptInstanceResult slow = fx.vm.createInstance(KEY);
    CHECK(scriptStatusLabel(slow.status) == "Interrupted");
    CHECK_FALSE(slow.instance.valid());
    CHECK(firstLine(theOneError(log)) == interrupted);
    static_cast<void>(script_test::completion(fx.vm, "globalThis.slow = false; 0"));
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance(KEY).status) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);

    // A constructor that builds its Proxy before the slow executor and returns it after: the executor's interrupt is
    // a rejection and nothing after it polls, so the Proxy is a finished value of a call whose budget ended.
    const std::string preBuilt = classSource(
        "  tag = \"x\";\n  constructor() {\n    if (globalThis.slow) {\n      const p = new Proxy({}, {});\n"
        "      new Promise(() => { for (;;) {} });\n      return p;\n    }\n  }\n  hello() {\n    return \"v2\";\n  }");
    {
        script_test::LogCapture preLog;
        Fixture px{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
        px.files->set("s/p.js", preBuilt);
        REQUIRE(scriptStatusLabel(px.vm.loadScript(KEY).status) == "Ok");
        static_cast<void>(script_test::completion(px.vm, "globalThis.slow = true; 0"));
        preLog.clear();
        const ScriptInstanceResult early = px.vm.createInstance(KEY);
        CHECK(scriptStatusLabel(early.status) == "Interrupted");
        CHECK_FALSE(early.instance.valid());
        CHECK(firstLine(theOneError(preLog)) == interrupted);
        static_cast<void>(script_test::completion(px.vm, "globalThis.slow = false; 0"));
        preLog.clear();
        CHECK(scriptStatusLabel(px.vm.createInstance(KEY).status) == "Ok");
        CHECK(preLog.countAtLeast(LogLevel::Warn) == 0U);
        script_test::expectCleanShutdown(px.vm);
    }
    {
        script_test::LogCapture rebuildLog;
        Fixture rx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
        rx.files->set("s/p.js", versioned("v1"));
        REQUIRE(scriptStatusLabel(rx.vm.loadScript(KEY).status) == "Ok");
        const ScriptInstance live = instanceOf(rx.vm, KEY);
        static_cast<void>(script_test::completion(rx.vm, "globalThis.slow = true; 0"));
        rebuildLog.clear();
        rx.files->set("s/p.js", preBuilt);
        const ScriptLoadResult reloaded = rx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(reloaded.status) == "Interrupted");
        CHECK(reloaded.instancesMoved == 0U);
        CHECK(firstLine(theOneError(rebuildLog)) == interrupted);
        static_cast<void>(script_test::completion(rx.vm, "globalThis.slow = false; 0"));
        rebuildLog.clear();
        expose(rx.vm, live, "a");
        CHECK(script_test::completion(rx.vm, "a.hello() + a.tag") == "v1x");
        rebuildLog.clear();
        CHECK(scriptStatusLabel(rx.vm.createInstance(KEY).status) == "Ok");
        CHECK(rebuildLog.countAtLeast(LogLevel::Warn) == 0U);
        rx.files->set("s/p.js", versioned("v3"));
        const ScriptLoadResult working = rx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(working.status) == "Ok");
        CHECK(working.instancesMoved == 2U);
        expose(rx.vm, live, "a");
        CHECK(script_test::completion(rx.vm, "a.hello()") == "v3");
        script_test::expectCleanShutdown(rx.vm);
    }
}

// ---- HR34-HR37: a script's imports, and the link, load and constructor failure arms -----------------------------

namespace {

constexpr std::string_view DEP_V1 = "export const word = \"d1\";\n";

// A class importing `name` from ./dep.js as `word`: hello() answers `<word>-<version>`, and `tag` starts at "x".
[[nodiscard]] std::string importing(std::string_view version, std::string_view name = "word") {
    return std::format("import {{ {} as word }} from \"./dep.js\";\n", name) +
           classSource(std::format("  tag = \"x\";\n  hello() {{\n    return word + \"-{}\";\n  }}", version));
}

}  // namespace

TEST_CASE("script: a link error is refused with one record and a reload into it changes nothing (HR34)") {
    script_test::LogCapture log;
    Fixture fx;
    fx.files->set("s/dep.js", std::string{DEP_V1});
    const std::string linkError = importing("v9", "nope");

    // A first load: the loader's CompileError, its one record, and no class.
    fx.files->set("s/l.js", linkError);
    const ScriptLoadResult first = fx.vm.loadScript("res://s/l.js");
    CHECK(scriptStatusLabel(first.status) == "CompileError");
    CHECK(first.error.message == "Could not find export 'nope' in module 'res://s/dep.js'");
    CHECK(theOneError(log) ==
          "script: res://s/l.js: compile error: SyntaxError: Could not find export 'nope' in module 'res://s/dep.js'");
    log.clear();
    CHECK(scriptStatusLabel(fx.vm.createInstance("res://s/l.js").status) == "NotLoaded");

    // A reload of a working importer into the same link error: every instance and the class stay.
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", importing("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(fx.vm, R"(a.tag = "a"; b.tag = "b"; 0)"));
    const auto readsAll = [&] { return script_test::completion(fx.vm, "[a.hello(), a.tag, b.hello(), b.tag].join()"); };
    REQUIRE(readsAll() == "d1-v1,a,d1-v1,b");

    fx.files->set("s/a.js", linkError);
    log.clear();
    const ScriptLoadResult reloaded = fx.vm.loadScript(KEY);
    CHECK(reloaded.heapBytesInUse == fx.vm.stats().heapBytesInUse);
    CHECK(scriptStatusLabel(reloaded.status) == "CompileError");
    // dep.js runs in its second generation: the failed first load retired the instance it had created (R22).
    CHECK(theOneError(log) ==
          "script: res://s/a.js: compile error: SyntaxError: Could not find export 'nope' in module "
          "'res://s/dep.js?gen=2'");
    CHECK(reloaded.instancesMoved == 0U);
    CHECK(readsAll() == "d1-v1,a,d1-v1,b");

    log.clear();
    const ScriptInstance c = instanceOf(fx.vm, KEY);  // from the last good class, v1's
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "c.hello()") == "d1-v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    fx.files->set("s/a.js", importing("v2"));
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 3U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "[a.hello(), a.tag, b.hello(), b.tag, c.hello()].join()") ==
          "d1-v2,a,d1-v2,b,d1-v2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a reload of a deleted file is a LoadError that changes nothing, and a restored file loads (HR35)") {
    script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    const ScriptInstance b = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    static_cast<void>(script_test::completion(fx.vm, R"(a.tag = "a"; b.tag = "b"; 0)"));
    const auto readsAll = [&] { return script_test::completion(fx.vm, "[a.hello(), a.tag, b.hello(), b.tag].join()"); };
    REQUIRE(readsAll() == "v1,a,v1,b");

    fx.files->remove("s/a.js");
    // A failed peek compares equal to nothing, so each trigger is the loader's, with its own one record.
    for (int trigger = 1; trigger <= 2; ++trigger) {
        INFO("trigger ", trigger);
        log.clear();
        const ScriptLoadResult gone = fx.vm.loadScript(KEY);
        CHECK(gone.heapBytesInUse == fx.vm.stats().heapBytesInUse);
        CHECK(scriptStatusLabel(gone.status) == "LoadError");
        CHECK(
            theOneError(log) ==
            "script: res://s/a.js: load error: TypeError: cannot load 'res://s/a.js': not found or unreadable through "
            "the VFS");
        CHECK(gone.instancesMoved == 0U);
        CHECK(readsAll() == "v1,a,v1,b");
    }

    log.clear();
    const ScriptInstance c = instanceOf(fx.vm, KEY);  // from the last good class, v1's
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "c.hello()") == "v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    fx.files->set("s/a.js", versioned("v2"));
    const ScriptLoadResult restored = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(restored.status) == "Ok");
    CHECK(restored.instancesMoved == 3U);
    expose(fx.vm, a, "a");
    expose(fx.vm, b, "b");
    expose(fx.vm, c, "c");
    CHECK(script_test::completion(fx.vm, "[a.hello(), a.tag, b.hello(), b.tag, c.hello()].join()") == "v2,a,v2,b,v2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a constructor that fills the heap or runs past the budget refuses one createInstance (HR36)") {
    // Each constructor fails once: it disarms itself first, so the very next call is a working createInstance.
    SUBCASE("OutOfMemory") {
        script_test::LogCapture log;
        Fixture fx{script_test::patient(VmConfig{.heapLimitBytes = 2 * MIB})};
        constexpr std::string_view KEY = "res://s/c.js";
        const std::string hogging =
            std::string{HOG} + classSource(
                                   "  constructor() {\n    if (globalThis.hogs) {\n      globalThis.hogs = false;\n"
                                   "      hog();\n    }\n  }\n  hello() {\n    return \"v1\";\n  }");
        fx.files->set("s/c.js", padded(hogging));
        REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
        const ScriptInstance a = instanceOf(fx.vm, KEY);
        static_cast<void>(script_test::completion(fx.vm, "globalThis.hogs = true; 0"));
        log.clear();
        const ScriptInstanceResult full = fx.vm.createInstance(KEY);
        CHECK(scriptStatusLabel(full.status) == "OutOfMemory");
        CHECK_FALSE(full.instance.valid());
        const std::string record = theOneError(log);
        CHECK(firstLine(record).starts_with("script: res://s/c.js: out of memory (heap limit 2097152 bytes, "));
        CHECK(record.find("the script and its instances are unchanged") == std::string::npos);  // a load's words

        log.clear();
        const ScriptInstance b = instanceOf(fx.vm, KEY);
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expose(fx.vm, a, "a");
        expose(fx.vm, b, "b");
        CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello(), globalThis.hogs].join()") == "v1,v1,false");
        // The refused construction made nothing: a reload moves exactly the two instances made.
        fx.files->set("s/c.js", padded(versioned("v2")));
        const ScriptLoadResult moved = fx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(moved.status) == "Ok");
        CHECK(moved.instancesMoved == 2U);
        script_test::expectCleanShutdown(fx.vm);
    }
    SUBCASE("Interrupted") {
        script_test::LogCapture log;
        Fixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
        constexpr std::string_view KEY = "res://s/t.js";
        fx.files->set("s/t.js",
                      classSource("  doubled = [1, 2, 3].map((n) => n * 2).join(\"\");\n"
                                  "  constructor() {\n    if (globalThis.spin) {\n      globalThis.spin = false;\n"
                                  "      for (;;) {}\n    }\n  }\n  hello() {\n    return \"v1\";\n  }"));
        REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
        const ScriptInstance a = instanceOf(fx.vm, KEY);
        static_cast<void>(script_test::completion(fx.vm, "globalThis.spin = true; 0"));
        log.clear();
        const ScriptInstanceResult stuck = fx.vm.createInstance(KEY);
        CHECK(scriptStatusLabel(stuck.status) == "Interrupted");
        CHECK_FALSE(stuck.instance.valid());
        CHECK(firstLine(theOneError(log)) ==
              "script: res://s/t.js: interrupted after exceeding the 100 ms time budget");

        // The next call gets its own budget and its stack back: its constructor calls a native and a callback.
        log.clear();
        const ScriptInstance b = instanceOf(fx.vm, KEY);
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expose(fx.vm, a, "a");
        expose(fx.vm, b, "b");
        CHECK(script_test::completion(fx.vm, "[a.hello(), b.hello(), b.doubled, globalThis.spin].join()") ==
              "v1,v1,246,false");
        fx.files->set("s/t.js", versioned("v2"));
        const ScriptLoadResult moved = fx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(moved.status) == "Ok");
        CHECK(moved.instancesMoved == 2U);
        script_test::expectCleanShutdown(fx.vm);
    }
}

TEST_CASE("script: a load compares the script's own bytes, never those of what it imports (HR37)") {
    const script_test::LogCapture log;
    Fixture fx;
    constexpr std::string_view KEY = "res://s/a.js";
    const std::string entry = importing("v1");
    fx.files->set("s/dep.js", std::string{DEP_V1});
    fx.files->set("s/a.js", entry);
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    REQUIRE(script_test::completion(fx.vm, "a.hello()") == "d1-v1");
    const std::size_t depReads = fx.files->reads("s/dep.js");
    CHECK(depReads == 1U);

    // The same entry bytes: the generation recorded the entry's bytes, not its import's, read after them.
    fx.files->set("s/a.js", entry);
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Unchanged");

    // Only the import changed: still Unchanged, and nothing ran or was read beyond the entry.
    fx.files->set("s/dep.js", "export const word = \"d2\";\n");
    CHECK(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Unchanged");
    CHECK(fx.files->reads("s/dep.js") == depReads);
    CHECK(recordOf(fx.vm, KEY).generation == 1U);
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "d1-v1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a script file over the size cap is refused by the loader and never read by the comparison (HR40)") {
    script_test::LogCapture log;
    Fixture fx{script_test::patient(VmConfig{.heapLimitBytes = MIB})};
    constexpr std::string_view KEY = "res://s/a.js";
    fx.files->set("s/a.js", versioned("v1"));
    REQUIRE(scriptStatusLabel(fx.vm.loadScript(KEY).status) == "Ok");
    const ScriptInstance a = instanceOf(fx.vm, KEY);
    expose(fx.vm, a, "a");
    const std::size_t readsBefore = fx.files->reads("s/a.js");

    // A valid script, padded by one comment to exactly one byte over the cap: it would load, were it read.
    std::string big = versioned("v-big") + "//";
    big.append(MIB + 1 - big.size(), 'x');
    REQUIRE(big.size() == MIB + 1);
    fx.files->set("s/a.js", big);
    // The comparison reads nothing past the cap, so no trigger finds its bytes remembered: each is the loader's.
    for (int trigger = 1; trigger <= 2; ++trigger) {
        INFO("trigger ", trigger);
        log.clear();
        const ScriptLoadResult refused = fx.vm.loadScript(KEY);
        CHECK(scriptStatusLabel(refused.status) == "LoadError");
        CHECK(theOneError(log) ==
              "script: res://s/a.js: load error: TypeError: cannot load 'res://s/a.js': its 1048577 bytes are over "
              "the 1048576-byte source limit (the VM's heap limit)");
        CHECK(refused.instancesMoved == 0U);
        CHECK(fx.files->reads("s/a.js") == readsBefore);
    }
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v1");

    log.clear();
    fx.files->set("s/a.js", versioned("v2"));
    const ScriptLoadResult working = fx.vm.loadScript(KEY);
    CHECK(scriptStatusLabel(working.status) == "Ok");
    CHECK(working.instancesMoved == 1U);
    expose(fx.vm, a, "a");
    CHECK(script_test::completion(fx.vm, "a.hello()") == "v2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(fx.vm);
}
