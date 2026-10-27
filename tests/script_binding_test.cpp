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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
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
