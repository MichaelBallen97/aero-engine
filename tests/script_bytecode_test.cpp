// tests/script_bytecode_test.cpp -- task 4.3.2: the bytecode cook (BY8-BY17) and the loader's container branch
// (BY18-BY29, BY35-BY37), and the same-results cases (BY30-BY34). A TU of aero_tests, which supplies
// main() from test_main.cpp. The cook runs the real backend: the committed fixtures are esbuild 0.28.2's own bundles
// of the three .ts files beside them (tests/fixtures/scripts/README.md says how they were made).
#include <aero/assets/cooked_mesh.hpp>
#include <aero/assets/cooked_script.hpp>
#include <aero/core/content_hash.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/internal/bytecode_cook.hpp>
#include <aero/script/internal/bytecode_format.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>
#include <vector>

namespace {

using engine::LogLevel;
using engine::script::COOK_HEAP_LIMIT_BYTES;
using engine::script::cookScriptBytecode;
using engine::script::EvalResult;
using engine::script::evalStatusLabel;
using engine::script::MAX_COOK_SOURCE_BYTES;
using engine::script::MIN_HEAP_LIMIT_BYTES;
using engine::script::ModuleRecord;
using engine::script::scriptBackendRelease;
using engine::script::scriptBytecodeVersion;
using engine::script::ScriptCookResult;
using engine::script::scriptCookStatusLabel;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::internal::bytecodeChecksum;
using engine::script::internal::bytecodeReadHeadroom;
using engine::script::internal::COOK_MODULE_NAME;
using engine::script::internal::COOK_PROBE_NAME;
using engine::script::internal::cookedScriptRefusedMessage;
using engine::script::internal::cookImportMessage;
using script_test::expectCleanShutdown;
using script_test::LogCapture;

constexpr std::string_view SOURCE_NAME = "scripts/mover.ts";
constexpr std::size_t GENERATED_BYTES = std::size_t{200} * 1024U;

[[nodiscard]] std::string label(const ScriptCookResult& result) {
    return std::string{scriptCookStatusLabel(result.status)};
}

[[nodiscard]] std::string readFixture(std::string_view relative) {
    const std::filesystem::path path = std::filesystem::path{AERO_SCRIPT_FIXTURES_DIR} / "bytecode" / relative;
    std::ifstream file{path, std::ios::binary};
    REQUIRE_MESSAGE(file.is_open(), "missing fixture: " << path.string());
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::uint32_t wordAt(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + k]) << (8U * k);
    }
    return value;
}

[[nodiscard]] std::size_t occurrences(const std::vector<std::byte>& bytes, std::string_view token) {
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    std::size_t count = 0;
    for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
        ++count;
    }
    return count;
}

