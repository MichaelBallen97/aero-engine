// tests/editor/script_transpiler_test.cpp -- task 4.3.1: the transpile driver over a REAL esbuild. Tier-0, UNGATED, no
// GPU, no window, no ImGui context.
//
// ST1-ST19 drive ScriptTranspiler (editor/src/script_transpiler.hpp) over real scans of real temporary projects with
// the pinned esbuild the configure fetched (AERO_TEST_ESBUILD). Every bounded wait sleeps one millisecond per
// iteration under a wall-clock cap and injects 0.0 seconds into service(); a timeout is always ONE explicit
// injection (the 3.2.4 WAIT_DT rule). Every case pumps to idle before its TempDir goes: Windows cannot remove an
// open file.
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. script_transpiler.hpp is SRC-PRIVATE and is reached by an explicit relative
// path (the blender_service_test.cpp precedent).
//
// ST20-ST22 (task 4.3.1, step 5) evaluate the editor's own output in the engine's VM: this binary links aero::script
// for that (tests sit outside the boundary rule; aero_editor_core itself does not link it).
//
// TypeScript sources are the plan-time fixtures of task 4.3.1. A rewritten file always changes SIZE: the asset scan's
// (size, mtime) fast path would otherwise reuse the old hash for a same-size rewrite within one timestamp tick.
#include "../../editor/src/script_transpiler.hpp"

#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/core/vfs.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/esbuild_tool.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/script/vm.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using engine::ContentHash;
using engine::Guid;
using engine::GuidGenerator;
namespace ed = engine::editor;

namespace {

// A unique temp directory that removes itself on destruction (the esbuild_tool_test.cpp shape).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_script_transpiler_test_" + std::to_string(++counter));
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

private:
    std::filesystem::path dirPath;
};

