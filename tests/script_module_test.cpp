// Aero Engine -- ES modules through the VFS (task 4.1.2): the specifier grammar, the cache and its reload hook,
// the statuses, the limits, and import(). ML1-ML12 test the pure half (module_graph.hpp) through
// aero::script_internal; every other case drives the public API and ends with a teardown that must find nothing
// leaked -- retired instances included.

#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/script.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// No operator<< for EvalStatus or ModuleState here: script_vm_test.cpp already defines the EvalStatus printer in
// engine::script with external linkage, and a second one in this binary would be a duplicate symbol. Statuses and
// states are compared through their labels instead, which also print readably on failure.

namespace {

using engine::script::ModuleRecord;
using engine::script::ModuleState;
using engine::script::moduleStateLabel;

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = KIB * KIB;

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

// ---- ML1-ML12: the pure half, tier 0 -------------------------------------------------------------------------

TEST_CASE("script: a res:// specifier resolves to its canonical key (ML1)") {
    using namespace engine::script::internal;
    struct Row {
        std::string_view specifier;
        std::string_view importer;
        std::string_view key;
    };
    const std::array<Row, 6> rows{{
        {"res://a.js", "", "res://a.js"},
        {"res://dir/./b/../c.js", "", "res://dir/c.js"},
        {"res://dir//c.js", "res://x.js", "res://dir/c.js"},
        {"res://dir/c.js/", "", "res://dir/c.js"},
        {"res://\xC3\xBC/n.js", "", "res://\xC3\xBC/n.js"},
        {"res://a b.js", "game.js", "res://a b.js"},
    }};
    for (const Row& row : rows) {
        INFO("specifier: " << std::string{row.specifier} << ", importer: " << std::string{row.importer});
        const SpecifierResolution resolved = resolveModuleSpecifier(row.specifier, row.importer);
        INFO("refusal: " << std::string{specifierRefusalText(resolved.refusal)});
        CHECK((resolved.refusal == SpecifierRefusal::None));
        CHECK(resolved.key == row.key);
    }
    // Anti-vacuity: the dot-segment row's key is not its specifier -- the normalizer ran.
    CHECK(resolveModuleSpecifier("res://dir/./b/../c.js", "").key != "res://dir/./b/../c.js");
}

TEST_CASE("script: ./ and ../ resolve against the importing module, a generation mark stripped (ML2)") {
    using namespace engine::script::internal;
    struct Row {
        std::string_view specifier;
        std::string_view importer;
        std::string_view key;
    };
    const std::array<Row, 7> rows{{
        {"./a.js", "res://scripts/main.js", "res://scripts/a.js"},
        {"../lib/b.js", "res://scripts/main.js", "res://lib/b.js"},
        {"./x/../y.js", "res://scripts/main.js", "res://scripts/y.js"},
        {"././a.js", "res://scripts/main.js", "res://scripts/a.js"},
        {"./x.js", "res://main.js", "res://x.js"},
        {"./a.js", "res://scripts/main.js?gen=7", "res://scripts/a.js"},
        {"../x.js", "res://scripts/main.js?gen=7", "res://x.js"},
    }};
    for (const Row& row : rows) {
        INFO("specifier: " << std::string{row.specifier} << ", importer: " << std::string{row.importer});
        const SpecifierResolution resolved = resolveModuleSpecifier(row.specifier, row.importer);
        INFO("refusal: " << std::string{specifierRefusalText(resolved.refusal)});
        CHECK((resolved.refusal == SpecifierRefusal::None));
        CHECK(resolved.key == row.key);
    }
    // "../" from a module at the root climbs above it.
    const SpecifierResolution above = resolveModuleSpecifier("../x.js", "res://main.js");
    CHECK((above.refusal == SpecifierRefusal::NotAddressable));
    CHECK(above.key.empty());
    // Anti-vacuity: the marked importer resolves exactly as the clean one does -- the mark was stripped, never joined.
    CHECK(resolveModuleSpecifier("./a.js", "res://scripts/main.js?gen=7").key ==
          resolveModuleSpecifier("./a.js", "res://scripts/main.js").key);
}

TEST_CASE("script: every refused specifier class is refused for its own reason (ML3)") {
    using namespace engine::script::internal;
    using namespace std::string_view_literals;
    struct Row {
        std::string_view specifier;
        SpecifierRefusal refusal;
    };
    const std::array<Row, 27> rows{{
        {""sv, SpecifierRefusal::Empty},
        {"lodash"sv, SpecifierRefusal::Bare},
        {"aero"sv, SpecifierRefusal::Bare},
        {"."sv, SpecifierRefusal::Bare},
        {".."sv, SpecifierRefusal::Bare},
        {"scripts/a.js"sv, SpecifierRefusal::Bare},
        {".hidden.js"sv, SpecifierRefusal::Bare},
        {"/scripts/a.js"sv, SpecifierRefusal::RootRelative},
        {"file:///a.js"sv, SpecifierRefusal::OtherScheme},
        {"http://x/a.js"sv, SpecifierRefusal::OtherScheme},
        {"C:/a.js"sv, SpecifierRefusal::OtherScheme},
        {R"(C:\a.js)"sv, SpecifierRefusal::OtherScheme},
        {"res:/a.js"sv, SpecifierRefusal::OtherScheme},
        {"RES://a.js"sv, SpecifierRefusal::OtherScheme},
        {"./a.js?x=1"sv, SpecifierRefusal::Reserved},
        {"./a.js#f"sv, SpecifierRefusal::Reserved},
        {"res://a.js?gen=2"sv, SpecifierRefusal::Reserved},
        {R"(./a\b.js)"sv, SpecifierRefusal::NotAddressable},
        {"res://a:b.js"sv, SpecifierRefusal::NotAddressable},
        {"res://../a.js"sv, SpecifierRefusal::NotAddressable},
        {"./a\0b.js"sv, SpecifierRefusal::NotAddressable},
        {"res://"sv, SpecifierRefusal::Root},
        {"res://."sv, SpecifierRefusal::Root},
        {"./"sv, SpecifierRefusal::Root},
        {"./ok.js"sv, SpecifierRefusal::None},  // the control row: the same table resolves a good specifier
        {"../ok.js"sv, SpecifierRefusal::NotAddressable},
        {"res://ok.js"sv, SpecifierRefusal::None},
    }};
    std::size_t resolvedRows = 0;
    for (const Row& row : rows) {
        INFO("specifier: " << std::string{row.specifier});
        const SpecifierResolution resolved = resolveModuleSpecifier(row.specifier, "res://a.js");
        INFO("refusal: " << std::string{specifierRefusalText(resolved.refusal)});
        CHECK((resolved.refusal == row.refusal));
        if (row.refusal == SpecifierRefusal::None) {
            CHECK_FALSE(resolved.key.empty());
            ++resolvedRows;
        } else {
            CHECK(resolved.key.empty());
        }
    }
    // Anti-vacuity: the refusals are about the specifiers -- the good rows really resolved.
    CHECK(resolvedRows == 2U);
}

TEST_CASE("script: a relative specifier needs a res:// importer; an absolute one needs none (ML4)") {
    using namespace engine::script::internal;
    for (const std::string_view importer :
         std::array<std::string_view, 6>{"", "game.js", "<input>", "res://", "res://a.js?gen=1", "res://a.js?x"}) {
        INFO("importer: " << std::string{importer});
        const SpecifierResolution resolved = resolveModuleSpecifier("./a.js", importer);
        CHECK((resolved.refusal == SpecifierRefusal::NoModuleImporter));
        CHECK(resolved.key.empty());
    }
    // A script evaluated under a res:// name can import relatively.
    const SpecifierResolution fromBoot = resolveModuleSpecifier("./a.js", "res://scripts/boot.js");
    CHECK((fromBoot.refusal == SpecifierRefusal::None));
    CHECK(fromBoot.key == "res://scripts/a.js");
    for (const std::string_view importer : std::array<std::string_view, 3>{"", "game.js", "<input>"}) {
        INFO("importer: " << std::string{importer});
        const SpecifierResolution absolute = resolveModuleSpecifier("res://a.js", importer);
        CHECK((absolute.refusal == SpecifierRefusal::None));
        CHECK(absolute.key == "res://a.js");
    }

    CHECK(importerKey("res://s/m.js?gen=3") == std::optional<std::string>{"res://s/m.js"});
    CHECK(importerKey("res://s/./m.js") == std::optional<std::string>{"res://s/m.js"});
    for (const std::string_view importer :
         std::array<std::string_view, 4>{"game.js", "<input>", "res://", "res://a?b"}) {
        INFO("importer: " << std::string{importer});
        CHECK_FALSE(importerKey(importer).has_value());
    }
}

TEST_CASE("script: a generation name encodes, decodes, and can never be written as a specifier (ML5)") {
    using namespace engine::script::internal;
    CHECK(encodeModuleName("res://a.js", 1) == "res://a.js");
    CHECK(encodeModuleName("res://a.js", 2) == "res://a.js?gen=2");
    CHECK(encodeModuleName("res://a.js", 4294967295U) == "res://a.js?gen=4294967295");

    for (const std::uint32_t generation : std::array<std::uint32_t, 4>{1, 2, 10, 4294967295U}) {
        INFO("generation: " << generation);
        const std::string name = encodeModuleName("res://dir/a.js", generation);
        const std::optional<ModuleName> decoded = decodeModuleName(name);
        REQUIRE(decoded.has_value());
        CHECK(decoded->key == "res://dir/a.js");
        CHECK(decoded->generation == generation);
        if (generation >= 2) {
            // No user-written specifier can name a generation: every one is refused for its '?'.
            const SpecifierResolution asSpecifier = resolveModuleSpecifier(name, "");
            CHECK((asSpecifier.refusal == SpecifierRefusal::Reserved));
        }
    }

    for (const std::string_view name : std::array<std::string_view, 11>{
             "res://a.js?gen=1", "res://a.js?gen=01", "res://a.js?gen=", "res://a.js?gen=x", "res://a.js?gen=2x",
             "res://a.js?gen=4294967296", "res://a.js?v=2", "a.js", "res://dir/../a.js", "res://", "res://a.js#b"}) {
        INFO("name: " << std::string{name});
        CHECK_FALSE(decodeModuleName(name).has_value());
    }
    // Anti-vacuity: a decoder refusing every name would pass the rows above; generation 1 decodes, to 1.
    const std::optional<ModuleName> first = decodeModuleName("res://a.js");
    REQUIRE(first.has_value());
    CHECK(first->generation == 1U);
}

TEST_CASE(
    "script: a module name is pure ASCII, decodes only in its canonical spelling, and round-trips every byte "
    "(ML53)") {
    using namespace engine::script::internal;
    const auto pureAscii = [](std::string_view text) {
        return std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
    };
    // The atom collision's two keys: u-umlaut's UTF-8 bytes, and those bytes misread as Latin-1 and re-encoded as
    // UTF-8. Hex escapes are split where a hex digit follows.
    const std::string umlaut = encodeModuleName("res://\xC3\xBC.js", 1);
    const std::string latin = encodeModuleName("res://\xC3\x83\xC2\xBC.js", 1);
    CHECK(umlaut == "res://%C3%BC.js");
    CHECK(latin == "res://%C3%83%C2%BC.js");
    CHECK(pureAscii(umlaut));
    CHECK(pureAscii(latin));
    CHECK(umlaut != latin);
    CHECK(encodeModuleName("res://a%b.js", 2) == "res://a%25b.js?gen=2");
    CHECK(encodeModuleName("res://\xFF/a.js", 1) == "res://%FF/a.js");
    CHECK(encodeModuleName("res://a.js", 2) == "res://a.js?gen=2");  // an ASCII key is unchanged

    // Every byte value a key can carry above ASCII, at two generations.
    std::size_t roundTrips = 0;
    for (unsigned value = 0x80U; value <= 0xFFU; ++value) {
        const std::string key = std::string{"res://x"} + static_cast<char>(value) + ".js";
        for (const std::uint32_t generation : std::array<std::uint32_t, 2>{1, 7}) {
            INFO("byte " << value << ", generation " << generation);
            const std::string name = encodeModuleName(key, generation);
            CHECK(pureAscii(name));
            const std::optional<ModuleName> decoded = decodeModuleName(name);
            REQUIRE(decoded.has_value());
            CHECK(decoded->key == key);
            CHECK(decoded->generation == generation);
            ++roundTrips;
        }
    }
    CHECK(roundTrips == 256U);
    const std::optional<ModuleName> percent = decodeModuleName(encodeModuleName("res://a%b.js", 3));
    REQUIRE(percent.has_value());
    CHECK(percent->key == "res://a%b.js");
    CHECK(percent->generation == 3U);

    const std::array<std::string_view, 8> refused{
        "res://%c3%bc.js", "res://%41.js",      "res://a%2.js",           "res://a%",
        "res://%ZZ.js",    "res://\xC3\xBC.js", "res://%C3%BC.js?gen=01", "res://%25.js%"};
    for (const std::string_view name : refused) {
        INFO("name: " << std::string{name});
        CHECK_FALSE(decodeModuleName(name).has_value());
    }
    // Anti-vacuity: an escaped '%' decodes.
    const std::optional<ModuleName> escaped = decodeModuleName("res://%25.js");
    REQUIRE(escaped.has_value());
    CHECK(escaped->key == "res://%.js");

    CHECK(importerKey("res://%C3%BC/m.js?gen=3") == std::optional<std::string>{"res://\xC3\xBC/m.js"});
    // Script names are read leniently.
    CHECK(importerKey("res://\xC3\xBC/boot.js") == std::optional<std::string>{"res://\xC3\xBC/boot.js"});
    CHECK(importerKey("res://a%41.js") == std::optional<std::string>{"res://a%41.js"});

    CHECK(alreadyLoadedMessage("res://a.js", "res://b.js") ==
          "cannot load 'res://a.js' imported by 'res://b.js': its current instance is already loaded under another "
          "name");
    CHECK(alreadyLoadedMessage("res://a.js", "") ==
          "cannot load 'res://a.js': its current instance is already loaded under another name");
}

TEST_CASE("script: the record table creates, marks and lists records, and refuses nothing silently (ML6)") {
    using namespace engine::script::internal;
    int tokenA = 0;
    ModuleTable table;
    table.beginCompile("res://b.js");
    table.beginCompile("res://a.js");
    std::vector<ModuleRecord> listed = table.records();
    REQUIRE(listed.size() == 2U);
    CHECK(listed[0].path == "res://a.js");  // sorted by path
    CHECK(listed[1].path == "res://b.js");
    for (const ModuleRecord& record : listed) {
        CHECK(record.generation == 1U);
        CHECK(moduleStateLabel(record.state) == "Unloaded");
        CHECK(record.imports.empty());
    }
    // The public default ModuleRecord is generation 1 and Unloaded (the probe deliberately leaves this to tier 0).
    const ModuleRecord fresh{};
    CHECK(fresh.generation == 1U);
    CHECK(moduleStateLabel(fresh.state) == "Unloaded");

    table.markLoaded("res://a.js", &tokenA);
    CHECK(table.instance("res://a.js") == &tokenA);
    const std::optional<ModuleRecord> loaded = recordIn(table, "res://a.js");
    REQUIRE(loaded.has_value());
    CHECK(moduleStateLabel(loaded->state) == "Loaded");  // read back off records(), not off instance()

    table.addImport("res://a.js", "res://b.js", ImportKind::Static);
    table.addImport("res://a.js", "res://b.js", ImportKind::Dynamic);
    table.addImport("res://a.js", "res://c.js", ImportKind::Dynamic);
    const std::optional<ModuleRecord> withImports = recordIn(table, "res://a.js");
    REQUIRE(withImports.has_value());
    CHECK(withImports->imports == std::vector<std::string>{"res://b.js", "res://c.js"});

    // Nothing creates a record but beginCompile.
    table.addImport("res://script.js", "res://b.js", ImportKind::Dynamic);
    table.markLoaded("res://nope.js", &tokenA);
    table.markFailed("res://nope.js");
    CHECK(table.records().size() == 2U);

    // A mark on a record with no instance changes nothing.
    table.markEvaluating("res://b.js");
    table.markFailed("res://b.js");
    const std::optional<ModuleRecord> b = recordIn(table, "res://b.js");
    REQUIRE(b.has_value());
    CHECK(moduleStateLabel(b->state) == "Unloaded");
    CHECK(table.instance("res://b.js") == nullptr);

    // A new compile of a key starts its imports afresh.
    table.beginCompile("res://a.js");
    const std::optional<ModuleRecord> recompiled = recordIn(table, "res://a.js");
    REQUIRE(recompiled.has_value());
    CHECK(recompiled->imports.empty());

    CHECK(table.generation("res://never.js") == 1U);
    CHECK(table.instance("res://never.js") == nullptr);
}

namespace {

// Loads `key` into `table` under `token`, as the glue does after a successful compile.
void loadInto(engine::script::internal::ModuleTable& table, std::string_view key, void* token) {
    table.beginCompile(key);
    table.markLoaded(key, token);
}

}  // namespace

TEST_CASE("script: invalidation reaches every transitive importer, static or dynamic, and nothing else (ML7)") {
    using namespace engine::script::internal;
    std::array<int, 8> tokens{};
    const std::array<std::string_view, 8> keys{"res://main.js", "res://ui.js", "res://logic.js", "res://util.js",
                                               "res://tool.js", "res://x.js",  "res://y.js",     "res://other.js"};
    ModuleTable table;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        loadInto(table, keys[i], &tokens[i]);
    }
    table.addImport("res://main.js", "res://ui.js", ImportKind::Static);
    table.addImport("res://main.js", "res://logic.js", ImportKind::Static);
    table.addImport("res://ui.js", "res://util.js", ImportKind::Static);
    table.addImport("res://logic.js", "res://util.js", ImportKind::Static);
    table.addImport("res://tool.js", "res://util.js", ImportKind::Dynamic);
    table.addImport("res://x.js", "res://y.js", ImportKind::Static);
    table.addImport("res://y.js", "res://x.js", ImportKind::Static);
    table.addImport("res://x.js", "res://util.js", ImportKind::Static);

