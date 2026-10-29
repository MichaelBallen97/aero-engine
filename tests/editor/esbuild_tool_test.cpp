// tests/editor/esbuild_tool_test.cpp -- task 4.3.1: the pinned esbuild and its pure rules. Tier-0, UNGATED, no
// GPU, no window, no ImGui context.
//
// EB1 holds the binary the configure fetched: the pinned version, and it runs. EB2-EB26 are the pure half
// (esbuild_tool.hpp): the script predicate, the command line, input classification, the metafile and log parsers,
// the record format and its freshness rule.
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. tool_process.hpp is SRC-PRIVATE and is reached by an explicit relative
// path (the blender_service_test.cpp precedent).
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/esbuild_tool.hpp>

#include "../../editor/src/tool_process.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using engine::ContentHash;
using engine::Guid;
using engine::editor::ProcessState;
using engine::editor::ToolProcess;
namespace ed = engine::editor;

namespace {

// A unique temp directory that removes itself on destruction (the blender_service_test.cpp shape).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_esbuild_tool_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// The whole file, read and closed before returning (a stream still open would block the TempDir's removal on
// Windows).
[[nodiscard]] std::string readAll(std::string_view absolutePathUtf8) {
    std::ifstream in(pathOf(absolutePathUtf8), std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr std::chrono::seconds WAIT_CAP{20};

}  // namespace

TEST_CASE("esbuild_tool: the fetched binary is executable and reports the pinned version (EB1)") {
    const std::string esbuildPath = AERO_TEST_ESBUILD;
    const std::string pinnedVersion = AERO_TEST_ESBUILD_VERSION;

    std::error_code ec;
    const std::filesystem::path binary = pathOf(esbuildPath);
    REQUIRE(std::filesystem::is_regular_file(binary, ec));
    CHECK_FALSE(pinnedVersion.empty());

    const TempDir tmp;
    ToolProcess process;
    const std::vector<std::string> args = {esbuildPath, "--version"};
    const std::string failure = process.start(args, tmp.utf8(), tmp.join("version.log"));
    REQUIRE(failure.empty());

    ProcessState state = ProcessState::Running;
    int exitCode = -1;
    const auto deadline = std::chrono::steady_clock::now() + WAIT_CAP;
    while (std::chrono::steady_clock::now() < deadline) {
        state = process.poll(exitCode);
        if (state != ProcessState::Running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(state == ProcessState::Exited);
    CHECK(exitCode == 0);

    std::string log = readAll(tmp.join("version.log"));
    if (log.size() >= 2 && log.compare(log.size() - 2, 2, "\r\n") == 0) {
        log.erase(log.size() - 2);
    } else if (!log.empty() && log.back() == '\n') {
        log.pop_back();
    }
    CHECK(log == pinnedVersion);
}

// ---- EB2-EB25: the pure half ---------------------------------------------------------------------------------------

namespace {

// esbuild's log glyphs as \x escapes, each in its own literal: an escape swallows every following hex digit, and no
// source file here may depend on the compiler's input charset.
constexpr std::string_view CROSS = "\xE2\x9C\x98";     // the error mark
constexpr std::string_view TRIANGLE = "\xE2\x96\xB2";  // the warning mark
constexpr std::string_view BAR = "\xE2\x94\x82";       // the excerpt gutter
constexpr std::string_view TICK = "\xE2\x95\xB5";      // the excerpt underline gutter

[[nodiscard]] std::string errorHeader(std::string_view text) {
    return std::string(CROSS) + " [ERROR] " + std::string(text) + "\n";
}
[[nodiscard]] std::string warningHeader(std::string_view text) {
    return std::string(TRIANGLE) + " [WARNING] " + std::string(text) + "\n";
}
[[nodiscard]] std::string excerpt(std::string_view source, std::string_view underline) {
    return "      1 " + std::string(BAR) + " " + std::string(source) + "\n        " + std::string(TICK) +
           underline.data() + "\n";
}

// The logs below were produced by the pinned esbuild over the task's fixture project, run from the project root.
[[nodiscard]] std::string syntaxLog() {
    return errorHeader("Unexpected \";\"") + "\n    assets/broken/syntax.ts:1:17:\n" +
           excerpt("export const x = ;", "                  ^") + "\n";
}
[[nodiscard]] std::string warnLog() {
    return warningHeader("Duplicate key \"k\" in object literal [duplicate-object-key]") +
           "\n    assets/broken/warn.ts:1:25:\n" +
           excerpt("export const o = { k: 1, k: 2 };", "                          ^") +
           "\n  The original key \"k\" is here:\n\n    assets/broken/warn.ts:1:19:\n" +
           excerpt("export const o = { k: 1, k: 2 };", "                    ^") + "\n";
}
[[nodiscard]] std::string bareLog() {
    return errorHeader("Could not resolve \"lodash\"") + "\n    assets/broken/bare.ts:1:22:\n" +
           excerpt("import { chunk } from \"lodash\";", "                       ~~~~~~~~") +
           "\n  You can mark the path \"lodash\" as external to exclude it from the bundle, which will remove this "
           "error and leave the unresolved path in the bundle.\n\n";
}

[[nodiscard]] std::string withCrlf(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
}

[[nodiscard]] ContentHash hashOf(std::string_view text) {
    return engine::hashBytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

[[nodiscard]] std::string fingerprintOf(std::span<const std::string_view> flags) {
    std::string joined;
    for (const std::string_view flag : flags) {
        joined += flag;
        joined += '\n';
    }
    const std::string text = engine::formatContentHash(hashOf(joined));
    return text;
}

// The eleven flags, spelled again here so a drifting constant cannot pass its own test.
const std::array<std::string_view, 11> testFlags{
    "--bundle",
    "--format=esm",
    "--platform=neutral",
    "--target=es2023",
    "--supported:using=false",
    "--preserve-symlinks",
    "--sourcemap=linked",
    R"(--tsconfig-raw={"compilerOptions":{"verbatimModuleSyntax":true,)"
    R"("useDefineForClassFields":true,"experimentalDecorators":false}})",
    "--log-level=warning",
    "--color=false",
    "--log-limit=0",
};

const std::string playerMetafile = R"({
  "inputs": {
    "assets/scripts/util.ts": { "bytes": 99, "imports": [], "format": "esm" },
    "assets/shared/math.ts": { "bytes": 21, "imports": [], "format": "esm" },
    "assets/scripts/player.ts": {
      "bytes": 213,
      "imports": [
        { "path": "assets/scripts/util.ts", "kind": "import-statement", "original": "./util" },
        { "path": "assets/scripts/util.ts", "kind": "import-statement", "original": "./util.js" },
        { "path": "assets/shared/math.ts", "kind": "import-statement", "original": "../shared/math" }
      ],
      "format": "esm"
    }
  },
  "outputs": {
    "Library/ScriptStaging/g.js.map": { "imports": [], "exports": [], "inputs": {}, "bytes": 789 },
    "Library/ScriptStaging/g.js": {
      "imports": [],
      "exports": ["result"],
      "entryPoint": "assets/scripts/player.ts",
      "inputs": {
        "assets/scripts/util.ts": { "bytesInOutput": 22 },
        "assets/shared/math.ts": { "bytesInOutput": 12 },
        "assets/scripts/player.ts": { "bytesInOutput": 61 }
      },
      "bytes": 240
    }
  }
})";

const std::string r21Metafile = R"({
  "inputs": {
    "assets/scripts/r21.ts": {
      "bytes": 126,
      "imports": [ { "path": "<runtime>", "kind": "import-statement", "external": true } ]
    }
  },
  "outputs": {
    "Library/ScriptStaging/g.js.map": { "imports": [], "exports": [], "inputs": {}, "bytes": 546 },
    "Library/ScriptStaging/g.js": {
      "imports": [],
      "exports": [],
      "entryPoint": "assets/scripts/r21.ts",
      "inputs": { "assets/scripts/r21.ts": { "bytesInOutput": 323 } },
      "bytes": 1936
    }
  }
})";

}  // namespace

TEST_CASE("esbuild_tool: the pinned version is the one CMake fetched (EB2)") {
    CHECK(ed::esbuildPinnedVersion() == std::string_view(AERO_TEST_ESBUILD_VERSION));
}

TEST_CASE("esbuild_tool: only .ts files that are not declarations are scripts (EB3)") {
    CHECK(ed::isScriptAssetName("a.ts"));
    CHECK(ed::isScriptAssetName("dir/a.ts"));
    CHECK(ed::isScriptAssetName("scripts dir/pl\xC3\xA5yer.ts"));
    CHECK_FALSE(ed::isScriptAssetName("a.d.ts"));
    CHECK_FALSE(ed::isScriptAssetName("x/types.d.ts"));
    CHECK_FALSE(ed::isScriptAssetName("a.tsx"));
    CHECK_FALSE(ed::isScriptAssetName("a.mts"));
    CHECK_FALSE(ed::isScriptAssetName("a.cts"));
    CHECK_FALSE(ed::isScriptAssetName("a.js"));
    CHECK_FALSE(ed::isScriptAssetName(".ts"));
    CHECK_FALSE(ed::isScriptAssetName("dir/.ts"));
    CHECK_FALSE(ed::isScriptAssetName("a.TS"));
    CHECK_FALSE(ed::isScriptAssetName("a.ts.bak"));
    CHECK_FALSE(ed::isScriptAssetName(""));
}

TEST_CASE("esbuild_tool: the executable name and the path resolution order (EB4)") {
    CHECK(ed::esbuildExecutableName(ed::HostOs::Windows) == "esbuild.exe");
    CHECK(ed::esbuildExecutableName(ed::HostOs::MacOs) == "esbuild");
    CHECK(ed::esbuildExecutableName(ed::HostOs::Linux) == "esbuild");

    CHECK(ed::resolveEsbuildPath(ed::HostOs::Linux, "/cfg/esbuild", "/env/esbuild", "/opt/x") == "/cfg/esbuild");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::Linux, "", "/env/esbuild", "/opt/x") == "/env/esbuild");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::Linux, "", "", "/opt/x/") == "/opt/x/esbuild");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::Linux, "", "", "/opt/x") == "/opt/x/esbuild");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::Windows, "", "", "C:\\x\\") == "C:\\x\\esbuild.exe");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::Windows, "", "", "C:\\x") == "C:\\x/esbuild.exe");
    CHECK(ed::resolveEsbuildPath(ed::HostOs::MacOs, "", "", "").empty());
}