[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// The whole file, read and closed before returning.
[[nodiscard]] std::string readPath(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string readAll(std::string_view absolutePathUtf8) { return readPath(pathOf(absolutePathUtf8)); }

void writeFile(const std::string& root, std::string_view relative, std::string_view text) {
    const std::filesystem::path target = pathOf(root) / pathOf(relative);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] bool existsAt(const std::string& root, std::string_view relative) {
    std::error_code ec;
    return std::filesystem::exists(pathOf(root) / pathOf(relative), ec);
}

// The sorted file names directly inside root/relative.
[[nodiscard]] std::vector<std::string> namesIn(const std::string& root, std::string_view relative) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(pathOf(root) / pathOf(relative), ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

constexpr std::chrono::seconds WAIT_CAP{20};
constexpr float WAIT_DT = 0.0F;

// A project with an assets root, a database and a fixed-seed GUID generator.
struct ScriptFixture {
    TempDir dir;
    std::string root;
    ed::AssetDatabase db;
    GuidGenerator generator{0x4311ULL};

    explicit ScriptFixture(std::string_view nested = {}) : root(dir.utf8()) {
        if (!nested.empty()) {
            root += '/';
            root += nested;
        }
        std::error_code ec;
        std::filesystem::create_directories(pathOf(root) / "assets", ec);
    }

    void write(std::string_view relative, std::string_view text) const { writeFile(root, relative, text); }
    void scan() { static_cast<void>(db.rescan(root, root + "/assets", generator)); }
    [[nodiscard]] ed::ScriptProjectView view() const { return {root, "assets", ed::ProjectLanguage::Ts}; }
    [[nodiscard]] Guid guidOf(std::string_view assetsRelative) const {
        const std::optional<Guid> guid = db.guidForPath(assetsRelative);
        REQUIRE(guid.has_value());
        return *guid;
    }
    [[nodiscard]] ContentHash hashOf(std::string_view assetsRelative) const {
        const ed::AssetRecord* const record = db.findByPath(assetsRelative);
        REQUIRE(record != nullptr);
        return record->contentHash;
    }
    [[nodiscard]] std::string artifactPath(Guid guid, std::string_view suffix) const {
        return "Library/Scripts/" + engine::formatGuid(guid) + std::string(suffix);
    }
    [[nodiscard]] std::string artifactBytes(Guid guid, std::string_view suffix) const {
        return readAll(root + "/" + artifactPath(guid, suffix));
    }
    [[nodiscard]] std::optional<ed::ScriptRecord> recordOf(Guid guid) const {
        return ed::parseScriptRecord(artifactBytes(guid, ".json"));
    }
};

[[nodiscard]] ed::ScriptTranspilerConfig configWith(std::string_view esbuildPath) {
    ed::ScriptTranspilerConfig config;
    config.esbuildPath = std::string(esbuildPath);
    return config;
}

void pumpUntilIdle(ed::ScriptTranspiler& transpiler, const ed::AssetDatabase& db) {
    const auto deadline = std::chrono::steady_clock::now() + WAIT_CAP;
    while (!transpiler.idle() && std::chrono::steady_clock::now() < deadline) {
        transpiler.service(db, WAIT_DT);
        if (!transpiler.idle()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    REQUIRE(transpiler.idle());
}

// One scan's worth of work: note it, pump it.
void runPass(ed::ScriptTranspiler& transpiler, const ScriptFixture& fixture) {
    transpiler.noteScan(fixture.db, fixture.view());
    pumpUntilIdle(transpiler, fixture.db);
}

// "INFO text" | "WARN text" | "ERROR text": a log compares as one vector of strings.
[[nodiscard]] std::vector<std::string> rendered(const std::vector<ed::ScriptLogLine>& lines) {
    std::vector<std::string> out;
    for (const ed::ScriptLogLine& line : lines) {
        std::string text;
        switch (line.level) {
            case ed::ScriptLogLevel::Info:
                text = "INFO ";
                break;
            case ed::ScriptLogLevel::Warn:
                text = "WARN ";
                break;
            case ed::ScriptLogLevel::Error:
                text = "ERROR ";
                break;
        }
        text += line.text;
        out.push_back(std::move(text));
    }
    return out;
}

[[nodiscard]] std::string binaryIdentity() {
    const ed::FileHashResult hashed = ed::hashFileContents(AERO_TEST_ESBUILD);
    REQUIRE(hashed.hash.has_value());
    return engine::formatContentHash(*hashed.hash);
}

constexpr std::string_view UTIL_TS = "export const a = 1;\nexport const b = 2;\n";
constexpr std::string_view MATH_TS = "export const m = 20;\n";
constexpr std::string_view PLAYER_TS =
    "import { a } from \"./util\";\n"
    "import { b } from \"./util.js\";\n"
    "import { m } from \"../shared/math\";\n"
    "import type { Unused } from \"./types\";\n"
    "export const result: number = a + b + m;\n";

}  // namespace

TEST_CASE("script_transpiler: one script is published as js, map and record and logged once (ST1)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid guid = f.guidOf("scripts/player.ts");
    const std::string stem = engine::formatGuid(guid);
    std::vector<std::string> expected = {stem + ".js", stem + ".js.map", stem + ".json"};
    std::sort(expected.begin(), expected.end());
    CHECK(namesIn(f.root, "Library/Scripts") == expected);

    const std::string js = f.artifactBytes(guid, ".js");
    const std::string tail = "//# sourceMappingURL=" + stem + ".js.map\n";
    REQUIRE(js.size() > tail.size());
    CHECK(js.compare(js.size() - tail.size(), tail.size(), tail) == 0);

    // The map's single source resolves, against the directory the map is published in, to the script itself.
    const std::string map = f.artifactBytes(guid, ".js.map");
    const std::string key = "\"sources\": [\"";
    const std::size_t at = map.find(key);
    REQUIRE(at != std::string::npos);
    const std::size_t from = at + key.size();
    const std::size_t to = map.find('"', from);
    REQUIRE(to != std::string::npos);
    const std::filesystem::path resolved =
        (std::filesystem::path("Library/Scripts") / map.substr(from, to - from)).lexically_normal();
    CHECK(resolved.generic_string() == "assets/scripts/player.ts");

    const std::optional<ed::ScriptRecord> record = f.recordOf(guid);
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Ok);
    CHECK(record->script == "assets/scripts/player.ts");
    CHECK(record->esbuildVersion == ed::esbuildPinnedVersion());
    CHECK(record->flags == ed::esbuildFlagsFingerprint());
    CHECK(record->esbuildBinary == binaryIdentity());
    const std::vector<ed::ScriptRecordInput> inputs = {{"assets/scripts/player.ts", f.hashOf("scripts/player.ts")}};
    CHECK(record->inputs == inputs);

    CHECK(transpiler.runCount() == 1U);
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"INFO scripts: 1 transpiled, 0 failed, 0 up to date"});
}

TEST_CASE("script_transpiler: imports are bundled and recorded as inputs (ST2)") {
    ScriptFixture f;
    f.write("assets/scripts/util.ts", UTIL_TS);
    f.write("assets/shared/math.ts", MATH_TS);
    f.write("assets/scripts/player.ts", PLAYER_TS);
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid guid = f.guidOf("scripts/player.ts");
    const std::optional<ed::ScriptRecord> record = f.recordOf(guid);
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Ok);
    const std::vector<ed::ScriptRecordInput> inputs = {
        {"assets/scripts/player.ts", f.hashOf("scripts/player.ts")},
        {"assets/scripts/util.ts", f.hashOf("scripts/util.ts")},
        {"assets/shared/math.ts", f.hashOf("shared/math.ts")},
    };
    CHECK(record->inputs == inputs);

    const std::string js = f.artifactBytes(guid, ".js");
    REQUIRE_FALSE(js.empty());
    bool importLine = false;
    std::size_t lineStart = 0;
    while (lineStart < js.size()) {
        if (js.compare(lineStart, 6, "import") == 0) {
            importLine = true;
        }
        const std::size_t newline = js.find('\n', lineStart);
        if (newline == std::string::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    CHECK_FALSE(importLine);
    CHECK(js.find("from \"") == std::string::npos);

    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"INFO scripts: 3 transpiled, 0 failed, 0 up to date"});
}