[[nodiscard]] bool hasPlaceholder(const engine::script::ScriptError& error) {
    for (const std::string* field : {&error.name, &error.message, &error.stack}) {
        if (field->find("res://?") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// About `bytes` of source: many small functions, each its own piece of bytecode.
[[nodiscard]] std::string generatedSource(std::size_t bytes) {
    std::string out;
    for (std::size_t i = 0; out.size() < bytes; ++i) {
        out += "export function f" + std::to_string(i) +
               "(a, b) { let total = 0; for (let k = 0; k < a; k++) { total += k * b + " + std::to_string(i) +
               "; } return total; }\n";
    }
    return out;
}

constexpr int TEMPLATE_SUBSTITUTIONS = 100;

// About `bytes` of source: tagged templates of one hundred empty substitutions inside one never-called function. Each
// template object is a constant the read rebuilds string by string, the densest read measured (about 34 times its
// payload in Release, 40 in Debug); the link and the run add only a constant.
[[nodiscard]] std::string templateSource(std::size_t bytes) {
    std::string line = "t`";
    for (int k = 0; k < TEMPLATE_SUBSTITUTIONS; ++k) {
        line += "${0}";
    }
    line += "`;\n";
    std::string out = "export function never(t) {\n";
    while (out.size() < bytes) {
        out += line;
    }
    out += "}\n";
    return out;
}

}  // namespace

TEST_CASE(
    "script: the payload's checksum is the mirror's at every remainder of four, and the release is packed (BY8)") {
    std::set<std::size_t> remainders;
    for (std::size_t pad = 0; pad < 8; ++pad) {
        const std::string source = "export const s = \"" + std::string(pad, 'a') + "\";\n";
        const ScriptCookResult cooked = cookScriptBytecode(source, SOURCE_NAME);
        REQUIRE(label(cooked) == "Ok");
        const std::vector<std::byte>& p = cooked.payload;
        REQUIRE(p.size() > 5U);
        CHECK(std::to_integer<unsigned>(p[0]) == 26U);
        // The backend is the oracle: its own checksum word against the engine's mirror of bc_csum.
        CHECK(wordAt(p, 1) == bytecodeChecksum(std::span<const std::byte>{p}.subspan(5)));
        remainders.insert((p.size() - 5U) % 4U);
        CHECK(cooked.backendRelease == 0x00000F01U);
        CHECK(cooked.backendRelease == scriptBackendRelease());
        CHECK(cooked.bytecodeVersion == 26U);
        CHECK(cooked.bytecodeVersion == scriptBytecodeVersion());
        CHECK(cooked.stripFlags == 1U);
    }
    // Anti-vacuity: the sweep reached all four remainders, the multiple-of-four gap's among them.
    CHECK(remainders == std::set<std::size_t>{0U, 1U, 2U, 3U});
}

TEST_CASE("script: the function source text is stripped and the module name is written once, first (BY9)") {
    const std::string marker = "UNIQUE_BODY_MARKER_4F2A";
    const std::string source = "export function f(a) { /* " + marker + " */ return a + 1; }\n";
    const ScriptCookResult cooked = cookScriptBytecode(source, SOURCE_NAME);
    REQUIRE(label(cooked) == "Ok");
    CHECK(occurrences(cooked.payload, marker) == 0U);
    CHECK(occurrences(cooked.payload, COOK_MODULE_NAME) == 1U);
    // version, checksum word, a one-byte atom count, the type byte, a one-byte length word: the name starts at 8.
    const std::string_view text{reinterpret_cast<const char*>(cooked.payload.data()), cooked.payload.size()};
    CHECK(text.find(COOK_MODULE_NAME) == 8U);
    // Anti-vacuity: the marker is in the source and an unstripped write would keep it (the identifier `f` is stored).
    CHECK(source.find(marker) != std::string::npos);
    CHECK(occurrences(cooked.payload, "f") >= 1U);
}

TEST_CASE("script: the same source gives the same bytes in any order, under any name (BY10)") {
    const std::string source = readFixture("source-text.js");
    const ScriptCookResult first = cookScriptBytecode(source, SOURCE_NAME);
    const ScriptCookResult second = cookScriptBytecode(source, SOURCE_NAME);
    REQUIRE(label(first) == "Ok");
    CHECK(first.payload == second.payload);
    {
        // Another VM's history in the same process: 500 global definitions.
        engine::script::VmCreateResult created = engine::script::Vm::create();
        REQUIRE(created.vm.valid());
        std::string churn;
        for (int i = 0; i < 500; ++i) {
            churn += "globalThis.g" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        }
        CHECK(created.vm.evaluateScript(churn, "churn.js").status == engine::script::EvalStatus::Ok);
        const ScriptCookResult third = cookScriptBytecode(source, SOURCE_NAME);
        CHECK(first.payload == third.payload);
    }
    const ScriptCookResult renamed = cookScriptBytecode(source, "elsewhere/other-name.js");
    REQUIRE(label(renamed) == "Ok");
    CHECK(first.payload == renamed.payload);
    // Anti-vacuity: another source is other bytes.
    CHECK(cookScriptBytecode(readFixture("throws.js"), SOURCE_NAME).payload != first.payload);
}

TEST_CASE("script: a static import is refused, naming the specifier; a dynamic import() cooks (BY11)") {
    // The message is spelled out per row, literal: the source name fills the first quoted slot and the specifier the
    // second, which a comparison against cookImportMessage itself cannot tell apart from the two swapped.
    struct ImportRow {
        std::string_view source;
        std::string_view specifier;
        std::string_view message;
    };
    const std::array<ImportRow, 3> imports{{
        {"import x from \"./a.js\";\nexport const y = x;\n", "./a.js",
         "cannot cook 'scripts/mover.ts': it imports './a.js', and a cooked script imports nothing (bundle it first)"},
        {"export { y } from \"res://b.js\";\n", "res://b.js",
         "cannot cook 'scripts/mover.ts': it imports 'res://b.js', and a cooked script imports nothing (bundle it "
         "first)"},
        {"export * from \"./c.js\";\n", "./c.js",
         "cannot cook 'scripts/mover.ts': it imports './c.js', and a cooked script imports nothing (bundle it first)"},
    }};
    for (const auto& [source, specifier, message] : imports) {
        INFO("specifier: " << std::string{specifier});
        const ScriptCookResult cooked = cookScriptBytecode(source, SOURCE_NAME);
        CHECK(label(cooked) == "LoadError");
        CHECK(cooked.error.message == message);
        CHECK(cooked.error.message == cookImportMessage(SOURCE_NAME, specifier));
        CHECK(cooked.payload.empty());
        CHECK_FALSE(hasPlaceholder(cooked.error));
    }
    const std::string dynamicSource = "export const load = () => import(\"./d.js\");\n";
    const ScriptCookResult dynamic = cookScriptBytecode(dynamicSource, SOURCE_NAME);
    CHECK(label(dynamic) == "Ok");
    CHECK_FALSE(dynamic.payload.empty());
}

TEST_CASE("script: a syntax error is the author's, at its line, with no placeholder (BY12)") {
    const std::string source = "export const a = 1;\nexport const b = 2;\nexport const c = ;\n";
    const ScriptCookResult cooked = cookScriptBytecode(source, SOURCE_NAME);
    CHECK(label(cooked) == "CompileError");
    CHECK(cooked.error.name == "SyntaxError");
    CHECK(cooked.error.stack.find(std::string{SOURCE_NAME} + ":3:") != std::string::npos);
    CHECK_FALSE(hasPlaceholder(cooked.error));
    CHECK(cooked.payload.empty());
}

TEST_CASE("script: the source cap is exact (BY13)") {
    const std::string head = "export const a = 1;";
    std::string atCap = head;
    atCap.append(MAX_COOK_SOURCE_BYTES - head.size(), ' ');
    REQUIRE(atCap.size() == MAX_COOK_SOURCE_BYTES);
    const ScriptCookResult ok = cookScriptBytecode(atCap, SOURCE_NAME);
    CHECK(label(ok) == "Ok");
    atCap.push_back(' ');
    const ScriptCookResult over = cookScriptBytecode(atCap, SOURCE_NAME);
    CHECK(label(over) == "TooLarge");
    CHECK(over.error.message.find(std::to_string(MAX_COOK_SOURCE_BYTES + 1U)) != std::string::npos);
    CHECK(over.error.message.find(std::to_string(MAX_COOK_SOURCE_BYTES)) != std::string::npos);
    CHECK(over.payload.empty());
    CHECK(COOK_HEAP_LIMIT_BYTES == std::size_t{1024} * 1024U * 1024U);
}

TEST_CASE("script: a script that holds the cook's own name cannot be renamed safely (BY14)") {
    const std::string reserved = "export const o = { \"" + std::string{COOK_MODULE_NAME} + "\": 1 };\n";
    const ScriptCookResult refused = cookScriptBytecode(reserved, SOURCE_NAME);
    CHECK(label(refused) == "NotRenameable");
    CHECK(refused.payload.empty());
    CHECK_FALSE(hasPlaceholder(refused.error));
    const std::string probe = "export const o = { \"" + std::string{COOK_PROBE_NAME} + "\": 1 };\n";
    CHECK(label(cookScriptBytecode(probe, SOURCE_NAME)) == "NotRenameable");
    // The same object keyed one character off the name cooks.
    const std::string near = "export const o = { \"" + std::string{COOK_MODULE_NAME} + "!\": 1 };\n";
    CHECK(label(cookScriptBytecode(near, SOURCE_NAME)) == "Ok");
}

TEST_CASE("script: every fixture reads back and writes again to the same bytes, and the payload is small (BY15)") {
    for (const std::string_view name : {"features.js", "throws.js", "source-text.js"}) {
        INFO("fixture: " << std::string{name});
        const ScriptCookResult cooked = cookScriptBytecode(readFixture(name), name);
        CHECK(label(cooked) == "Ok");  // Ok means the read-back identity held
        CHECK_FALSE(cooked.payload.empty());
    }
    const std::string features = readFixture("features.js");
    const ScriptCookResult featuresCooked = cookScriptBytecode(features, "features.js");
    REQUIRE(label(featuresCooked) == "Ok");
    // Stripped bytecode is about 1.3x its source; unstripped, 2.4x.
    CHECK(featuresCooked.payload.size() * 10U < features.size() * 16U);

    const std::string generated = generatedSource(GENERATED_BYTES);
    REQUIRE(generated.size() >= GENERATED_BYTES);
    const ScriptCookResult generatedCooked = cookScriptBytecode(generated, "generated.js");
    REQUIRE(label(generatedCooked) == "Ok");
    CHECK(generatedCooked.payload.size() * 10U < generated.size() * 16U);
    CHECK(generatedCooked.payload.size() > generated.size() / 4U);  // anti-vacuity: it is a real payload
}

TEST_CASE("script: a cook logs nothing at Warn or above, success or failure (BY16)") {
    const LogCapture log;
    CHECK(label(cookScriptBytecode("export const a = 1;\n", SOURCE_NAME)) == "Ok");
    CHECK(label(cookScriptBytecode("export const a = ;\n", SOURCE_NAME)) == "CompileError");
    CHECK(label(cookScriptBytecode("import \"./x.js\";\n", SOURCE_NAME)) == "LoadError");
    const std::string reserved = "export const o = { \"" + std::string{COOK_MODULE_NAME} + "\": 1 };\n";
    CHECK(label(cookScriptBytecode(reserved, SOURCE_NAME)) == "NotRenameable");
    const std::string tooBig(MAX_COOK_SOURCE_BYTES + 1U, ' ');
    CHECK(label(cookScriptBytecode(tooBig, SOURCE_NAME)) == "TooLarge");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
}

TEST_CASE("script: the layer has one module read and one module write, each in its own file (BY17)") {
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
    const auto count = [](std::string_view text, std::string_view token) {
        std::size_t n = 0;
        for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
            ++n;
        }
        return n;
    };
    std::size_t reads = 0;
    std::size_t readsInGlue = 0;
    std::size_t writes = 0;
    std::size_t writesInCook = 0;
    std::size_t stripDebug = 0;
    std::size_t moduleCompiles = 0;
    std::size_t files = 0;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator{fs::path{AERO_SCRIPT_SRC_DIR}.parent_path()}) {
        const fs::path& path = entry.path();
        if (!entry.is_regular_file() || (path.extension() != ".cpp" && path.extension() != ".hpp")) {
            continue;
        }
        ++files;
        const std::string text = strippedSource(path);
        const std::string name = path.filename().string();
        reads += count(text, "JS_ReadObject(");
        writes += count(text, "JS_WriteObject(");
        stripDebug += count(text, "JS_WRITE_OBJ_STRIP_DEBUG");
        moduleCompiles += count(text, "JS_EVAL_TYPE_MODULE");
        if (name == "vm_modules.cpp") {
            readsInGlue += count(text, "JS_ReadObject(");
        }
        if (name == "bytecode_cook.cpp") {
            writesInCook += count(text, "JS_WriteObject(");
        }
    }
    CHECK(reads == 1U);
    CHECK(readsInGlue == 1U);
    CHECK(writes == 1U);
    CHECK(writesInCook == 1U);
    CHECK(stripDebug == 0U);
    // Anti-vacuity: the scan visited the layer and the same stripped scan still finds the one module compile.
    CHECK(files >= 8U);
    CHECK(moduleCompiles == 1U);
}