    const std::vector<std::string> changed = table.invalidate("res://util.js");
    CHECK(changed == std::vector<std::string>{"res://logic.js", "res://main.js", "res://tool.js", "res://ui.js",
                                              "res://util.js", "res://x.js", "res://y.js"});
    for (const std::string& key : changed) {
        INFO("key: " << key);
        const std::optional<ModuleRecord> record = recordIn(table, key);
        REQUIRE(record.has_value());
        CHECK(record->generation == 2U);
        CHECK(moduleStateLabel(record->state) == "Unloaded");
        CHECK(table.instance(key) == nullptr);
    }
    // Anti-vacuity: a closure that bumped everything would touch `other`.
    const std::optional<ModuleRecord> other = recordIn(table, "res://other.js");
    REQUIRE(other.has_value());
    CHECK(other->generation == 1U);
    CHECK(moduleStateLabel(other->state) == "Loaded");
    CHECK(table.instance("res://other.js") == &tokens[7]);

    const std::size_t before = table.records().size();
    CHECK(table.invalidate("res://nope.js").empty());
    CHECK(table.records().size() == before);

    CHECK(table.invalidate("res://main.js") == std::vector<std::string>{"res://main.js"});
    CHECK(table.generation("res://main.js") == 3U);
}

TEST_CASE("script: retire bumps exactly the attempt's modules (ML8)") {
    using namespace engine::script::internal;
    std::array<int, 3> tokens{};
    ModuleTable table;
    loadInto(table, "res://a.js", &tokens[0]);
    loadInto(table, "res://b.js", &tokens[1]);
    loadInto(table, "res://c.js", &tokens[2]);
    table.addImport("res://a.js", "res://b.js", ImportKind::Static);

    table.retire({"res://a.js", "res://c.js"});
    for (const std::string_view key : std::array<std::string_view, 2>{"res://a.js", "res://c.js"}) {
        INFO("key: " << std::string{key});
        const std::optional<ModuleRecord> record = recordIn(table, key);
        REQUIRE(record.has_value());
        CHECK(record->generation == 2U);
        CHECK(moduleStateLabel(record->state) == "Unloaded");
        CHECK(table.instance(key) == nullptr);
    }
    // Anti-vacuity: retire is not a closure -- b is untouched although a imports it.
    const std::optional<ModuleRecord> b = recordIn(table, "res://b.js");
    REQUIRE(b.has_value());
    CHECK(b->generation == 1U);
    CHECK(moduleStateLabel(b->state) == "Loaded");
    CHECK(table.instance("res://b.js") == &tokens[1]);
    const std::optional<ModuleRecord> a = recordIn(table, "res://a.js");
    REQUIRE(a.has_value());
    CHECK(a->imports == std::vector<std::string>{"res://b.js"});

    table.retire({"res://a.js"});
    CHECK(table.generation("res://a.js") == 3U);
}

TEST_CASE("script: an evaluated entry marks its static graph only (ML9)") {
    using namespace engine::script::internal;
    std::array<int, 4> tokens{};
    ModuleTable table;
    loadInto(table, "res://entry.js", &tokens[0]);
    loadInto(table, "res://dep1.js", &tokens[1]);
    loadInto(table, "res://dep2.js", &tokens[2]);
    loadInto(table, "res://dyn.js", &tokens[3]);
    table.beginCompile("res://gone.js");  // a record, never an instance
    table.addImport("res://entry.js", "res://dep1.js", ImportKind::Static);
    table.addImport("res://dep1.js", "res://dep2.js", ImportKind::Static);
    table.addImport("res://dep2.js", "res://entry.js", ImportKind::Static);  // a cycle
    table.addImport("res://entry.js", "res://dyn.js", ImportKind::Dynamic);
    table.addImport("res://dep1.js", "res://gone.js", ImportKind::Static);

    table.markEvaluatedGraph("res://entry.js");
    for (const std::string_view key :
         std::array<std::string_view, 3>{"res://entry.js", "res://dep1.js", "res://dep2.js"}) {
        INFO("key: " << std::string{key});
        const std::optional<ModuleRecord> record = recordIn(table, key);
        REQUIRE(record.has_value());
        CHECK(moduleStateLabel(record->state) == "Evaluated");
    }
    // Anti-vacuity: following dynamic edges would mark dyn.
    const std::optional<ModuleRecord> dyn = recordIn(table, "res://dyn.js");
    REQUIRE(dyn.has_value());
    CHECK(moduleStateLabel(dyn->state) == "Loaded");
    const std::optional<ModuleRecord> gone = recordIn(table, "res://gone.js");
    REQUIRE(gone.has_value());
    CHECK(moduleStateLabel(gone->state) == "Unloaded");
}

TEST_CASE("script: the depth and size limits are the stated arithmetic (ML10)") {
    using namespace engine::script::internal;
    CHECK(maxImportDepth(engine::script::MIN_STACK_LIMIT_BYTES) == 8U);
    CHECK(maxImportDepth(engine::script::DEFAULT_STACK_LIMIT_BYTES) == 64U);
    CHECK(maxImportDepth(engine::script::MAX_STACK_LIMIT_BYTES) == 1024U);
    CHECK(maxImportDepth(64 * KIB + 8191) == 8U);  // anti-vacuity: a rounding-up implementation gives 9
    CHECK(maxImportDepth(0) == 1U);
    CHECK(maxImportDepth(8 * KIB - 1) == 1U);
    CHECK(sourceWithinCap(MIB, MIB));
    CHECK_FALSE(sourceWithinCap(MIB + 1, MIB));
    CHECK(sourceWithinCap(0, MIB));
}

TEST_CASE("script: every load-error text is exact, and a '%' in a path is text (ML11)") {
    using namespace engine::script::internal;
    CHECK(refusedSpecifierMessage("lodash", "res://a.js", SpecifierRefusal::Bare) ==
          "cannot resolve 'lodash' imported by 'res://a.js': a bare specifier names no file; use res://, ./ or ../");
    CHECK(refusedSpecifierMessage("lodash", "", SpecifierRefusal::Bare) ==
          "cannot resolve 'lodash': a bare specifier names no file; use res://, ./ or ../");
    CHECK(notFoundMessage("res://b.js", "res://a.js") ==
          "cannot load 'res://b.js' imported by 'res://a.js': not found or unreadable through the VFS");
    CHECK(tooLargeMessage("res://big.js", "", 1048577, 1048576) ==
          "cannot load 'res://big.js': its 1048577 bytes are over the 1048576-byte source limit (the VM's heap limit)");
    CHECK(tooDeepMessage("res://d9.js", "res://d8.js", 8, 65536) ==
          "cannot load 'res://d9.js' imported by 'res://d8.js': the import chain is deeper than 8 modules, the most a "
          "65536-byte stack limit allows");
    CHECK(noModuleSourceMessage("res://a.js", "") ==
          "cannot load 'res://a.js': the VM has no module source (it was created without a VirtualFileSystem)");
    CHECK(unsupportedAttributeMessage("type", "res://a.js") ==
          "import attribute 'type' in 'res://a.js' is not supported (this engine supports none)");
    CHECK(unsupportedAttributeMessage("type", "") ==
          "import attribute 'type' is not supported (this engine supports none)");
    CHECK(notFoundMessage("res://100%.js", "") ==
          "cannot load 'res://100%.js': not found or unreadable through the VFS");

    CHECK(specifierRefusalText(SpecifierRefusal::None).empty());
    struct Text {
        SpecifierRefusal refusal;
        std::string_view text;
    };
    const std::array<Text, 8> texts{{
        {SpecifierRefusal::Empty, "an empty specifier names no module"},
        {SpecifierRefusal::Bare, "a bare specifier names no file; use res://, ./ or ../"},
        {SpecifierRefusal::RootRelative, "a path starting with '/' is not a res:// path; use res://, ./ or ../"},
        {SpecifierRefusal::OtherScheme, "only res:// paths are supported, not a URL or a drive path"},
        {SpecifierRefusal::Reserved, "a '?' or a '#' is not supported in a module specifier"},
        {SpecifierRefusal::NoModuleImporter, "a relative specifier needs a module under res:// to resolve against"},
        {SpecifierRefusal::NotAddressable,
         "it is not an addressable res:// path (a backslash, a NUL, a ':' after the scheme, or a '..' above the root)"},
        {SpecifierRefusal::Root, "it names the res:// root, not a file"},
    }};
    std::set<std::string_view> distinct;
    for (const Text& row : texts) {
        CHECK(specifierRefusalText(row.refusal) == row.text);
        distinct.insert(specifierRefusalText(row.refusal));
    }
    // Anti-vacuity: no two refusals read alike.
    CHECK(distinct.size() == 8U);
}

TEST_CASE("script: the new labels are exact and distinct (ML12)") {
    const std::array<std::pair<ModuleState, std::string_view>, 5> states{{
        {ModuleState::Unloaded, "Unloaded"},
        {ModuleState::Loaded, "Loaded"},
        {ModuleState::Evaluating, "Evaluating"},
        {ModuleState::Evaluated, "Evaluated"},
        {ModuleState::Failed, "Failed"},
    }};
    std::set<std::string_view> distinct;
    for (const auto& [state, label] : states) {
        CHECK(moduleStateLabel(state) == label);
        CHECK(moduleStateLabel(state) != "Unknown");  // anti-vacuity: no state falls through the switch
        distinct.insert(moduleStateLabel(state));
    }
    CHECK(distinct.size() == 5U);

    using engine::script::EvalStatus;
    CHECK(engine::script::evalStatusLabel(EvalStatus::LoadError) == "LoadError");
    std::set<std::string_view> statuses;
    for (const EvalStatus status : std::array<EvalStatus, 8>{
             EvalStatus::Ok, EvalStatus::CompileError, EvalStatus::Exception, EvalStatus::OutOfMemory,
             EvalStatus::Interrupted, EvalStatus::InvalidVm, EvalStatus::WrongThread, EvalStatus::LoadError}) {
        CHECK(engine::script::evalStatusLabel(status) != "Unknown");
        statuses.insert(engine::script::evalStatusLabel(status));
    }
    CHECK(statuses.size() == 8U);
}

// ---- the VM cases' helpers (copied, not shared: script_vm_test.cpp's are TU-local) ---------------------------

namespace {

using engine::LogLevel;
using engine::script::EvalResult;
using engine::script::evalStatusLabel;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::VmCreateResult;
using engine::script::VmTeardownReport;

struct CapturedRecord {
    LogLevel level = LogLevel::Info;
    std::string message;
};

// Every record, from any thread (ML42 logs from a second one), copied under a lock.
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

    [[nodiscard]] std::vector<CapturedRecord> atLeast(LogLevel floor) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::vector<CapturedRecord> out;
        for (const CapturedRecord& record : records) {
            if (record.level >= floor) {
                out.push_back(record);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t countAtLeast(LogLevel floor) const { return atLeast(floor).size(); }

    [[nodiscard]] std::size_t countExactly(LogLevel level) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::size_t count = 0;
        for (const CapturedRecord& record : records) {
            if (record.level == level) {
                ++count;
            }
        }
        return count;
    }

    void clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        records.clear();
    }

private:
    mutable std::mutex mutex;
    std::vector<CapturedRecord> records;
};

// The tail of EVERY VM case: nothing leaked, retired module instances included.
void expectCleanShutdown(Vm& vm) {
    const VmTeardownReport report = vm.shutdown();
    CHECK(report.leakedAllocations == 0U);
    CHECK(report.leakedBytes == 0U);
    // R23: the teardown wrote nothing into memory the backend had already released. A case that turns this red has
    // found a new shape of that backend defect -- record it on R23's row, and never relax the check.
    CHECK(report.writtenAfterRelease == 0U);
    CHECK(report.repeatedReleases == 0U);
    CHECK_FALSE(vm.valid());
}

// "One ERROR": exactly one record at WARN or above, so a stray WARN fails too, and it is an ERROR carrying
// exactly `message`.
void expectOnlyError(const LogCapture& log, std::string_view message) {
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[0].message == message);
}

// The one ERROR's text (exactly one record at WARN or above, and it is an ERROR).
[[nodiscard]] std::string onlyErrorMessage(const LogCapture& log) {
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    return records[0].message;
}

[[nodiscard]] std::string_view firstLine(std::string_view text) { return text.substr(0, text.find('\n')); }

// For a case bounded by memory or depth, or one that measures: it must never be cut short by the clock.
[[nodiscard]] VmConfig patient(VmConfig config = {}) {
    config.callTimeBudget = engine::script::MAX_CALL_TIME_BUDGET;
    return config;
}

[[nodiscard]] std::string label(const EvalResult& result) { return std::string{evalStatusLabel(result.status)}; }

// A module source in memory, with per-path counters: how often the VFS asked for a size and for the bytes.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    void set(std::string relPath, std::string text) { files[std::move(relPath)] = std::move(text); }
    void erase(std::string_view relPath) {
        if (const auto found = files.find(relPath); found != files.end()) {
            files.erase(found);
        }
    }

    [[nodiscard]] bool exists(std::string_view relPath) const override { return files.contains(relPath); }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        ++sizeCounts[std::string{relPath}];
        const auto found = files.find(relPath);
        if (found == files.end()) {
            return std::nullopt;
        }
        if (const auto lie = sizeOverrides.find(relPath); lie != sizeOverrides.end()) {
            return lie->second;  // ML36: a size that is not the file's (the file grew after it was asked)
        }
        return static_cast<std::uint64_t>(found->second.size());
    }

    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        ++readCounts[std::string{relPath}];
        if (failingReads.contains(relPath)) {
            return std::nullopt;  // ML65: the size answered, the read does not
        }
        if (readDelay.count() > 0) {
            std::this_thread::sleep_for(readDelay);
        }
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

    [[nodiscard]] std::size_t sizeQueries(std::string_view relPath) const { return countOf(sizeCounts, relPath); }
    [[nodiscard]] std::size_t reads(std::string_view relPath) const { return countOf(readCounts, relPath); }
    [[nodiscard]] std::size_t totalReads() const {
        std::size_t total = 0;
        for (const auto& [path, count] : readCounts) {
            total += count;
        }
        return total;
    }

    std::chrono::milliseconds readDelay{0};
    std::map<std::string, std::uint64_t, std::less<>> sizeOverrides;
    std::set<std::string, std::less<>> failingReads;  // ML65: their size answers, their read does not