TEST_CASE("script_transpiler: a failing script keeps its old output and the others still run (ST3)") {
    ScriptFixture f;
    f.write("assets/scripts/a.ts", "export const x = 1;\n");
    f.write("assets/scripts/b.ts", "export const y = 2;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);
    static_cast<void>(transpiler.takeLog());

    const Guid a = f.guidOf("scripts/a.ts");
    const Guid b = f.guidOf("scripts/b.ts");
    const std::string aJs = f.artifactBytes(a, ".js");
    const std::string aMap = f.artifactBytes(a, ".js.map");
    const std::string bJs = f.artifactBytes(b, ".js");
    REQUIRE_FALSE(aJs.empty());

    f.write("assets/scripts/a.ts", "export const x = ;\n");
    f.write("assets/scripts/b.ts", "export const y = 22;\n");
    f.scan();
    runPass(transpiler, f);

    CHECK(f.artifactBytes(a, ".js") == aJs);
    CHECK(f.artifactBytes(a, ".js.map") == aMap);
    const std::optional<ed::ScriptRecord> record = f.recordOf(a);
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Failed);
    const std::vector<ed::ScriptDiagnostic> diagnostics = {
        {ed::ScriptDiagnosticSeverity::Error, "assets/scripts/a.ts", 1U, 18U, "Unexpected \";\""}};
    CHECK(record->diagnostics == diagnostics);
    CHECK(f.artifactBytes(b, ".js") != bJs);

    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"ERROR scripts: assets/scripts/a.ts:1:18: error: Unexpected \";\"",
                                   "INFO scripts: 1 transpiled, 1 failed, 0 up to date"});
}

TEST_CASE("script_transpiler: an error in an imported file is attributed to that file (ST4)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "import { a } from \"./util\";\nexport const p = a;\n");
    f.write("assets/scripts/util.ts", "export const a = ;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid player = f.guidOf("scripts/player.ts");
    const Guid util = f.guidOf("scripts/util.ts");
    const std::optional<ed::ScriptRecord> failed = f.recordOf(player);
    REQUIRE(failed.has_value());
    CHECK(failed->status == ed::ScriptRecordStatus::Failed);
    REQUIRE(failed->diagnostics.size() == 1U);
    CHECK(failed->diagnostics[0].file == "assets/scripts/util.ts");
    CHECK(failed->diagnostics[0].line == 1U);
    CHECK(failed->diagnostics[0].column == 18U);
    const std::vector<ed::ScriptRecordInput> inputs = {
        {"assets/scripts/player.ts", f.hashOf("scripts/player.ts")},
        {"assets/scripts/util.ts", f.hashOf("scripts/util.ts")},
    };
    CHECK(failed->inputs == inputs);
    CHECK(transpiler.runCount() == 2U);

    f.write("assets/scripts/util.ts", "export const a = 11;\n");
    f.scan();
    runPass(transpiler, f);

    CHECK(transpiler.runCount() == 4U);
    const std::optional<ed::ScriptRecord> playerAfter = f.recordOf(player);
    const std::optional<ed::ScriptRecord> utilAfter = f.recordOf(util);
    REQUIRE(playerAfter.has_value());
    REQUIRE(utilAfter.has_value());
    CHECK(playerAfter->status == ed::ScriptRecordStatus::Ok);
    CHECK(utilAfter->status == ed::ScriptRecordStatus::Ok);
}