// ---- BY18-BY29: the loader's container branch ------------------------------------------------------------------

namespace {

using std::chrono_literals::operator""ms;

constexpr std::size_t HEADER_BYTES = engine::assets::COOKED_SCRIPT_HEADER_BYTES;

// A module source in memory, with per-path read counts and an optional delay on every read.
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
        ++readCounts[std::string{relPath}];
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

    [[nodiscard]] std::size_t reads(std::string_view relPath) const {
        const auto found = readCounts.find(relPath);
        return found == readCounts.end() ? 0U : found->second;
    }

    std::chrono::milliseconds readDelay{0};

private:
    std::map<std::string, std::string, std::less<>> files;
    mutable std::map<std::string, std::size_t, std::less<>> readCounts;
};

// A VM over a VFS whose root is one MemoryBackend. Non-movable: the Vm borrows the VFS's address.
class Fixture {
public:
    explicit Fixture(const VmConfig& config = {}) {
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

[[nodiscard]] std::vector<std::byte> toBytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

[[nodiscard]] std::string toText(const std::vector<std::byte>& bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The cooked container of `js`, cooked under `path`: the cook, then writeCookedScript, both required Ok.
[[nodiscard]] std::string containerOf(std::string_view js, std::string_view path = "res://x.js",
                                      engine::Guid guid = engine::Guid{}) {
    const ScriptCookResult cooked = cookScriptBytecode(js, path);
    REQUIRE(label(cooked) == "Ok");
    const engine::assets::CookedScriptWriteResult written =
        engine::assets::writeCookedScript(engine::assets::CookedScriptInput{
            guid, cooked.backendRelease, cooked.bytecodeVersion, cooked.stripFlags, cooked.payload});
    REQUIRE(written.status == engine::assets::CookedScriptStatus::Ok);
    return toText(written.bytes);
}

// Recomputes the payload hash a mutated container carries (offsets 56 and 64).
[[nodiscard]] std::vector<std::byte> rehash(std::vector<std::byte> container) {
    REQUIRE(container.size() > HEADER_BYTES);
    const std::span<const std::byte> payload = std::span<const std::byte>{container}.subspan(HEADER_BYTES);
    const engine::ContentHash hash = engine::hashBytes(payload);
    engine::assets::putU64(container, engine::assets::cooked_script_detail::H_PAYLOAD_HASH_HI, hash.hi);
    engine::assets::putU64(container, engine::assets::cooked_script_detail::H_PAYLOAD_HASH_LO, hash.lo);
    return container;
}

[[nodiscard]] std::string evalLabel(const EvalResult& result) { return std::string{evalStatusLabel(result.status)}; }

[[nodiscard]] std::string completion(Vm& vm, std::string_view source) {
    const EvalResult result = vm.evaluateScript(source, "probe.js");
    REQUIRE(evalLabel(result) == "Ok");
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

[[nodiscard]] std::string stateOf(const Vm& vm, std::string_view path) {
    const std::optional<ModuleRecord> record = recordOf(vm, path);
    return record.has_value() ? std::string{moduleStateLabel(record->state)} : std::string{"<absent>"};
}

// A container whose payload is about 200 KB: one exported constant and one never-called function.
[[nodiscard]] std::string bigContainer(std::string_view path) {
    std::string source = "export const x = 1;\nexport function never(a, b) {\n  let total = 0;\n";
    for (std::size_t i = 0; source.size() < std::size_t{200} * 1024U; ++i) {
        source += "  for (let k = 0; k < a; k++) { total += k * b + " + std::to_string(i) + "; }\n";
    }
    source += "  return total;\n}\n";
    return containerOf(source, path);
}

}  // namespace

TEST_CASE("script: a cooked module loads with the name it is read under, and runs as its JavaScript would (BY18)") {
    const LogCapture log;
    Fixture fx;
    const std::string js =
        "globalThis.frame = new Error().stack; globalThis.metaType = typeof import.meta; export const x = 1;";
    fx.files->set("a/b.js", containerOf(js, "res://a/b.js"));
    const std::uint64_t before = fx.vm.stats().moduleInstances;
    const EvalResult result = fx.vm.evaluateModule("res://a/b.js");
    CHECK(evalLabel(result) == "Ok");
    CHECK(completion(fx.vm, "frame.includes('res://a/b.js:1:')") == "true");
    CHECK(completion(fx.vm, "frame.includes('res://?')") == "false");
    CHECK(completion(fx.vm, "metaType") == "object");
    const std::optional<ModuleRecord> record = recordOf(fx.vm, "res://a/b.js");
    REQUIRE(record.has_value());
    CHECK(record->generation == 1U);
    CHECK(moduleStateLabel(record->state) == "Evaluated");
    CHECK(fx.vm.stats().moduleInstances == before + 1U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the magic decides what a file is, never its name (BY19)") {
    Fixture fx;
    fx.files->set("m/x.txt", containerOf("globalThis.fromBytecode = 1;"));
    fx.files->set("m/y.aeroscript", "globalThis.fromText = 2;");
    CHECK(evalLabel(fx.vm.evaluateModule("res://m/x.txt")) == "Ok");
    CHECK(evalLabel(fx.vm.evaluateModule("res://m/y.aeroscript")) == "Ok");
    CHECK(completion(fx.vm, "fromBytecode + fromText") == "3");

    const std::string magic{engine::assets::COOKED_SCRIPT_MAGIC};
    REQUIRE(magic.size() == 8U);
    fx.files->set("m/seven.js", magic.substr(0, 7));
    CHECK(evalLabel(fx.vm.evaluateModule("res://m/seven.js")) == "CompileError");
    fx.files->set("m/short.js", magic + std::string(20, 'a'));
    const EvalResult shortResult = fx.vm.evaluateModule("res://m/short.js");
    CHECK(evalLabel(shortResult) == "LoadError");
    CHECK(shortResult.error.message.find("shorter than the 80-byte header") != std::string::npos);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: every cause that makes a container unusable is a LoadError before the backend reads it (BY20)") {
    constexpr std::string_view PATH = "res://t/m.js";
    const std::string container = containerOf("globalThis.v = 12; export const lit = 12;", PATH);
    const std::vector<std::byte> good = toBytes(container);
    namespace detail = engine::assets::cooked_script_detail;
    namespace head = engine::script::internal;
    REQUIRE(good.size() > HEADER_BYTES + 24U);

    struct Mutation {
        std::string_view what;
        std::vector<std::byte> bytes;
        std::string reason;  // "" -> the parser's own message for these bytes
    };
    const auto withWord = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::byte> bytes = good;
        engine::assets::putU32(bytes, offset, value);
        return bytes;
    };
    const auto withPayload = [&](auto edit) {
        std::vector<std::byte> bytes = good;
        edit(bytes);
        return rehash(std::move(bytes));
    };
    std::vector<Mutation> mutations;
    mutations.push_back({"format version 2", withWord(detail::H_FORMAT_VERSION, 2U), ""});
    mutations.push_back({"reserved flags", withWord(detail::H_RESERVED_FLAGS, 1U), ""});
    mutations.push_back({"reserved word", withWord(detail::H_RESERVED, 1U), ""});
    mutations.push_back({"strip flags 2", withWord(detail::H_STRIP_FLAGS, 2U), ""});
    {
        std::vector<std::byte> appended = good;
        appended.push_back(std::byte{0});
        mutations.push_back({"a byte appended", std::move(appended), ""});
    }
    {
        std::vector<std::byte> flipped = good;
        flipped[HEADER_BYTES + 12] ^= std::byte{0x40};
        mutations.push_back({"a payload byte flipped", std::move(flipped), ""});
    }
    mutations.push_back({"another release", withWord(detail::H_BACKEND_RELEASE, 0x00000F02U),
                         "it was cooked for quickjs-ng 0.15.2, and this engine runs 0.15.1"});
    mutations.push_back({"header version 27", withWord(detail::H_BYTECODE_VERSION, 27U),
                         "its header declares bytecode version 27, and this engine reads version 26"});
    mutations.push_back({"payload version byte 27",
                         withPayload([](std::vector<std::byte>& b) { b[HEADER_BYTES] = std::byte{27}; }),
                         std::string{head::bytecodeHeadStatusText(head::BytecodeHeadStatus::WrongVersion)}});
    mutations.push_back({"checksum bypass", withPayload([](std::vector<std::byte>& b) {
                             for (std::size_t k = 1; k <= 4; ++k) {
                                 b[HEADER_BYTES + k] = std::byte{0xFF};
                             }
                         }),
                         std::string{head::bytecodeHeadStatusText(head::BytecodeHeadStatus::ChecksumBypass)}});
    mutations.push_back({"a mid-payload byte flipped, re-hashed",
                         withPayload([](std::vector<std::byte>& b) { b[HEADER_BYTES + 30] ^= std::byte{0x01}; }),
                         std::string{head::bytecodeHeadStatusText(head::BytecodeHeadStatus::ChecksumMismatch)}});
    {
        const engine::assets::CookedScriptParseResult parsed = engine::assets::parseCookedScript(good);
        REQUIRE(parsed.status == engine::assets::CookedScriptStatus::Ok);
        const head::RenamedBytecode renamed =
            head::renameBytecodeModule(parsed.script.payload, COOK_MODULE_NAME, "res://elsewhere.js");
        REQUIRE(renamed.status == head::BytecodeHeadStatus::Ok);
        const engine::assets::CookedScriptWriteResult wrapped =
            engine::assets::writeCookedScript(engine::assets::CookedScriptInput{
                engine::Guid{}, scriptBackendRelease(), scriptBytecodeVersion(), 1U, renamed.bytes});
        REQUIRE(wrapped.status == engine::assets::CookedScriptStatus::Ok);
        mutations.push_back({"renamed to another module", wrapped.bytes,
                             std::string{head::bytecodeHeadStatusText(head::BytecodeHeadStatus::NameNotPlaceholder)}});
    }
    REQUIRE(mutations.size() == 12U);

    for (const Mutation& mutation : mutations) {
        INFO("mutation: " << std::string{mutation.what});
        const LogCapture log;
        Fixture fx;
        std::string reason = mutation.reason;
        if (reason.empty()) {
            const engine::assets::CookedScriptParseResult parsed = engine::assets::parseCookedScript(mutation.bytes);
            REQUIRE(parsed.status != engine::assets::CookedScriptStatus::Ok);
            reason = parsed.message;
        }
        REQUIRE_FALSE(reason.empty());
        fx.files->set("t/m.js", toText(mutation.bytes));
        const EvalResult result = fx.vm.evaluateModule(PATH);
        CHECK(evalLabel(result) == "LoadError");
        CHECK(result.error.message == cookedScriptRefusedMessage(PATH, "", reason));
        const std::vector<script_test::CapturedRecord> records = log.atLeast(LogLevel::Warn);
        REQUIRE(records.size() == 1U);
        CHECK(records[0].level == LogLevel::Error);
        CHECK_FALSE(recordOf(fx.vm, PATH).has_value());
        CHECK(fx.vm.stats().moduleInstances == 0U);
        CHECK(fx.vm.stats().refusedAllocations == 0U);
        expectCleanShutdown(fx.vm);
    }
    // Anti-vacuity: the unmutated container loads in the same shape of fixture.
    Fixture fx;
    fx.files->set("t/m.js", container);
    CHECK(evalLabel(fx.vm.evaluateModule(PATH)) == "Ok");
    CHECK(completion(fx.vm, "v") == "12");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a damaged container is refused with the importer named (BY21)") {
    Fixture fx;
    std::vector<std::byte> damaged = toBytes(containerOf("export const x = 1;", "res://t/dep.js"));
    damaged[HEADER_BYTES + 12] ^= std::byte{0x40};  // the payload no longer hashes to the header's value
    fx.files->set("t/dep.js", toText(damaged));
    fx.files->set("t/main.js", "import \"./dep.js\";\n");
    const EvalResult result = fx.vm.evaluateModule("res://t/main.js");
    CHECK(evalLabel(result) == "LoadError");
    CHECK(result.error.message.find("imported by 'res://t/main.js'") != std::string::npos);
    CHECK(result.error.message.find("its cooked bytecode is refused") != std::string::npos);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a read the heap has no room for is OutOfMemory before the backend sees a byte (BY22)") {
    const std::string container = bigContainer("res://h/big.js");
    const engine::assets::CookedScriptParseResult parsed = engine::assets::parseCookedScript(toBytes(container));
    REQUIRE(parsed.status == engine::assets::CookedScriptStatus::Ok);
    const std::size_t payloadBytes = parsed.script.payload.size();
    INFO("payload bytes: " << payloadBytes);
    REQUIRE(payloadBytes >= std::size_t{100} * 1024U);
    REQUIRE(payloadBytes <= std::size_t{300} * 1024U);
    REQUIRE(bytecodeReadHeadroom(payloadBytes) > MIN_HEAP_LIMIT_BYTES);
    {
        const LogCapture log;
        VmConfig config;
        config.heapLimitBytes = MIN_HEAP_LIMIT_BYTES;
        Fixture fx{script_test::patient(config)};
        fx.files->set("h/big.js", container);
        const EvalResult result = fx.vm.evaluateModule("res://h/big.js");
        CHECK(evalLabel(result) == "OutOfMemory");
        CHECK(result.error.name == "InternalError");
        CHECK(result.error.message == "out of memory");
        CHECK(fx.vm.stats().refusedAllocations == 1U);
        CHECK(fx.vm.stats().moduleInstances == 0U);
        const std::vector<script_test::CapturedRecord> records = log.atLeast(LogLevel::Warn);
        REQUIRE(records.size() == 1U);
        CHECK(records[0].message.starts_with("script: res://h/big.js: out of memory (heap limit 1048576 bytes, "));
        expectCleanShutdown(fx.vm);
    }
    // Anti-vacuity: the same container loads where the room exists.
    Fixture roomy{script_test::patient()};
    roomy.files->set("h/big.js", container);
    CHECK(evalLabel(roomy.vm.evaluateModule("res://h/big.js")) == "Ok");
    expectCleanShutdown(roomy.vm);
}

TEST_CASE("script: a container's own refusal is decided before the memory rule (BY23)") {
    std::vector<std::byte> damaged = toBytes(bigContainer("res://h/big.js"));
    damaged[HEADER_BYTES + 12] ^= std::byte{0x40};
    VmConfig config;
    config.heapLimitBytes = MIN_HEAP_LIMIT_BYTES;
    Fixture fx{script_test::patient(config)};
    fx.files->set("h/big.js", toText(damaged));
    const EvalResult result = fx.vm.evaluateModule("res://h/big.js");
    CHECK(evalLabel(result) == "LoadError");
    CHECK(fx.vm.stats().refusedAllocations == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the read headroom covers the measured peak of a real read (BY24)") {
    struct Subject {
        std::string_view name;
        std::string container;
        std::size_t minimumPerByte;
    };
    const std::array<Subject, 4> subjects{{
        {"a one-line module", containerOf("export const x = 1;\n", "res://p/m.js"), 0},
        {"a ~200 KB module", bigContainer("res://p/m.js"), 0},
        // Exported small functions: the link and the run allocate a closure and two references for each, so this
        // window is the loosest bound of the read (its read alone is about 3.5 times the payload).
        {"a ~200 KB module of small functions", containerOf(generatedSource(GENERATED_BYTES), "res://p/m.js"), 0},
        {"a ~200 KB module of tagged templates", containerOf(templateSource(GENERATED_BYTES), "res://p/m.js"), 24},
    }};
    for (const Subject& subject : subjects) {
        INFO("subject: " << std::string{subject.name});
        const engine::assets::CookedScriptParseResult parsed =
            engine::assets::parseCookedScript(toBytes(subject.container));
        REQUIRE(parsed.status == engine::assets::CookedScriptStatus::Ok);
        const std::size_t payloadBytes = parsed.script.payload.size();
        Fixture fx{script_test::patient()};
        fx.files->set("p/m.js", subject.container);
        const engine::script::VmStats before = fx.vm.stats();
        CHECK(evalLabel(fx.vm.evaluateModule("res://p/m.js")) == "Ok");
        const engine::script::VmStats after = fx.vm.stats();
        REQUIRE(after.heapBytesPeak >= before.heapBytesInUse);
        const std::size_t peakGrowth = after.heapBytesPeak - before.heapBytesInUse;
        CHECK(peakGrowth <= bytecodeReadHeadroom(payloadBytes));
        CHECK(peakGrowth > subject.minimumPerByte * payloadBytes);
        CHECK(after.heapBytesPeak > before.heapBytesPeak);  // anti-vacuity: the load did allocate
        MESSAGE(std::string{subject.name} << ": payload " << payloadBytes << " bytes, peak growth " << peakGrowth
                                          << " bytes, ratio "
                                          << (static_cast<double>(peakGrowth) / static_cast<double>(payloadBytes)));
        expectCleanShutdown(fx.vm);
    }
}

TEST_CASE("script: a reloaded bytecode module is read again under its next generation's name (BY25)") {
    Fixture fx;
    const std::string js =
        "globalThis.stacks = globalThis.stacks ?? []; globalThis.metas = globalThis.metas ?? [];\n"
        "globalThis.fns = globalThis.fns ?? []; globalThis.stacks.push(new Error().stack);\n"
        "globalThis.metas.push(import.meta); export const f = () => 41 + 1; globalThis.fns.push(f);";
    fx.files->set("g/m.js", containerOf(js, "res://g/m.js"));
    CHECK(evalLabel(fx.vm.evaluateModule("res://g/m.js")) == "Ok");
    CHECK(fx.files->reads("g/m.js") == 1U);
    CHECK_FALSE(fx.vm.invalidateModule("res://g/m.js").empty());
    CHECK(evalLabel(fx.vm.evaluateModule("res://g/m.js")) == "Ok");
    CHECK(fx.files->reads("g/m.js") == 2U);
    CHECK(completion(fx.vm, "stacks.length") == "2");
    CHECK(completion(fx.vm, "stacks[0].includes('res://g/m.js:') && !stacks[0].includes('?gen=')") == "true");
    CHECK(completion(fx.vm, "stacks[1].includes('res://g/m.js?gen=2:')") == "true");
    CHECK(completion(fx.vm, "metas[0] !== metas[1]") == "true");
    CHECK(completion(fx.vm, "fns[0]()") == "42");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a failed load retires the bytecode modules it read (BY26)") {
    Fixture fx;
    fx.files->set("g/dep.js", containerOf("globalThis.depStack = new Error().stack;", "res://g/dep.js"));
    fx.files->set("g/entry.js", "import \"./dep.js\";\nimport \"./missing.js\";\n");
    const EvalResult failed = fx.vm.evaluateModule("res://g/entry.js");
    CHECK(evalLabel(failed) == "LoadError");
    const std::optional<ModuleRecord> dep = recordOf(fx.vm, "res://g/dep.js");
    REQUIRE(dep.has_value());
    CHECK(dep->generation == 2U);
    CHECK(moduleStateLabel(dep->state) == "Unloaded");
    CHECK(fx.files->reads("g/dep.js") == 1U);

    fx.files->set("g/missing.js", "export {};");
    fx.files->set("g/second.js", "import \"./dep.js\";\n");
    CHECK(evalLabel(fx.vm.evaluateModule("res://g/second.js")) == "Ok");
    CHECK(fx.files->reads("g/dep.js") == 2U);
    CHECK(completion(fx.vm, "depStack.includes('res://g/dep.js?gen=2:')") == "true");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: import() reaches a container, and a container's import() resolves against its own path (BY27)") {
    Fixture fx;
    fx.files->set("c/dep.js", containerOf("export const x = 5;", "res://c/dep.js"));
    fx.files->set("c/entry.js", "import(\"./dep.js\").then((m) => { globalThis.fromDep = m.x; });\n");
    CHECK(evalLabel(fx.vm.evaluateModule("res://c/entry.js")) == "Ok");
    CHECK(completion(fx.vm, "fromDep") == "5");

    fx.files->set("c/sib.js", "export const s = 9;");
    const std::string js = "const m = await import(\"./sib.js\"); globalThis.sib = m.s;";
    fx.files->set("c/main.js", containerOf(js, "res://c/main.js"));
    CHECK(evalLabel(fx.vm.evaluateModule("res://c/main.js")) == "Ok");
    CHECK(completion(fx.vm, "sib") == "9");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the call's budget covers a container's load (BY28)") {
    const std::string container = containerOf("globalThis.ran = 1;", "res://d/m.js");
    {
        VmConfig config;
        config.callTimeBudget = 20ms;
        Fixture fx{config};
        fx.files->set("d/m.js", container);
        fx.files->readDelay = 100ms;
        const EvalResult result = fx.vm.evaluateModule("res://d/m.js");
        CHECK(evalLabel(result) == "Interrupted");
        CHECK(fx.vm.stats().moduleInstances == 0U);
        expectCleanShutdown(fx.vm);
    }
    Fixture fx;
    fx.files->set("d/m.js", container);
    CHECK(evalLabel(fx.vm.evaluateModule("res://d/m.js")) == "Ok");
    CHECK(completion(fx.vm, "ran") == "1");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a refused container is a LoadError even when the call's budget has also run out (BY38)") {
    const std::string container = containerOf("globalThis.ran = 1;", "res://d/bad.js");
    std::vector<std::byte> damaged = toBytes(container);
    REQUIRE(damaged.size() > HEADER_BYTES + 12U);
    damaged[HEADER_BYTES + 12] ^= std::byte{0x40};  // the payload no longer hashes to the header's value
    VmConfig config;
    config.callTimeBudget = 20ms;
    Fixture fx{config};
    fx.files->set("d/bad.js", toText(damaged));
    fx.files->readDelay =
        100ms;  // the read outlasts the budget, so the deadline has passed when the container is judged
    const EvalResult result = fx.vm.evaluateModule("res://d/bad.js");
    CHECK(evalLabel(result) == "LoadError");
    CHECK(fx.vm.stats().moduleInstances == 0U);
    CHECK(fx.files->reads("d/bad.js") == 1U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: the source size cap applies to a container before it is read (BY29)") {
    VmConfig config;
    config.heapLimitBytes = MIN_HEAP_LIMIT_BYTES;
    Fixture fx{config};
    std::string big{engine::assets::COOKED_SCRIPT_MAGIC};
    big.resize(MIN_HEAP_LIMIT_BYTES + 1U, 'a');
    fx.files->set("big/m.js", big);
    const EvalResult result = fx.vm.evaluateModule("res://big/m.js");
    CHECK(evalLabel(result) == "LoadError");
    CHECK(result.error.message == engine::script::internal::tooLargeMessage(
                                      "res://big/m.js", "", MIN_HEAP_LIMIT_BYTES + 1U, MIN_HEAP_LIMIT_BYTES));
    CHECK(fx.files->reads("big/m.js") == 0U);
    expectCleanShutdown(fx.vm);
}

namespace {

// The offset of the first byte after a payload's atom table (JS_ReadObjectAtoms, quickjs.c:39649-39709): the version
// byte, the checksum word, the atom count, then per atom a type byte and either a predefined atom's word (type 0) or a
// string (a LEB128 of length << 1 | wide, then the characters).
[[nodiscard]] std::size_t atomTableEnd(std::span<const std::byte> payload) {
    std::size_t at = 5;
    const auto leb = [&payload, &at]() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 35U; shift += 7U) {
            REQUIRE(at < payload.size());
            const auto byte = static_cast<std::uint32_t>(payload[at++]);
            value |= (byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0U) {
                return value;
            }
        }
        FAIL("a LEB128 longer than five bytes");
        return std::uint32_t{0};
    };
    const std::uint32_t count = leb();
    for (std::uint32_t i = 0; i < count; ++i) {
        REQUIRE(at < payload.size());
        const auto type = static_cast<unsigned>(payload[at++]);
        if (type == 0U) {
            at += 4U;
            continue;
        }
        const std::uint32_t word = leb();
        at += static_cast<std::size_t>(word >> 1U) * ((word & 1U) != 0U ? 2U : 1U);
    }
    REQUIRE(at < payload.size());
    return at;
}

}  // namespace

TEST_CASE("script: a refused container's name is retired, so a module nested in its payload is never linked (BY36)") {
    constexpr std::string_view PATH = "res://t/nested.js";
    constexpr std::byte BC_TAG_ARRAY{9};
    constexpr std::byte BC_TAG_MODULE{13};
    // A cooked module whose body sets a global, with its bytes (from the atom table's end) put inside an array: the
    // root is an array holding one module named by atom #0, which the rename rewrites to the key's current name.
    const std::vector<std::byte> container = toBytes(containerOf("globalThis.ran = 1;", PATH));
    const engine::assets::CookedScriptParseResult parsed = engine::assets::parseCookedScript(container);
    REQUIRE(parsed.status == engine::assets::CookedScriptStatus::Ok);
    const std::size_t moduleAt = atomTableEnd(parsed.script.payload);
    REQUIRE(parsed.script.payload[moduleAt] == BC_TAG_MODULE);
    std::vector<std::byte> payload(parsed.script.payload.begin(),
                                   parsed.script.payload.begin() + static_cast<std::ptrdiff_t>(moduleAt));
    payload.push_back(BC_TAG_ARRAY);
    payload.push_back(std::byte{1});  // one element
    payload.insert(payload.end(), parsed.script.payload.begin() + static_cast<std::ptrdiff_t>(moduleAt),
                   parsed.script.payload.end());
    const std::span<std::byte> view{payload};
    engine::assets::putU32(view, 1, bytecodeChecksum(std::span<const std::byte>{view}.subspan(5)));
    const engine::assets::CookedScriptInput input{engine::Guid{}, scriptBackendRelease(), scriptBytecodeVersion(), 1U,
                                                  payload};
    const engine::assets::CookedScriptWriteResult wrapped = engine::assets::writeCookedScript(input);
    REQUIRE(wrapped.status == engine::assets::CookedScriptStatus::Ok);

    const LogCapture log;
    Fixture fx;
    fx.files->set("t/nested.js", toText(wrapped.bytes));
    fx.files->set("t/importer.js", "import \"./nested.js\";\nglobalThis.imported = 1;\n");
    const EvalResult first = fx.vm.evaluateModule(PATH);
    CHECK(evalLabel(first) == "LoadError");
    CHECK(first.error.message == cookedScriptRefusedMessage(PATH, "", "its payload is not a module"));
    const std::optional<ModuleRecord> record = recordOf(fx.vm, PATH);
    REQUIRE(record.has_value());
    CHECK(record->generation == 2U);
    CHECK(stateOf(fx.vm, PATH) != "Loaded");

    // The import names the next generation, so the orphan listed under the first one is unreachable: the container is
    // refused again and its body never runs.
    const EvalResult second = fx.vm.evaluateModule("res://t/importer.js");
    CHECK(evalLabel(second) == "LoadError");
    CHECK(second.error.message.find("its payload is not a module") != std::string::npos);
    CHECK(completion(fx.vm, "typeof globalThis.ran") == "undefined");
    CHECK(completion(fx.vm, "typeof globalThis.imported") == "undefined");
    CHECK(fx.vm.stats().moduleInstances == 0U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a read that fails after listing a module retires the name, so the orphan is never linked (BY37)") {
    constexpr std::string_view PATH = "res://t/half.js";
    constexpr std::byte BC_TAG_ARRAY{9};
    constexpr std::byte BC_TAG_MODULE{13};
    constexpr std::byte INVALID_TAG{0xFF};
    // BY36's container with the array's length 2: the module named by atom #0 is read and listed first, then the
    // second element's tag is invalid, so the reader fails with that module still listed under the key's name.
    const std::vector<std::byte> container = toBytes(containerOf("globalThis.ran = 1;", PATH));
    const engine::assets::CookedScriptParseResult parsed = engine::assets::parseCookedScript(container);
    REQUIRE(parsed.status == engine::assets::CookedScriptStatus::Ok);
    const std::size_t moduleAt = atomTableEnd(parsed.script.payload);
    REQUIRE(parsed.script.payload[moduleAt] == BC_TAG_MODULE);
    std::vector<std::byte> payload(parsed.script.payload.begin(),
                                   parsed.script.payload.begin() + static_cast<std::ptrdiff_t>(moduleAt));
    payload.push_back(BC_TAG_ARRAY);
    payload.push_back(std::byte{2});  // two elements
    payload.insert(payload.end(), parsed.script.payload.begin() + static_cast<std::ptrdiff_t>(moduleAt),
                   parsed.script.payload.end());
    payload.push_back(INVALID_TAG);
    const std::span<std::byte> view{payload};
    engine::assets::putU32(view, 1, bytecodeChecksum(std::span<const std::byte>{view}.subspan(5)));
    const engine::assets::CookedScriptInput input{engine::Guid{}, scriptBackendRelease(), scriptBytecodeVersion(), 1U,
                                                  payload};
    const engine::assets::CookedScriptWriteResult wrapped = engine::assets::writeCookedScript(input);
    REQUIRE(wrapped.status == engine::assets::CookedScriptStatus::Ok);

    const LogCapture log;
    Fixture fx;
    fx.files->set("t/half.js", toText(wrapped.bytes));
    fx.files->set("t/importer.js", "import \"./half.js\";\nglobalThis.imported = 1;\n");
    // The reader's failure arrives as a pending SyntaxError, which classify reports as a CompileError.
    const EvalResult first = fx.vm.evaluateModule(PATH);
    CHECK(evalLabel(first) == "CompileError");
    CHECK(first.error.message.find("invalid tag") != std::string::npos);
    const std::optional<ModuleRecord> record = recordOf(fx.vm, PATH);
    REQUIRE(record.has_value());
    CHECK(record->generation == 2U);
    CHECK(stateOf(fx.vm, PATH) != "Loaded");

    // The import names the next generation, so the orphan listed under the first one is unreachable: the container
    // is read again, fails again, and its body never runs.
    const EvalResult second = fx.vm.evaluateModule("res://t/importer.js");
    CHECK(evalLabel(second) == "CompileError");
    CHECK(second.error.message.find("invalid tag") != std::string::npos);
    CHECK(completion(fx.vm, "typeof globalThis.ran") == "undefined");
    CHECK(completion(fx.vm, "typeof globalThis.imported") == "undefined");
    CHECK(fx.vm.stats().moduleInstances == 0U);
    expectCleanShutdown(fx.vm);
}

// ---- BY30-BY34: the same results, the one exception, and R22's shapes through bytecode -------------------------

namespace {

constexpr std::string_view VFS_SCHEME = "res://";

[[nodiscard]] std::string relativeOf(std::string_view path) {
    REQUIRE(path.starts_with(VFS_SCHEME));
    return std::string{path.substr(VFS_SCHEME.size())};
}

// Everything one run of a module can show: its status, its error, and the results it left behind.
struct Observation {
    std::string status;
    std::string errorName;
    std::string errorMessage;
    std::string errorStack;
    std::string results;
};

// A fresh VM over a fresh MemoryBackend holding `bytes` at `path`; the module runs, the results are read, and the
// VM is shut down cleanly.
[[nodiscard]] Observation observe(std::string_view path, const std::string& bytes) {
    Fixture fx;
    fx.files->set(relativeOf(path), bytes);
    const EvalResult result = fx.vm.evaluateModule(path);
    Observation seen;
    seen.status = evalLabel(result);
    seen.errorName = result.error.name;
    seen.errorMessage = result.error.message;
    seen.errorStack = result.error.stack;
    seen.results = completion(fx.vm, "String(globalThis.results)");
    expectCleanShutdown(fx.vm);
    return seen;
}

void checkSame(const Observation& js, const Observation& bytecode) {
    INFO("status");
    CHECK(js.status == bytecode.status);
    INFO("error name");
    CHECK(js.errorName == bytecode.errorName);
    INFO("error message");
    CHECK(js.errorMessage == bytecode.errorMessage);
    INFO("error stack");
    CHECK(js.errorStack == bytecode.errorStack);
    INFO("results");
    CHECK(js.results == bytecode.results);
}

[[nodiscard]] std::size_t countOf(std::string_view text, std::string_view token) {
    std::size_t count = 0;
    for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
        ++count;
    }
    return count;
}

// A VFS of JavaScript modules, where the seam's cook reads what its allowed imports name.
class SeamVfs {
public:
    SeamVfs() {
        auto backend = std::make_unique<MemoryBackend>();
        files = backend.get();
        vfs.mount(std::move(backend));
    }
    SeamVfs(const SeamVfs&) = delete;
    SeamVfs& operator=(const SeamVfs&) = delete;
    SeamVfs(SeamVfs&&) = delete;
    SeamVfs& operator=(SeamVfs&&) = delete;
    ~SeamVfs() = default;

    void set(std::string_view path, std::string text) { files->set(relativeOf(path), std::move(text)); }

    // The container of `source` cooked under `path`, its imports read from this VFS.
    [[nodiscard]] std::string cook(std::string_view source, std::string_view path) const {
        const ScriptCookResult cooked = engine::script::internal::cookScriptBytecodeWith(
            source, path, engine::script::internal::CookOptions{false, &vfs});
        REQUIRE(label(cooked) == "Ok");
        const engine::assets::CookedScriptWriteResult written =
            engine::assets::writeCookedScript(engine::assets::CookedScriptInput{
                engine::Guid{}, cooked.backendRelease, cooked.bytecodeVersion, cooked.stripFlags, cooked.payload});
        REQUIRE(written.status == engine::assets::CookedScriptStatus::Ok);
        return toText(written.bytes);
    }

    engine::VirtualFileSystem vfs;
    MemoryBackend* files = nullptr;
};

}  // namespace

TEST_CASE("script: the language-feature bundle gives the same results from bytecode as from source (BY30)") {
    constexpr std::string_view PATH = "res://bytecode/features.js";
    const std::string js = readFixture("features.js");
    const Observation fromJs = observe(PATH, js);
    const Observation fromBytecode = observe(PATH, containerOf(js, PATH));
    CHECK(fromJs.status == "Ok");
    CHECK(fromBytecode.status == "Ok");
    checkSame(fromJs, fromBytecode);
    // Anti-vacuity: the debug information survived (a stripped cook shows <null>:0:1 and no import.meta).
    CHECK(fromBytecode.results.find("\"meta\":[\"object\",0,true]") != std::string::npos);
    const std::size_t caught = fromBytecode.results.find("res://bytecode/features.js:");
    REQUIRE(caught != std::string::npos);
    CHECK(fromBytecode.results.find_first_of("0123456789", caught + PATH.size() + 1U) == caught + PATH.size() + 1U);
    CHECK(fromBytecode.results.find("<null>") == std::string::npos);
}

TEST_CASE("script: a throw reaches the caller with the same error and the same frames from bytecode (BY31)") {
    constexpr std::string_view PATH = "res://bytecode/throws.js";
    const std::string js = readFixture("throws.js");
    const Observation fromJs = observe(PATH, js);
    const Observation fromBytecode = observe(PATH, containerOf(js, PATH));
    checkSame(fromJs, fromBytecode);
    CHECK(fromBytecode.status == "Exception");
    CHECK(fromBytecode.errorName == "TypeError");
    CHECK(fromBytecode.errorMessage == "too big: 3");
    // Anti-vacuity: five frames, each naming the module under its own path and no placeholder.
    CHECK(countOf(fromBytecode.errorStack, "    at ") == 5U);
    CHECK(countOf(fromBytecode.errorStack, "res://bytecode/throws.js:") == 5U);
    CHECK(fromBytecode.errorStack.find("res://?") == std::string::npos);
}

TEST_CASE("script: stripped bytecode changes toString() and nothing else about a function (BY32)") {
    constexpr std::string_view PATH = "res://bytecode/source-text.js";
    const std::string js = readFixture("source-text.js");
    const std::string container = containerOf(js, PATH);
    // One field of one function's description, read from a fresh VM that ran `bytes` as the module.
    const auto field = [&](const std::string& bytes, std::string_view key, std::string_view member) {
        Fixture fx;
        fx.files->set(relativeOf(PATH), bytes);
        REQUIRE(evalLabel(fx.vm.evaluateModule(PATH)) == "Ok");
        const std::string expression = "JSON.parse(globalThis.results)." + std::string{key} + "." + std::string{member};
        std::string out = completion(fx.vm, expression);
        expectCleanShutdown(fx.vm);
        return out;
    };
    struct Expected {
        std::string_view key;
        std::string_view sourcePrefix;
        std::string_view bytecodeText;
    };
    const std::array<Expected, 5> expected{{
        {"plain", "function plain(", "function plain() {\n    [native code]\n}"},
        {"klass", "class ", "function Klass() {\n    [native code]\n}"},
        {"method", "method(x)", "function method() {\n    [native code]\n}"},
        {"named", "(p) =>", "function named() {\n    [native code]\n}"},
        {"anonymous", "(q) =>", "function () {\n    [native code]\n}"},
    }};
    for (const Expected& each : expected) {
        INFO("function: " << std::string{each.key});
        const std::string sourceText = field(js, each.key, "text");
        CHECK(sourceText.starts_with(each.sourcePrefix));
        CHECK(field(container, each.key, "text") == each.bytecodeText);
        // The exception stops at toString(): name and length are the source's own.
        CHECK(field(container, each.key, "name") == field(js, each.key, "name"));
        CHECK(field(container, each.key, "length") == field(js, each.key, "length"));
    }
    // Anti-vacuity: the identity arms are not all empty.
    CHECK(field(container, "plain", "name") == "plain");
    CHECK(field(container, "plain", "length") == "2");
}

TEST_CASE("script: R22's dangling shape through bytecode is retired, as through source (BY33)") {
    LogCapture log;
    const std::string entrySource = R"(import "res://r/x.js"; import "res://r/missing.js"; export const e = 1;)";
    const std::string xSource = "import { e } from \"res://r/entry.js\"; export const x = 2;";
    SeamVfs seam;
    seam.set("res://r/entry.js", entrySource);
    seam.set("res://r/x.js", xSource);
    seam.set("res://r/missing.js", "export {};");

    Fixture fx;
    fx.files->set("r/entry.js", seam.cook(entrySource, "res://r/entry.js"));
    fx.files->set("r/x.js", seam.cook(xSource, "res://r/x.js"));
    fx.files->set("r/entry2.js", "import { x } from \"./x.js\"; globalThis.got = x;");

    CHECK(evalLabel(fx.vm.evaluateModule("res://r/entry.js")) == "LoadError");
    const std::optional<ModuleRecord> x = recordOf(fx.vm, "res://r/x.js");
    REQUIRE(x.has_value());
    CHECK(x->generation == 2U);
    CHECK(moduleStateLabel(x->state) == "Unloaded");
    const std::optional<ModuleRecord> entry = recordOf(fx.vm, "res://r/entry.js");
    REQUIRE(entry.has_value());
    // A bytecode read that started retires its key whatever its outcome (the reader lists a module before it can
    // fail), so the entry whose read failed on its import moves too; the JavaScript path's failed compile does not.
    CHECK(entry->generation == 2U);

    fx.files->set("r/missing.js", "export {};");
    log.clear();
    CHECK(evalLabel(fx.vm.evaluateModule("res://r/entry2.js")) == "Ok");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    CHECK(completion(fx.vm, "got") == "2");
    CHECK(fx.files->reads("r/x.js") == 2U);
    const std::optional<ModuleRecord> fresh = recordOf(fx.vm, "res://r/x.js");
    REQUIRE(fresh.has_value());
    CHECK(fresh->generation == 2U);
    CHECK(moduleStateLabel(fresh->state) == "Evaluated");
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: R22's half-linked shape through bytecode is never linked again (BY34)") {
    SeamVfs seam;
    const std::string half =
        R"(import { ok } from "res://r/j.js"; import { nope } from "res://r/j.js"; export const h = ok;)";
    seam.set("res://r/j.js", "export const ok = 1;");

    Fixture fx;
    fx.files->set("r/j.js", "export const ok = 1;");
    fx.files->set("r/half.js", seam.cook(half, "res://r/half.js"));
    fx.files->set("r/top1.js", "import \"./half.js\";");
    fx.files->set("r/top2.js", "import \"./half.js\";");

    const EvalResult first = fx.vm.evaluateModule("res://r/top1.js");
    CHECK(evalLabel(first) == "CompileError");
    CHECK(first.error.message == "Could not find export 'nope' in module 'res://r/j.js'");
    CHECK(first.error.stack.empty());
    for (const std::string_view path :
         std::array<std::string_view, 3>{"res://r/top1.js", "res://r/half.js", "res://r/j.js"}) {
        INFO("path: " << std::string{path});
        const std::optional<ModuleRecord> record = recordOf(fx.vm, path);
        REQUIRE(record.has_value());
        CHECK(record->generation == 2U);
    }

    const EvalResult second = fx.vm.evaluateModule("res://r/top2.js");
    CHECK(evalLabel(second) == "CompileError");
    CHECK(second.error.message == "Could not find export 'nope' in module 'res://r/j.js?gen=2'");
    CHECK(fx.files->reads("r/half.js") == 2U);
    expectCleanShutdown(fx.vm);
}

TEST_CASE("script: a container whose payload is not a module is refused, never taken for one (BY35)") {
    constexpr std::string_view PATH = "res://t/notmodule.js";
    // A payload of the backend's own format: the version byte, the checksum word, one atom (the cook's placeholder, as
    // a narrow string) and, where a module would be, an object with no properties. The head is valid, so only the
    // value's kind is wrong.
    std::vector<std::byte> payload{std::byte{engine::script::internal::SCRIPT_BYTECODE_VERSION}, std::byte{0},
                                   std::byte{0}, std::byte{0}, std::byte{0}};
    payload.push_back(std::byte{1});  // one atom
    payload.push_back(std::byte{1});  // of the string type
    payload.push_back(static_cast<std::byte>(COOK_MODULE_NAME.size() << 1U));
    for (const char c : COOK_MODULE_NAME) {
        payload.push_back(static_cast<std::byte>(c));
    }
    payload.push_back(std::byte{8});  // BC_TAG_OBJECT
    payload.push_back(std::byte{0});  // no properties
    const std::span<std::byte> view{payload};
    engine::assets::putU32(view, 1, bytecodeChecksum(std::span<const std::byte>{view}.subspan(5)));
    const engine::assets::CookedScriptInput input{engine::Guid{}, scriptBackendRelease(), scriptBytecodeVersion(), 1U,
                                                  payload};
    const engine::assets::CookedScriptWriteResult wrapped = engine::assets::writeCookedScript(input);
    REQUIRE(wrapped.status == engine::assets::CookedScriptStatus::Ok);

    const LogCapture log;
    Fixture fx;
    fx.files->set("t/notmodule.js", toText(wrapped.bytes));
    const EvalResult result = fx.vm.evaluateModule(PATH);
    CHECK(evalLabel(result) == "LoadError");
    CHECK(result.error.message == cookedScriptRefusedMessage(PATH, "", "its payload is not a module"));
    CHECK(result.error.message.find(PATH) != std::string::npos);
    CHECK(stateOf(fx.vm, PATH) != "Loaded");
    CHECK(fx.vm.stats().moduleInstances == 0U);
    expectCleanShutdown(fx.vm);
    // Anti-vacuity: the same fixture loads a real container.
    Fixture good;
    good.files->set("t/ok.js", containerOf("globalThis.v = 1;", "res://t/ok.js"));
    CHECK(evalLabel(good.vm.evaluateModule("res://t/ok.js")) == "Ok");
    expectCleanShutdown(good.vm);
}
