// Aero Engine -- the binding runtime over a fixture table (task 4.2.1; BN1-BN5, and from the later steps BN7-BN36).
//
// BN1-BN5 cover the quickjs-free descriptor contract: how a C++ type maps to a script kind, how the two templates
// build a descriptor exactly as the generated code does, and what ComponentBindingTable::add refuses. BN7-BN14 drive
// the frozen `engine` namespace every VM holds: the class set, the detached Vec3 and Quat, the install and the
// teardown. The fixture component lives in script_binding_support.hpp.

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/component.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace engine::script::internal {
// doctest streams a failing operand through an unqualified operator<< found by ADL.
std::ostream& operator<<(std::ostream& out, FieldKind kind) { return out << static_cast<unsigned>(kind); }
}  // namespace engine::script::internal

namespace {

using engine::ComponentTypeId;
using engine::componentTypeId;
using engine::LogLevel;
using engine::script::EvalResult;
using engine::script::EvalStatus;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::internal::bindComponent;
using engine::script::internal::bindField;
using engine::script::internal::ComponentBinding;
using engine::script::internal::ComponentBindingTable;
using engine::script::internal::FieldBinding;
using engine::script::internal::FieldKind;
using engine::script::internal::fieldKindOf;
using engine::script::internal::MAX_BOUND_COMPONENTS;
using engine::script::internal::MAX_BOUND_FIELDS;
using script_test::expectCleanShutdown;
using script_test::LogCapture;
using script_test::makeBuiltinVm;
using script_test::makeProbeVm;
using script_test::patient;
using script_test::probeTable;
using script_test::ScriptProbe;
using script_test::ScriptTag;

constexpr std::size_t MIB = std::size_t{1024} * 1024U;

constexpr std::string_view REFUSED_PREFIX = "script: component binding '";

[[nodiscard]] void* dummyAddress(void* component) noexcept { return component; }

[[nodiscard]] FieldBinding field(std::string_view name) { return FieldBinding{name, FieldKind::Float, &dummyAddress}; }

// A valid one-field component under a given name and a given (fabricated or real) type id.
[[nodiscard]] ComponentBinding component(std::string_view name, ComponentTypeId type) {
    return ComponentBinding{name, type, {field("x")}};
}

// Distinct, valid type ids that name no real type: the addresses of an array's elements.
[[nodiscard]] ComponentTypeId fabricated(const std::array<char, 200>& storage, std::size_t index) {
    return ComponentTypeId{&storage[index]};
}

// Exactly one record at WARN or above, an ERROR that starts with the refusal prefix and names `name` and `rule`.
void expectRefusal(const LogCapture& log, std::string_view name, std::string_view rule) {
    const std::vector<script_test::CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    const std::string expectedHead = std::string{REFUSED_PREFIX} + std::string{name} + "' refused: ";
    CHECK(records[0].message.rfind(expectedHead, 0) == 0U);
    CHECK(records[0].message.find(rule) != std::string::npos);
}

}  // namespace

TEST_CASE("script: fieldKindOf maps the seven bindable types to seven distinct kinds (BN1)") {
    constexpr FieldKind FLOAT_KIND = fieldKindOf<float>();
    constexpr FieldKind BOOL_KIND = fieldKindOf<bool>();
    constexpr FieldKind UINT_KIND = fieldKindOf<std::uint32_t>();
    constexpr FieldKind VEC3_KIND = fieldKindOf<engine::Vec3>();
    constexpr FieldKind QUAT_KIND = fieldKindOf<engine::Quat>();
    constexpr FieldKind GUID_KIND = fieldKindOf<engine::Guid>();
    constexpr FieldKind STRING_KIND = fieldKindOf<std::string>();

    // Static: the mapping is a compile-time fact.
    static_assert(FLOAT_KIND == FieldKind::Float);
    static_assert(BOOL_KIND == FieldKind::Bool);
    static_assert(UINT_KIND == FieldKind::UInt32);
    static_assert(VEC3_KIND == FieldKind::Vec3);
    static_assert(QUAT_KIND == FieldKind::Quat);
    static_assert(GUID_KIND == FieldKind::Guid);
    static_assert(STRING_KIND == FieldKind::String);

    // Runtime: seven values, no two alike, so an integer field can never share a kind with a float.
    const std::set<FieldKind> kinds{FLOAT_KIND, BOOL_KIND, UINT_KIND, VEC3_KIND, QUAT_KIND, GUID_KIND, STRING_KIND};
    CHECK(kinds.size() == 7U);
    CHECK(FLOAT_KIND != UINT_KIND);
    CHECK(FLOAT_KIND != BOOL_KIND);
    CHECK(VEC3_KIND != QUAT_KIND);
}

TEST_CASE("script: bindField keeps the name and kind and addresses its own member (BN2)") {
    ScriptProbe probe;
    const std::array<FieldBinding, 7> fields{
        bindField<&ScriptProbe::f>("f").binding, bindField<&ScriptProbe::b>("b").binding,
        bindField<&ScriptProbe::u>("u").binding, bindField<&ScriptProbe::v>("v").binding,
        bindField<&ScriptProbe::q>("q").binding, bindField<&ScriptProbe::g>("g").binding,
        bindField<&ScriptProbe::s>("s").binding};
    const std::array<std::string_view, 7> names{"f", "b", "u", "v", "q", "g", "s"};
    const std::array<FieldKind, 7> kinds{FieldKind::Float, FieldKind::Bool, FieldKind::UInt32, FieldKind::Vec3,
                                         FieldKind::Quat,  FieldKind::Guid, FieldKind::String};
    const std::array<void*, 7> expected{&probe.f, &probe.b, &probe.u, &probe.v, &probe.q, &probe.g, &probe.s};

    std::set<void*> distinct;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        CHECK(fields[i].name == names[i]);
        CHECK(fields[i].kind == kinds[i]);
        REQUIRE(fields[i].address != nullptr);
        void* const address = fields[i].address(&probe);
        CHECK(address == expected[i]);
        distinct.insert(address);
    }
    CHECK(distinct.size() == 7U);
}

TEST_CASE("script: bindComponent keeps the name, the type id and the declaration order (BN3)") {
    const ComponentBinding probe = bindComponent<ScriptProbe>(
        "ScriptProbe", bindField<&ScriptProbe::f>("f"), bindField<&ScriptProbe::b>("b"),
        bindField<&ScriptProbe::u>("u"), bindField<&ScriptProbe::v>("v"), bindField<&ScriptProbe::q>("q"),
        bindField<&ScriptProbe::g>("g"), bindField<&ScriptProbe::s>("s"));
    CHECK(probe.scriptName == "ScriptProbe");
    CHECK(probe.type == componentTypeId<ScriptProbe>());
    CHECK(probe.type.valid());
    REQUIRE(probe.fields.size() == 7U);
    const std::array<std::string_view, 7> order{"f", "b", "u", "v", "q", "g", "s"};
    for (std::size_t i = 0; i < order.size(); ++i) {
        CHECK(probe.fields[i].name == order[i]);
    }

    const ComponentBinding tag = bindComponent<ScriptTag>("ScriptTag");
    CHECK(tag.scriptName == "ScriptTag");
    CHECK(tag.type == componentTypeId<ScriptTag>());
    CHECK(tag.type != probe.type);
    CHECK(tag.fields.empty());
}

TEST_CASE("script: ComponentBindingTable::add refuses each invalid entry with one ERROR and appends nothing (BN4)") {
    const std::array<char, 200> storage{};
    LogCapture log;
    ComponentBindingTable table;
    REQUIRE(table.add(component("Good", fabricated(storage, 0))));
    REQUIRE(table.components().size() == 1U);
    log.clear();

    struct Refusal {
        ComponentBinding binding;
        std::string_view reportedName;
        std::string_view rule;
    };
    std::vector<Refusal> refusals;
    refusals.push_back({component("", fabricated(storage, 1)), "", "name"});
    refusals.push_back({component("1a", fabricated(storage, 1)), "1a", "name"});
    refusals.push_back({component("a b", fabricated(storage, 1)), "a b", "name"});
    refusals.push_back({component("Vec3", fabricated(storage, 1)), "Vec3", "reserved"});
    refusals.push_back({component("Quat", fabricated(storage, 1)), "Quat", "reserved"});
    refusals.push_back({component("Entity", fabricated(storage, 1)), "Entity", "reserved"});
    refusals.push_back({component("Good", fabricated(storage, 1)), "Good", "name is already bound"});
    refusals.push_back({component("Other", fabricated(storage, 0)), "Other", "type is already bound"});
    refusals.push_back({component("Other", ComponentTypeId{}), "Other", "type is invalid"});
    refusals.push_back(
        {ComponentBinding{"Other", fabricated(storage, 1), {field("constructor")}}, "Other", "constructor"});
    refusals.push_back(
        {ComponentBinding{"Other", fabricated(storage, 1), {field("x"), field("x")}}, "Other", "repeated"});
    refusals.push_back({ComponentBinding{"Other", fabricated(storage, 1), {field("")}}, "Other", "field name"});
    refusals.push_back(
        {ComponentBinding{"Other", fabricated(storage, 1), {FieldBinding{"x", FieldKind::Float, nullptr}}}, "Other",
         "address"});

    for (Refusal& refusal : refusals) {
        CAPTURE(refusal.reportedName);
        log.clear();
        CHECK_FALSE(table.add(std::move(refusal.binding)));
        expectRefusal(log, refusal.reportedName, refusal.rule);
        CHECK(table.components().size() == 1U);
    }

    // The leading underscore and the dollar sign are identifier characters.
    log.clear();
    CHECK(table.add(component("_x", fabricated(storage, 1))));
    CHECK(table.add(component("$x", fabricated(storage, 2))));
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(table.components().size() == 3U);
}