TEST_CASE("script_transpiler: a failed script is retried when a missing file appears (ST5)") {
    ScriptFixture f;
    f.write("assets/scripts/missing.ts", "import { z } from \"./no-such-file.js\";\nexport const y = z;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid guid = f.guidOf("scripts/missing.ts");
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{
              "ERROR scripts: assets/scripts/missing.ts:1:19: error: Could not resolve \"./no-such-file.js\"",
              "INFO scripts: 0 transpiled, 1 failed, 0 up to date"});
    const std::optional<ed::ScriptRecord> failed = f.recordOf(guid);
    REQUIRE(failed.has_value());
    CHECK(failed->status == ed::ScriptRecordStatus::Failed);
    const std::size_t runsBefore = transpiler.runCount();

    f.write("assets/scripts/no-such-file.ts", "export const z = 1;\n");
    f.scan();
    runPass(transpiler, f);

    CHECK(transpiler.runCount() > runsBefore);
    const std::optional<ed::ScriptRecord> fixed = f.recordOf(guid);
    REQUIRE(fixed.has_value());
    CHECK(fixed->status == ed::ScriptRecordStatus::Ok);
}

TEST_CASE("script_transpiler: a reopen over unchanged scripts runs nothing (ST6)") {
    ScriptFixture f;
    f.write("assets/scripts/a.ts", "export const x = 1;\n");
    f.write("assets/scripts/b.ts", "export const y = 2;\n");
    f.scan();
    {
        ed::ScriptTranspiler first(configWith(AERO_TEST_ESBUILD));
        runPass(first, f);
        CHECK(first.runCount() == 2U);
    }

    f.scan();
    ed::ScriptTranspiler second(configWith(AERO_TEST_ESBUILD));
    second.noteScan(f.db, f.view());
    CHECK(second.idle());
    CHECK(second.runCount() == 0U);
    CHECK(rendered(second.takeLog()) == std::vector<std::string>{"INFO scripts: 0 transpiled, 0 failed, 2 up to date"});
}

TEST_CASE("script_transpiler: changing an imported file re-runs only its dependents (ST7)") {
    ScriptFixture f;
    f.write("assets/scripts/util.ts", UTIL_TS);
    f.write("assets/scripts/player.ts", "import { a } from \"./util\";\nexport const p = a;\n");
    f.write("assets/scripts/enemy.ts", "export const e = 3;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);
    CHECK(transpiler.runCount() == 3U);
    static_cast<void>(transpiler.takeLog());

    const Guid enemy = f.guidOf("scripts/enemy.ts");
    const std::string enemyRecord = f.artifactBytes(enemy, ".json");
    REQUIRE_FALSE(enemyRecord.empty());

    f.write("assets/scripts/util.ts", "export const a = 100;\nexport const b = 2;\n");
    f.scan();
    runPass(transpiler, f);

    CHECK(transpiler.runCount() == 5U);
    CHECK(f.artifactBytes(enemy, ".json") == enemyRecord);
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"INFO scripts: 2 transpiled, 0 failed, 1 up to date"});
}

TEST_CASE("script_transpiler: inputs outside the assets root or from a package are refused (ST8)") {
    ScriptFixture f;
    f.write("outside.ts", "export const o = 1;\n");
    f.write("assets/scripts/out.ts", "import { o } from \"../../outside\";\nexport const q = o;\n");
    f.write("node_modules/lodash/index.js", "export const c = 1;\n");
    f.write("node_modules/lodash/package.json", "{\"name\":\"lodash\",\"version\":\"1.0.0\",\"main\":\"index.js\"}\n");
    f.write("assets/scripts/pkg.ts", "import { c } from \"lodash\";\nexport const k = c;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{
              "ERROR scripts: assets/scripts/out.ts: error: 'outside.ts' is outside the assets root",
              "ERROR scripts: assets/scripts/pkg.ts: error: 'node_modules/lodash/index.js' comes from a package "
              "(node_modules); bare imports are not supported",
              "INFO scripts: 0 transpiled, 2 failed, 0 up to date"});

    const Guid out = f.guidOf("scripts/out.ts");
    const std::optional<ed::ScriptRecord> record = f.recordOf(out);
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Failed);
    CHECK_FALSE(existsAt(f.root, f.artifactPath(out, ".js")));
    const Guid pkg = f.guidOf("scripts/pkg.ts");
    CHECK_FALSE(existsAt(f.root, f.artifactPath(pkg, ".js")));
}