TEST_CASE("esbuild_tool: the assets prefix drops empty and dot segments (EB5)") {
    CHECK(ed::assetsPrefixOf("assets") == "assets");
    CHECK(ed::assetsPrefixOf("assets/") == "assets");
    CHECK(ed::assetsPrefixOf("./assets") == "assets");
    CHECK(ed::assetsPrefixOf("game//assets/") == "game/assets");
    CHECK(ed::assetsPrefixOf(".").empty());
    CHECK(ed::assetsPrefixOf("./").empty());
    CHECK(ed::assetsPrefixOf("").empty());
}

TEST_CASE("esbuild_tool: the run paths stage one level under Library (EB6)") {
    const Guid guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const std::string guidText = engine::formatGuid(guid);

    const ed::EsbuildRunPaths paths = ed::esbuildRunPaths("assets", "scripts/player.ts", guid);
    CHECK(paths.entry == "assets/scripts/player.ts");
    CHECK(paths.outfile == "Library/ScriptStaging/" + guidText + ".js");
    CHECK(paths.metafile == "Library/ScriptStaging/" + guidText + ".meta.json");

    const ed::EsbuildRunPaths rooted = ed::esbuildRunPaths("", "scripts/player.ts", guid);
    CHECK(rooted.entry == "scripts/player.ts");
    CHECK(rooted.outfile == paths.outfile);
}