TEST_CASE("script: the binding table caps components at 127 and fields at 255 (BN5)") {
    const std::array<char, 200> storage{};
    LogCapture log;
    static_assert(MAX_BOUND_COMPONENTS == 127U);
    static_assert(MAX_BOUND_FIELDS == 255U);

    // The table keeps views of its names, so they live in a vector that never reallocates and outlives it.
    std::vector<std::string> names;
    names.reserve(MAX_BOUND_COMPONENTS + 1U);
    for (std::size_t i = 0; i <= MAX_BOUND_COMPONENTS; ++i) {
        names.push_back("C" + std::to_string(i));
    }

    ComponentBindingTable table;
    for (std::size_t i = 0; i < MAX_BOUND_COMPONENTS; ++i) {
        CHECK(table.add(component(names[i], fabricated(storage, i))));
    }
    CHECK(table.components().size() == MAX_BOUND_COMPONENTS);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    const std::string& overflowName = names[MAX_BOUND_COMPONENTS];
    CHECK_FALSE(table.add(component(overflowName, fabricated(storage, MAX_BOUND_COMPONENTS))));
    expectRefusal(log, overflowName, "127");
    CHECK(table.components().size() == MAX_BOUND_COMPONENTS);

    // Fields: 255 distinct names are accepted, 256 are refused.
    std::vector<std::string> fieldNames;
    fieldNames.reserve(MAX_BOUND_FIELDS + 1U);
    for (std::size_t i = 0; i <= MAX_BOUND_FIELDS; ++i) {
        fieldNames.push_back("f" + std::to_string(i));
    }
    const auto manyFields = [&](std::size_t count) {
        std::vector<FieldBinding> fields;
        for (std::size_t i = 0; i < count; ++i) {
            fields.push_back(field(fieldNames[i]));
        }
        return fields;
    };

    ComponentBindingTable wide;
    log.clear();
    CHECK(wide.add(ComponentBinding{"Wide", fabricated(storage, 0), manyFields(MAX_BOUND_FIELDS)}));
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK_FALSE(wide.add(ComponentBinding{"Wider", fabricated(storage, 1), manyFields(MAX_BOUND_FIELDS + 1U)}));
    expectRefusal(log, "Wider", "255");
    CHECK(wide.components().size() == 1U);
}

// No operator<< for EvalStatus here: script_vm_test.cpp already defines the printer in this binary, and a second
// definition is a duplicate symbol at link time.

namespace {

// Runs `source`, requires it to complete, and returns String(completion value).
[[nodiscard]] std::string evalOk(Vm& vm, std::string_view source) {
    const EvalResult result = vm.evaluateScript(source, "bn.js");
    INFO("script: " << source);
    INFO("error: " << result.error.name << ": " << result.error.message);
    CHECK(result.status == EvalStatus::Ok);
    return result.completion;
}

// The shared script prologue of the throw checks: `t(f)` is "Name: message" of what f threw.
constexpr std::string_view THROWS =
    "const t = (f) => { try { f(); return 'no throw'; } catch (e) { return e.name + ': ' + e.message; } };";

// `body` runs as a function body (so every case may declare what it likes), with `t` in scope.
[[nodiscard]] std::string iife(std::string_view body) {
    return "(() => { " + std::string{THROWS} + " " + std::string{body} + " })()";
}

struct ScriptRow {
    std::string_view script;
    std::string_view expected;
};

}  // namespace