TEST_CASE("script_transpiler: a missing esbuild is one error per pass and writes no record (ST9)") {
    ScriptFixture f;
    f.write("assets/scripts/a.ts", "export const x = 1;\n");
    f.write("assets/scripts/b.ts", "export const y = 2;\n");
    f.scan();

    const std::string missing = f.root + "/no-such-esbuild";
    ed::ScriptTranspiler transpiler(configWith(missing));
    runPass(transpiler, f);

    const std::vector<std::string> expected = {"ERROR scripts: esbuild was not found at '" + missing + "'",
                                               "INFO scripts: 0 transpiled, 2 failed, 0 up to date"};
    CHECK(rendered(transpiler.takeLog()) == expected);
    CHECK(transpiler.runCount() == 0U);
    CHECK_FALSE(existsAt(f.root, "Library/Scripts"));

    runPass(transpiler, f);
    CHECK(rendered(transpiler.takeLog()) == expected);
    CHECK(transpiler.runCount() == 0U);
}

TEST_CASE("script_transpiler: a run past the limit is stopped and leaves the published files alone (ST10)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);
    static_cast<void>(transpiler.takeLog());

    const Guid guid = f.guidOf("scripts/player.ts");
    const std::string js = f.artifactBytes(guid, ".js");
    const std::string map = f.artifactBytes(guid, ".js.map");
    const std::string record = f.artifactBytes(guid, ".json");
    REQUIRE_FALSE(record.empty());

    f.write("assets/scripts/player.ts", "export const hp: number = 1000;\n");
    f.scan();
    transpiler.noteScan(f.db, f.view());
    transpiler.service(f.db, WAIT_DT);  // spawns
    REQUIRE_FALSE(transpiler.idle());
    transpiler.service(f.db, 31.0F);  // the one explicit injection past the 30 s limit
    pumpUntilIdle(transpiler, f.db);

    const std::string path = AERO_TEST_ESBUILD;
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"ERROR scripts: esbuild at '" + path +
                                       "' did not exit within 30 seconds while transpiling "
                                       "'assets/scripts/player.ts' and was stopped",
                                   "INFO scripts: 0 transpiled, 1 failed, 0 up to date"});
    CHECK(f.artifactBytes(guid, ".js") == js);
    CHECK(f.artifactBytes(guid, ".js.map") == map);
    CHECK(f.artifactBytes(guid, ".json") == record);
}

TEST_CASE("script_transpiler: a child that never exits is killed and reaped (ST11)") {
    if (ed::currentHostOs() == ed::HostOs::Windows) {
        MESSAGE("skipped on Windows: no scripted fake can hold a process open there");
        return;
    }
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    const std::string slow = f.root + "/slow-esbuild";
    f.write("slow-esbuild", "#!/bin/sh\nexec sleep 60\n");
    std::error_code ec;
    std::filesystem::permissions(
        pathOf(slow),
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
        std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    ed::ScriptTranspiler transpiler(configWith(slow));
    transpiler.noteScan(f.db, f.view());
    transpiler.service(f.db, WAIT_DT);  // spawns
    REQUIRE_FALSE(transpiler.idle());
    transpiler.service(f.db, 31.0F);

    const auto begin = std::chrono::steady_clock::now();
    pumpUntilIdle(transpiler, f.db);
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds{10});

    const std::vector<std::string> log = rendered(transpiler.takeLog());
    REQUIRE(log.size() == 2U);
    CHECK(log[0].find("did not exit within 30 seconds") != std::string::npos);
}

TEST_CASE("script_transpiler: a warning is logged and the script is still published (ST12)") {
    ScriptFixture f;
    f.write("assets/scripts/warn.ts", "export const o = { k: 1, k: 2 };\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const std::vector<std::string> log = rendered(transpiler.takeLog());
    CHECK(log == std::vector<std::string>{"WARN scripts: assets/scripts/warn.ts:1:26: warning: Duplicate key \"k\" "
                                          "in object literal [duplicate-object-key]",
                                          "INFO scripts: 1 transpiled, 0 failed, 0 up to date"});
    for (const std::string& line : log) {
        CHECK(line.find(":1:20:") == std::string::npos);
    }
    const std::optional<ed::ScriptRecord> record = f.recordOf(f.guidOf("scripts/warn.ts"));
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Ok);
}

TEST_CASE("script_transpiler: a C++ project resolves, hashes and spawns nothing (ST13)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptProjectView view = f.view();
    view.language = ed::ProjectLanguage::Cpp;
    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    transpiler.noteScan(f.db, view);
    pumpUntilIdle(transpiler, f.db);

    f.write("assets/scripts/player.ts", "export const hp: number = 1000;\n");
    f.scan();
    transpiler.noteScan(f.db, view);
    pumpUntilIdle(transpiler, f.db);

    CHECK(transpiler.runCount() == 0U);
    CHECK(transpiler.takeLog().empty());
    CHECK_FALSE(existsAt(f.root, "Library/Scripts"));
    CHECK(transpiler.esbuildPath().empty());
}