private:
    using Counts = std::map<std::string, std::size_t, std::less<>>;
    [[nodiscard]] static std::size_t countOf(const Counts& counts, std::string_view relPath) {
        const auto found = counts.find(relPath);
        return found == counts.end() ? 0U : found->second;
    }

    std::map<std::string, std::string, std::less<>> files;
    mutable Counts sizeCounts;
    mutable Counts readCounts;
};

// A VM over a VFS whose root is one MemoryBackend. Non-copyable and non-movable: the Vm borrows the VFS's address,
// so the VFS must never move. Declared VFS first, so the VM is destroyed first.
class ModuleFixture {
public:
    explicit ModuleFixture(const VmConfig& config = {}) {
        auto backend = std::make_unique<MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
        VmCreateResult created = Vm::create(config, vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
    }
    ModuleFixture(const ModuleFixture&) = delete;
    ModuleFixture& operator=(const ModuleFixture&) = delete;
    ModuleFixture(ModuleFixture&&) = delete;
    ModuleFixture& operator=(ModuleFixture&&) = delete;
    ~ModuleFixture() = default;

    engine::VirtualFileSystem vfs;
    MemoryBackend* files = nullptr;
    Vm vm;
};

// A VM over a caller's VFS, which must outlive it.
[[nodiscard]] Vm makeModuleVm(const engine::VirtualFileSystem& modules, const VmConfig& config = {}) {
    VmCreateResult created = Vm::create(config, modules);
    REQUIRE(created.vm.valid());
    return std::move(created.vm);
}

// A script's completion value; the script must succeed.
[[nodiscard]] std::string completion(Vm& vm, std::string_view source) {
    const EvalResult result = vm.evaluateScript(source, "probe.js");
    REQUIRE(label(result) == "Ok");
    return result.completion;
}

[[nodiscard]] std::optional<ModuleRecord> recordOf(const Vm& vm, std::string_view path) {
    for (ModuleRecord& record : vm.moduleRecords()) {
        if (record.path == path) {
            return std::move(record);
        }
    }
    return std::nullopt;
}

// The state label of a path's record, or "<absent>".
[[nodiscard]] std::string stateOf(const Vm& vm, std::string_view path) {
    const std::optional<ModuleRecord> record = recordOf(vm, path);
    return record.has_value() ? std::string{moduleStateLabel(record->state)} : std::string{"<absent>"};
}

[[nodiscard]] std::unique_ptr<engine::DirectoryBackend> fixtureBackend() {
    return std::make_unique<engine::DirectoryBackend>(AERO_SCRIPT_FIXTURES_DIR);
}

}  // namespace

// ---- ML13-ML19: the Definition of Done, the refusals and the cache --------------------------------------------

TEST_CASE("script: Epic 4.1's DoD -- a module graph on disk loads through the VFS and runs (ML13)") {
    namespace fs = std::filesystem;
    REQUIRE(fs::is_regular_file(fs::path{AERO_SCRIPT_FIXTURES_DIR} / "dod" / "main.js"));  // never a silent skip
    const LogCapture log;
    engine::VirtualFileSystem vfs;
    vfs.mount(fixtureBackend());
    Vm vm = makeModuleVm(vfs);
    // Anti-vacuity: the result is the module's doing, not present beforehand.
    CHECK(completion(vm, "typeof dodResult") == "undefined");

    const EvalResult result = vm.evaluateModule("res://dod/main.js");
    CHECK(label(result) == "Ok");
    CHECK(result.completion.empty());
    CHECK(result.error.name.empty());
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(vm, "dodResult") == "hello from res://shared/greeting.js 5");
    CHECK(completion(vm, "mathEvaluations") == "1");

    const std::vector<ModuleRecord> records = vm.moduleRecords();
    REQUIRE(records.size() == 3U);
    CHECK(records[0].path == "res://dod/lib/math.js");
    CHECK(records[1].path == "res://dod/main.js");
    CHECK(records[2].path == "res://shared/greeting.js");
    for (const ModuleRecord& record : records) {
        INFO("path: " << record.path);
        CHECK(record.generation == 1U);
        CHECK(moduleStateLabel(record.state) == "Evaluated");
    }
    CHECK(records[1].imports == std::vector<std::string>{"res://dod/lib/math.js", "res://shared/greeting.js"});
    CHECK(vm.stats().moduleInstances == 3U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: Epic 4.1's DoD -- a module's error reaches the engine log once, with its file and line (ML14)") {
    LogCapture log;
    engine::VirtualFileSystem vfs;
    vfs.mount(fixtureBackend());
    Vm vm = makeModuleVm(vfs);

    const EvalResult result = vm.evaluateModule("res://dod/throws.js");
    CHECK(label(result) == "Exception");
    CHECK(result.error.name == "Error");
    CHECK(result.error.message == "fixture failure 2");
    const std::string stack = "    at fail (res://dod/throws.js:5:15)\n    at <anonymous> (res://dod/throws.js:8:6)";
    CHECK(result.error.stack == stack);
    // ONE record: no unhandled-rejection record joined the module's own failure.
    expectOnlyError(log, "script: res://dod/throws.js: uncaught Error: fixture failure 2\n" + stack);
    CHECK(vm.stats().unhandledRejections == 0U);
    CHECK(stateOf(vm, "res://dod/throws.js") == "Failed");
    CHECK(stateOf(vm, "res://dod/lib/math.js") == "Loaded");  // a dependency of a failed entry

    log.clear();
    const EvalResult main = vm.evaluateModule("res://dod/main.js");
    CHECK(label(main) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(vm, "mathEvaluations") == "1");  // the shared instance did not run again
    // Anti-vacuity: the states are observed, not assumed -- Loaded became Evaluated.
    CHECK(stateOf(vm, "res://dod/lib/math.js") == "Evaluated");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a VM with no module source refuses evaluateModule with a LoadError saying so (ML15)") {
    const LogCapture log;
    VmCreateResult created = Vm::create(VmConfig{});
    REQUIRE(created.vm.valid());
    Vm vm = std::move(created.vm);

    const EvalResult result = vm.evaluateModule("res://a.js");
    CHECK(label(result) == "LoadError");
    CHECK(result.error.name == "TypeError");
    CHECK(result.error.message ==
          "cannot load 'res://a.js': the VM has no module source (it was created without a VirtualFileSystem)");
    CHECK(result.error.stack.empty());
    expectOnlyError(log, "script: res://a.js: load error: TypeError: " + result.error.message);
    CHECK(vm.moduleRecords().empty());
    CHECK(vm.stats().moduleInstances == 0U);
    // Anti-vacuity: the VM works; only the source is absent.
    CHECK(completion(vm, "1 + 1") == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a refused specifier is a LoadError naming it, its importer and the rule, and reads nothing (ML16)") {
    using engine::script::internal::refusedSpecifierMessage;
    using engine::script::internal::SpecifierRefusal;
    LogCapture log;
    ModuleFixture fx;
    struct Row {
        std::string_view specifier;
        SpecifierRefusal refusal;
    };
    const std::array<Row, 9> rows{{
        {"lodash", SpecifierRefusal::Bare},
        {"/abs.js", SpecifierRefusal::RootRelative},
        {"file:///a.js", SpecifierRefusal::OtherScheme},
        {"http://x/a.js", SpecifierRefusal::OtherScheme},
        {"C:/a.js", SpecifierRefusal::OtherScheme},
        {"", SpecifierRefusal::Empty},
        {"./a.js?x", SpecifierRefusal::Reserved},
        {"./a.js#f", SpecifierRefusal::Reserved},
        {"res://../a.js", SpecifierRefusal::NotAddressable},
    }};
    std::vector<std::string> recordTexts;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string index = std::to_string(i);
        const std::string module = "res://bad" + index + ".js";
        INFO("module: " << module << ", specifier: " << std::string{rows[i].specifier});
        fx.files->set("bad" + index + ".js",
                      "globalThis.ran" + index + " = true; import \"" + std::string{rows[i].specifier} + "\";");
        log.clear();
        const EvalResult result = fx.vm.evaluateModule(module);
        CHECK(label(result) == "LoadError");
        CHECK(result.error.name == "TypeError");
        CHECK(result.error.message == refusedSpecifierMessage(rows[i].specifier, module, rows[i].refusal));
        recordTexts.push_back(onlyErrorMessage(log));
        CHECK(completion(fx.vm, "typeof ran" + index) == "undefined");  // an import resolves before any statement
    }
    // Two rows spelled literally, so the case does not only compare the glue with the builder.
    REQUIRE(recordTexts.size() == rows.size());
    CHECK(recordTexts[0] ==
          "script: res://bad0.js: load error: TypeError: cannot resolve 'lodash' imported by 'res://bad0.js': a bare "
          "specifier names no file; use res://, ./ or ../");
    CHECK(recordTexts[7].ends_with("a '?' or a '#' is not supported in a module specifier"));

    // The entry arm.
    log.clear();
    CHECK(label(fx.vm.evaluateModule("scripts/a.js")) == "LoadError");
    expectOnlyError(log,
                    "script: scripts/a.js: load error: TypeError: cannot resolve 'scripts/a.js': a bare "
                    "specifier names no file; use res://, ./ or ../");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("./a.js")) == "LoadError");
    expectOnlyError(log,
                    "script: ./a.js: load error: TypeError: cannot resolve './a.js': a relative specifier "
                    "needs a module under res:// to resolve against");

    // Each bad module was read once, and nothing else was read; anti-vacuity: the counter counts.
    CHECK(fx.files->totalReads() == rows.size());
    fx.files->set("good.js", "import \"./ok.js\";");
    fx.files->set("ok.js", "export {};");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://good.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a second evaluateModule reads nothing, compiles nothing and re-runs nothing (ML17)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", "import { v } from \"./b.js\"; globalThis.aRuns = (globalThis.aRuns ?? 0) + 1;");
    fx.files->set("b.js", "globalThis.bRuns = (globalThis.bRuns ?? 0) + 1; export const v = 1;");

    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    // Anti-vacuity: the counters count -- exactly one read each.
    CHECK(fx.files->reads("a.js") == 1U);
    CHECK(fx.files->reads("b.js") == 1U);
    CHECK(fx.vm.stats().moduleInstances == 2U);

    fx.files->set("b.js", "export const v = 2;");  // the file changes; nothing invalidates
    for (int call = 2; call <= 3; ++call) {
        INFO("call " << call);
        CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
        CHECK(fx.files->reads("a.js") == 1U);
        CHECK(fx.files->reads("b.js") == 1U);
        CHECK(fx.vm.stats().moduleInstances == 2U);
        CHECK(completion(fx.vm, "aRuns") == "1");
        CHECK(completion(fx.vm, "bRuns") == "1");
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a cached failure is returned again, logged again, and not re-run (ML18)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", "globalThis.tries = (globalThis.tries ?? 0) + 1; throw new TypeError(\"once\");");

    const EvalResult first = fx.vm.evaluateModule("res://a.js");
    CHECK(label(first) == "Exception");
    CHECK(first.error.name == "TypeError");
    CHECK(first.error.message == "once");
    const std::string firstRecord = onlyErrorMessage(log);
    // Anti-vacuity: there is a stack, and it names the module.
    CHECK(firstLine(first.error.stack).find("(res://a.js:1:") != std::string_view::npos);

    log.clear();
    const EvalResult second = fx.vm.evaluateModule("res://a.js");
    CHECK(label(second) == "Exception");
    CHECK(second.error.name == first.error.name);  // the backend rethrows the recorded object
    CHECK(second.error.message == first.error.message);
    CHECK(second.error.stack == first.error.stack);
    expectOnlyError(log, firstRecord);
    CHECK(fx.files->reads("a.js") == 1U);
    CHECK(completion(fx.vm, "tries") == "1");
    CHECK(fx.vm.stats().unhandledRejections == 0U);  // the re-observed promise is not a new unhandled rejection
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: two entries importing one module share one instance (ML19)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("c.js", "globalThis.cRuns = (globalThis.cRuns ?? 0) + 1;");
    fx.files->set("e1.js", "import \"./c.js\";");
    fx.files->set("e2.js", "import \"./c.js\";");

    CHECK(label(fx.vm.evaluateModule("res://e1.js")) == "Ok");
    CHECK(stateOf(fx.vm, "res://c.js") == "Evaluated");  // after the first call already
    CHECK(label(fx.vm.evaluateModule("res://e2.js")) == "Ok");
    CHECK(fx.files->reads("c.js") == 1U);
    CHECK(completion(fx.vm, "cRuns") == "1");
    CHECK(fx.vm.stats().moduleInstances == 3U);
    // Anti-vacuity: the second entry was really loaded.
    CHECK(fx.files->reads("e2.js") == 1U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

// ---- ML22-ML30: retirement and the statuses -------------------------------------------------------------------

TEST_CASE("script: a load that failed retires what it compiled, and fixing the file is enough to retry (ML22)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", "import \"./b.js\"; globalThis.aDone = true;");

    const EvalResult first = fx.vm.evaluateModule("res://a.js");
    // Anti-vacuity: the first call really failed.
    CHECK(label(first) == "LoadError");
    CHECK(first.error.message ==
          "cannot load 'res://b.js' imported by 'res://a.js': not found or unreadable through the VFS");
    // a.js's own compile failed, so the backend freed it: generation 1, nothing to retire.
    const std::optional<ModuleRecord> a = recordOf(fx.vm, "res://a.js");
    REQUIRE(a.has_value());
    CHECK(a->generation == 1U);
    CHECK(moduleStateLabel(a->state) == "Unloaded");
    CHECK_FALSE(recordOf(fx.vm, "res://b.js").has_value());  // its read failed before any compile began
    CHECK(fx.files->reads("b.js") == 0U);                    // refused at the size query

    fx.files->set("b.js", "export {};");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "aDone") == "true");
    CHECK(fx.files->reads("a.js") == 2U);
    CHECK(fx.files->reads("b.js") == 1U);
    CHECK(stateOf(fx.vm, "res://a.js") == "Evaluated");
    CHECK(stateOf(fx.vm, "res://b.js") == "Evaluated");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: R22's dangling shape -- a cycle back-edge into a module whose load failed -- is retired (ML23)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("entry.js", R"(import "./x.js"; import "./missing.js"; export const e = 1;)");
    fx.files->set("x.js", "import { e } from \"./entry.js\"; export const x = 2;");
    fx.files->set("entry2.js", "import { x } from \"./x.js\"; globalThis.got = x;");

    CHECK(label(fx.vm.evaluateModule("res://entry.js")) == "LoadError");
    const std::optional<ModuleRecord> x = recordOf(fx.vm, "res://x.js");
    REQUIRE(x.has_value());
    CHECK(x->generation == 2U);  // it compiled, then its attempt failed
    CHECK(moduleStateLabel(x->state) == "Unloaded");
    const std::optional<ModuleRecord> entry = recordOf(fx.vm, "res://entry.js");
    REQUIRE(entry.has_value());
    CHECK(entry->generation == 1U);  // its own compile failed: the backend freed it, nothing to retire

    // Without the retire rule this call finds the first x.js by name and links through the freed entry.js.
    fx.files->set("missing.js", "export {};");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://entry2.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "got") == "2");
    // Anti-vacuity: the retired name forced a fresh read.
    CHECK(fx.files->reads("x.js") == 2U);
    const std::optional<ModuleRecord> fresh = recordOf(fx.vm, "res://x.js");
    REQUIRE(fresh.has_value());
    CHECK(fresh->generation == 2U);
    CHECK(moduleStateLabel(fresh->state) == "Evaluated");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a link that failed after binding an import is never linked again (ML24)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("j.js", "export const ok = 1;");
    fx.files->set("half.js", R"(import { ok } from "./j.js"; import { nope } from "./j.js"; export const h = ok;)");
    fx.files->set("top1.js", "import \"./half.js\";");
    fx.files->set("top2.js", "import \"./half.js\";");

    const EvalResult first = fx.vm.evaluateModule("res://top1.js");
    CHECK(label(first) == "CompileError");
    CHECK(first.error.name == "SyntaxError");
    CHECK(first.error.message == "Could not find export 'nope' in module 'res://j.js'");
    CHECK(first.error.stack.empty());
    expectOnlyError(log,
                    "script: res://top1.js: compile error: SyntaxError: Could not find export 'nope' in module "
                    "'res://j.js'");
    for (const std::string_view path :
         std::array<std::string_view, 3>{"res://top1.js", "res://half.js", "res://j.js"}) {
        INFO("path: " << std::string{path});
        const std::optional<ModuleRecord> record = recordOf(fx.vm, path);
        REQUIRE(record.has_value());
        CHECK(record->generation == 2U);
    }

    log.clear();
    const EvalResult second = fx.vm.evaluateModule("res://top2.js");
    CHECK(label(second) == "CompileError");
    // Anti-vacuity: a fresh instance of j.js, under its next generation name.
    CHECK(second.error.message == "Could not find export 'nope' in module 'res://j.js?gen=2'");
    CHECK(fx.files->reads("half.js") == 2U);
    // Without retirement half.js is linked twice and teardown aborts (Debug) or leaks a block (Release).
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module the VFS cannot read is a LoadError, entry or dependency (ML25)") {
    LogCapture log;
    ModuleFixture fx;
    CHECK(label(fx.vm.evaluateModule("res://nope.js")) == "LoadError");
    expectOnlyError(log,
                    "script: res://nope.js: load error: TypeError: cannot load 'res://nope.js': not found or "
                    "unreadable through the VFS");

    fx.files->set("dir/a.js", "globalThis.dirA = 1;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://dir")) == "LoadError");  // a directory of the backend is no module
    expectOnlyError(log,
                    "script: res://dir: load error: TypeError: cannot load 'res://dir': not found or unreadable "
                    "through the VFS");

    fx.files->set("a.js", "import \"./missing.js\";");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "LoadError");
    expectOnlyError(log,
                    "script: res://a.js: load error: TypeError: cannot load 'res://missing.js' imported by "
                    "'res://a.js': not found or unreadable through the VFS");

    // Anti-vacuity: the file inside the directory loads in the same VM.
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://dir/a.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a syntax error in a dependency is a CompileError naming that file, and nothing ran (ML26)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", "globalThis.aRan = true; import \"./b.js\";");
    fx.files->set("b.js", "globalThis.bRan = true;\nlet x = ;");

    const EvalResult result = fx.vm.evaluateModule("res://a.js");
    CHECK(label(result) == "CompileError");
    CHECK(result.error.name == "SyntaxError");
    // Anti-vacuity: the frame names b.js, not a.js.
    CHECK(firstLine(result.error.stack).starts_with("    at res://b.js:2:"));
    expectOnlyError(
        log, "script: res://a.js: compile error: SyntaxError: " + result.error.message + "\n" + result.error.stack);
    CHECK(completion(fx.vm, "typeof aRan") == "undefined");
    CHECK(completion(fx.vm, "typeof bRan") == "undefined");

    fx.files->set("b.js", "globalThis.bRan = true;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "aRan") == "true");
    CHECK(completion(fx.vm, "bRan") == "true");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a link error is a CompileError with the backend's message and no stack (ML27)") {
    LogCapture log;
    ModuleFixture fx;
    // (a) a missing export.
    fx.files->set("j.js", "export const ok = 1;");
    fx.files->set("a.js", "import { nope } from \"./j.js\";");
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "CompileError");
    expectOnlyError(log,
                    "script: res://a.js: compile error: SyntaxError: Could not find export 'nope' in module "
                    "'res://j.js'");

    // (b) an ambiguous star export.
    fx.files->set("a1.js", "export const x = 1;");
    fx.files->set("a2.js", "export const x = 2;");
    fx.files->set("star.js", R"(export * from "./a1.js"; export * from "./a2.js";)");
    fx.files->set("e.js", "import { x } from \"./star.js\";");
    log.clear();
    const EvalResult ambiguous = fx.vm.evaluateModule("res://e.js");
    CHECK(label(ambiguous) == "CompileError");
    CHECK(ambiguous.error.message == "export 'x' in module 'res://star.js' is ambiguous");
    CHECK(ambiguous.error.stack.empty());
    CHECK(onlyErrorMessage(log) ==
          "script: res://e.js: compile error: SyntaxError: export 'x' in module 'res://star.js' is ambiguous");

    // (c) anti-vacuity: the module itself is loadable -- the failure was the import's. A namespace import omits the
    // ambiguous name, and star.js loads afresh: (b)'s attempt retired it.
    fx.files->set("n.js", "import * as ns from \"./star.js\"; globalThis.keys = Object.keys(ns).join();");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://n.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "keys") == "");
    const std::optional<ModuleRecord> star = recordOf(fx.vm, "res://star.js");
    REQUIRE(star.has_value());
    CHECK(star->generation == 2U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module that throws is one Exception and never also an unhandled rejection (ML28)") {
    LogCapture log;
    ModuleFixture fx;
    // (a) a throw in the entry.
    fx.files->set("t.js", "throw new Error(\"boom\");");
    const EvalResult thrown = fx.vm.evaluateModule("res://t.js");
    CHECK(label(thrown) == "Exception");
    CHECK(firstLine(thrown.error.stack) == "    at <anonymous> (res://t.js:1:10)");
    CHECK(onlyErrorMessage(log).starts_with("script: res://t.js: uncaught Error: boom"));
    CHECK(fx.vm.stats().unhandledRejections == 0U);

    // (b) a throw in a dependency.
    fx.files->set("d.js", "import \"./dep.js\";");
    fx.files->set("dep.js", "throw new TypeError(\"dep boom\");");
    log.clear();
    const EvalResult dependency = fx.vm.evaluateModule("res://d.js");
    CHECK(label(dependency) == "Exception");
    CHECK(firstLine(dependency.error.stack).find("(res://dep.js:1:") != std::string_view::npos);
    CHECK(onlyErrorMessage(log).starts_with("script: res://d.js: uncaught TypeError: dep boom"));

    // (c) a primitive.
    fx.files->set("p.js", "throw 42;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://p.js")) == "Exception");
    expectOnlyError(log, "script: res://p.js: uncaught exception: 42");

    // (d) anti-vacuity: the watch drops the module's own failure and keeps the script's own rejection.
    fx.files->set("own.js", R"(Promise.reject(new Error("own")); throw new Error("mine");)");
    log.clear();
    const EvalResult own = fx.vm.evaluateModule("res://own.js");
    CHECK(label(own) == "Exception");
    CHECK(own.error.message == "mine");
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].message.starts_with("script: res://own.js: uncaught Error: mine"));
    CHECK(records[1].message.starts_with("script: res://own.js: unhandled promise rejection: Error: own"));
    CHECK(fx.vm.stats().unhandledRejections == 1U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a top-level await that rejects is an Exception; one that never settles is Ok with a WARN (ML29)") {
    LogCapture log;
    ModuleFixture fx;
    // (a) rejected after an await: recorded during the drain, attributed by identity.
    fx.files->set("late.js", "await null; throw new RangeError(\"late\");");
    const EvalResult late = fx.vm.evaluateModule("res://late.js");
    CHECK(label(late) == "Exception");
    CHECK(late.error.name == "RangeError");
    CHECK(late.error.message == "late");
    CHECK(onlyErrorMessage(log).starts_with("script: res://late.js: uncaught RangeError: late"));
    CHECK(fx.vm.stats().unhandledRejections == 0U);

    // (b) pending at the end of the call, settled by a later one.
    const std::string pending =
        "script: res://wait.js: module evaluation is still pending at the end of the call (a top-level await has not "
        "settled)";
    fx.files->set("wait.js",
                  "globalThis.release = null; await new Promise((r) => { globalThis.release = r; }); "
                  "globalThis.after = true;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://wait.js")) == "Ok");
    std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Warn);
    CHECK(records[0].message == pending);
    CHECK(stateOf(fx.vm, "res://wait.js") == "Evaluating");
    CHECK(completion(fx.vm, "typeof after") == "undefined");
    log.clear();
    CHECK(completion(fx.vm, "release(); 0") == "0");  // its drain resumes the module
    CHECK(label(fx.vm.evaluateModule("res://wait.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    // Anti-vacuity: the record follows the module.
    CHECK(stateOf(fx.vm, "res://wait.js") == "Evaluated");
    CHECK(completion(fx.vm, "after") == "true");

    // (c) never settles.
    fx.files->set("never.js", "await new Promise(() => {});");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://never.js")) == "Ok");
    records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Warn);
    CHECK(records[0].message ==
          "script: res://never.js: module evaluation is still pending at the end of the call (a top-level await has "
          "not settled)");
    CHECK(stateOf(fx.vm, "res://never.js") == "Evaluating");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module can run out of heap or budget like a script, and the VM recovers (ML30)") {
    {
        // (a) out of heap.
        const LogCapture log;
        ModuleFixture fx{patient(VmConfig{.heapLimitBytes = 4 * MIB})};
        fx.files->set("m.js", "globalThis.big = \"x\".repeat(8 * 1024 * 1024);");
        CHECK(label(fx.vm.evaluateModule("res://m.js")) == "OutOfMemory");
        CHECK(firstLine(onlyErrorMessage(log))
                  .starts_with("script: res://m.js: out of memory (heap limit 4194304 bytes, "));
        CHECK(completion(fx.vm, "1 + 1") == "2");
        expectCleanShutdown(fx.vm);
    }
    {
        // (b) out of budget, in a module body.
        const LogCapture log;
        ModuleFixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{50}}};
        fx.files->set("loop.js", "for (;;) {}");
        const auto start = std::chrono::steady_clock::now();
        const EvalResult result = fx.vm.evaluateModule("res://loop.js");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(label(result) == "Interrupted");
        CHECK(result.error.name == "InternalError");
        CHECK(result.error.message == "interrupted");
        CHECK(firstLine(onlyErrorMessage(log)) ==
              "script: res://loop.js: interrupted after exceeding the 50 ms time budget");
        CHECK(fx.vm.stats().unhandledRejections == 0U);
        CHECK(stateOf(fx.vm, "res://loop.js") == "Failed");
        // Anti-vacuity, a lower bound only: the budget really ran.
        CHECK(elapsed >= std::chrono::milliseconds{50});
        CHECK(completion(fx.vm, "1 + 1") == "2");
        expectCleanShutdown(fx.vm);
    }
}

