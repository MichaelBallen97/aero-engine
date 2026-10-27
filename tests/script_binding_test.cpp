// Aero Engine -- the binding runtime over a fixture table (task 4.2.1; BN1-BN5, and from the later steps BN7-BN36).
//
// Step 2 of the task covers the quickjs-free descriptor contract: how a C++ type maps to a script kind, how the two
// templates build a descriptor exactly as the generated code does, and what ComponentBindingTable::add refuses. The
// fixture component lives in script_binding_support.hpp.

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/component.hpp>
#include <aero/script/internal/component_binding.hpp>

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
using engine::script::internal::bindComponent;
using engine::script::internal::bindField;
using engine::script::internal::ComponentBinding;
using engine::script::internal::ComponentBindingTable;
using engine::script::internal::FieldBinding;
using engine::script::internal::FieldKind;
using engine::script::internal::fieldKindOf;
using engine::script::internal::MAX_BOUND_COMPONENTS;
using engine::script::internal::MAX_BOUND_FIELDS;
using script_test::LogCapture;
using script_test::ScriptProbe;
using script_test::ScriptTag;

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