TEST_CASE("script_transpiler: a scan that completes during a run makes it run again (ST14)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    transpiler.noteScan(f.db, f.view());
    transpiler.service(f.db, WAIT_DT);  // spawns
    REQUIRE_FALSE(transpiler.idle());

    f.scan();  // completes before the first poll
    transpiler.noteScan(f.db, f.view());
    pumpUntilIdle(transpiler, f.db);

    CHECK(transpiler.runCount() == 2U);
    const std::optional<ed::ScriptRecord> record = f.recordOf(f.guidOf("scripts/player.ts"));
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Ok);
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"INFO scripts: 1 transpiled, 0 failed, 0 up to date"});
}

TEST_CASE("script_transpiler: only .ts scripts are queued, types-only ones included (ST15)") {
    ScriptFixture f;
    f.write("assets/a.ts", "export const a = 1;\n");
    f.write("assets/types.d.ts", "export interface T { x: number }\n");
    f.write("assets/comp.tsx", "export const c = 1;\n");
    f.write("assets/mod.mts", "export const m = 1;\n");
    f.write("assets/legacy.js", "export const l = 1;\n");
    f.write("assets/onlytypes.ts", "export interface Shape { x: number }\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    CHECK(transpiler.runCount() == 2U);
    CHECK(f.recordOf(f.guidOf("a.ts")).has_value());
    const Guid onlyTypes = f.guidOf("onlytypes.ts");
    CHECK(f.recordOf(onlyTypes).has_value());
    CHECK(existsAt(f.root, f.artifactPath(onlyTypes, ".js")));
    CHECK(namesIn(f.root, "Library/Scripts").size() == 6U);
    for (const std::string_view other : {"types.d.ts", "comp.tsx", "mod.mts", "legacy.js"}) {
        CHECK_FALSE(existsAt(f.root, f.artifactPath(f.guidOf(other), ".json")));
    }
}

TEST_CASE("script_transpiler: a deleted artifact is restored by the next scan (ST16)") {
    ScriptFixture f;
    f.write("assets/scripts/player.ts", "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);
    static_cast<void>(transpiler.takeLog());

    const Guid guid = f.guidOf("scripts/player.ts");
    const std::string js = f.artifactBytes(guid, ".js");
    REQUIRE_FALSE(js.empty());
    std::error_code ec;
    std::filesystem::remove(pathOf(f.root) / pathOf(f.artifactPath(guid, ".js")), ec);
    REQUIRE_FALSE(ec);

    f.scan();
    runPass(transpiler, f);

    CHECK(transpiler.runCount() == 2U);
    CHECK(f.artifactBytes(guid, ".js") == js);
    CHECK(rendered(transpiler.takeLog()) ==
          std::vector<std::string>{"INFO scripts: 1 transpiled, 0 failed, 0 up to date"});
}

TEST_CASE("script_transpiler: the output does not depend on where the project lives (ST17)") {
    const Guid fixedGuid{0x1234567890abcdefULL, 0x0fedcba098765432ULL};
    ScriptFixture shallow;
    ScriptFixture deep("nested/deeper/still/project");

    std::vector<std::string> js;
    std::vector<std::string> map;
    for (ScriptFixture* fixture : {&shallow, &deep}) {
        fixture->write("assets/scripts/player.ts" + std::string(ed::ASSET_META_SUFFIX), ed::writeMetaText(fixedGuid));
        fixture->write("assets/scripts/player.ts", PLAYER_TS);
        fixture->write("assets/scripts/util.ts", UTIL_TS);
        fixture->write("assets/shared/math.ts", MATH_TS);
        fixture->scan();
        REQUIRE(fixture->guidOf("scripts/player.ts") == fixedGuid);

        ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
        runPass(transpiler, *fixture);
        js.push_back(fixture->artifactBytes(fixedGuid, ".js"));
        map.push_back(fixture->artifactBytes(fixedGuid, ".js.map"));
    }
    REQUIRE_FALSE(js[0].empty());
    CHECK(js[0] == js[1]);
    CHECK(map[0] == map[1]);
}

TEST_CASE("script_transpiler: a path with a space and a non-ASCII letter transpiles (ST18)") {
    ScriptFixture f;
    const std::string relative =
        "scripts dir/pl\xC3\xA5"
        "yer.ts";
    f.write("assets/" + relative, "export const hp: number = 100;\n");
    f.scan();

    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const std::optional<ed::ScriptRecord> record = f.recordOf(f.guidOf(relative));
    REQUIRE(record.has_value());
    CHECK(record->status == ed::ScriptRecordStatus::Ok);
    CHECK(record->script == "assets/" + relative);
}

namespace {

[[nodiscard]] std::string strippedLines(std::string_view text) {
    std::string out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t newline = text.find('\n', at);
        const std::string_view line =
            newline == std::string_view::npos ? text.substr(at) : text.substr(at, newline - at);
        const std::size_t comment = line.find("//");
        out.append(comment == std::string_view::npos ? line : line.substr(0, comment));
        out.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        at = newline + 1;
    }
    return out;
}

[[nodiscard]] std::string sourceText(const std::string& directory, std::string_view relative) {
    const ed::FileReadResult read = ed::readTextFile(directory + "/" + std::string(relative));
    REQUIRE_MESSAGE(read.text.has_value(), directory, "/", relative);
    return *read.text;
}

}  // namespace

TEST_CASE("script_transpiler: the sources keep to the process and logging rules (ST19)") {
    const std::string srcDir = AERO_EDITOR_SRC_DIR;
    const std::string includeDir = AERO_EDITOR_INCLUDE_DIR;

    // (a) none of the three TUs logs: the one logging site is EditorApp::drainScriptLog.
    const std::string token = std::string("AERO_") + "LOG_";
    for (const std::string_view file : {"esbuild_tool.cpp", "script_transpiler.cpp", "tool_process.cpp"}) {
        CHECK(strippedLines(sourceText(srcDir, file)).find(token) == std::string::npos);
    }

    // (b) SDL's process-lifecycle symbols appear in tool_process.cpp ONLY, in the raw text of every file.
    const std::string symbols[] = {std::string("SDL_") + "CreateProcess", std::string("SDL_") + "WaitProcess",
                                   std::string("SDL_") + "KillProcess", std::string("SDL_") + "DestroyProcess"};
    std::size_t scanned = 0;
    for (const std::string& root : {srcDir, includeDir}) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pathOf(root), ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            ++scanned;
            const std::string name = entry.path().filename().string();
            const std::string text = readPath(entry.path());
            for (const std::string& symbol : symbols) {
                if (text.find(symbol) != std::string::npos) {
                    INFO(name, " names ", symbol);
                    CHECK(name == "tool_process.cpp");
                }
            }
        }
    }
    CHECK(scanned > 100U);

    // (c) the one wait is non-blocking: `false` is its second argument.
    const std::string process = strippedLines(sourceText(srcDir, "tool_process.cpp"));
    const std::string waitCall = std::string("SDL_") + "WaitProcess(";
    std::size_t calls = 0;
    for (std::size_t at = process.find(waitCall); at != std::string::npos; at = process.find(waitCall, at + 1U)) {
        ++calls;
        const std::size_t comma = process.find(',', at);
        REQUIRE(comma != std::string::npos);
        std::size_t from = comma + 1U;
        while (from < process.size() && process[from] == ' ') {
            ++from;
        }
        CHECK(process.compare(from, 5, "false") == 0);
    }
    CHECK(calls == 1U);

    // (d) no thread, lock, atomic, future or condition variable in either new TU.
    const std::string banned[] = {std::string("<") + "thread>", std::string("<") + "mutex>",
                                  std::string("<") + "atomic>", std::string("<") + "future>",
                                  std::string("<") + "condition_variable>"};
    for (const std::string_view file : {"esbuild_tool.cpp", "script_transpiler.cpp"}) {
        const std::string text = sourceText(srcDir, file);
        for (const std::string& header : banned) {
            INFO(file, " names ", header);
            CHECK(text.find(header) == std::string::npos);
        }
    }
}