// ---- ML35-ML37, ML39: the limits and the static attribute check -------------------------------------------------

namespace {

// A chain prefix1.js -> prefix2.js -> ... -> prefix<length>.js, the last one importing nothing.
void writeChain(MemoryBackend& files, std::string_view prefix, int length) {
    for (int i = 1; i <= length; ++i) {
        const std::string name = std::string{prefix} + std::to_string(i) + ".js";
        files.set(name, i < length ? "import \"./" + std::string{prefix} + std::to_string(i + 1) + ".js\";"
                                   : std::string{"export {};"});
    }
}

}  // namespace

TEST_CASE("script: the import chain is capped by depth, and the cap is the engine's, not the backend's (ML35)") {
    LogCapture log;
    {
        ModuleFixture fx{patient(VmConfig{.stackLimitBytes = engine::script::MIN_STACK_LIMIT_BYTES})};
        // Anti-vacuity: the longest chain the cap admits loads -- the backend's own RangeError did not fire first.
        writeChain(*fx.files, "c", 8);
        CHECK(label(fx.vm.evaluateModule("res://c1.js")) == "Ok");
        CHECK(fx.vm.stats().moduleInstances == 8U);

        writeChain(*fx.files, "d", 9);
        log.clear();
        const EvalResult deep = fx.vm.evaluateModule("res://d1.js");
        CHECK(label(deep) == "LoadError");
        CHECK(deep.error.message ==
              "cannot load 'res://d9.js' imported by 'res://d8.js': the import chain is deeper "
              "than 8 modules, the most a 65536-byte stack limit allows");
        CHECK(fx.files->reads("d9.js") == 0U);
        CHECK(log.countAtLeast(LogLevel::Warn) == 1U);

        // The cap counts nesting, not imports: nine modules side by side are one level.
        std::string wide;
        for (int i = 1; i <= 9; ++i) {
            const std::string name = "w" + std::to_string(i) + ".js";
            fx.files->set(name, "export {};");
            wide += "import \"./" + name + "\";\n";
        }
        fx.files->set("wide.js", wide);
        log.clear();
        CHECK(label(fx.vm.evaluateModule("res://wide.js")) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expectCleanShutdown(fx.vm);
    }
    {
        ModuleFixture fx{patient()};  // the default 512 KiB
        writeChain(*fx.files, "e", 64);
        log.clear();
        CHECK(label(fx.vm.evaluateModule("res://e1.js")) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        writeChain(*fx.files, "f", 65);
        const EvalResult deep = fx.vm.evaluateModule("res://f1.js");
        CHECK(label(deep) == "LoadError");
        CHECK(deep.error.message ==
              "cannot load 'res://f65.js' imported by 'res://f64.js': the import chain is "
              "deeper than 64 modules, the most a 524288-byte stack limit allows");
        expectCleanShutdown(fx.vm);
    }
}

TEST_CASE("script: a source over the heap limit is refused before it is read (ML36)") {
    LogCapture log;
    ModuleFixture fx{patient(VmConfig{.heapLimitBytes = MIB})};
    // One comment line of exactly MIB + 1 bytes: a source that would compile, were it read.
    std::string big = "//";
    big.append(MIB + 1 - big.size(), 'x');
    REQUIRE(big.size() == MIB + 1);
    fx.files->set("big.js", big);

    const EvalResult entry = fx.vm.evaluateModule("res://big.js");
    CHECK(label(entry) == "LoadError");
    CHECK(entry.error.message ==
          "cannot load 'res://big.js': its 1048577 bytes are over the 1048576-byte source limit (the VM's heap limit)");
    CHECK(fx.files->reads("big.js") == 0U);
    CHECK(fx.files->sizeQueries("big.js") == 1U);

    fx.files->set("a.js", "import \"./big.js\";");
    const EvalResult dependency = fx.vm.evaluateModule("res://a.js");
    CHECK(label(dependency) == "LoadError");
    CHECK(dependency.error.message ==
          "cannot load 'res://big.js' imported by 'res://a.js': its 1048577 bytes are over "
          "the 1048576-byte source limit (the VM's heap limit)");
    CHECK(fx.files->reads("big.js") == 0U);

    // A size that lied (the file grew after it was asked): the bytes read are checked too.
    fx.files->set("grown.js", big);
    fx.files->sizeOverrides["grown.js"] = 16;
    const EvalResult grown = fx.vm.evaluateModule("res://grown.js");
    CHECK(label(grown) == "LoadError");
    CHECK(grown.error.message.find("1048577") != std::string::npos);
    CHECK(fx.files->reads("grown.js") == 1U);

    // Anti-vacuity: a 1 KiB module loads in the same VM.
    std::string small = "//";
    small.append(KIB - small.size(), 'x');
    fx.files->set("small.js", small);
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://small.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a deadline that passes while loading ends the call Interrupted and reads no more (ML37)") {
    LogCapture log;
    ModuleFixture fx{VmConfig{.callTimeBudget = engine::script::MIN_CALL_TIME_BUDGET}};
    fx.files->readDelay = std::chrono::milliseconds{20};
    fx.files->set("a.js", "import \"./b.js\";");
    fx.files->set("b.js", "export {};");

    const EvalResult result = fx.vm.evaluateModule("res://a.js");
    CHECK(label(result) == "Interrupted");
    expectOnlyError(log, "script: res://a.js: interrupted after exceeding the 1 ms time budget");  // nothing ran
    CHECK(fx.files->reads("b.js") == 0U);
    CHECK(fx.vm.stats().moduleInstances == 0U);

    // Anti-vacuity: the files are loadable, so only the deadline stopped the first VM.
    fx.files->readDelay = std::chrono::milliseconds{0};
    {
        Vm unhurried = makeModuleVm(fx.vfs);  // the default budget, over the same files
        log.clear();
        CHECK(label(unhurried.evaluateModule("res://a.js")) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expectCleanShutdown(unhurried);
    }

    // The check AFTER a read: the slow read is made, but the compile never begins.
    fx.files->readDelay = std::chrono::milliseconds{20};
    fx.files->set("solo.js", "globalThis.solo = 1;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://solo.js")) == "Interrupted");
    CHECK(fx.vm.stats().moduleInstances == 0U);
    CHECK_FALSE(recordOf(fx.vm, "res://solo.js").has_value());
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a static import attribute is a LoadError; an empty clause is no attribute (ML39)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("j.js", "export default 5;");
    fx.files->set("a.js", R"(import v from "./j.js" with { type: "json" };)");
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "LoadError");
    expectOnlyError(log,
                    "script: res://a.js: load error: SyntaxError: import attribute 'type' in 'res://a.js' is not "
                    "supported (this engine supports none)");
    CHECK(fx.files->reads("j.js") == 0U);  // refused in the parser, before the specifier is resolved

    // Anti-vacuity: the checker refuses keys, not imports.
    fx.files->set("b.js", "import v from \"./j.js\" with {}; globalThis.v = v;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://b.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "v") == "5");
    expectCleanShutdown(fx.vm);
}

// ---- ML40-ML43, ML45, ML46, ML48, ML50, ML51: boundaries, encoding, spellings, mounts, real paths, cost, a stale job

TEST_CASE("script: the script layer reads only through the VFS and makes modules in one place (ML40)") {
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
    const auto occurrences = [](std::string_view text, std::string_view token) {
        std::size_t count = 0;
        for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
            ++count;
        }
        return count;
    };

    // A DERIVED set: every .cpp and .hpp under the script layer -- src, include and internal -- never an enumeration.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator{fs::path{AERO_SCRIPT_SRC_DIR}.parent_path()}) {
        if (entry.is_regular_file() && (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")) {
            files.push_back(entry.path());
        }
    }

    std::size_t moduleCompiles = 0;
    std::size_t moduleCompilesInGlue = 0;
    bool glueReadsText = false;
    bool graphNormalizes = false;
    bool vmInstallsLoader = false;
    for (const fs::path& file : files) {
        const std::string text = strippedSource(file);
        const std::string name = file.filename().string();
        INFO("file: " << name);
        // Every module is read through the VFS: no file system, no stream, and none of the backend's own loaders.
        for (const std::string_view banned : std::array<std::string_view, 6>{
                 "std::filesystem", "<filesystem>", "fstream", "fopen(", "JS_LoadModule(", "JS_ResolveModule("}) {
            INFO("token: " << std::string{banned});
            CHECK(text.find(banned) == std::string::npos);
        }
        const std::size_t compiles = occurrences(text, "JS_EVAL_TYPE_MODULE");
        moduleCompiles += compiles;
        if (name == "vm_modules.cpp") {
            moduleCompilesInGlue += compiles;
            glueReadsText = text.find("readText(") != std::string::npos;
        }
        if (name == "module_graph.cpp") {
            graphNormalizes = text.find("normalizeVirtualPath(") != std::string::npos;
        }
        if (name == "vm.cpp") {
            vmInstallsLoader = text.find("JS_SetModuleLoaderFunc2(") != std::string::npos;
        }
    }
    // ONE place makes a module out of source text.
    CHECK(moduleCompiles == 1U);
    CHECK(moduleCompilesInGlue == 1U);
    CHECK(glueReadsText);
    CHECK(graphNormalizes);
    // Anti-vacuity: the scan visited the layer, and the same stripped scan DOES find a real backend call.
    CHECK(files.size() >= 8U);
    CHECK(vmInstallsLoader);
}