TEST_CASE("esbuild_tool: the command line is the fifteen pinned elements, unquoted (EB7)") {
    const Guid guid{1, 2};
    const ed::EsbuildRunPaths paths = ed::esbuildRunPaths("assets", "scripts/player.ts", guid);
    const std::vector<std::string> args = ed::buildEsbuildArgs("/bin/esbuild", paths);

    std::vector<std::string> expected;
    expected.emplace_back("/bin/esbuild");
    expected.push_back(paths.entry);
    for (const std::string_view flag : testFlags) {
        expected.emplace_back(flag);
    }
    expected.push_back("--metafile=" + paths.metafile);
    expected.push_back("--outfile=" + paths.outfile);

    REQUIRE(args.size() == 15);
    REQUIRE(expected.size() == 15);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(args[i] == expected[i]);
    }
    const std::string& tsconfig = args[9];
    CHECK(tsconfig.rfind("--tsconfig-raw={\"compilerOptions\":", 0) == 0);
    CHECK(tsconfig.find('\\') == std::string::npos);
    CHECK(std::count(tsconfig.begin(), tsconfig.end(), '"') == 8);  // the JSON's own quotes, none added
    CHECK(tsconfig.front() != '"');
    CHECK(tsconfig.back() == '}');
}

TEST_CASE("esbuild_tool: the flags fingerprint covers every flag (EB8)") {
    const std::string fingerprint = ed::esbuildFlagsFingerprint();
    REQUIRE(fingerprint.size() == 32);
    CHECK(std::all_of(fingerprint.begin(), fingerprint.end(),
                      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK(fingerprint == fingerprintOf(testFlags));

    for (std::size_t skipped = 0; skipped < testFlags.size(); ++skipped) {
        std::vector<std::string_view> variant;
        for (std::size_t i = 0; i < testFlags.size(); ++i) {
            if (i != skipped) {
                variant.push_back(testFlags[i]);
            }
        }
        CHECK(fingerprintOf(variant) != fingerprint);
    }
}

TEST_CASE("esbuild_tool: input classification is segment-wise and ordered (EB9)") {
    using V = ed::ScriptInputVerdict;
    CHECK(ed::classifyScriptInput("assets/a.ts", "assets") == V::Accepted);
    CHECK(ed::classifyScriptInput("assets/dir/a.ts", "assets") == V::Accepted);
    CHECK(ed::classifyScriptInput("assets2/a.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("outside.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("../x.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("assets/../x.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("/abs/a.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("C:/a.ts", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("assets", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("assets/", "assets") == V::OutsideAssetsRoot);
    CHECK(ed::classifyScriptInput("node_modules/lodash/index.js", "assets") == V::PackageImport);
    CHECK(ed::classifyScriptInput("assets/node_modules/x/i.js", "assets") == V::PackageImport);
    CHECK(ed::classifyScriptInput("Library/Scripts/x.js", "assets") == V::InsideLibrary);

    CHECK(ed::classifyScriptInput("x.ts", "") == V::Accepted);
    CHECK(ed::classifyScriptInput("Library/x.js", "") == V::InsideLibrary);
    CHECK(ed::classifyScriptInput("../x.ts", "") == V::OutsideAssetsRoot);
}

TEST_CASE("esbuild_tool: the assets-relative remainder has no leading separator (EB10)") {
    CHECK(ed::assetsRelativeOf("assets/scripts/a.ts", "assets") == "scripts/a.ts");
    CHECK(ed::assetsRelativeOf("a.ts", "") == "a.ts");
    CHECK(ed::assetsRelativeOf("assets2/a.ts", "assets").empty());
    CHECK(ed::assetsRelativeOf("node_modules/x/i.js", "assets").empty());
    CHECK(ed::assetsRelativeOf("Library/x.js", "").empty());
    CHECK(ed::projectRelativeOf("", "scripts/a.ts") == "scripts/a.ts");
    CHECK(ed::projectRelativeOf("assets", "scripts/a.ts") == "assets/scripts/a.ts");
}

TEST_CASE("esbuild_tool: the metafile's inputs are its files, never the runtime (EB11)") {
    const std::optional<ed::EsbuildMetafile> player = ed::parseEsbuildMetafile(playerMetafile);
    REQUIRE(player.has_value());
    const std::vector<std::string> expectedPlayer = {"assets/scripts/player.ts", "assets/scripts/util.ts",
                                                     "assets/shared/math.ts"};
    CHECK(player->inputs == expectedPlayer);

    const std::optional<ed::EsbuildMetafile> r21 = ed::parseEsbuildMetafile(r21Metafile);
    REQUIRE(r21.has_value());
    const std::vector<std::string> expectedR21 = {"assets/scripts/r21.ts"};
    CHECK(r21->inputs == expectedR21);
}

TEST_CASE("esbuild_tool: a malformed metafile is refused, a Windows spelling is normalised (EB12)") {
    CHECK_FALSE(ed::parseEsbuildMetafile("garbage").has_value());
    CHECK_FALSE(ed::parseEsbuildMetafile("").has_value());
    CHECK_FALSE(ed::parseEsbuildMetafile("[]").has_value());
    CHECK_FALSE(ed::parseEsbuildMetafile("{}").has_value());
    CHECK_FALSE(ed::parseEsbuildMetafile(R"({"inputs": []})").has_value());
    CHECK_FALSE(ed::parseEsbuildMetafile(R"({"inputs": {"": {}}})").has_value());

    const std::optional<ed::EsbuildMetafile> windows =
        ed::parseEsbuildMetafile(R"({"inputs": {"assets\\scripts\\a.ts": {}, "assets/scripts/a.ts": {}}})");
    REQUIRE(windows.has_value());
    const std::vector<std::string> expected = {"assets/scripts/a.ts"};
    CHECK(windows->inputs == expected);
}

TEST_CASE("esbuild_tool: a syntax error is one diagnostic with a 1-based column (EB13)") {
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(syntaxLog());
    CHECK(parse.total == 1);
    REQUIRE(parse.diagnostics.size() == 1);
    const ed::ScriptDiagnostic expected{ed::ScriptDiagnosticSeverity::Error, "assets/broken/syntax.ts", 1, 18,
                                        "Unexpected \";\""};
    CHECK(parse.diagnostics[0] == expected);
}

TEST_CASE("esbuild_tool: a warning takes its own location, not its note's (EB14)") {
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(warnLog());
    CHECK(parse.total == 1);
    REQUIRE(parse.diagnostics.size() == 1);
    const ed::ScriptDiagnostic expected{ed::ScriptDiagnosticSeverity::Warning, "assets/broken/warn.ts", 1, 26,
                                        "Duplicate key \"k\" in object literal [duplicate-object-key]"};
    CHECK(parse.diagnostics[0] == expected);
}

TEST_CASE("esbuild_tool: an unresolved package with a trailing note is one diagnostic (EB15)") {
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(bareLog());
    CHECK(parse.total == 1);
    REQUIRE(parse.diagnostics.size() == 1);
    const ed::ScriptDiagnostic expected{ed::ScriptDiagnosticSeverity::Error, "assets/broken/bare.ts", 1, 23,
                                        "Could not resolve \"lodash\""};
    CHECK(parse.diagnostics[0] == expected);
}

TEST_CASE("esbuild_tool: order, CRLF and backslashes do not change the reading (EB16)") {
    const std::string log = syntaxLog() + warnLog();
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(log);
    REQUIRE(parse.diagnostics.size() == 2);
    CHECK(parse.total == 2);
    CHECK(parse.diagnostics[0].severity == ed::ScriptDiagnosticSeverity::Error);
    CHECK(parse.diagnostics[1].severity == ed::ScriptDiagnosticSeverity::Warning);

    const ed::EsbuildLogParse crlf = ed::parseEsbuildLog(withCrlf(log));
    CHECK(crlf.diagnostics == parse.diagnostics);
    CHECK(crlf.total == parse.total);

    const std::string windows = errorHeader("boom") + "\n    assets\\broken\\x.ts:2:3:\n";
    const ed::EsbuildLogParse backslashed = ed::parseEsbuildLog(windows);
    REQUIRE(backslashed.diagnostics.size() == 1);
    CHECK(backslashed.diagnostics[0].file == "assets/broken/x.ts");
    CHECK(backslashed.diagnostics[0].line == 2);
    CHECK(backslashed.diagnostics[0].column == 4);
}

TEST_CASE("esbuild_tool: a message without a location names the script (EB17)") {
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(errorHeader("The build was cancelled") + "\n");
    REQUIRE(parse.diagnostics.size() == 1);
    const ed::ScriptDiagnostic& diagnostic = parse.diagnostics[0];
    CHECK(diagnostic.file.empty());
    CHECK(diagnostic.line == 0);
    CHECK(diagnostic.column == 0);
    CHECK(ed::formatScriptDiagnostic(diagnostic, "assets/scripts/p.ts") ==
          "assets/scripts/p.ts: error: The build was cancelled");
}

TEST_CASE("esbuild_tool: text with no header is no diagnostic, and a hostile number is no location (EB18)") {
    const ed::EsbuildLogParse garbage = ed::parseEsbuildLog("\n  \r\n  panic: out of memory\nsecond\n");
    CHECK(garbage.diagnostics.empty());
    CHECK(garbage.total == 0);
    CHECK(garbage.firstLine == "panic: out of memory");

    const ed::EsbuildLogParse empty = ed::parseEsbuildLog("");
    CHECK(empty.total == 0);
    CHECK(empty.firstLine.empty());

    const ed::EsbuildLogParse overflow =
        ed::parseEsbuildLog(errorHeader("boom") + "\n    a.ts:99999999999999999999:1:\n");
    REQUIRE(overflow.diagnostics.size() == 1);
    CHECK(overflow.diagnostics[0].file.empty());
    CHECK(overflow.diagnostics[0].line == 0);

    const ed::EsbuildLogParse zero = ed::parseEsbuildLog(errorHeader("boom") + "\n    a.ts:0:5:\n");
    REQUIRE(zero.diagnostics.size() == 1);
    CHECK(zero.diagnostics[0].file.empty());
    CHECK(zero.diagnostics[0].column == 0);
}

TEST_CASE("esbuild_tool: the stored diagnostics are capped, the count is not (EB19)") {
    std::string log;
    for (std::size_t i = 0; i < ed::MAX_SCRIPT_DIAGNOSTICS + 3; ++i) {
        log += errorHeader("error number " + std::to_string(i)) + "\n";
    }
    const ed::EsbuildLogParse parse = ed::parseEsbuildLog(log);
    CHECK(parse.diagnostics.size() == ed::MAX_SCRIPT_DIAGNOSTICS);
    CHECK(parse.total == ed::MAX_SCRIPT_DIAGNOSTICS + 3);
    CHECK(parse.diagnostics.front().text == "error number 0");
}

TEST_CASE("esbuild_tool: a Console line reads file, line, column, severity, text (EB20)") {
    const ed::EsbuildLogParse syntax = ed::parseEsbuildLog(syntaxLog());
    REQUIRE(syntax.diagnostics.size() == 1);
    CHECK(ed::formatScriptDiagnostic(syntax.diagnostics[0], "assets/broken/syntax.ts") ==
          "assets/broken/syntax.ts:1:18: error: Unexpected \";\"");

    const ed::EsbuildLogParse warn = ed::parseEsbuildLog(warnLog());
    REQUIRE(warn.diagnostics.size() == 1);
    CHECK(ed::formatScriptDiagnostic(warn.diagnostics[0], "assets/broken/warn.ts") ==
          "assets/broken/warn.ts:1:26: warning: Duplicate key \"k\" in object literal [duplicate-object-key]");
}

namespace {

[[nodiscard]] ContentHash parsedHash(std::string_view text) {
    const std::optional<ContentHash> hash = engine::parseContentHash(text);
    REQUIRE(hash.has_value());
    return *hash;
}

[[nodiscard]] ed::ScriptRecord failedRecord() {
    ed::ScriptRecord record;
    record.status = ed::ScriptRecordStatus::Failed;
    record.script = "assets/scripts/player.ts";
    record.esbuildVersion = "0.28.2";
    record.esbuildBinary = "0123456789abcdef0123456789abcdef";
    record.flags = "fedcba9876543210fedcba9876543210";
    record.inputs.push_back({"assets/scripts/player.ts", parsedHash("00112233445566778899aabbccddeeff")});
    record.inputs.push_back({"assets/scripts/util.ts", std::nullopt});
    record.sourceSet = "ffeeddccbbaa99887766554433221100";
    record.diagnostics.push_back(
        {ed::ScriptDiagnosticSeverity::Error, "assets/scripts/util.ts", 1, 18, "Unexpected \";\""});
    return record;
}

[[nodiscard]] ed::ScriptRecord okRecord() {
    ed::ScriptRecord record;
    record.status = ed::ScriptRecordStatus::Ok;
    record.script = "assets/scripts/player.ts";
    record.esbuildVersion = "0.28.2";
    record.esbuildBinary = "0123456789abcdef0123456789abcdef";
    record.flags = "fedcba9876543210fedcba9876543210";
    record.inputs.push_back({"assets/scripts/player.ts", parsedHash("00112233445566778899aabbccddeeff")});
    record.inputs.push_back({"assets/scripts/util.ts", parsedHash("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f")});
    return record;
}

// A complete record document with the variable parts spliced in as raw JSON.
[[nodiscard]] std::string recordJson(std::string_view version, std::string_view status, std::string_view inputs,
                                     std::string_view diagnostics) {
    return R"({"version": )" + std::string(version) + R"(, "status": )" + std::string(status) +
           R"(, "script": "s.ts", "esbuildVersion": "0.28.2", "esbuildBinary": "b", "flags": "f", "inputs": )" +
           std::string(inputs) + R"(, "sourceSet": "", "diagnostics": )" + std::string(diagnostics) + "}";
}

}  // namespace

TEST_CASE("esbuild_tool: a record is written in one fixed key order (EB21)") {
    const std::string expected = R"({
  "version": 1,
  "status": "failed",
  "script": "assets/scripts/player.ts",
  "esbuildVersion": "0.28.2",
  "esbuildBinary": "0123456789abcdef0123456789abcdef",
  "flags": "fedcba9876543210fedcba9876543210",
  "inputs": [
    {
      "path": "assets/scripts/player.ts",
      "hash": "00112233445566778899aabbccddeeff"
    },
    {
      "path": "assets/scripts/util.ts",
      "hash": null
    }
  ],
  "sourceSet": "ffeeddccbbaa99887766554433221100",
  "diagnostics": [
    {
      "severity": "error",
      "file": "assets/scripts/util.ts",
      "line": 1,
      "column": 18,
      "text": "Unexpected \";\""
    }
  ]
}
)";
    CHECK(ed::writeScriptRecordText(failedRecord()) == expected);

    const std::string ok = ed::writeScriptRecordText(okRecord());
    CHECK(ok.find("\"status\": \"ok\"") != std::string::npos);
    CHECK(ok.find("\"sourceSet\": \"\"") != std::string::npos);
    CHECK(ok.find("\"diagnostics\": []") != std::string::npos);
    REQUIRE(ok.size() > 2);
    CHECK(ok.back() == '\n');
    CHECK(ok[ok.size() - 2] == '}');
}

TEST_CASE("esbuild_tool: a record survives its own text, a null hash stays null (EB22)") {
    const ed::ScriptRecord ok = okRecord();
    const std::optional<ed::ScriptRecord> okBack = ed::parseScriptRecord(ed::writeScriptRecordText(ok));
    REQUIRE(okBack.has_value());
    CHECK(*okBack == ok);

    const ed::ScriptRecord failed = failedRecord();
    const std::optional<ed::ScriptRecord> failedBack = ed::parseScriptRecord(ed::writeScriptRecordText(failed));
    REQUIRE(failedBack.has_value());
    CHECK(*failedBack == failed);
    REQUIRE(failedBack->inputs.size() == 2);
    CHECK_FALSE(failedBack->inputs[1].hash.has_value());

    // The empty file's real digest is all zeros and must not be confused with "unknown".
    ed::ScriptRecord zero = okRecord();
    zero.inputs[0].hash = ContentHash{};
    const std::optional<ed::ScriptRecord> zeroBack = ed::parseScriptRecord(ed::writeScriptRecordText(zero));
    REQUIRE(zeroBack.has_value());
    CHECK(zeroBack->inputs[0].hash.has_value());
}

TEST_CASE("esbuild_tool: any wrong record is a miss, never a half-read one (EB23)") {
    const std::string good = recordJson("1", "\"ok\"", R"([{"path": "s.ts", "hash": null}])", "[]");
    CHECK(ed::parseScriptRecord(good).has_value());

    CHECK_FALSE(ed::parseScriptRecord("garbage").has_value());
    CHECK_FALSE(ed::parseScriptRecord("[]").has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("2", "\"ok\"", "[]", "[]")).has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1.0", "\"ok\"", "[]", "[]")).has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"done\"", "[]", "[]")).has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", "{}", "[]")).has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", R"([{"hash": null}])", "[]")).has_value());
    CHECK_FALSE(
        ed::parseScriptRecord(recordJson("1", "\"ok\"", R"([{"path": "s.ts", "hash": "xyz"}])", "[]")).has_value());
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", R"([{"path": "s.ts"}])", "[]")).has_value());
    const std::string badSeverity = R"([{"severity": "info", "file": "", "line": 0, "column": 0, "text": "t"}])";
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", "[]", badSeverity)).has_value());
    const std::string badColumn = R"([{"severity": "error", "file": "", "line": 0, "column": -1, "text": "t"}])";
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", "[]", badColumn)).has_value());
    const std::string bigLine = R"([{"severity": "error", "file": "", "line": 4294967296, "column": 0, "text": "t"}])";
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"ok\"", "[]", bigLine)).has_value());

    const std::string written = ed::writeScriptRecordText(failedRecord());
    for (const std::string_view key : {"version", "status", "script", "esbuildVersion", "esbuildBinary", "flags",
                                       "inputs", "sourceSet", "diagnostics"}) {
        const std::string needle = "\"" + std::string(key) + "\":";
        const std::size_t at = written.find(needle);
        REQUIRE(at != std::string::npos);
        std::string removed = written;
        removed.insert(at + 1, "x");
        INFO("key removed: " << key);
        CHECK_FALSE(ed::parseScriptRecord(removed).has_value());
    }
}

