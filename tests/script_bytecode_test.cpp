// tests/script_bytecode_test.cpp -- task 4.3.2: the bytecode cook (BY8-BY17). A TU of aero_tests, which supplies
// main() from test_main.cpp. The cook runs the real backend: the committed fixtures are esbuild 0.28.2's own bundles
// of the three .ts files beside them (tests/fixtures/scripts/README.md says how they were made).
#include <aero/script/internal/bytecode_cook.hpp>
#include <aero/script/internal/bytecode_format.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>
#include <vector>

namespace {

using engine::LogLevel;
using engine::script::COOK_HEAP_LIMIT_BYTES;
using engine::script::cookScriptBytecode;
using engine::script::MAX_COOK_SOURCE_BYTES;
using engine::script::scriptBackendRelease;
using engine::script::scriptBytecodeVersion;
using engine::script::ScriptCookResult;
using engine::script::scriptCookStatusLabel;
using engine::script::internal::bytecodeChecksum;
using engine::script::internal::COOK_MODULE_NAME;
using engine::script::internal::COOK_PROBE_NAME;
using engine::script::internal::cookImportMessage;
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
    const std::array<std::pair<std::string_view, std::string_view>, 3> imports{{
        {"import x from \"./a.js\";\nexport const y = x;\n", "./a.js"},
        {"export { y } from \"res://b.js\";\n", "res://b.js"},
        {"export * from \"./c.js\";\n", "./c.js"},
    }};
    for (const auto& [source, specifier] : imports) {
        INFO("specifier: " << std::string{specifier});
        const ScriptCookResult cooked = cookScriptBytecode(source, SOURCE_NAME);
        CHECK(label(cooked) == "LoadError");
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