TEST_CASE("script: engine, its classes and their prototypes are frozen, and engine cannot be replaced (BN7)") {
    const LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    const std::string completion = evalOk(vm, iife(R"js(
        const out = [];
        const everything = [engine];
        for (const name of Object.keys(engine)) { everything.push(engine[name], engine[name].prototype); }
        out.push(everything.every((o) => Object.isFrozen(o)));
        const d = Object.getOwnPropertyDescriptor(globalThis, 'engine');
        out.push(d.writable, d.configurable, d.enumerable);
        out.push(t(() => { engine = 1; }).split(':')[0]);
        out.push(t(() => { delete globalThis.engine; }).split(':')[0]);
        out.push(t(() => { engine.Vec3 = 1; }).split(':')[0]);
        out.push(t(() => { Object.defineProperty(engine.Vec3.prototype, 'x', { value: 1 }); }).split(':')[0]);
        out.push(t(() => { engine.ScriptProbe.prototype.extra = 1; }).split(':')[0]);
        out.push(typeof engine.Vec3, Object.keys(engine).length, new engine.Vec3(1, 2, 3).x);
        return out.join();
    )js"));
    CHECK(completion == "true,false,false,false,TypeError,TypeError,TypeError,TypeError,TypeError,function,5,1");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: Object.keys(engine) is the table's script names, then Vec3, Quat and Entity (BN8)") {
    const LogCapture log;
    {
        const auto table = probeTable();
        Vm vm = makeProbeVm(table);
        CHECK(evalOk(vm, "Object.keys(engine).join()") == "ScriptProbe,ScriptTag,Vec3,Quat,Entity");
        // Nothing leaked onto the global object beside the namespace.
        CHECK(evalOk(vm, "[typeof Vec3, typeof Quat, typeof Entity, typeof ScriptProbe, typeof ScriptTag].join()") ==
              "undefined,undefined,undefined,undefined,undefined");
        expectCleanShutdown(vm);
    }
    std::string expected;
    for (const engine::script::internal::ComponentBinding& binding :
         engine::script::internal::builtinComponentBindings().components()) {
        expected += std::string{binding.scriptName} + ",";
    }
    expected += "Vec3,Quat,Entity";
    Vm vm = makeBuiltinVm();
    CHECK(evalOk(vm, "Object.keys(engine).join()") == expected);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a component class and Entity cannot be constructed, and Vec3 needs new (BN9)") {
    const LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    const std::string probe = "TypeError: engine.ScriptProbe cannot be constructed; use entity.add(engine.ScriptProbe)";
    const std::string tag = "TypeError: engine.ScriptTag cannot be constructed; use entity.add(engine.ScriptTag)";
    const std::string entity = "TypeError: engine.Entity cannot be constructed by a script";
    const std::string needsNew = "TypeError: must be called with new";
    const std::string completion = evalOk(vm, iife(R"js(
        return [t(() => new engine.ScriptProbe()), t(() => engine.ScriptProbe()), t(() => new engine.ScriptTag()),
                t(() => new engine.Entity()), t(() => engine.Entity()), t(() => engine.Vec3(1, 2, 3)),
                t(() => engine.Quat(1, 2, 3, 4))].join('|');
    )js"));
    CHECK(completion ==
          probe + "|" + probe + "|" + tag + "|" + entity + "|" + entity + "|" + needsNew + "|" + needsNew);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a constructed Vec3 or Quat reads back what it was given, narrowed to a float (BN10)") {
    const LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    const std::array<ScriptRow, 17> rows{{
        {"const v = new engine.Vec3(); return [v.x, v.y, v.z].join();", "0,0,0"},
        {"const q = new engine.Quat(); return [q.x, q.y, q.z, q.w].join();", "0,0,0,1"},
        {"const v = new engine.Vec3(1.5, -2.25, 1024); return [v.x, v.y, v.z].join();", "1.5,-2.25,1024"},
        {"const q = new engine.Quat(0.5, 0.25, -8, 2); return [q.x, q.y, q.z, q.w].join();", "0.5,0.25,-8,2"},
        {"const q = new engine.Quat(1, 2, 3); return [q.x, q.y, q.z, q.w].join();", "1,2,3,1"},
        {"const v = new engine.Vec3(undefined, 5); return [v.x, v.y, v.z].join();", "0,5,0"},
        {"return new engine.Vec3(0.1, 0, 0).x;", "0.10000000149011612"},
        {"const v = new engine.Vec3(1e300, -1e300, 0); return [v.x, v.y, v.z].join();", "Infinity,-Infinity,0"},
        {"return [Object.is(new engine.Vec3(NaN).x, NaN), Object.is(new engine.Vec3(-0).x, -0), "
         "Object.is(new engine.Vec3(0).x, -0)].join();",
         "true,true,false"},
        {"return t(() => new engine.Vec3(1, '2'));", "TypeError: new engine.Vec3: y: expected a number, got a string"},
        {"return t(() => new engine.Vec3(1, 2, null));", "TypeError: new engine.Vec3: z: expected a number, got null"},
        {"return t(() => new engine.Quat(1, 2, 3, 'w'));",
         "TypeError: new engine.Quat: w: expected a number, got a string"},
        // The axis accessors narrow the same way and refuse the same values.
        {"const v = new engine.Vec3(); v.x = 0.1; v.y = 1e300; v.z = -0; "
         "return [v.x, v.y, Object.is(v.z, -0)].join();",
         "0.10000000149011612,Infinity,true"},
        {"const q = new engine.Quat(); q.w = 2; q.x = NaN; return [q.w, Number.isNaN(q.x)].join();", "2,true"},
        {"const v = new engine.Vec3(1, 2, 3); const m = t(() => { v.z = '1'; }); "
         "return m + ' ' + [v.x, v.y, v.z].join();",
         "TypeError: engine.Vec3.z: expected a number, got a string 1,2,3"},
        {"const q = new engine.Quat(1, 2, 3, 4); return t(() => { q.w = undefined; }) + ' ' + q.w;",
         "TypeError: engine.Quat.w: expected a number, got undefined 4"},
        {"const v = new engine.Vec3(); return t(() => { v.x = true; });",
         "TypeError: engine.Vec3.x: expected a number, got a boolean"},
    }};
    for (const ScriptRow& row : rows) {
        INFO("script: " << row.script);
        CHECK(evalOk(vm, iife(row.script)) == row.expected);
    }
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: set and copy write whole vectors or nothing, and refuse a foreign receiver or argument (BN11)") {
    const LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    const std::array<ScriptRow, 14> rows{{
        {"const v = new engine.Vec3(); const r = v.set(1, 2, 3); return [r === v, v.x, v.y, v.z].join();",
         "true,1,2,3"},
        {"const q = new engine.Quat(); const r = q.set(1, 2, 3, 4); return [r === q, q.x, q.y, q.z, q.w].join();",
         "true,1,2,3,4"},
        // A refused set writes none of its axes, whichever argument was the bad one.
        {"const v = new engine.Vec3(7, 8, 9); const m = t(() => v.set(1, 2)); return m + ' ' + [v.x, v.y, v.z].join();",
         "TypeError: engine.Vec3.set: z: expected a number, got undefined 7,8,9"},
        {"const v = new engine.Vec3(7, 8, 9); const m = t(() => v.set(1, 2, 'z')); "
         "return m + ' ' + [v.x, v.y, v.z].join();",
         "TypeError: engine.Vec3.set: z: expected a number, got a string 7,8,9"},
        {"const q = new engine.Quat(1, 2, 3, 4); const m = t(() => q.set(5, 6, 7)); "
         "return m + ' ' + [q.x, q.y, q.z, q.w].join();",
         "TypeError: engine.Quat.set: w: expected a number, got undefined 1,2,3,4"},
        {"const a = new engine.Vec3(); const b = new engine.Vec3(4, 5, 6); const r = a.copy(b); "
         "return [r === a, a.x, a.y, a.z].join();",
         "true,4,5,6"},
        {"const a = new engine.Quat(); const b = new engine.Quat(1, 2, 3, 4); a.copy(b); "
         "return [a.x, a.y, a.z, a.w].join();",
         "1,2,3,4"},
        {"const v = new engine.Vec3(1, 2, 3); const m = t(() => v.copy(new engine.Quat())); "
         "return m + ' ' + [v.x, v.y, v.z].join();",
         "TypeError: engine.Vec3.copy: expected an engine.Vec3, got an engine.Quat 1,2,3"},
        {"const q = new engine.Quat(1, 2, 3, 4); return t(() => q.copy(new engine.Vec3()));",
         "TypeError: engine.Quat.copy: expected an engine.Quat, got an engine.Vec3"},
        {"const v = new engine.Vec3(1, 2, 3); return [t(() => v.copy({ x: 7, y: 8, z: 9 })), t(() => v.copy([7, 8, "
         "9])), "
         "t(() => v.copy()), [v.x, v.y, v.z].join()].join('|');",
         "TypeError: engine.Vec3.copy: expected an engine.Vec3, got an object|"
         "TypeError: engine.Vec3.copy: expected an engine.Vec3, got an object|"
         "TypeError: engine.Vec3.copy: expected an engine.Vec3, got undefined|1,2,3"},
        {"const v = new engine.Vec3(); const q = new engine.Quat(); "
         "return [v instanceof engine.Vec3, v instanceof engine.Quat, q instanceof engine.Quat, "
         "q instanceof engine.Vec3].join();",
         "true,false,true,false"},
        // A getter or method lifted off the prototype names the class it wants.
        {"return [t(() => engine.Vec3.prototype.x), t(() => engine.Quat.prototype.w)].join('|');",
         "TypeError: engine.Vec3.x: the receiver is not an engine.Vec3|"
         "TypeError: engine.Quat.w: the receiver is not an engine.Quat"},
        {"const get = Object.getOwnPropertyDescriptor(engine.Vec3.prototype, 'x').get; "
         "return [t(() => get.call(new engine.Quat())), t(() => engine.Vec3.prototype.set.call({}, 1, 2, 3)), "
         "t(() => engine.Quat.prototype.copy.call(new engine.Vec3(), new engine.Quat()))].join('|');",
         "TypeError: engine.Vec3.x: the receiver is not an engine.Vec3|"
         "TypeError: engine.Vec3.set: the receiver is not an engine.Vec3|"
         "TypeError: engine.Quat.copy: the receiver is not an engine.Quat"},
        {"return Object.keys(Object.getOwnPropertyDescriptors(engine.Vec3.prototype)).join();",
         "x,y,z,set,copy,constructor"},
    }};
    for (const ScriptRow& row : rows) {
        INFO("script: " << row.script);
        CHECK(evalOk(vm, iife(row.script)) == row.expected);
    }
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: no script code runs inside a Vec3 or Quat binding (BN12)") {
    const LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    // Every hostile value is refused by tag or class, and not one of its hooks runs: not a conversion, not a property
    // read, not a Proxy trap (the handler is itself a Proxy that counts the lookups).
    const std::string refusals = evalOk(vm, iife(R"js(
        let hooks = 0;
        const hostile = [
            { valueOf() { hooks++; return 1; } },
            { toString() { hooks++; return '1'; } },
            { [Symbol.toPrimitive]() { hooks++; return 1; } },
            { get x() { hooks++; return 1; }, y: 2, z: 3, w: 4 },
            new Proxy({ x: 1, y: 2, z: 3 }, new Proxy({}, { get(target, trap) { hooks++; return undefined; } })),
        ];
        let attempts = 0;
        let refused = 0;
        const attempt = (f) => { attempts++; try { f(); } catch (e) { if (e instanceof TypeError) { refused++; } } };
        for (const h of hostile) {
            attempt(() => new engine.Vec3(h));
            attempt(() => new engine.Vec3(1, 2, h));
            attempt(() => new engine.Quat(h));
            attempt(() => { new engine.Vec3().x = h; });
            attempt(() => { new engine.Quat().w = h; });
            attempt(() => new engine.Vec3().set(h, 1, 2));
            attempt(() => new engine.Vec3().set(1, 2, h));
            attempt(() => new engine.Vec3().copy(h));
            attempt(() => new engine.Quat().copy(h));
        }
        return [attempts, refused, hooks].join();
    )js"));
    CHECK(refusals == "45,45,0");

    // Building and reading a vector sets no property: a setter a script put on Object.prototype never runs.
    const std::string polluted = evalOk(vm, iife(R"js(
        let hooks = 0;
        for (const name of ['x', 'constructor', 'prototype']) {
            Object.defineProperty(Object.prototype, name, { set(v) { hooks++; }, get() { return undefined; },
                                                             configurable: true });
        }
        const v = new engine.Vec3(1, 2, 3);
        const q = new engine.Quat();
        v.set(4, 5, 6);
        q.copy(new engine.Quat(0, 0, 0, 1));
        return [v.x, v.y, v.z, q.w, hooks].join();
    )js"));
    CHECK(polluted == "4,5,6,1,0");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: fifty VMs holding vectors in globals, arrays and cycles all tear down clean (BN13)") {
    const LogCapture log;
    const auto table = probeTable();
    for (int round = 0; round < 50; ++round) {
        Vm vm = makeProbeVm(table);
        const std::string held = evalOk(vm, R"js(
            globalThis.single = new engine.Vec3(1, 2, 3);
            globalThis.many = Array.from({ length: 1000 }, () => new engine.Quat());
            globalThis.cycle = {};
            globalThis.cycle.self = globalThis.cycle;
            globalThis.cycle.v = new engine.Vec3();
            globalThis.cls = engine.ScriptProbe;
            [many.length, single.y, cycle.self.v.z, cls === engine.ScriptProbe].join();
        )js");
        CHECK(held == "1000,2,0,true");
        expectCleanShutdown(vm);
    }
    for (int round = 0; round < 5; ++round) {
        Vm vm = makeBuiltinVm();
        CHECK(evalOk(vm, "globalThis.keep = [new engine.Vec3(), new engine.Quat()]; keep.length") == "2");
        expectCleanShutdown(vm);
    }
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
}

TEST_CASE("script: a heap that runs out while vectors are made is OutOfMemory, and the VM works on (BN14)") {
    const LogCapture log;
    const auto table = probeTable();
    VmConfig config;
    config.heapLimitBytes = 4 * MIB;
    Vm vm = makeProbeVm(table, patient(config));
    const EvalResult filled =
        vm.evaluateScript("(() => { const a = []; for (;;) { a.push(new engine.Vec3()); } })()", "bn14.js");
    CHECK(filled.status == EvalStatus::OutOfMemory);
    // The array was function-local, so the heap is empty again and the failed allocation left no object whose opaque
    // is null: a read of a fresh vector, and of a new one, completes.
    CHECK(evalOk(vm, "new engine.Vec3(1, 2, 3).x") == "1");
    CHECK(evalOk(vm, "new engine.Quat().w") == "1");
    expectCleanShutdown(vm);
}

// ---- BN15-BN30: the World binding, engine.Entity, component objects and the scalar, Guid and string fields
// -----------

namespace {

using engine::Entity;
using engine::World;
using engine::script::BindStatus;
using engine::script::bindStatusLabel;
using script_test::completion;
using script_test::ProbeScene;
using script_test::registerProbe;

[[nodiscard]] std::string_view label(BindStatus status) { return bindStatusLabel(status); }

// Exactly `count` records at ERROR or above, each starting with "script: ".
void expectErrors(const LogCapture& log, std::size_t count) {
    const std::vector<script_test::CapturedRecord> records = log.atLeast(LogLevel::Warn);
    CHECK(records.size() == count);
    for (const script_test::CapturedRecord& record : records) {
        CHECK(record.level == LogLevel::Error);
        CHECK(record.message.starts_with("script: "));
    }
}

}  // namespace

TEST_CASE("script: bindWorld refuses a World that lacks a bound component, and every refusal is one ERROR (BN15)") {
    LogCapture log;
    const auto table = probeTable();
    Vm vm = makeProbeVm(table);
    World world;
    registerProbe(world);
    const Entity entity = world.create();

    Vm invalid;
    CHECK(label(invalid.bindWorld(world)) == "InvalidVm");
    expectErrors(log, 1U);
    log.clear();

    BindStatus foreign = BindStatus::Ok;
    std::thread{[&] { foreign = vm.bindWorld(world); }}.join();
    CHECK(label(foreign) == "WrongThread");
    expectErrors(log, 1U);
    log.clear();

    // A good binding first, so the refusals below have a binding to leave alone.
    CHECK(label(vm.bindWorld(world)) == "Ok");
    CHECK(label(vm.defineEntityGlobal("e", entity)) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    World bare;  // a constructed World holds the ten built-ins, not the fixture components
    CHECK(label(vm.bindWorld(bare)) == "UnregisteredComponent");
    expectErrors(log, 1U);
    CHECK(log.atLeast(LogLevel::Warn).front().message.find("engine.ScriptProbe") != std::string::npos);
    log.clear();

    World source;
    registerProbe(source);
    const World taken{std::move(source)};
    CHECK(label(vm.bindWorld(source)) == "UnregisteredComponent");  // NOLINT(bugprone-use-after-move) -- deliberate
    expectErrors(log, 1U);
    log.clear();
    CHECK(completion(vm, "e.alive") == "true");  // the earlier binding still serves

    // unbindWorld: an invalid Vm and another thread are one ERROR each, twice in a row is silent.
    invalid.unbindWorld();
    expectErrors(log, 1U);
    log.clear();
    std::thread{[&] { vm.unbindWorld(); }}.join();
    expectErrors(log, 1U);
    log.clear();
    CHECK(completion(vm, "e.alive") == "true");
    vm.unbindWorld();
    vm.unbindWorld();
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(vm, "e.alive") == "false");

    std::set<std::string_view> labels;
    for (std::uint8_t status = 0; status <= static_cast<std::uint8_t>(BindStatus::OutOfMemory); ++status) {
        labels.insert(label(static_cast<BindStatus>(status)));
    }
    CHECK(labels.size() == 8U);
    CHECK(labels.count("Unknown") == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: defineEntityGlobal refuses in order, one ERROR each, and defines without running a setter (BN16)") {
    LogCapture log;
    ProbeScene scene;
    const Entity second = scene.world.create();

    Vm invalid;
    CHECK(label(invalid.defineEntityGlobal("x", scene.entity)) == "InvalidVm");
    expectErrors(log, 1U);
    log.clear();
    BindStatus foreign = BindStatus::Ok;
    std::thread{[&] { foreign = scene.vm.defineEntityGlobal("x", scene.entity); }}.join();
    CHECK(label(foreign) == "WrongThread");
    expectErrors(log, 1U);
    log.clear();

    const std::string_view embeddedNul{"x\0y", 3};
    for (const std::string_view name : {std::string_view{""}, std::string_view{"1a"}, std::string_view{"a b"},
                                        std::string_view{"engine"}, embeddedNul}) {
        INFO("name size: " << name.size());
        CHECK(label(scene.vm.defineEntityGlobal(name, scene.entity)) == "InvalidName");
        expectErrors(log, 1U);
        log.clear();
    }
    CHECK(completion(scene.vm, "typeof x + typeof a + typeof engine") == "undefinedundefinedobject");

    // A script-made global that cannot be redefined.
    completion(scene.vm, "Object.defineProperty(globalThis, 'fixed', { value: 1 }); 0");
    CHECK(label(scene.vm.defineEntityGlobal("fixed", scene.entity)) == "InvalidName");
    expectErrors(log, 1U);
    log.clear();
    CHECK(completion(scene.vm, "fixed") == "1");

    // Success: a writable, configurable, enumerable data property holding an engine.Entity.
    CHECK(completion(scene.vm,
                     "const d = Object.getOwnPropertyDescriptor(globalThis, 'e');"
                     "[e instanceof engine.Entity, e.alive, d.writable, d.configurable, d.enumerable].join()") ==
          "true,true,true,true,true");
    // The same name again replaces it, and a setter a script installed for a new name never runs.
    completion(scene.vm,
               "globalThis.old = e; globalThis.ran = 0;"
               "Object.defineProperty(globalThis, 'watched', { configurable: true,"
               "  get() { return 1; }, set(v) { globalThis.ran += 1; } }); 0");
    CHECK(label(scene.vm.defineEntityGlobal("e", second)) == "Ok");
    CHECK(label(scene.vm.defineEntityGlobal("watched", second)) == "Ok");
    CHECK(completion(scene.vm, "[e !== old, e.alive, ran, watched instanceof engine.Entity].join()") ==
          "true,true,0,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    // The remaining refusals, in order: no World, then a dead entity, then a global object that cannot grow.
    scene.vm.unbindWorld();
    CHECK(label(scene.vm.defineEntityGlobal("x", second)) == "NoWorld");
    expectErrors(log, 1U);
    log.clear();
    CHECK(label(scene.vm.bindWorld(scene.world)) == "Ok");
    CHECK(scene.world.destroy(second));
    CHECK(label(scene.vm.defineEntityGlobal("x", second)) == "DeadEntity");
    expectErrors(log, 1U);
    log.clear();
    completion(scene.vm, "Object.preventExtensions(globalThis); 0");
    CHECK(label(scene.vm.defineEntityGlobal("fresh", scene.entity)) == "InvalidName");
    expectErrors(log, 1U);
    log.clear();
    CHECK(completion(scene.vm, "typeof fresh") == "undefined");
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: has, get, add and remove follow World::add's replace and never return undefined (BN17)") {
    const LogCapture log;
    ProbeScene scene;
    const std::string removed = completion(scene.vm, iife(R"js(
        return [e.remove(engine.ScriptProbe), e.has(engine.ScriptProbe), e.get(engine.ScriptProbe)].map(String).join();
    )js"));
    CHECK(removed == "true,false,null");
    CHECK(scene.world.get<ScriptProbe>(scene.entity) == nullptr);

    const std::string added = completion(scene.vm, iife(R"js(
        const c = e.add(engine.ScriptProbe);
        return [c instanceof engine.ScriptProbe, e.has(engine.ScriptProbe)].join();
    )js"));
    CHECK(added == "true,true");
    CHECK(scene.probe() == ScriptProbe{});

    // The World's own semantics: adding to an entity that holds one replaces it with the defaults (A13).
    scene.probe().f = 9.0F;
    scene.probe().s = "changed";
    completion(scene.vm, "e.add(engine.ScriptProbe); 0");
    CHECK(scene.probe() == ScriptProbe{});

    scene.probe().f = 4.0F;
    const std::string twice = completion(scene.vm, iife(R"js(
        const a = e.get(engine.ScriptProbe);
        const b = e.get(engine.ScriptProbe);
        return [a === b, a.f === b.f, a.f, a instanceof engine.ScriptProbe].join();
    )js"));
    CHECK(twice == "false,true,4,true");

    const std::string gone = completion(scene.vm, iife(R"js(
        return [e.remove(engine.ScriptProbe), e.remove(engine.ScriptProbe), e.get(engine.ScriptProbe)]
            .map(String).join();
    )js"));
    CHECK(gone == "true,false,null");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: an entity method identifies a component class by identity, never by its properties (BN18)") {
    const LogCapture log;
    ProbeScene scene;
    const std::string mismatches = completion(scene.vm, iife(R"js(
        const bad = [
            [undefined, 'undefined'], [{}, 'an object'], [engine.Vec3, 'a function'], [engine.Entity, 'a function'],
            [e.get(engine.ScriptProbe), 'an engine.ScriptProbe'], [function ScriptProbe() {}, 'a function'],
            [new Proxy(engine.ScriptProbe, {}), 'a function'], [null, 'null'], [1, 'a number'], ['x', 'a string'],
            [true, 'a boolean'], [Symbol('s'), 'a symbol'], [1n, 'a bigint'], [new engine.Vec3(), 'an engine.Vec3'],
        ];
        const out = [];
        for (const m of ['has', 'get', 'add', 'remove']) {
            for (const [value, actual] of bad) {
                const want = 'TypeError: engine.Entity.' + m + ': expected an engine component class, got ' + actual;
                const got = t(() => e[m](value));
                if (got !== want) { out.push(m + ' -> ' + got); }
            }
            const none = t(() => e[m]());
            if (!none.endsWith('got undefined')) { out.push(m + ' with no argument -> ' + none); }
        }
        return out.join('|');
    )js"));
    CHECK(mismatches.empty());
    CHECK(scene.probe() == ScriptProbe{});  // none of them added, replaced or removed anything
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a Float field reads exactly, writes narrowed to a float, and refuses a non-number (BN19)") {
    const LogCapture log;
    ProbeScene scene;
    CHECK(completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); c.f") == "0.5");

    completion(scene.vm, "c.f = 2.25; 0");
    CHECK(scene.probe().f == 2.25F);
    CHECK(completion(scene.vm, "c.f") == "2.25");
    completion(scene.vm, "c.f = -0; 0");
    CHECK(scene.probe().f == 0.0F);
    CHECK(std::signbit(scene.probe().f));
    CHECK(completion(scene.vm, "Object.is(c.f, -0)") == "true");
    completion(scene.vm, "c.f = NaN; 0");
    CHECK(std::isnan(scene.probe().f));
    completion(scene.vm, "c.f = Infinity; 0");
    CHECK(std::isinf(scene.probe().f));
    CHECK(scene.probe().f > 0.0F);
    completion(scene.vm, "c.f = -1e300; 0");
    CHECK(std::isinf(scene.probe().f));
    CHECK(scene.probe().f < 0.0F);
    completion(scene.vm, "c.f = 1e300; 0");
    CHECK(std::isinf(scene.probe().f));
    CHECK(scene.probe().f > 0.0F);
    completion(scene.vm, "c.f = 0.1; 0");
    CHECK(scene.probe().f == static_cast<float>(0.1));
    CHECK(completion(scene.vm, "c.f") == "0.10000000149011612");

    completion(scene.vm, "c.f = 3.5; 0");
    const std::array<ScriptRow, 5> wrong{{{"'1'", "a string"},
                                          {"true", "a boolean"},
                                          {"null", "null"},
                                          {"undefined", "undefined"},
                                          {"({})", "an object"}}};
    for (const ScriptRow& row : wrong) {
        INFO("value: " << row.script);
        CHECK(completion(scene.vm, iife("return t(() => { c.f = " + std::string{row.script} + "; });")) ==
              "TypeError: engine.ScriptProbe.f: expected a number, got " + std::string{row.expected});
        CHECK(scene.probe().f == 3.5F);
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a Bool field takes a boolean and nothing truthy (BN20)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); c.b = false; 0");
    CHECK_FALSE(scene.probe().b);
    CHECK(completion(scene.vm, "c.b") == "false");
    completion(scene.vm, "c.b = true; 0");
    CHECK(scene.probe().b);
    CHECK(completion(scene.vm, "c.b") == "true");

    scene.probe().b = false;
    const std::array<ScriptRow, 5> wrong{
        {{"0", "a number"}, {"1", "a number"}, {"'true'", "a string"}, {"null", "null"}, {"undefined", "undefined"}}};
    for (const ScriptRow& row : wrong) {
        INFO("value: " << row.script);
        CHECK(completion(scene.vm, iife("return t(() => { c.b = " + std::string{row.script} + "; });")) ==
              "TypeError: engine.ScriptProbe.b: expected a boolean, got " + std::string{row.expected});
        CHECK_FALSE(scene.probe().b);
    }
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a UInt32 field takes an integral number in range, never a wrapped one (BN21)") {
    const LogCapture log;
    ProbeScene scene;
    CHECK(completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); c.u") == "7");
    completion(scene.vm, "c.u = 4294967295; 0");
    CHECK(scene.probe().u == 4294967295U);
    CHECK(completion(scene.vm, "c.u") == "4294967295");
    completion(scene.vm, "c.u = 0; 0");
    CHECK(scene.probe().u == 0U);
    scene.probe().u = 9;
    completion(scene.vm, "c.u = -0; 0");
    CHECK(scene.probe().u == 0U);

    scene.probe().u = 11;
    const std::array<ScriptRow, 5> range{
        {{"1.5", "1.5"}, {"-1", "-1"}, {"4294967296", "4294967296"}, {"NaN", "NaN"}, {"Infinity", "Infinity"}}};
    for (const ScriptRow& row : range) {
        INFO("value: " << row.script);
        CHECK(completion(scene.vm, iife("return t(() => { c.u = " + std::string{row.script} + "; });")) ==
              "RangeError: engine.ScriptProbe.u: expected an integer in [0, 4294967295], got " +
                  std::string{row.expected});
        CHECK(scene.probe().u == 11U);
    }
    CHECK(completion(scene.vm, iife("return t(() => { c.u = -Infinity; });")) ==
          "RangeError: engine.ScriptProbe.u: expected an integer in [0, 4294967295], got -Infinity");
    const std::array<ScriptRow, 3> wrong{{{"'1'", "a string"}, {"1n", "a bigint"}, {"true", "a boolean"}}};
    for (const ScriptRow& row : wrong) {
        INFO("value: " << row.script);
        CHECK(completion(scene.vm, iife("return t(() => { c.u = " + std::string{row.script} + "; });")) ==
              "TypeError: engine.ScriptProbe.u: expected a number, got " + std::string{row.expected});
        CHECK(scene.probe().u == 11U);
    }
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a Guid field reads 32 lowercase digits and writes only 32 hex digits (BN22)") {
    const LogCapture log;
    ProbeScene scene;
    CHECK(completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); c.g") == std::string(32, '0'));
    const engine::Guid known{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    scene.probe().g = known;
    CHECK(completion(scene.vm, "c.g") == "0123456789abcdeffedcba9876543210");

    scene.probe().g = engine::Guid{};
    completion(scene.vm, "c.g = '0123456789ABCDEFFEDCBA9876543210'; 0");  // upper case is accepted
    CHECK(scene.probe().g == known);
    scene.probe().g = engine::Guid{};
    completion(scene.vm, "c.g = '0123456789abcdeffedcba9876543210'; 0");
    CHECK(scene.probe().g == known);

    const std::string refused = "RangeError: engine.ScriptProbe.g: expected 32 hex digits";
    const std::array<std::string_view, 4> bad{
        "'01234567-89ab-cdef-fedc-ba9876543210'", "'0123456789abcdeffedcba987654321'",
        "'0123456789abcdeffedcba98765432100'", "'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'"};
    for (const std::string_view value : bad) {
        INFO("value: " << value);
        CHECK(completion(scene.vm, iife("return t(() => { c.g = " + std::string{value} + "; });")) == refused);
        CHECK(scene.probe().g == known);
    }
    CHECK(completion(scene.vm, iife("return t(() => { c.g = 5; });")) ==
          "TypeError: engine.ScriptProbe.g: expected a string, got a number");
    CHECK(scene.probe().g == known);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a String field round-trips UTF-8 and an embedded U+0000, and takes only a string (BN23)") {
    const LogCapture log;
    ProbeScene scene;
    CHECK(completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); c.s") == "probe");
    completion(scene.vm, "c.s = ''; 0");
    CHECK(scene.probe().s.empty());
    CHECK(completion(scene.vm, "c.s.length") == "0");
    completion(scene.vm, "c.s = 'plain ascii'; 0");
    CHECK(scene.probe().s == "plain ascii");
    completion(scene.vm, R"js(c.s = '\u00e9\u6f22\u5b57'; 0)js");  // e-acute and two CJK characters
    CHECK(scene.probe().s == "\xC3\xA9\xE6\xBC\xA2\xE5\xAD\x97");
    CHECK(completion(scene.vm, "c.s.length + ':' + c.s.charCodeAt(0).toString(16)") == "3:e9");
    completion(scene.vm, R"js(c.s = 'a\u0000b'; 0)js");
    CHECK(scene.probe().s == std::string("a\0b", 3));
    CHECK(completion(scene.vm, "c.s.length + ':' + c.s.charCodeAt(1)") == "3:0");

    scene.probe().s = "kept";
    const std::array<ScriptRow, 3> wrong{
        {{"5", "a number"}, {"new String('x')", "an object"}, {"Symbol('s')", "a symbol"}}};
    for (const ScriptRow& row : wrong) {
        INFO("value: " << row.script);
        CHECK(completion(scene.vm, iife("return t(() => { c.s = " + std::string{row.script} + "; });")) ==
              "TypeError: engine.ScriptProbe.s: expected a string, got " + std::string{row.expected});
        CHECK(scene.probe().s == "kept");
    }
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: after the entity is destroyed every access throws and nothing reaches the recycled slot (BN24)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); 0");
    CHECK(scene.world.destroy(scene.entity));

    const std::string after = completion(scene.vm, iife(R"js(
        const out = [];
        out.push(t(() => c.f), t(() => { c.f = 1; }), t(() => c.s), t(() => { c.g = '0'.repeat(32); }));
        for (const m of ['has', 'get', 'add', 'remove']) { out.push(t(() => e[m](engine.ScriptProbe))); }
        out.push(e.alive);
        return out.join('|');
    )js"));
    CHECK(after ==
          "TypeError: engine.ScriptProbe.f: the entity is not alive|TypeError: engine.ScriptProbe.f: the entity is "
          "not alive|TypeError: engine.ScriptProbe.s: the entity is not alive|TypeError: engine.ScriptProbe.g: the "
          "entity is not alive|TypeError: engine.Entity.has: the entity is not alive|TypeError: engine.Entity.get: "
          "the entity is not alive|TypeError: engine.Entity.add: the entity is not alive|TypeError: "
          "engine.Entity.remove: the entity is not alive|false");

    // The next entity takes the recycled index with a newer generation; the stale object must not reach it.
    const Entity fresh = scene.world.create();
    CHECK(fresh.index == scene.entity.index);
    CHECK(fresh != scene.entity);
    ScriptProbe seeded;
    seeded.f = 9.0F;
    seeded.s = "fresh";
    REQUIRE(scene.world.add<ScriptProbe>(fresh, seeded) != nullptr);
    CHECK(completion(scene.vm, iife("return t(() => { c.f = 1; }) + '|' + t(() => c.f);")) ==
          "TypeError: engine.ScriptProbe.f: the entity is not alive|TypeError: engine.ScriptProbe.f: the entity is "
          "not alive");
    CHECK(completion(scene.vm, "e.alive") == "false");
    // The value is checked before the World is read, so a wrong value on a dead entity names the value.
    CHECK(completion(scene.vm, iife("return t(() => { c.f = 'x'; });")) ==
          "TypeError: engine.ScriptProbe.f: expected a number, got a string");
    const ScriptProbe* const kept = scene.world.get<ScriptProbe>(fresh);
    REQUIRE(kept != nullptr);
    CHECK(*kept == seeded);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a component object names the entity and the type, not one stored instance (BN25)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); 0");
    scene.probe().f = 9.0F;
    CHECK(completion(scene.vm, "c.f") == "9");
    CHECK(completion(scene.vm, "e.remove(engine.ScriptProbe)") == "true");

    const std::string removed = completion(scene.vm, iife("return [t(() => c.f), t(() => { c.f = 1; })].join('|');"));
    CHECK(removed ==
          "TypeError: engine.ScriptProbe.f: the entity has no engine.ScriptProbe|TypeError: "
          "engine.ScriptProbe.f: the entity has no engine.ScriptProbe");
    CHECK(scene.world.get<ScriptProbe>(scene.entity) == nullptr);

    completion(scene.vm, "e.add(engine.ScriptProbe); 0");
    CHECK(completion(scene.vm, "c.f") == "0.5");  // the same object, the new defaults
    CHECK(completion(scene.vm, "c.s") == "probe");
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: an entity object made under an earlier World binding never reaches a later one (BN26)") {
    const LogCapture log;
    ProbeScene scene;
    World other = script_test::makeProbeWorld();
    const Entity twin = script_test::makeProbeEntity(other);
    REQUIRE(twin == scene.entity);  // the same index and generation, in another World
    other.get<ScriptProbe>(twin)->f = 7.0F;

    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); 0");
    scene.vm.unbindWorld();
    const std::string unbound = completion(scene.vm, iife(R"js(
        return [t(() => c.f), t(() => { c.f = 1; }), t(() => e.has(engine.ScriptProbe)), e.alive].join('|');
    )js"));
    CHECK(unbound ==
          "TypeError: engine.ScriptProbe.f: no World is bound|TypeError: engine.ScriptProbe.f: no World is "
          "bound|TypeError: engine.Entity.has: no World is bound|false");

    CHECK(label(scene.vm.bindWorld(other)) == "Ok");
    const std::string later = completion(scene.vm, iife(R"js(
        return [t(() => c.f), t(() => { c.f = 1; }), t(() => e.has(engine.ScriptProbe)), e.alive].join('|');
    )js"));
    CHECK(later ==
          "TypeError: engine.ScriptProbe.f: the entity belongs to an earlier World binding|TypeError: "
          "engine.ScriptProbe.f: the entity belongs to an earlier World binding|TypeError: "
          "engine.Entity.has: the entity belongs to an earlier World binding|false");
    CHECK(other.get<ScriptProbe>(twin)->f == 7.0F);  // the write went nowhere

    // Binding the same World again starts a new binding too.
    CHECK(label(scene.vm.bindWorld(scene.world)) == "Ok");
    CHECK(completion(scene.vm, iife("return t(() => c.f);")) ==
          "TypeError: engine.ScriptProbe.f: the entity belongs to an earlier World binding");
    CHECK(label(scene.vm.defineEntityGlobal("e", scene.entity)) == "Ok");
    CHECK(completion(scene.vm, "e.alive") == "true");
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: an accessor or method lifted onto a foreign receiver is refused (BN27)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); 0");
    const std::string refused = completion(scene.vm, iife(R"js(
        const field = Object.getOwnPropertyDescriptor(engine.ScriptProbe.prototype, 'f');
        const alive = Object.getOwnPropertyDescriptor(engine.Entity.prototype, 'alive');
        const out = [];
        for (const receiver of [{}, engine.ScriptProbe.prototype, e, new engine.Vec3(), 5, undefined]) {
            out.push(t(() => field.get.call(receiver)), t(() => field.set.call(receiver, 1)));
        }
        out.push(t(() => alive.get.call(c)));
        out.push(t(() => alive.get.call({})));
        out.push(t(() => engine.Entity.prototype.has.call(c, engine.ScriptProbe)));
        out.push(t(() => engine.Entity.prototype.get.call({}, engine.ScriptProbe)));
        return out.join('|');
    )js"));
    const std::string field = "TypeError: engine.ScriptProbe.f: the receiver is not an engine.ScriptProbe";
    std::string expected;
    for (int i = 0; i < 12; ++i) {
        expected += field + "|";
    }
    expected +=
        "TypeError: engine.Entity.alive: the receiver is not an engine.Entity|"
        "TypeError: engine.Entity.alive: the receiver is not an engine.Entity|"
        "TypeError: engine.Entity.has: the receiver is not an engine.Entity|"
        "TypeError: engine.Entity.get: the receiver is not an engine.Entity";
    CHECK(refused == expected);
    CHECK(scene.probe() == ScriptProbe{});
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a tag component is added, found and removed by presence alone (BN28)") {
    const LogCapture log;
    ProbeScene scene;
    const std::string tag = completion(scene.vm, iife(R"js(
        const out = [e.has(engine.ScriptTag)];
        const added = e.add(engine.ScriptTag);
        out.push(added instanceof engine.ScriptTag, e.has(engine.ScriptTag));
        out.push(e.get(engine.ScriptTag) instanceof engine.ScriptTag);
        out.push(e.remove(engine.ScriptTag), e.remove(engine.ScriptTag), e.get(engine.ScriptTag));
        return out.map(String).join();
    )js"));
    CHECK(tag == "false,true,true,true,true,false,null");
    CHECK_FALSE(scene.world.has<ScriptTag>(scene.entity));
    CHECK(completion(scene.vm, "Object.getOwnPropertyNames(engine.ScriptTag.prototype).join()") == "constructor");
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: an uncaught field error ends the call Exception with the native frame in the stack (BN29)") {
    LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); 0");

    const auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        std::size_t begin = 0;
        for (std::size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', begin)) {
            out.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        out.push_back(text.substr(begin));
        return out;
    };
    struct Row {
        std::string_view source;
        std::string_view message;
        std::string_view firstFrame;
    };
    const std::array<Row, 3> rows{{
        {"c.f = 'x';", "engine.ScriptProbe.f: expected a number, got a string", "    at set f (native)"},
        {"c.b = 1;", "engine.ScriptProbe.b: expected a boolean, got a number", "    at set b (native)"},
        {"e.get(5);", "engine.Entity.get: expected an engine component class, got a number", "    at get (native)"},
    }};
    for (const Row& row : rows) {
        INFO("source: " << row.source);
        log.clear();
        const EvalResult result = scene.vm.evaluateScript(row.source, "res://bn29.js");
        CHECK(result.status == EvalStatus::Exception);
        CHECK(result.error.name == "TypeError");
        CHECK(result.error.message == row.message);
        const std::vector<std::string> frames = lines(result.error.stack);
        REQUIRE(frames.size() >= 2U);
        CHECK(frames[0] == row.firstFrame);
        CHECK(frames[1].find("res://bn29.js:") != std::string::npos);
        const std::vector<script_test::CapturedRecord> records = log.atLeast(LogLevel::Warn);
        REQUIRE(records.size() == 1U);
        CHECK(records[0].level == LogLevel::Error);
        CHECK(records[0].message ==
              "script: res://bn29.js: uncaught TypeError: " + std::string{row.message} + "\n" + result.error.stack);
    }

    // A getter's frame is named for the property, and a caught error is an ordinary TypeError.
    log.clear();
    scene.probe().f = 1.0F;
    const EvalResult getter = scene.vm.evaluateScript(
        "const p = Object.getOwnPropertyDescriptor("
        "engine.ScriptProbe.prototype, 'f'); p.get.call({});",
        "res://bn29.js");
    CHECK(getter.status == EvalStatus::Exception);
    CHECK(getter.error.message == "engine.ScriptProbe.f: the receiver is not an engine.ScriptProbe");
    CHECK(getter.error.stack.starts_with("    at get f (native)"));
    log.clear();
    CHECK(
        completion(scene.vm, "try { c.f = {}; 'no throw'; } catch (err) { (err instanceof TypeError) + err.name; }") ==
        "trueTypeError");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: no hand-written script source names a built-in component (BN30)") {
    namespace fs = std::filesystem;
    const auto strippedSource = [](const fs::path& path) {
        std::ifstream file{path};
        std::string out;
        std::string line;
        while (std::getline(file, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            out += line;
            out += '\n';
        }
        return out;
    };
    const auto isIdentifierChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    const auto holdsIdentifier = [&](const std::string& text, std::string_view name) {
        for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
            const bool leftOpen = at == 0 || !isIdentifierChar(text[at - 1]);
            const std::size_t after = at + name.size();
            const bool rightOpen = after >= text.size() || !isIdentifierChar(text[after]);
            if (leftOpen && rightOpen) {
                return true;
            }
        }
        return false;
    };

    // The names a fresh World registers, `engine::` stripped.
    const World world;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < world.componentTypeCount(); ++i) {
        std::string name{world.componentTypeName(world.componentTypeAt(i))};
        if (name.starts_with("engine::")) {
            name.erase(0, std::string_view{"engine::"}.size());
        }
        names.push_back(name);
    }
    REQUIRE_FALSE(names.empty());

    // A DERIVED set: every .cpp and .hpp under the script layer, never an enumeration.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator{fs::path{AERO_SCRIPT_SRC_DIR}.parent_path()}) {
        if (entry.is_regular_file() && (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")) {
            files.push_back(entry.path());
        }
    }
    bool sawApiSource = false;
    bool sawApiHeader = false;
    bool sawPublicHeader = false;    // include/
    bool sawInternalHeader = false;  // internal/
    for (const fs::path& file : files) {
        const std::string text = strippedSource(file);
        INFO("file: " << file.filename().string());
        sawApiSource = sawApiSource || file.filename() == "script_api.cpp";
        sawApiHeader = sawApiHeader || (file.filename() == "script_api.hpp" && holdsIdentifier(text, "ScriptApi"));
        sawPublicHeader = sawPublicHeader || (file.filename() == "vm.hpp" && holdsIdentifier(text, "bindWorld"));
        sawInternalHeader = sawInternalHeader || (file.filename() == "component_binding.hpp" &&
                                                  holdsIdentifier(text, "ComponentBindingTable"));
        for (const std::string& name : names) {
            INFO("component: " << name);
            CHECK_FALSE(holdsIdentifier(text, name));
        }
    }
    // Anti-vacuity: the scan reached all three roots (src, include, internal), and the same scan finds a real
    // identifier in each.
    CHECK(sawApiSource);
    CHECK(sawApiHeader);
    CHECK(sawPublicHeader);
    CHECK(sawInternalHeader);
}

// ---- BN31-BN36: Vec3 and Quat views of a component's vector fields -----------------------------------------------

TEST_CASE("script: a Vec3 field read is a live view, and a write through it lands in the World (BN31)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); globalThis.p = c.v; p.x += 0.5; 0");
    CHECK(scene.probe().v == engine::Vec3{1.5F, 2.0F, 3.0F});
    CHECK(completion(scene.vm, "[c.v === c.v, p instanceof engine.Vec3, p.x, p.y, p.z].join()") ==
          "false,true,1.5,2,3");

    // The view names the field, not a value read once: the World's own change and another field's write show through.
    scene.probe().v.y = 9.0F;
    CHECK(completion(scene.vm, "p.y") == "9");
    completion(scene.vm, "c.f = 4; c.s = 'other'; 0");
    CHECK(completion(scene.vm, "[p.x, p.y, p.z].join()") == "1.5,9,3");

    // add replaces the component with the defaults; the same view reads them.
    completion(scene.vm, "e.add(engine.ScriptProbe); 0");
    CHECK(completion(scene.vm, "[p.x, p.y, p.z].join()") == "1,2,3");
    CHECK(scene.probe() == ScriptProbe{});
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a Vec3 field takes an engine.Vec3 and nothing else, and reads no property of the value (BN32)") {
    const LogCapture log;
    ProbeScene scene;
    const Entity second = scene.world.create();
    ScriptProbe seeded;
    seeded.v = engine::Vec3{10.0F, 11.0F, 12.0F};
    REQUIRE(scene.world.add<ScriptProbe>(second, seeded) != nullptr);
    REQUIRE(label(scene.vm.defineEntityGlobal("o", second)) == "Ok");
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); globalThis.oc = o.get(engine.ScriptProbe); 0");

    completion(scene.vm, "c.v = new engine.Vec3(4, 5, 6); 0");
    CHECK(scene.probe().v == engine::Vec3{4.0F, 5.0F, 6.0F});
    scene.probe().v = engine::Vec3{7.0F, 8.0F, 9.0F};
    completion(scene.vm, "c.v = c.v; 0");
    CHECK(scene.probe().v == engine::Vec3{7.0F, 8.0F, 9.0F});
    completion(scene.vm, "c.v = oc.v; 0");
    CHECK(scene.probe().v == engine::Vec3{10.0F, 11.0F, 12.0F});
    CHECK(scene.world.get<ScriptProbe>(second)->v == engine::Vec3{10.0F, 11.0F, 12.0F});

    struct Row {
        std::string_view value;
        std::string_view actual;
    };
    const std::array<Row, 5> rows{{
        {"new engine.Quat()", "an engine.Quat"},
        {"({x: 1, y: 2, z: 3})", "an object"},
        {"[1, 2, 3]", "an object"},
        {"3", "a number"},
        {"null", "null"},
    }};
    for (const Row& row : rows) {
        INFO("value: " << row.value);
        CHECK(completion(scene.vm, iife("return t(() => { c.v = " + std::string{row.value} + "; });")) ==
              "TypeError: engine.ScriptProbe.v: expected an engine.Vec3, got " + std::string{row.actual});
    }
    // Nothing a script wrote runs: a getter on the offered object is never read.
    CHECK(completion(scene.vm,
                     iife("globalThis.hooks = 0; const r = t(() => { c.v = { get x() { hooks++; return 1; }, y: 2, z: "
                          "3 }; }); return r + '|' + hooks;")) ==
          "TypeError: engine.ScriptProbe.v: expected an engine.Vec3, got an object|0");
    CHECK(scene.probe().v == engine::Vec3{10.0F, 11.0F, 12.0F});
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a Quat field's view reads and writes four axes and stores what was written (BN33)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); globalThis.q = c.q; 0");
    CHECK(completion(scene.vm, "[q.x, q.y, q.z, q.w, q instanceof engine.Quat, c.q === c.q].join()") ==
          "0,0,0,1,true,false");
    completion(scene.vm, "q.x = 1; q.y = 2; q.z = 3; q.w = 2; 0");
    CHECK(scene.probe().q == engine::Quat{1.0F, 2.0F, 3.0F, 2.0F});  // never normalised
    completion(scene.vm, "c.q = new engine.Quat(2, 0, 0, 0); 0");
    CHECK(scene.probe().q == engine::Quat{2.0F, 0.0F, 0.0F, 0.0F});
    CHECK(completion(scene.vm, "[q.x, q.w].join()") == "2,0");
    CHECK(completion(scene.vm, iife("return t(() => { c.q = new engine.Vec3(); });")) ==
          "TypeError: engine.ScriptProbe.q: expected an engine.Quat, got an engine.Vec3");
    CHECK(completion(scene.vm, iife("return t(() => { q.w = 'x'; });")) ==
          "TypeError: engine.ScriptProbe.q.w: expected a number, got a string");
    CHECK(scene.probe().q == engine::Quat{2.0F, 0.0F, 0.0F, 0.0F});
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a view held across destroy, remove and unbind throws on every use and writes nothing (BN34)") {
    const LogCapture log;
    ProbeScene scene;
    completion(scene.vm, R"js(
        globalThis.c = e.get(engine.ScriptProbe);
        globalThis.p = c.v;
        globalThis.qv = c.q;
        globalThis.d = new engine.Vec3();
        d.copy(p);
        0
    )js");
    const std::string probeScript = iife(R"js(
        return [t(() => p.x), t(() => { p.x = 1; }), t(() => p.set(1, 2, 3)), t(() => p.copy(d)),
                t(() => d.copy(p)), t(() => qv.w), t(() => { qv.w = 5; }), [d.x, d.y, d.z].join()].join('|');
    )js");
    const auto expectedFor = [](std::string_view reason) {
        const std::string tail = std::string{": "} + std::string{reason};
        return "TypeError: engine.ScriptProbe.v.x" + tail + "|TypeError: engine.ScriptProbe.v.x" + tail +
               "|TypeError: engine.ScriptProbe.v" + tail + "|TypeError: engine.ScriptProbe.v" + tail +
               "|TypeError: engine.ScriptProbe.v" + tail + "|TypeError: engine.ScriptProbe.q.w" + tail +
               "|TypeError: engine.ScriptProbe.q.w" + tail + "|1,2,3";
    };

    // Remove: the entity lives, the component is gone, and nothing re-adds it.
    CHECK(completion(scene.vm, "e.remove(engine.ScriptProbe)") == "true");
    CHECK(completion(scene.vm, probeScript) == expectedFor("the entity has no engine.ScriptProbe"));
    CHECK(scene.world.get<ScriptProbe>(scene.entity) == nullptr);
    REQUIRE(scene.world.add<ScriptProbe>(scene.entity) != nullptr);

    // Unbind, then a binding that is not the one the view was made under.
    scene.vm.unbindWorld();
    CHECK(completion(scene.vm, probeScript) == expectedFor("no World is bound"));
    REQUIRE(label(scene.vm.bindWorld(scene.world)) == "Ok");
    CHECK(completion(scene.vm, probeScript) == expectedFor("the entity belongs to an earlier World binding"));
    CHECK(scene.probe() == ScriptProbe{});

    // Destroy: the recycled index takes a newer generation, and the stale view never reaches it.
    REQUIRE(label(scene.vm.defineEntityGlobal("e", scene.entity)) == "Ok");
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); globalThis.p = c.v; globalThis.qv = c.q; 0");
    CHECK(scene.world.destroy(scene.entity));
    const Entity fresh = scene.world.create();
    CHECK(fresh.index == scene.entity.index);
    ScriptProbe seeded;
    seeded.v = engine::Vec3{20.0F, 21.0F, 22.0F};
    seeded.q = engine::Quat{0.5F, 0.5F, 0.5F, 0.5F};
    REQUIRE(scene.world.add<ScriptProbe>(fresh, seeded) != nullptr);
    CHECK(completion(scene.vm, probeScript) == expectedFor("the entity is not alive"));
    const ScriptProbe* const kept = scene.world.get<ScriptProbe>(fresh);
    REQUIRE(kept != nullptr);
    CHECK(*kept == seeded);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a view's set and copy write through, and a refused set writes nothing (BN35)") {
    const LogCapture log;
    ProbeScene scene;
    const Entity second = scene.world.create();
    ScriptProbe seeded;
    seeded.v = engine::Vec3{10.0F, 11.0F, 12.0F};
    REQUIRE(scene.world.add<ScriptProbe>(second, seeded) != nullptr);
    REQUIRE(label(scene.vm.defineEntityGlobal("o", second)) == "Ok");
    completion(scene.vm, "globalThis.c = e.get(engine.ScriptProbe); globalThis.oc = o.get(engine.ScriptProbe); 0");

    CHECK(completion(scene.vm, "(() => { const p = c.v; return p.set(4, 5, 6) === p; })()") == "true");
    CHECK(scene.probe().v == engine::Vec3{4.0F, 5.0F, 6.0F});
    CHECK(completion(scene.vm, "(() => { const q = c.q; return q.set(1, 2, 3, 4) === q; })()") == "true");
    CHECK(scene.probe().q == engine::Quat{1.0F, 2.0F, 3.0F, 4.0F});

    CHECK(completion(scene.vm, "c.v.copy(c.v).x") == "4");
    CHECK(scene.probe().v == engine::Vec3{4.0F, 5.0F, 6.0F});
    completion(scene.vm, "c.v.copy(oc.v); 0");
    CHECK(scene.probe().v == engine::Vec3{10.0F, 11.0F, 12.0F});
    CHECK(scene.world.get<ScriptProbe>(second)->v == engine::Vec3{10.0F, 11.0F, 12.0F});
    completion(scene.vm, "c.v.copy(new engine.Vec3(1, 1, 1)); new engine.Vec3(7, 8, 9).copy(c.v); 0");
    CHECK(scene.probe().v == engine::Vec3{1.0F, 1.0F, 1.0F});
    completion(scene.vm, "globalThis.d = new engine.Vec3(7, 8, 9); d.copy(oc.v); 0");
    CHECK(completion(scene.vm, "[d.x, d.y, d.z].join()") == "10,11,12");

    // A refused set or copy writes none of the axes.
    const std::string refused = completion(scene.vm, iife(R"js(
        return [t(() => c.v.set(1, 2)), t(() => c.v.set(1, 2, 'z')), t(() => c.q.set(1, 2, 3)),
                t(() => c.v.copy(c.q)), t(() => c.q.copy(c.v)), t(() => c.v.copy({ x: 1, y: 2, z: 3 }))].join('|');
    )js"));
    CHECK(refused ==
          "TypeError: engine.Vec3.set: z: expected a number, got undefined|"
          "TypeError: engine.Vec3.set: z: expected a number, got a string|"
          "TypeError: engine.Quat.set: w: expected a number, got undefined|"
          "TypeError: engine.Vec3.copy: expected an engine.Vec3, got an engine.Quat|"
          "TypeError: engine.Quat.copy: expected an engine.Quat, got an engine.Vec3|"
          "TypeError: engine.Vec3.copy: expected an engine.Vec3, got an object");
    CHECK(scene.probe().v == engine::Vec3{1.0F, 1.0F, 1.0F});
    CHECK(scene.probe().q == engine::Quat{1.0F, 2.0F, 3.0F, 4.0F});
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a call past its budget is refused at every binding call, a view's included (BN36)") {
    const LogCapture log;
    const auto table = probeTable();
    World world = script_test::makeProbeWorld();
    const Entity entity = script_test::makeProbeEntity(world);
    VmConfig config;
    config.callTimeBudget = std::chrono::milliseconds{50};
    Vm vm = makeProbeVm(table, config);
    REQUIRE(label(vm.bindWorld(world)) == "Ok");
    REQUIRE(label(vm.defineEntityGlobal("e", entity)) == "Ok");

    // The executor turns the first interrupt into a rejection, so the loop runs on past the fire: from then on every
    // binding call must be refused before its body runs, and the catch only increments and assigns.
    const EvalResult spun = vm.evaluateScript(
        "globalThis.refused = 0; globalThis.lastMessage = ''; const c = e.get(engine.ScriptProbe); "
        "new Promise(() => { for (;;) {} }); "
        "for (;;) { try { c.v.x = c.v.x + 1; } catch (err) { refused++; lastMessage = err.message; } }",
        "bn36.js");
    CHECK(spun.status == EvalStatus::Interrupted);
    CHECK(completion(vm, "[refused > 0, lastMessage].join()") == "true,Maximum call stack size exceeded");
    const float x = world.get<ScriptProbe>(entity)->v.x;
    CHECK(std::isfinite(x));
    CHECK(x >= 1.0F);
    CHECK(completion(vm, "1 + 1") == "2");
    expectCleanShutdown(vm);
}