namespace {

namespace sc = engine::script;

// The probe: String(globalThis.<name>) read back through the same VM.
[[nodiscard]] std::string probe(sc::Vm& vm, std::string_view name) {
    const std::string source = "String(globalThis." + std::string(name) + ")";
    const sc::EvalResult result = vm.evaluateScript(source, "probe.js");
    REQUIRE(result.status == sc::EvalStatus::Ok);
    return result.completion;
}

// Library/Scripts mounted at the res:// root, so a published script is "res://<guid>.js".
struct ScriptVm {
    engine::VirtualFileSystem files;
    sc::Vm vm;

    ScriptVm(const std::string& root, const sc::VmConfig& config) {
        files.mount(std::make_unique<engine::DirectoryBackend>(root + "/Library/Scripts"));
        sc::VmCreateResult created = sc::Vm::create(config, files);
        REQUIRE(created.status == sc::VmCreateStatus::Ok);
        vm = std::move(created.vm);
    }
    ScriptVm(const ScriptVm&) = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;
    ScriptVm(ScriptVm&&) = delete;
    ScriptVm& operator=(ScriptVm&&) = delete;
    ~ScriptVm() = default;
};

[[nodiscard]] std::string modulePath(Guid guid) { return "res://" + engine::formatGuid(guid) + ".js"; }

// True when `js` still holds a `using` or `await using` declaration (R21: the VM's unlowered loop can outlast its
// budget for good, so this runs before anything evaluates).
[[nodiscard]] bool hasUsingDeclaration(const std::string& js) {
    static const std::regex pattern(R"((^|[^A-Za-z0-9_$])(await\s+)?using\s+[A-Za-z_$])");
    return std::regex_search(js, pattern);
}

}  // namespace