namespace {

struct FreshFixture {
    ed::ScriptRecord record = okRecord();
    std::vector<std::pair<std::string, ContentHash>> disk;
    ed::ScriptFreshnessContext context;

    FreshFixture() {
        for (const ed::ScriptRecordInput& input : record.inputs) {
            disk.emplace_back(input.path, *input.hash);
        }
        context.esbuildBinary = record.esbuildBinary;
        context.flags = record.flags;
        context.sourceSet = "";
        rebind();
    }
    FreshFixture(const FreshFixture&) = delete;
    FreshFixture& operator=(const FreshFixture&) = delete;
    FreshFixture(FreshFixture&&) = delete;
    FreshFixture& operator=(FreshFixture&&) = delete;
    ~FreshFixture() = default;

    void rebind() {
        context.currentHash = [this](std::string_view path) -> std::optional<ContentHash> {
            for (const auto& entry : disk) {
                if (entry.first == path) {
                    return entry.second;
                }
            }
            return std::nullopt;
        };
    }
    [[nodiscard]] bool fresh() const { return ed::scriptRecordFresh(record, "assets/scripts/player.ts", context); }
};

}  // namespace

TEST_CASE("esbuild_tool: a record is fresh only while everything it depends on is unchanged (EB24)") {
    {
        const FreshFixture base;
        CHECK(base.fresh());
    }
    SUBCASE("flags differ") {
        FreshFixture f;
        f.context.flags = "other";
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("binary differs and the current one is known") {
        FreshFixture f;
        f.context.esbuildBinary = "ffffffffffffffffffffffffffffffff";
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("one input hash differs") {
        FreshFixture f;
        f.disk[1].second = parsedHash("11111111111111111111111111111111");
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("one input is unknown to the scan") {
        FreshFixture f;
        f.disk.pop_back();
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("one recorded input has no hash") {
        FreshFixture f;
        f.record.inputs[1].hash = std::nullopt;
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("the inputs lack the script itself") {
        FreshFixture f;
        f.record.inputs.erase(f.record.inputs.begin());
        CHECK_FALSE(f.fresh());
    }
    SUBCASE("a failed record with a different source set") {
        FreshFixture f;
        f.record.status = ed::ScriptRecordStatus::Failed;
        f.record.sourceSet = "aaaa";
        f.context.sourceSet = "bbbb";
        CHECK_FALSE(f.fresh());
        f.context.sourceSet = "aaaa";
        CHECK(f.fresh());
    }
    SUBCASE("an unknown binary is not compared") {
        FreshFixture f;
        f.context.esbuildBinary.clear();
        f.record.esbuildBinary = "anything";
        CHECK(f.fresh());
    }
    SUBCASE("informational fields are not compared") {
        FreshFixture f;
        f.record.esbuildVersion = "9.9.9";
        f.record.script = "elsewhere.ts";
        f.record.diagnostics = failedRecord().diagnostics;
        CHECK(f.fresh());
    }
    SUBCASE("an ok record ignores the source set") {
        FreshFixture f;
        f.record.sourceSet = "aaaa";
        f.context.sourceSet = "bbbb";
        CHECK(f.fresh());
    }
}

TEST_CASE("esbuild_tool: the source-set fingerprint tracks paths only (EB25)") {
    const auto makeRecords = [](std::initializer_list<std::string_view> paths) {
        std::vector<ed::AssetRecord> records;
        for (const std::string_view path : paths) {
            ed::AssetRecord record;
            record.relativePath = std::string(path);
            records.push_back(record);
        }
        return records;
    };

    std::vector<ed::AssetRecord> base = makeRecords({"a.ts", "b/c.ts"});
    const std::string fingerprint = ed::scriptSourceSetFingerprint(base);
    CHECK(fingerprint.size() == 32);

    std::vector<ed::AssetRecord> rehashed = base;
    rehashed[0].contentHash = ContentHash{1, 2};
    rehashed[1].contentHash = ContentHash{3, 4};
    CHECK(ed::scriptSourceSetFingerprint(rehashed) == fingerprint);

    const std::vector<ed::AssetRecord> added = makeRecords({"a.ts", "b/c.ts", "d.ts"});
    const std::vector<ed::AssetRecord> removed = makeRecords({"a.ts"});
    const std::vector<ed::AssetRecord> renamed = makeRecords({"a.ts", "b/d.ts"});
    CHECK(ed::scriptSourceSetFingerprint(added) != fingerprint);
    CHECK(ed::scriptSourceSetFingerprint(removed) != fingerprint);
    CHECK(ed::scriptSourceSetFingerprint(renamed) != fingerprint);
    CHECK(ed::scriptSourceSetFingerprint(makeRecords({})) != fingerprint);
}

TEST_CASE("esbuild_tool: a failed record that holds no diagnostic is a miss (EB26)") {
    const std::string inputs = R"([{"path": "s.ts", "hash": null}])";
    CHECK_FALSE(ed::parseScriptRecord(recordJson("1", "\"failed\"", inputs, "[]")).has_value());

    const std::string one = R"([{"severity": "error", "file": "", "line": 0, "column": 0, "text": "t"}])";
    const std::optional<ed::ScriptRecord> failed = ed::parseScriptRecord(recordJson("1", "\"failed\"", inputs, one));
    REQUIRE(failed.has_value());
    CHECK(failed->diagnostics.size() == 1U);

    // An ok record keeps its empty array: it has nothing to show.
    CHECK(ed::parseScriptRecord(recordJson("1", "\"ok\"", inputs, "[]")).has_value());
}
