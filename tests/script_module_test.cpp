// Aero Engine -- ES modules through the VFS (task 4.1.2): the specifier grammar, the cache and its reload hook,
// the statuses, the limits, and import(). ML1-ML12 test the pure half (module_graph.hpp) through
// aero::script_internal; every other case drives the public API and ends with a teardown that must find nothing
// leaked -- retired instances included.

#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/script.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
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
}