TEST_CASE("script_transpiler: a transpiled script evaluates in the engine's VM (ST20)") {
    ScriptFixture f;
    f.write("assets/scripts/hello.ts",
            "const n: number = 6 * 7;\n(globalThis as any).__r = \"hello \" + n;\nexport {};\n");
    f.scan();
    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid guid = f.guidOf("scripts/hello.ts");
    ScriptVm host(f.root, sc::VmConfig{});
    const sc::EvalResult result = host.vm.evaluateModule(modulePath(guid));
    CHECK(result.status == sc::EvalStatus::Ok);
    CHECK(probe(host.vm, "__r") == "hello 42");
}

TEST_CASE("script_transpiler: a bundled script with imports evaluates unmodified (ST21)") {
    ScriptFixture f;
    f.write("assets/scripts/util.ts", UTIL_TS);
    f.write("assets/shared/math.ts", MATH_TS);
    f.write("assets/scripts/player.ts", std::string(PLAYER_TS) + "(globalThis as any).__r = \"player \" + result;\n");
    f.scan();
    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid guid = f.guidOf("scripts/player.ts");
    ScriptVm host(f.root, sc::VmConfig{});
    const sc::EvalResult result = host.vm.evaluateModule(modulePath(guid));
    CHECK(result.status == sc::EvalStatus::Ok);
    CHECK(probe(host.vm, "__r") == "player 23");
}

TEST_CASE("script_transpiler: the using lowering reaches the VM (ST22)") {
    ScriptFixture f;
    f.write("assets/scripts/r21.ts",
            "const res = { [Symbol.dispose]() { throw new Error(\"dispose\"); } };\n"
            "for (;;) {\n"
            "    try {\n"
            "        using r = res;\n"
            "        throw 1;\n"
            "    } catch (e) {}\n"
            "}\n");
    f.write("assets/scripts/awaitusing.ts",
            "const res = {\n"
            "    async [Symbol.asyncDispose]() {\n"
            "        (globalThis as any).__d = \"disposed\";\n"
            "    },\n"
            "};\n"
            "{\n"
            "    await using r = res;\n"
            "}\n"
            "export {};\n");
    f.scan();
    ed::ScriptTranspiler transpiler(configWith(AERO_TEST_ESBUILD));
    runPass(transpiler, f);

    const Guid r21 = f.guidOf("scripts/r21.ts");
    const Guid awaiting = f.guidOf("scripts/awaitusing.ts");
    const std::string r21Js = f.artifactBytes(r21, ".js");
    const std::string awaitingJs = f.artifactBytes(awaiting, ".js");
    REQUIRE_FALSE(r21Js.empty());
    REQUIRE_FALSE(awaitingJs.empty());
    REQUIRE_FALSE(hasUsingDeclaration(r21Js));
    REQUIRE_FALSE(hasUsingDeclaration(awaitingJs));

    {
        ScriptVm host(f.root, sc::VmConfig{.callTimeBudget = std::chrono::milliseconds{100}});
        CHECK(host.vm.evaluateModule(modulePath(r21)).status == sc::EvalStatus::Interrupted);
    }
    {
        ScriptVm host(f.root, sc::VmConfig{});
        CHECK(host.vm.evaluateModule(modulePath(awaiting)).status == sc::EvalStatus::Ok);
        CHECK(probe(host.vm, "__d") == "disposed");
    }
}
