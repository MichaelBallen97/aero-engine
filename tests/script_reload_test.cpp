// tests/script_reload_test.cpp -- task 4.3.3: script classes, instances and hot reload. HR1-HR3 are tier 0 through
// aero::script_internal (the carry rule and a module's state accessor); the later cases drive the public API and
// internal::VmAccess::defineInstanceGlobal. A TU of aero_tests, which supplies main() from test_main.cpp.
#include <aero/core/vfs.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/internal/script_carry.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
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