TEST_CASE("script: a module sees the language and nothing else (ML41)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("probe.js",
                  "globalThis.probe = [typeof require, typeof module, typeof exports, typeof __filename, "
                  "Object.keys(import.meta).length, Object.getPrototypeOf(import.meta) === null, "
                  "typeof JSON].join();");
    const std::string before = completion(fx.vm, "Object.getOwnPropertyNames(globalThis).length");
    CHECK(label(fx.vm.evaluateModule("res://probe.js")) == "Ok");
    // Anti-vacuity: typeof JSON is "object" in the same probe.
    CHECK(completion(fx.vm, "probe") == "undefined,undefined,undefined,undefined,0,true,object");
    // The global object gained exactly one own property: the probe's.
    const std::string after = completion(fx.vm, "Object.getOwnPropertyNames(globalThis).length");
    CHECK(std::stoi(after) == std::stoi(before) + 1);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: evaluateModule and moduleRecords keep the VM's thread rule and refuse an invalid Vm (ML42)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", "globalThis.a = 1;");
    Vm vm = std::move(fx.vm);  // fx.vm is now a moved-from Vm

    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "InvalidVm");
    expectOnlyError(log, "script: evaluateModule on an invalid Vm (a failed create or a moved-from Vm)");
    log.clear();
    CHECK(fx.vm.moduleRecords().empty());
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);

    EvalResult fromThread;
    std::vector<ModuleRecord> recordsFromThread{ModuleRecord{}};
    bool ran = false;
    std::thread worker{[&] {
        fromThread = vm.evaluateModule("res://a.js");
        recordsFromThread = vm.moduleRecords();
        ran = true;
    }};
    worker.join();
    // Anti-vacuity: the thread really ran.
    REQUIRE(ran);
    CHECK(label(fromThread) == "WrongThread");
    CHECK(recordsFromThread.empty());
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[0].message == "script: evaluateModule called from a thread that is not the VM's; refused");
    CHECK(records[1].level == LogLevel::Error);
    CHECK(records[1].message == "script: moduleRecords called from a thread that is not the VM's; refused");

    // Back on its thread, the VM works.
    log.clear();
    CHECK(label(vm.evaluateModule("res://a.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: source bytes reach the parser verbatim (ML43)") {
    namespace fs = std::filesystem;
    const LogCapture log;
    {
        // Anti-vacuity: the BOM bytes are really in the committed fixture.
        std::ifstream file{fs::path{AERO_SCRIPT_FIXTURES_DIR} / "encoding" / "bom.js", std::ios::binary};
        REQUIRE(file);
        std::array<char, 3> head{};
        file.read(head.data(), static_cast<std::streamsize>(head.size()));
        REQUIRE(file.gcount() == 3);
        CHECK(static_cast<unsigned char>(head[0]) == 0xEFU);
        CHECK(static_cast<unsigned char>(head[1]) == 0xBBU);
        CHECK(static_cast<unsigned char>(head[2]) == 0xBFU);

        engine::VirtualFileSystem vfs;
        vfs.mount(fixtureBackend());
        Vm vm = makeModuleVm(vfs);
        CHECK(label(vm.evaluateModule("res://encoding/bom.js")) == "Ok");
        CHECK(completion(vm, "bomLoaded") == "true");
        expectCleanShutdown(vm);
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    ModuleFixture fx;
    fx.files->set("shebang.js", "#!/usr/bin/env qjs\nglobalThis.sb = 1;");
    CHECK(label(fx.vm.evaluateModule("res://shebang.js")) == "Ok");
    CHECK(completion(fx.vm, "sb") == "1");

    fx.files->set("bomshebang.js", "\xEF\xBB\xBF#!x\nglobalThis.x = 1;");
    const EvalResult bomShebang = fx.vm.evaluateModule("res://bomshebang.js");
    CHECK(label(bomShebang) == "CompileError");
    CHECK(bomShebang.error.message == "invalid first character of private name");

    fx.files->set("badstring.js", "globalThis.bad = '\xC3\x28';");
    const EvalResult badString = fx.vm.evaluateModule("res://badstring.js");
    CHECK(label(badString) == "CompileError");
    CHECK(badString.error.message == "invalid UTF-8 sequence");

    fx.files->set("badcomment.js", "// \xC3\x28\nglobalThis.c = 1;");
    CHECK(label(fx.vm.evaluateModule("res://badcomment.js")) == "Ok");
    CHECK(completion(fx.vm, "c") == "1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 2U);  // the two CompileErrors, nothing else
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: one canonical key per spelling of a path, and case is part of the key (ML45)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("b.js", "export {};");
    fx.files->set("e1.js", R"(import "./b.js"; import "./x/../b.js";)");
    CHECK(label(fx.vm.evaluateModule("res://e1.js")) == "Ok");
    std::vector<ModuleRecord> records = fx.vm.moduleRecords();
    REQUIRE(records.size() == 2U);
    CHECK(records[0].path == "res://b.js");
    CHECK(records[1].path == "res://e1.js");
    CHECK(fx.files->reads("b.js") == 1U);

    fx.files->set("B.js", "export {};");  // the memory backend is case-sensitive
    fx.files->set("b2.js", "import \"./b.js\";");
    fx.files->set("e2.js", R"(import "./B.js"; import "./b2.js";)");
    CHECK(label(fx.vm.evaluateModule("res://e2.js")) == "Ok");
    // Anti-vacuity: two case spellings are two records -- what a case-insensitive disk turns into two instances.
    CHECK(recordOf(fx.vm, "res://B.js").has_value());
    CHECK(recordOf(fx.vm, "res://b.js").has_value());
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a backend mounted between calls serves the next call (ML46)") {
    LogCapture log;
    ModuleFixture fx;
    // Anti-vacuity: the first call fails -- and a failed load caches no negative answer.
    CHECK(label(fx.vm.evaluateModule("res://dlc/x.js")) == "LoadError");
    auto dlc = std::make_unique<MemoryBackend>();
    dlc->set("x.js", "globalThis.dlc = 1;");
    fx.vfs.mount("res://dlc", std::move(dlc));
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://dlc/x.js")) == "Ok");
    CHECK(completion(fx.vm, "dlc") == "1");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a 521-module graph loads, links and evaluates in one call (ML48)") {
    const LogCapture log;
    ModuleFixture fx{patient()};  // the case measures: it must not race the clock
    std::string root;
    for (int i = 0; i < 20; ++i) {
        const std::string middle = "m" + std::to_string(i);
        root += "import \"./" + middle + ".js\";\n";
        std::string leaves;
        for (int j = 0; j < 25; ++j) {
            const std::string leaf = "l" + std::to_string(i) + "_" + std::to_string(j) + ".js";
            fx.files->set(leaf, "export const v = 1;");
            leaves += "import \"./" + leaf + "\";\n";
        }
        fx.files->set(middle + ".js", leaves);
    }
    fx.files->set("root.js", root);

    const auto start = std::chrono::steady_clock::now();
    const EvalResult result = fx.vm.evaluateModule("res://root.js");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(label(result) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(fx.vm.stats().moduleInstances == 521U);
    // Anti-vacuity: a graph that silently stopped early would read fewer.
    CHECK(fx.files->totalReads() == 521U);
    const std::vector<ModuleRecord> records = fx.vm.moduleRecords();
    CHECK(records.size() == 521U);
    std::size_t evaluated = 0;
    for (const ModuleRecord& record : records) {
        evaluated += record.state == ModuleState::Evaluated ? 1U : 0U;
    }
    CHECK(evaluated == 521U);
    MESSAGE("large graph: 521 modules in " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                                           << " ms");
    expectCleanShutdown(fx.vm);
}

namespace {

// A temporary directory unique to this run (a random suffix: two concurrent runs never share it), removed with its
// contents on destruction.
class UniqueTempDir {
public:
    UniqueTempDir() {
        std::random_device entropy;
        dirPath = std::filesystem::temp_directory_path() /
                  ("aero_script_module_test_" + std::to_string(entropy()) + "_" + std::to_string(entropy()));
        std::filesystem::create_directories(dirPath);
    }
    UniqueTempDir(const UniqueTempDir&) = delete;
    UniqueTempDir& operator=(const UniqueTempDir&) = delete;
    UniqueTempDir(UniqueTempDir&&) = delete;
    UniqueTempDir& operator=(UniqueTempDir&&) = delete;
    ~UniqueTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return dirPath; }

    // The root as UTF-8, what DirectoryBackend expects (path::string() is the code page on Windows).
    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // Writes a file at a UTF-8 relative path, creating its parent directories.
    void write(std::string_view relativeUtf8, std::string_view contents) const {
        const std::u8string relative(reinterpret_cast<const char8_t*>(relativeUtf8.data()), relativeUtf8.size());
        const std::filesystem::path full = dirPath / std::filesystem::path(relative);
        std::filesystem::create_directories(full.parent_path());
        std::ofstream stream(full, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

private:
    std::filesystem::path dirPath;
};

}  // namespace

TEST_CASE("script: a module tree under a non-ASCII directory and behind a directory symlink loads (ML50)") {
    const LogCapture log;
    const UniqueTempDir dir;
    // "módulos" -- a hex escape keeps the literal 7-bit (MSVC reads source through the code page).
    dir.write(
        "m\xC3\xB3"
        "dulos/main.js",
        "import { x } from \"./lib/x.js\"; globalThis.nonAscii = x;");
    dir.write(
        "m\xC3\xB3"
        "dulos/lib/x.js",
        "export const x = \"ok\";");
    dir.write("real/y.js", "globalThis.linked = 1;");
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(dir.utf8()));
    Vm vm = makeModuleVm(vfs);

    // Anti-vacuity: this arm runs on every lane and asserts both records.
    CHECK(label(vm.evaluateModule("res://m\xC3\xB3"
                                  "dulos/main.js")) == "Ok");
    CHECK(completion(vm, "nonAscii") == "ok");
    const std::vector<ModuleRecord> records = vm.moduleRecords();
    REQUIRE(records.size() == 2U);
    CHECK(records[0].path ==
          "res://m\xC3\xB3"
          "dulos/lib/x.js");
    CHECK(records[1].path ==
          "res://m\xC3\xB3"
          "dulos/main.js");

    std::error_code ec;
    std::filesystem::create_directory_symlink(dir.path() / "real", dir.path() / "linked", ec);
    if (ec) {
        // A Windows host without the symlink privilege: the one environment-dependent arm reports, never asserts.
        MESSAGE("symlink arm not run: " << ec.message());
    } else {
        CHECK(label(vm.evaluateModule("res://linked/y.js")) == "Ok");
        CHECK(completion(vm, "linked") == "1");
        CHECK(recordOf(vm, "res://linked/y.js").has_value());  // the VIRTUAL path, never the resolved one
        CHECK_FALSE(recordOf(vm, "res://real/y.js").has_value());
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a job an earlier call left queued is its own Exception record in a refused call's drain (ML51)") {
    LogCapture log;
    ModuleFixture fx;
    // The end-of-call report describes a non-Error reason through its toString, AFTER the call's drain, so the
    // microtask it queues is left for the next call -- no timing involved.
    const EvalResult seed = fx.vm.evaluateScript(
        "Promise.reject({ toString() { queueMicrotask(() => { throw new Error('stale'); }); return 'r'; } }); 0",
        "seed.js");
    CHECK(label(seed) == "Ok");
    expectOnlyError(log, "script: seed.js: unhandled promise rejection: r");

    log.clear();
    const EvalResult refused = fx.vm.evaluateModule("lodash");
    CHECK(label(refused) == "LoadError");  // the entry's refusal decides: the first failure of the call
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].message ==
          "script: lodash: load error: TypeError: cannot resolve 'lodash': a bare specifier "
          "names no file; use res://, ./ or ../");
    // The stale job ran in the refused call's drain and was classified by ITS phase, not by the call's refusal.
    CHECK(firstLine(records[1].message) == "script: lodash: uncaught Error: stale");

    // Anti-vacuity: the job ran exactly once.
    log.clear();
    CHECK(label(fx.vm.evaluateScript("0", "after.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

// ---- ML52: the teardown hold, through the VFS -----------------------------------------------------------------

namespace {

// Tears the fixture's VM down and checks the report: no leak, nothing released twice, and the late writes counted
// (`expectWrites`) or not. The teardown itself must log nothing at WARN or above.
std::size_t shutdownCountingWrites(LogCapture& log, ModuleFixture& fx, bool expectWrites) {
    log.clear();
    const VmTeardownReport report = fx.vm.shutdown();
    CHECK(report.leakedAllocations == 0U);
    CHECK(report.leakedBytes == 0U);
    CHECK(report.repeatedReleases == 0U);
    if (expectWrites) {
        CHECK(report.writtenAfterRelease >= 1U);
    } else {
        CHECK(report.writtenAfterRelease == 0U);
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    return report.writtenAfterRelease;
}

}  // namespace

TEST_CASE(
    "script: a top-level await still pending at teardown is held, and its writes after release are counted "
    "(ML52)") {
    LogCapture log;
    const std::string held = "globalThis.release = null; await new Promise((r) => { globalThis.release = r; });";
    {
        // (a) a resolver a global keeps, evaluated as an entry.
        ModuleFixture fx;
        fx.files->set("held.js", held);
        CHECK(label(fx.vm.evaluateModule("res://held.js")) == "Ok");  // with its pending WARN, as ML29(b)
        MESSAGE("held (static): writtenAfterRelease " << shutdownCountingWrites(log, fx, true));
    }
    {
        // (b) the same module, reached through import().
        ModuleFixture fx;
        fx.files->set("held.js", held);
        CHECK(label(fx.vm.evaluateScript("import('res://held.js'); 0", "boot.js")) == "Ok");
        MESSAGE("held (import): writtenAfterRelease " << shutdownCountingWrites(log, fx, true));
    }
    {
        // (c) nothing can ever resolve it, yet the import's reaction functions hold it.
        ModuleFixture fx;
        fx.files->set("never.js", "await new Promise(() => {});");
        CHECK(label(fx.vm.evaluateScript("import('res://never.js'); 0", "boot.js")) == "Ok");
        MESSAGE("never (import): writtenAfterRelease " << shutdownCountingWrites(log, fx, true));
    }
    {
        // (d) anti-vacuity, both directions: the same never-settling await evaluated statically is not the shape -- its
        // pending evaluation is collected before the module is freed -- so the count can tell the two apart.
        ModuleFixture fx;
        fx.files->set("never.js", "await new Promise(() => {});");
        CHECK(label(fx.vm.evaluateModule("res://never.js")) == "Ok");  // with its pending WARN
        CHECK(shutdownCountingWrites(log, fx, false) == 0U);
    }
}

TEST_CASE(
    "script: paths whose bytes would collide as backend atoms load as distinct modules, and a key that is not "
    "UTF-8 loads (ML54)") {
    LogCapture log;
    {
        // (a) u-umlaut's path and its Latin-1 misreading: one atom in the backend unless the names are ASCII.
        ModuleFixture fx;
        fx.files->set("\xC3\x83\xC2\xBC.js", "export const v = 'latin';");
        fx.files->set("\xC3\xBC.js", "export const v = 'umlaut';");
        fx.files->set("a.js",
                      "import { v as one } from './\xC3\x83\xC2\xBC.js'; import { v as two } from "
                      "'./\xC3\xBC.js'; globalThis.got = one + '/' + two;");
        CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
        CHECK(completion(fx.vm, "got") == "latin/umlaut");
        CHECK(fx.files->reads("\xC3\x83\xC2\xBC.js") == 1U);
        CHECK(fx.files->reads("\xC3\xBC.js") == 1U);
        CHECK(fx.vm.stats().moduleInstances == 3U);
        CHECK(recordOf(fx.vm, "res://\xC3\x83\xC2\xBC.js").has_value());
        CHECK(recordOf(fx.vm, "res://\xC3\xBC.js").has_value());
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expectCleanShutdown(fx.vm);
    }
    {
        // (b) a script's property key spelling the key's UTF-8 bytes in Latin-1 cannot pre-empt the module's name.
        ModuleFixture fx;
        fx.files->set("\xC3\xBC.js", "globalThis.count = (globalThis.count ?? 0) + 1; export const v = 1;");
        fx.files->set("a.js", "import './\xC3\xBC.js';");
        fx.files->set("b.js", "import './\xC3\xBC.js';");
        CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
        CHECK(completion(fx.vm, "globalThis['res://\\u00c3\\u00bc.js'] = 1; 0") == "0");
        CHECK(label(fx.vm.evaluateModule("res://b.js")) == "Ok");
        CHECK(completion(fx.vm, "count") == "1");
        CHECK(fx.files->reads("\xC3\xBC.js") == 1U);
        CHECK(fx.vm.stats().moduleInstances == 3U);
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        expectCleanShutdown(fx.vm);
    }
    {
        // (c) R22's retired survivor stays unreachable behind a colliding name.
        ModuleFixture fx;
        fx.files->set("e.js", "import './\xC3\x83\xC2\xBC.js'; import './missing.js'; export const e = 1;");
        fx.files->set("\xC3\x83\xC2\xBC.js", "import { e } from './e.js'; export const x = 2;");
        fx.files->set("f.js", "import { x } from './\xC3\xBC.js'; globalThis.got = x;");  // no such file
        CHECK(label(fx.vm.evaluateModule("res://e.js")) == "LoadError");
        const std::optional<ModuleRecord> survivor = recordOf(fx.vm, "res://\xC3\x83\xC2\xBC.js");
        REQUIRE(survivor.has_value());
        CHECK(survivor->generation == 2U);
        log.clear();
        CHECK(label(fx.vm.evaluateModule("res://f.js")) == "LoadError");
        expectOnlyError(log,
                        "script: res://f.js: load error: TypeError: cannot load 'res://\xC3\xBC.js' imported by "
                        "'res://f.js': not found or unreadable through the VFS");
        expectCleanShutdown(fx.vm);
    }
    {
        // (d) a key that is not UTF-8 loads, and imports relatively, statically and through import().
        ModuleFixture fx;
        fx.files->set("\xFF/a.js",
                      "import './b.js'; globalThis.viaDynamic = 'pending'; "
                      "import('./c.js').then((ns) => { viaDynamic = ns.c; });");
        fx.files->set("\xFF/b.js", "globalThis.b = 1;");
        fx.files->set("\xFF/c.js", "export const c = 'dynamic';");
        log.clear();
        CHECK(label(fx.vm.evaluateModule("res://\xFF/a.js")) == "Ok");
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
        CHECK(completion(fx.vm, "viaDynamic") == "dynamic");
        const std::optional<ModuleRecord> a = recordOf(fx.vm, "res://\xFF/a.js");
        REQUIRE(a.has_value());
        CHECK(a->imports == std::vector<std::string>{"res://\xFF/b.js", "res://\xFF/c.js"});
        for (const std::string_view path :
             std::array<std::string_view, 3>{"res://\xFF/a.js", "res://\xFF/b.js", "res://\xFF/c.js"}) {
            CHECK(stateOf(fx.vm, path) == "Evaluated");
        }
        expectCleanShutdown(fx.vm);
    }
    {
        // (e) what a frame and a record read: the frame shows the name inside the VM, the log keeps the path.
        ModuleFixture fx;
        fx.files->set("\xC3\xBC/e.js", "export function fail() { throw new Error('frame'); }");
        fx.files->set("r.js", "import { fail } from './\xC3\xBC/e.js'; fail();");
        log.clear();
        const EvalResult thrown = fx.vm.evaluateModule("res://r.js");
        CHECK(label(thrown) == "Exception");
        CHECK(firstLine(thrown.error.stack) == "    at fail (res://%C3%BC/e.js:1:35)");
        CHECK(onlyErrorMessage(log).starts_with("script: res://r.js: uncaught Error: frame"));

        fx.files->set("\xC3\xBC/t.js", "throw new Error('top');");
        log.clear();
        const EvalResult top = fx.vm.evaluateModule("res://\xC3\xBC/t.js");
        CHECK(label(top) == "Exception");
        CHECK(onlyErrorMessage(log).starts_with("script: res://\xC3\xBC/t.js: uncaught Error: top"));
        CHECK(firstLine(top.error.stack) == "    at <anonymous> (res://%C3%BC/t.js:1:10)");
        CHECK(fx.vm.invalidateModule("res://\xC3\xBC/t.js") == std::vector<std::string>{"res://\xC3\xBC/t.js"});
        const EvalResult reloaded = fx.vm.evaluateModule("res://\xC3\xBC/t.js");
        CHECK(firstLine(reloaded.error.stack) == "    at <anonymous> (res://%C3%BC/t.js?gen=2:1:10)");
        expectCleanShutdown(fx.vm);
    }
}

// ---- ML55-ML60: a module's failure is reported once, whenever it happens ----------------------------------------

namespace {

// Evaluates `source` as global code named boot.js and returns how many records at INFO or above the call logged.
[[nodiscard]] std::size_t scriptRecords(LogCapture& log, Vm& vm, const std::string& source) {
    log.clear();
    const EvalResult result = vm.evaluateScript(source, "boot.js");
    CHECK(label(result) == "Ok");
    return log.countAtLeast(LogLevel::Info);
}

}  // namespace

TEST_CASE(
    "script: a synchronous module evaluated after a top-level await it imports fails once, as the call's status "
    "(ML55)") {
    LogCapture log;
    {
        ModuleFixture fx;
        // (a) the control: no await anywhere.
        fx.files->set("u.js", "export const u = 1;");
        fx.files->set("e2.js", "import { u } from './u.js'; throw new Error('control ' + u);");
        CHECK(label(fx.vm.evaluateModule("res://e2.js")) == "Exception");
        CHECK(onlyErrorMessage(log).starts_with("script: res://e2.js: uncaught Error: control 1"));

        // (b) the body runs in the drain, after t.js's await settled -- outside JS_EvalFunction.
        fx.files->set("t.js", "await null; export const t = 1;");
        fx.files->set("e.js", "import { t } from './t.js'; throw new Error('boom ' + t);");
        log.clear();
        const EvalResult late = fx.vm.evaluateModule("res://e.js");
        CHECK(label(late) == "Exception");
        CHECK(late.error.message == "boom 1");
        CHECK(onlyErrorMessage(log).starts_with("script: res://e.js: uncaught Error: boom 1"));
        CHECK(fx.vm.stats().unhandledRejections == 0U);
        CHECK(stateOf(fx.vm, "res://e.js") == "Failed");
        CHECK(stateOf(fx.vm, "res://t.js") == "Loaded");  // D14's stated limit
        expectCleanShutdown(fx.vm);
    }
    {
        // (c) one level down, in a fresh VM so that t.js has not run yet.
        ModuleFixture fx;
        fx.files->set("t.js", "await null; export const t = 1;");
        fx.files->set("d.js", "import { t } from './t.js'; throw new Error('dep ' + t);");
        fx.files->set("r.js", "import './d.js'; globalThis.rRan = true;");
        log.clear();
        const EvalResult dependency = fx.vm.evaluateModule("res://r.js");
        CHECK(label(dependency) == "Exception");
        CHECK(dependency.error.message == "dep 1");
        CHECK(firstLine(dependency.error.stack).find("(res://d.js:1:53)") != std::string_view::npos);
        CHECK(onlyErrorMessage(log).starts_with("script: res://r.js: uncaught Error: dep 1"));
        CHECK(fx.vm.stats().unhandledRejections == 0U);
        CHECK(completion(fx.vm, "typeof rRan") == "undefined");
        expectCleanShutdown(fx.vm);
    }
}

TEST_CASE(
    "script: a failed import() is silent when caught and one record when not, whenever its module fails "
    "(ML56)") {
    LogCapture log;
    ModuleFixture fx;
    // A module that throws at once.
    fx.files->set("s.js", "throw new Error('sync');");
    CHECK(scriptRecords(log, fx.vm,
                        "globalThis.r1 = 'none'; import('res://s.js').catch((e) => { r1 = e.message; }); 0") == 0U);
    CHECK(completion(fx.vm, "r1") == "sync");
    CHECK(scriptRecords(log, fx.vm, "import('res://s.js'); 0") == 1U);  // the cached failure, uncaught
    CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: sync"));

    // A top-level await that rejects.
    fx.files->set("t.js", "await null; throw new Error('tla');");
    CHECK(scriptRecords(log, fx.vm,
                        "globalThis.r2 = 'none'; import('res://t.js').catch((e) => { r2 = e.message; }); 0") == 0U);
    CHECK(completion(fx.vm, "r2") == "tla");
    fx.files->set("t2.js", "await null; throw new Error('tla2');");
    CHECK(scriptRecords(log, fx.vm, "import('res://t2.js'); 0") == 1U);
    CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: tla2"));

    // A synchronous module evaluated in the drain, after the await it imports settled.
    fx.files->set("w.js", "await null; export const w = 1;");
    fx.files->set("c.js", "import { w } from './w.js'; throw new Error('after ' + w);");
    CHECK(scriptRecords(log, fx.vm,
                        "globalThis.r3 = 'none'; import('res://c.js').catch((e) => { r3 = e.message; }); 0") == 0U);
    CHECK(completion(fx.vm, "r3") == "after 1");
    fx.files->set("w2.js", "await null; export const w = 2;");
    fx.files->set("c2.js", "import { w } from './w2.js'; throw new Error('after2 ' + w);");
    CHECK(scriptRecords(log, fx.vm, "import('res://c2.js'); 0") == 1U);
    CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: after2 2"));

    // The same from a module evaluated as an entry, so evaluateModule's own tail is the one observing.
    fx.files->set("w3.js", "await null; export const w = 3;");
    fx.files->set("c3.js", "import { w } from './w3.js'; throw new Error('after3 ' + w);");
    fx.files->set("dyn.js",
                  "globalThis.r4 = 'none'; "
                  "import('./c3.js').catch((e) => { r4 = 'caught ' + e.message; });");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://dyn.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(completion(fx.vm, "r4") == "caught after3 3");
    fx.files->set("w4.js", "await null; export const w = 4;");
    fx.files->set("c4.js", "import { w } from './w4.js'; throw new Error('after4 ' + w);");
    fx.files->set("dyn2.js", "import('./c4.js');");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://dyn2.js")) == "Ok");
    CHECK(onlyErrorMessage(log).starts_with("script: res://dyn2.js: unhandled promise rejection: Error: after4 4"));

    CHECK(fx.vm.stats().unhandledRejections == 4U);
    CHECK(fx.vm.stats().rejectionsHandledLate == 0U);
    const std::array<std::string_view, 7> failed{"res://s.js",  "res://t.js",  "res://t2.js", "res://c.js",
                                                 "res://c2.js", "res://c3.js", "res://c4.js"};
    for (const std::string_view path : failed) {
        INFO("path: " << std::string{path});
        CHECK(stateOf(fx.vm, path) == "Failed");  // each observed at the end of its call
    }
    for (const std::string_view path :
         std::array<std::string_view, 4>{"res://w.js", "res://w2.js", "res://w3.js", "res://w4.js"}) {
        INFO("path: " << std::string{path});
        CHECK(stateOf(fx.vm, path) == "Loaded");
    }
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module that throws NaN is one failure, statically and through import() (ML57)") {
    LogCapture log;
    ModuleFixture fx;
    // The control: a primitive strictly equal to itself.
    fx.files->set("f.js", "throw 42;");
    CHECK(label(fx.vm.evaluateModule("res://f.js")) == "Exception");
    expectOnlyError(log, "script: res://f.js: uncaught exception: 42");

    fx.files->set("n.js", "throw NaN;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://n.js")) == "Exception");
    expectOnlyError(log, "script: res://n.js: uncaught exception: NaN");
    CHECK(fx.vm.stats().unhandledRejections == 0U);

    fx.files->set("n2.js", "throw NaN;");
    CHECK(scriptRecords(log, fx.vm,
                        "globalThis.cn = 'none'; import('res://n2.js').catch((e) => { cn = 'caught ' + e; }); 0") ==
          0U);
    CHECK(completion(fx.vm, "cn") == "caught NaN");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a promise a module body rejected with the value it throws is silent when handled later (ML58)") {
    LogCapture log;
    ModuleFixture fx;
    // The control: a body that rejects and handles in a job, and throws nothing.
    fx.files->set("h0.js",
                  "const p = Promise.reject(new Error('y')); "
                  "queueMicrotask(() => { p.catch(() => {}); });");
    CHECK(label(fx.vm.evaluateModule("res://h0.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);

    fx.files->set("h.js",
                  "const e = new Error('x'); const p = Promise.reject(e); "
                  "queueMicrotask(() => { p.catch(() => {}); }); throw e;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://h.js")) == "Exception");
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Info);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].message.starts_with("script: res://h.js: uncaught Error: x"));
    CHECK(fx.vm.stats().rejectionsHandledLate == 0U);

    // Across calls.
    fx.files->set("h2.js", "const e = new Error('x2'); globalThis.p2 = Promise.reject(e); throw e;");
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://h2.js")) == "Exception");
    CHECK(scriptRecords(log, fx.vm, "p2.catch(() => {}); 0") == 0U);
    CHECK(fx.vm.stats().rejectionsHandledLate == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE(
    "script: a top-level await that rejects in a later call is that call's one record, and the record follows "
    "(ML59)") {
    LogCapture log;
    const std::string wait = "await new Promise((_, r) => { globalThis.rejectLater = r; });";
    const std::string later = "await new Promise((r) => { globalThis.go = r; }); export const t = 1;";
    const std::string sync = "import { t } from './t.js'; throw new Error('boom ' + t);";
    {
        // (a) rejected by a later evaluateScript.
        ModuleFixture fx;
        fx.files->set("wait.js", wait);
        CHECK(label(fx.vm.evaluateModule("res://wait.js")) == "Ok");  // with its one WARN
        CHECK(stateOf(fx.vm, "res://wait.js") == "Evaluating");
        CHECK(scriptRecords(log, fx.vm, "rejectLater(new Error('later')); 0") == 1U);
        CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: later"));
        CHECK(fx.vm.stats().unhandledRejections == 1U);
        CHECK(stateOf(fx.vm, "res://wait.js") == "Failed");  // from the end of that call
        log.clear();
        const EvalResult again = fx.vm.evaluateModule("res://wait.js");
        CHECK(label(again) == "Exception");
        CHECK(again.error.message == "later");
        CHECK(onlyErrorMessage(log).starts_with("script: res://wait.js: uncaught Error: later"));
        CHECK(fx.vm.stats().unhandledRejections == 1U);
        expectCleanShutdown(fx.vm);
    }
    {
        // (b) a synchronous importer whose await is settled by a later call.
        ModuleFixture fx;
        fx.files->set("t.js", later);
        fx.files->set("e.js", sync);
        CHECK(label(fx.vm.evaluateModule("res://e.js")) == "Ok");  // with its one WARN
        CHECK(scriptRecords(log, fx.vm, "go(); 0") == 1U);
        CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: boom 1"));
        CHECK(stateOf(fx.vm, "res://e.js") == "Failed");
        expectCleanShutdown(fx.vm);
    }
    {
        // (c) the dynamic twins.
        ModuleFixture fx;
        fx.files->set("wait.js", wait);
        fx.files->set("t.js", later);
        fx.files->set("e.js", sync);
        CHECK(scriptRecords(log, fx.vm,
                            "globalThis.d1 = 'none'; import('res://wait.js').catch((e) => { d1 = 'caught ' + "
                            "e.message; }); 0") == 0U);
        CHECK(scriptRecords(log, fx.vm, "import('res://e.js'); 0") == 0U);  // uncaught, but nothing has failed yet
        CHECK(scriptRecords(log, fx.vm, "rejectLater(new Error('later')); 0") == 0U);
        CHECK(completion(fx.vm, "d1") == "caught later");
        CHECK(scriptRecords(log, fx.vm, "go(); 0") == 1U);
        CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: boom 1"));
        CHECK(stateOf(fx.vm, "res://wait.js") == "Failed");
        CHECK(stateOf(fx.vm, "res://e.js") == "Failed");
        expectCleanShutdown(fx.vm);
    }
    {
        // (d) reloaded before the old instance rejects: the record keeps the new generation's state.
        ModuleFixture fx;
        fx.files->set("wait.js", wait);
        CHECK(label(fx.vm.evaluateModule("res://wait.js")) == "Ok");  // with its one WARN
        CHECK(fx.vm.invalidateModule("res://wait.js") == std::vector<std::string>{"res://wait.js"});
        fx.files->set("wait.js", "export const fresh = 1;");
        CHECK(label(fx.vm.evaluateModule("res://wait.js")) == "Ok");
        const std::optional<ModuleRecord> fresh = recordOf(fx.vm, "res://wait.js");
        REQUIRE(fresh.has_value());
        CHECK(fresh->generation == 2U);
        CHECK(moduleStateLabel(fresh->state) == "Evaluated");
        CHECK(scriptRecords(log, fx.vm, "rejectLater(new Error('old generation')); 0") == 1U);
        const std::string expected = "script: boot.js: unhandled promise rejection: Error: old generation";
        CHECK(onlyErrorMessage(log).starts_with(expected));
        CHECK(stateOf(fx.vm, "res://wait.js") == "Evaluated");
        expectCleanShutdown(fx.vm);
    }
}

TEST_CASE("script: a module waiting on its top-level await is watched once, however often it is evaluated (ML60)") {
    // The witness of "one watch per module promise" is the bound's assert in endCallWatches, which only the Debug
    // lanes compile: a Release lane cannot see a second watch, and this case cannot assert one from outside.
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("never.js", "await new Promise(() => {});");
    for (int call = 1; call <= 3; ++call) {
        INFO("call " << call);
        log.clear();
        CHECK(label(fx.vm.evaluateModule("res://never.js")) == "Ok");
        const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
        REQUIRE(records.size() == 1U);
        CHECK(records[0].level == LogLevel::Warn);
    }
    CHECK(completion(fx.vm, "1 + 1") == "2");
    CHECK(stateOf(fx.vm, "res://never.js") == "Evaluating");
    expectCleanShutdown(fx.vm);  // static evaluations only: an import() of it is ML52(c)'s R23 shape
}

// ---- ML61, ML62: a call its budget ended -------------------------------------------------------------------------

TEST_CASE(
    "script: a budget that runs out after a module's own failure is one interrupt record, and the module's "
    "error comes with its next evaluation (ML61)") {
    LogCapture log;
    ModuleFixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
    // The body throws in the first drain job; a chain it queued then loops until the budget fires, in the same drain.
    fx.files->set("late.js",
                  "await null; Promise.resolve().then(() => Promise.resolve()).then(() => { for (;;) {} }); "
                  "throw new RangeError('late');");
    const EvalResult result = fx.vm.evaluateModule("res://late.js");
    CHECK(label(result) == "Interrupted");
    CHECK(result.error.name == "InternalError");
    CHECK(result.error.message == "interrupted");
    const std::string interrupted = "script: res://late.js: interrupted after exceeding the 100 ms time budget";
    CHECK(firstLine(onlyErrorMessage(log)) == interrupted);
    CHECK(stateOf(fx.vm, "res://late.js") == "Failed");
    CHECK(fx.vm.stats().unhandledRejections == 0U);

    log.clear();
    CHECK(completion(fx.vm, "1 + 1") == "2");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    const EvalResult next = fx.vm.evaluateModule("res://late.js");
    CHECK(label(next) == "Exception");
    CHECK(next.error.name == "RangeError");
    CHECK(next.error.message == "late");
    CHECK(onlyErrorMessage(log).starts_with("script: res://late.js: uncaught RangeError: late"));
    expectCleanShutdown(fx.vm);
}

TEST_CASE(
    "script: a top-level await the budget interrupts is one interrupt record with no pending warning, and it "
    "stays pending (ML62)") {
    LogCapture log;
    ModuleFixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{100}}};
    fx.files->set("l.js", "await null; for (;;) {}");
    CHECK(label(fx.vm.evaluateModule("res://l.js")) == "Interrupted");
    const std::string interrupted = "script: res://l.js: interrupted after exceeding the 100 ms time budget";
    CHECK(firstLine(onlyErrorMessage(log)) == interrupted);
    CHECK(stateOf(fx.vm, "res://l.js") == "Evaluating");

    log.clear();
    CHECK(completion(fx.vm, "1 + 1") == "2");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(stateOf(fx.vm, "res://l.js") == "Evaluating");
    // The interrupted await never settles: a later evaluation is Ok with the pending WARN, truthfully.
    CHECK(label(fx.vm.evaluateModule("res://l.js")) == "Ok");
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Warn);
    CHECK(records[0].message ==
          "script: res://l.js: module evaluation is still pending at the end of the call (a "
          "top-level await has not settled)");
    CHECK(stateOf(fx.vm, "res://l.js") == "Evaluating");
    expectCleanShutdown(fx.vm);  // a zero count: the abandoned body leaves no reaction holding the module
}

// ---- ML63: an old generation's import() -------------------------------------------------------------------------

TEST_CASE("script: an import() from an old generation loads but records no edge on the current one (ML63)") {
    const LogCapture log;
    ModuleFixture fx;
    // Generation 1 hands a function to the script, which keeps it across a reload (no "??=": a trigraph in C++).
    fx.files->set("a.js",
                  "export function later() { return import('./x.js'); } "
                  "if (!globalThis.firstLater) { globalThis.firstLater = later; }");
    fx.files->set("x.js", "export const x = 1;");
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    CHECK(fx.vm.invalidateModule("res://a.js") == std::vector<std::string>{"res://a.js"});
    fx.files->set("a.js", "export function later2() { return import('./x.js'); } globalThis.secondLater = later2;");
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    std::optional<ModuleRecord> a = recordOf(fx.vm, "res://a.js");
    REQUIRE(a.has_value());
    CHECK(a->generation == 2U);
    CHECK(a->imports.empty());

    // The old generation's import() loads, and grows nothing on the current record.
    const std::string callOld = "globalThis.r1 = 'none'; firstLater().then((ns) => { r1 = 'old ' + ns.x; }); 0";
    CHECK(completion(fx.vm, callOld) == "0");
    CHECK(completion(fx.vm, "r1") == "old 1");
    a = recordOf(fx.vm, "res://a.js");
    REQUIRE(a.has_value());
    CHECK(a->generation == 2U);
    CHECK(a->imports.empty());
    CHECK(fx.vm.invalidateModule("res://x.js") == std::vector<std::string>{"res://x.js"});
    CHECK(stateOf(fx.vm, "res://a.js") == "Evaluated");
    CHECK(recordOf(fx.vm, "res://a.js")->generation == 2U);

    // Anti-vacuity: the current generation's own import() does record its edge.
    const std::string callNew = "globalThis.r2 = 'none'; secondLater().then((ns) => { r2 = 'new ' + ns.x; }); 0";
    CHECK(completion(fx.vm, callNew) == "0");
    CHECK(completion(fx.vm, "r2") == "new 1");
    a = recordOf(fx.vm, "res://a.js");
    REQUIRE(a.has_value());
    CHECK(a->imports == std::vector<std::string>{"res://x.js"});
    CHECK(fx.vm.invalidateModule("res://x.js") == std::vector<std::string>{"res://a.js", "res://x.js"});
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

// ---- ML64: a compile the heap limit starves ----------------------------------------------------------------------

namespace {

// e13's shape, "globalThis.x<n> = <n>;" with <n> the source's size so far, grown while it stays 40 bytes under
// `bytes`: 524 259 bytes for 512 KiB. Under a 1 MiB heap its compile runs out of heap -- which the backend's Release
// variant reports as a SyntaxError ("invalid assignment left-hand side") and its Debug variant as `null`.
[[nodiscard]] std::string heavySource(std::size_t bytes) {
    std::string source;
    while (source.size() + 40 < bytes) {
        source += "globalThis.x" + std::to_string(source.size()) + " = " + std::to_string(source.size()) + ";\n";
    }
    return source;
}

}  // namespace

TEST_CASE(
    "script: a dependency that runs out of heap while compiling is OutOfMemory, retires what the attempt "
    "created, and the VM recovers (ML64)") {
    // This tells the old classification from the new one on the Release lanes only: there the parser reports the
    // refused allocation as a SyntaxError, while on the Debug lanes the same size throws the backend's `null`, which
    // the old rule already classified OutOfMemory.
    const LogCapture log;
    ModuleFixture fx{patient(VmConfig{.heapLimitBytes = MIB})};
    fx.files->set("ok.js", "export const ok = 1;");
    fx.files->set("heavy.js", heavySource(512 * KIB));  // under the 1 MiB source cap: the loader reads it
    fx.files->set("a.js", R"(import "./ok.js"; import "./heavy.js";)");

    const EvalResult result = fx.vm.evaluateModule("res://a.js");
    CHECK(label(result) == "OutOfMemory");
    const std::string starvedPrefix = "script: res://a.js: out of memory (heap limit 1048576 bytes, ";
    CHECK(firstLine(onlyErrorMessage(log)).starts_with(starvedPrefix));
    CHECK(fx.vm.stats().refusedAllocations > 0U);
    const std::optional<ModuleRecord> ok = recordOf(fx.vm, "res://ok.js");
    REQUIRE(ok.has_value());
    CHECK(ok->generation == 2U);  // retired with the attempt that created it (D7)
    CHECK(moduleStateLabel(ok->state) == "Unloaded");
    for (const std::string_view path : std::array<std::string_view, 2>{"res://heavy.js", "res://a.js"}) {
        INFO("path: " << std::string{path});
        const std::optional<ModuleRecord> record = recordOf(fx.vm, path);
        REQUIRE(record.has_value());
        CHECK(record->generation == 1U);
        CHECK(moduleStateLabel(record->state) == "Unloaded");
    }
    CHECK(fx.files->reads("heavy.js") == 1U);
    CHECK(fx.vm.stats().moduleInstances == 1U);
    MESSAGE("starved compile: " << result.error.name << ": " << result.error.message);
    CHECK(completion(fx.vm, "1 + 1") == "2");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a module whose size answers but whose read fails is a LoadError after exactly one read (ML65)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("a.js", R"(import "./b.js"; globalThis.ran = true;)");
    fx.files->set("b.js", "export const b = 1;");
    fx.files->failingReads = {"b.js"};

    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "LoadError");
    expectOnlyError(log,
                    "script: res://a.js: load error: TypeError: cannot load 'res://b.js' imported by "
                    "'res://a.js': not found or unreadable through the VFS");
    // The size answered, so the arm that refused is the read's (step 6).
    CHECK(fx.files->sizeQueries("b.js") == 1U);
    CHECK(fx.files->reads("b.js") == 1U);
    CHECK(fx.vm.stats().moduleInstances == 0U);
    CHECK_FALSE(recordOf(fx.vm, "res://b.js").has_value());
    CHECK(completion(fx.vm, "typeof ran") == "undefined");

    // Anti-vacuity: the same files load once the read answers.
    fx.files->failingReads.clear();
    log.clear();
    CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "ran") == "true");
    CHECK(fx.files->reads("b.js") == 2U);
    expectCleanShutdown(fx.vm);
}

// ---- ML31-ML34, ML38, ML47: import() --------------------------------------------------------------------------

TEST_CASE("script: import() from a module resolves against that module and records a dynamic edge (ML31)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("sub/host.js", R"(globalThis.r = "pending";
import("./lazy.js").then((ns) => { r = "lazy " + ns.v; }, (e) => { r = "failed " + e.message; });)");
    fx.files->set("sub/lazy.js", "export const v = 8;");
    fx.files->set("lazy.js", "export const v = 7;");  // the decoy: "lazy 7" would mean the base was not the importer

    CHECK(label(fx.vm.evaluateModule("res://sub/host.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "r") == "lazy 8");  // the drain of the same call served the import
    const std::optional<ModuleRecord> host = recordOf(fx.vm, "res://sub/host.js");
    REQUIRE(host.has_value());
    CHECK(host->imports == std::vector<std::string>{"res://sub/lazy.js"});  // a DYNAMIC edge: nothing static
    CHECK(stateOf(fx.vm, "res://sub/lazy.js") == "Evaluated");
    CHECK(fx.files->reads("sub/lazy.js") == 1U);
    CHECK(fx.files->reads("lazy.js") == 0U);

    // A second module importing it dynamically re-reads nothing.
    fx.files->set("second.js", R"(import("res://sub/lazy.js").then((ns) => { globalThis.second = ns.v; });)");
    CHECK(label(fx.vm.evaluateModule("res://second.js")) == "Ok");
    CHECK(completion(fx.vm, "second") == "8");
    CHECK(fx.files->reads("sub/lazy.js") == 1U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

namespace {

// Runs `importCall` from global code named `fileName`, storing either outcome in `g`, and returns `g`.
[[nodiscard]] std::string importOutcome(Vm& vm, std::string_view importCall, std::string_view fileName) {
    const std::string source = "globalThis.g = 'pending'; " + std::string{importCall} +
                               ".then((ns) => { g = 'abs ' + ns.v; }, (e) => { g = e.name + ': ' + e.message; }); 0";
    const EvalResult result = vm.evaluateScript(source, fileName);
    REQUIRE(label(result) == "Ok");
    return completion(vm, "g");
}

}  // namespace

TEST_CASE("script: import() from global code takes an absolute res:// path, and refuses a relative one (ML32)") {
    using engine::script::internal::refusedSpecifierMessage;
    using engine::script::internal::SpecifierRefusal;
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("abs.js", "export const v = 3;");
    fx.files->set("scripts/abs2.js", "export const v = 4;");

    // Anti-vacuity: the absolute import succeeds in the same VM as the refusals.
    CHECK(importOutcome(fx.vm, "import('res://abs.js')", "boot.js") == "abs 3");
    CHECK(importOutcome(fx.vm, "import('./abs.js')", "boot.js") ==
          "TypeError: cannot resolve './abs.js' imported by 'boot.js': a relative specifier needs a module under "
          "res:// to resolve against");
    // A script evaluated under a res:// name imports relatively.
    CHECK(importOutcome(fx.vm, "import('./abs2.js')", "res://scripts/boot.js") == "abs 4");

    // The engine's text is never cut at the backend's 256-byte message buffer...
    const std::string longSpecifier = "./" + std::string(295, 'a') + ".js";
    REQUIRE(longSpecifier.size() == 300U);
    const std::string longMessage =
        refusedSpecifierMessage(longSpecifier, "boot.js", SpecifierRefusal::NoModuleImporter);
    CHECK(importOutcome(fx.vm, "import('" + longSpecifier + "')", "boot.js") == "TypeError: " + longMessage);
    CHECK(longMessage.size() > 255U);
    // ...and is never read as a format.
    CHECK(importOutcome(fx.vm, "import('./100%s.js')", "boot.js").find("'./100%s.js'") != std::string::npos);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // every rejection was handled
    expectCleanShutdown(fx.vm);

    // A VM with no module source.
    VmCreateResult created = Vm::create(VmConfig{});
    REQUIRE(created.vm.valid());
    Vm sourceless = std::move(created.vm);
    CHECK(importOutcome(sourceless, "import('res://abs.js')", "boot.js") ==
          "TypeError: cannot load 'res://abs.js' imported by 'boot.js': the VM has no module source (it was created "
          "without a VirtualFileSystem)");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(sourceless);
}

TEST_CASE("script: a failed import() the script catches logs nothing (ML33)") {
    const LogCapture log;
    ModuleFixture fx;
    // (a) a module that throws: its body's rejection is attributed by its watch, and the module promise is kept with
    // the skipped ones until the backend's own handler takes it.
    fx.files->set("t.js", "throw new Error(\"boom\");");
    const std::string catchThrow =
        "globalThis.ca = 'none'; import('res://t.js').catch((e) => { ca = 'caught ' + e.message; }); 0";
    CHECK(label(fx.vm.evaluateScript(catchThrow, "boot.js")) == "Ok");
    CHECK(completion(fx.vm, "ca") == "caught boom");

    // (b) a refused specifier.
    const std::string catchRefusal =
        "globalThis.cb = 'none'; import('lodash').catch((e) => { cb = 'caught ' + e.name; }); 0";
    CHECK(label(fx.vm.evaluateScript(catchRefusal, "boot.js")) == "Ok");
    CHECK(completion(fx.vm, "cb") == "caught TypeError");

    // (c) the dynamic twin of ML24: a link that failed after binding an import, imported twice.
    fx.files->set("j.js", "export const ok = 1;");
    fx.files->set("half.js", R"(import { ok } from "./j.js"; import { nope } from "./j.js"; export const h = ok;)");
    const std::string twice =
        "globalThis.c1 = 'none'; globalThis.c2 = 'none';"
        "import('res://half.js').catch((e) => { c1 = e.message; });"
        "import('res://half.js').catch((e) => { c2 = e.message; }); 0";
    CHECK(label(fx.vm.evaluateScript(twice, "boot.js")) == "Ok");
    CHECK(completion(fx.vm, "c1") == "Could not find export 'nope' in module 'res://j.js'");
    CHECK(completion(fx.vm, "c2") == "Could not find export 'nope' in module 'res://j.js?gen=2'");

    CHECK(log.countAtLeast(LogLevel::Info) == 0U);  // nothing at all: no ERROR, no WARN, no late-handling INFO
    CHECK(fx.vm.stats().unhandledRejections == 0U);
    CHECK(fx.vm.stats().rejectionsHandledLate == 0U);
    // The backend never linked half.js twice: a re-link would abort the Debug teardown or leak a block.
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: an uncaught failed import() is exactly one unhandled-rejection record (ML34)") {
    LogCapture log;
    ModuleFixture fx;
    CHECK(label(fx.vm.evaluateScript("import('res://missing.js'); 0", "boot.js")) == "Ok");
    // Anti-vacuity: exactly one, not zero -- the report still fires for a genuinely unhandled import.
    expectOnlyError(log,
                    "script: boot.js: unhandled promise rejection: TypeError: cannot load 'res://missing.js' "
                    "imported by 'boot.js': not found or unreadable through the VFS");
    CHECK(fx.vm.stats().unhandledRejections == 1U);

    log.clear();
    fx.files->set("t2.js", "throw new Error(\"boom2\");");
    CHECK(label(fx.vm.evaluateScript("import('res://t2.js'); 0", "boot.js")) == "Ok");
    // One, not two: the module body's internal rejection is attributed by its watch.
    CHECK(onlyErrorMessage(log).starts_with("script: boot.js: unhandled promise rejection: Error: boom2"));
    CHECK(fx.vm.stats().unhandledRejections == 2U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: an interrupted module's recorded error is an ordinary error to every later import (ML38)") {
    LogCapture log;
    ModuleFixture fx{VmConfig{.callTimeBudget = std::chrono::milliseconds{50}}};
    fx.files->set("loop.js", "for (;;) {}");
    fx.files->set("user.js", "import \"./loop.js\";");

    // Anti-vacuity: the error began uncatchable.
    CHECK(label(fx.vm.evaluateModule("res://loop.js")) == "Interrupted");

    log.clear();
    const EvalResult caught = fx.vm.evaluateScript(
        "globalThis.c = 'not reached'; import('res://loop.js').then(() => { c = 'resolved'; }, (e) => { c = 'caught "
        "' + e.name + ': ' + e.message; }); 0",
        "boot.js");
    CHECK(label(caught) == "Ok");
    CHECK(completion(fx.vm, "c") == "caught InternalError: interrupted");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);  // nothing at any level
    // Call 1 kept the module promise with the skipped ones, so the backend's handler in call 2 is silent.
    CHECK(fx.vm.stats().rejectionsHandledLate == 0U);

    const EvalResult later = fx.vm.evaluateModule("res://user.js");
    CHECK(label(later) == "Exception");  // not Interrupted: this call's budget never fired
    CHECK(later.error.message == "interrupted");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a dynamic import attribute is a TypeError rejection; an empty set is no attribute (ML47)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("j.js", "export default 5;");
    const auto outcome = [&fx](std::string_view options) {
        const std::string source = "globalThis.o = 'pending'; import('res://j.js', " + std::string{options} +
                                   ").then((ns) => { o = 'default ' + ns.default; }, (e) => { o = e.name + ': ' + "
                                   "e.message; }); 0";
        const EvalResult result = fx.vm.evaluateScript(source, "boot.js");
        REQUIRE(label(result) == "Ok");
        return completion(fx.vm, "o");
    };
    CHECK(outcome("{ with: { type: 'json' } }") ==
          "TypeError: import attribute 'type' is not supported (this engine supports none)");
    // Anti-vacuity: the checker is called for an empty `with` too, and must accept it.
    CHECK(outcome("{ with: {} }") == "default 5");
    CHECK(outcome("{}") == "default 5");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

// ---- ML20, ML21, ML44, ML49: the reload hook ------------------------------------------------------------------

namespace {

// A statement that counts one evaluation of `name` in globalThis.runs.
[[nodiscard]] std::string countRun(std::string_view name) {
    const std::string slot = "globalThis.runs." + std::string{name};
    // Never JavaScript's "?" "?=" in a C++ literal: those three characters are a trigraph (-Wtrigraphs).
    return "globalThis.runs = globalThis.runs ?? {}; " + slot + " = (" + slot + " ?? 0) + 1;\n";
}

}  // namespace

TEST_CASE("script: invalidateModule reloads exactly a module and its importers (ML20)") {
    const LogCapture log;
    ModuleFixture fx;
    fx.files->set("util.js", countRun("util") + "export const value = 1;");
    // ui.js re-exports util.js, and main.js imports through it: main.js is two levels above util.js.
    fx.files->set("ui.js", std::string{R"(export { value } from "./util.js";)"} + "\n" + countRun("ui"));
    fx.files->set("logic.js", R"(import { value } from "./util.js";)"
                              "\n" +
                                  countRun("logic"));
    fx.files->set("main.js", std::string{R"(import { value } from "./ui.js"; import "./logic.js";)"} + "\n" +
                                 countRun("main") + "globalThis.utilValue = value;");
    fx.files->set("other.js", countRun("other"));
    const std::array<std::string_view, 4> reloaded{"logic.js", "main.js", "ui.js", "util.js"};

    CHECK(label(fx.vm.evaluateModule("res://main.js")) == "Ok");
    CHECK(label(fx.vm.evaluateModule("res://other.js")) == "Ok");
    // Anti-vacuity: before the invalidation, a second call re-reads nothing -- the re-reads below are its doing.
    CHECK(label(fx.vm.evaluateModule("res://main.js")) == "Ok");
    for (const std::string_view file : reloaded) {
        CHECK(fx.files->reads(file) == 1U);
    }
    // Anti-vacuity: main.js has no direct edge to util.js -- the invalidation must climb through ui.js to reach it.
    const std::optional<ModuleRecord> mainBefore = recordOf(fx.vm, "res://main.js");
    REQUIRE(mainBefore.has_value());
    CHECK(mainBefore->imports == std::vector<std::string>{"res://logic.js", "res://ui.js"});
    const std::optional<ModuleRecord> uiBefore = recordOf(fx.vm, "res://ui.js");
    REQUIRE(uiBefore.has_value());
    CHECK(uiBefore->imports == std::vector<std::string>{"res://util.js"});

    fx.files->set("util.js", countRun("util") + "export const value = 2;");
    const std::vector<std::string> changed = fx.vm.invalidateModule("res://util.js");
    CHECK(changed == std::vector<std::string>{"res://logic.js", "res://main.js", "res://ui.js", "res://util.js"});
    for (const std::string& path : changed) {
        INFO("path: " << path);
        const std::optional<ModuleRecord> record = recordOf(fx.vm, path);
        REQUIRE(record.has_value());
        CHECK(record->generation == 2U);
        CHECK(moduleStateLabel(record->state) == "Unloaded");
    }
    const std::optional<ModuleRecord> other = recordOf(fx.vm, "res://other.js");
    REQUIRE(other.has_value());
    CHECK(other->generation == 1U);
    CHECK(moduleStateLabel(other->state) == "Evaluated");

    CHECK(label(fx.vm.evaluateModule("res://main.js")) == "Ok");
    for (const std::string_view file : reloaded) {
        INFO("file: " << std::string{file});
        CHECK(fx.files->reads(file) == 2U);
        const std::string stem{file.substr(0, file.size() - 3)};
        CHECK(completion(fx.vm, "runs." + stem) == "2");
    }
    CHECK(fx.files->reads("other.js") == 1U);
    CHECK(completion(fx.vm, "runs.other") == "1");
    CHECK(completion(fx.vm, "utilValue") == "2");
    CHECK(fx.vm.stats().moduleInstances == 9U);

    // The reloaded instances recorded their imports under their keys, never their generation names.
    CHECK(fx.vm.invalidateModule("res://util.js") == changed);
    CHECK(fx.vm.stats().moduleInstances == 9U);
    const std::optional<ModuleRecord> main = recordOf(fx.vm, "res://main.js");
    REQUIRE(main.has_value());
    CHECK(main->generation == 3U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a reloaded module's frames read <path>?gen=2, and the log keeps the canonical name (ML21)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("e.js", R"(export function fail() { throw new Error("gen"); })");
    fx.files->set("r.js", R"(import { fail } from "./e.js"; fail();)");

    const EvalResult first = fx.vm.evaluateModule("res://r.js");
    CHECK(label(first) == "Exception");
    CHECK(first.error.stack.find("(res://e.js:1:") != std::string::npos);
    CHECK(first.error.stack.find("(res://r.js:1:") != std::string::npos);
    // Anti-vacuity: the mark is the reload's -- generation 1 carries none.
    CHECK(first.error.stack.find("?gen=") == std::string::npos);

    CHECK(fx.vm.invalidateModule("res://e.js") == std::vector<std::string>{"res://e.js", "res://r.js"});
    log.clear();
    const EvalResult second = fx.vm.evaluateModule("res://r.js");
    CHECK(label(second) == "Exception");
    CHECK(firstLine(second.error.stack).starts_with("    at fail (res://e.js?gen=2:1:"));
    const std::string_view rest = std::string_view{second.error.stack}.substr(firstLine(second.error.stack).size());
    CHECK(firstLine(rest.substr(rest.empty() ? 0 : 1)).find("(res://r.js?gen=2:1:") != std::string_view::npos);
    CHECK(onlyErrorMessage(log).starts_with("script: res://r.js: uncaught Error: gen"));
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: invalidateModule changes nothing for an unknown, unaddressable or foreign request (ML44)") {
    LogCapture log;
    ModuleFixture fx;
    fx.files->set("k.js", "export const k = 1;");
    REQUIRE(label(fx.vm.evaluateModule("res://k.js")) == "Ok");
    const auto snapshot = [&fx] {
        std::vector<std::string> out;
        for (const ModuleRecord& record : fx.vm.moduleRecords()) {
            out.push_back(record.path + "#" + std::to_string(record.generation) + "#" +
                          std::string{moduleStateLabel(record.state)});
        }
        return out;
    };
    const std::vector<std::string> before = snapshot();

    log.clear();
    CHECK(fx.vm.invalidateModule("res://never.js").empty());
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(snapshot() == before);

    CHECK(fx.vm.invalidateModule("a.js").empty());
    expectOnlyError(log, "script: invalidateModule: 'a.js' is not an absolute res:// module path");
    CHECK(snapshot() == before);

    log.clear();
    CHECK(fx.vm.invalidateModule("res://x.js?gen=2").empty());
    expectOnlyError(log, "script: invalidateModule: 'res://x.js?gen=2' is not an absolute res:// module path");
    CHECK(snapshot() == before);

    log.clear();
    std::vector<std::string> fromThread{"unset"};
    bool ran = false;
    std::thread worker{[&] {
        fromThread = fx.vm.invalidateModule("res://k.js");
        ran = true;
    }};
    worker.join();
    REQUIRE(ran);
    CHECK(fromThread.empty());
    expectOnlyError(log, "script: invalidateModule called from a thread that is not the VM's; refused");
    CHECK(snapshot() == before);

    // A non-canonical spelling of a known path is that path. Anti-vacuity: a valid invalidation changes something.
    log.clear();
    CHECK(fx.vm.invalidateModule("res://./dir/../k.js") == std::vector<std::string>{"res://k.js"});
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(snapshot() != before);

    Vm vm = std::move(fx.vm);  // fx.vm is now a moved-from Vm
    log.clear();
    CHECK(fx.vm.invalidateModule("res://k.js").empty());
    expectOnlyError(log, "script: invalidateModule on an invalid Vm (a failed create or a moved-from Vm)");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a reload adds its instances and a heap cost that only a new VM reclaims (ML49)") {
    const LogCapture log;
    ModuleFixture fx{patient()};
    fx.files->set("a.js", R"(import { f } from "./b.js"; globalThis.calls = (globalThis.calls ?? 0) + f();)");
    fx.files->set("b.js", "export function f() { return 1; } export const table = [1, 2, 3, 4, 5, 6, 7, 8];");

    REQUIRE(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    fx.vm.collectGarbage();
    const std::size_t baseline = fx.vm.stats().heapBytesInUse;
    constexpr int ROUNDS = 50;
    for (int round = 0; round < ROUNDS; ++round) {
        CHECK(fx.vm.invalidateModule("res://b.js") == std::vector<std::string>{"res://a.js", "res://b.js"});
        CHECK(label(fx.vm.evaluateModule("res://a.js")) == "Ok");
    }
    fx.vm.collectGarbage();
    const std::size_t after = fx.vm.stats().heapBytesInUse;
    // Anti-vacuity: every round really re-ran both modules.
    CHECK(completion(fx.vm, "calls") == "51");
    CHECK(fx.vm.stats().moduleInstances == 2U + 2U * ROUNDS);
    REQUIRE(after > baseline);
    MESSAGE("reload growth: " << (after - baseline) / ROUNDS << " bytes per generation");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);  // the retained instances are freed by teardown, all of them
}
