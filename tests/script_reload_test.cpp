// tests/script_reload_test.cpp -- task 4.3.3: script classes, instances and hot reload. HR1-HR3 are tier 0 through
// aero::script_internal (the carry rule and a module's state accessor); the later cases drive the public API and
// internal::VmAccess::defineInstanceGlobal. A TU of aero_tests, which supplies main() from test_main.cpp.
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/internal/script_carry.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>
#include <vector>

namespace {

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
