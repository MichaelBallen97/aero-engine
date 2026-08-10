// tests/editor/blender_service_test.cpp -- task 3.2.4: the IMPURE half of the Blender CLI
// integration. Tier-0, UNGATED, no GPU, no window, no ImGui context.
//
// NO CI LANE HAS BLENDER (R2). Every case here therefore spawns `cmake`, which exists on every runner
// by definition, through the AERO_TEST_CMAKE_COMMAND compile definition -- and every recipe used is a
// documented `cmake -E` mode measured in the plan's §G-5. That is what makes the process layer and
// the whole state machine covered on all three lanes while "Blender itself behaves as measured" stays
// the only uncovered claim.
//
// NO TEST IN THIS FILE SLEEPS. Every timing is an injected deltaSeconds; a poll loop is bounded by an
// iteration count, never by a wall clock (AC-18).
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Every case must be PRESENT and PASSING in all three build
// configurations (AC-43).
//
// blender_process.hpp is SRC-PRIVATE and is reached by an explicit relative path rather than by
// putting editor/src on this target's include path. That is deliberate and narrower: a target-wide
// include directory would let ANY test TU pull in ANY src-private header -- thumbnail_store.hpp
// (stb_image), import_details_panel.hpp (ImGui), gltf_import.hpp (fastgltf) -- and the editor's
// "public headers stay ImGui-free BY FILE PLACEMENT" rule is held by exactly that discipline. One
// explicit path in one TU keeps each such reach a visible, reviewable act.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>  // task 3.2.4: the session cases drive a real scan
#include <aero/editor/blender_service.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/model_import_session.hpp>  // task 3.2.4: the .blend arm, end to end
#include <aero/editor/text_file.hpp>

#include "../../editor/src/blender_process.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using engine::ContentHash;
using engine::formatGuid;
using engine::Guid;
using engine::parseContentHash;
using engine::parseGuid;
using engine::editor::BLENDER_SCRIPT_FILE_NAME;
using engine::editor::BLENDER_TIMEOUT_SECONDS;
using engine::editor::BlenderEnv;
using engine::editor::blenderExportScriptText;
using engine::editor::BlenderFailure;
using engine::editor::BlenderProcess;
using engine::editor::BlenderService;
using engine::editor::BlenderState;
using engine::editor::HostOs;
using engine::editor::parseToolPrefs;
using engine::editor::PROCESS_FORCE_KILL_SECONDS;
using engine::editor::ProcessState;
using engine::editor::readBlenderEnv;
using engine::editor::readTextFile;
using engine::editor::ToolPrefs;
using engine::editor::writeToolPrefsText;

namespace {

// A unique temp directory that removes itself (and its contents) on destruction -- the NINTH copy of
// this shape (text_file_test.cpp's precedent): scaffolding is copied, the assertion is shared.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_blender_service_test_" + std::to_string(++counter));
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

void writeBytes(std::string_view absolutePathUtf8, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePathUtf8), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string readAll(std::string_view absolutePathUtf8) {
    std::ifstream in(pathOf(absolutePathUtf8), std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The one and only program these cases spawn. It is a PATH THAT MAY CONTAIN SPACES on Windows
// ("C:/Program Files/CMake/bin/cmake.exe") and it goes into argv[0] as ONE entry with no quoting of
// our own -- which is itself AC-12's property, exercised on every lane rather than only asserted.
constexpr std::string_view CMAKE_COMMAND = AERO_TEST_CMAKE_COMMAND;

[[nodiscard]] std::vector<std::string> cmakeArgs(std::vector<std::string> tail) {
    std::vector<std::string> args;
    args.reserve(tail.size() + 2);
    args.emplace_back(CMAKE_COMMAND);
    args.emplace_back("-E");
    for (std::string& entry : tail) {
        args.push_back(std::move(entry));
    }
    return args;
}

// A BOUNDED poll loop. It never SLEEPS and never consults a clock -- it polls until the child reports
// Exited or the iteration budget runs out, and exhausting the budget is a test failure rather than a
// hang.
//
// It YIELDS between polls, and that is not decoration. MEASURED on this machine: a bare
// `cmake -E true` costs 47 000 - 114 000 iterations of a pure spin, a margin of only 17-42x against a
// 2 000 000 budget -- and a loaded runner ate that margin for real, producing exactly one transient
// Release failure that vanished on re-run. A spin loop competes with the very child it is waiting
// for; yielding hands the core back instead, which collapses the iteration count and turns the margin
// into orders of magnitude. A yield is NOT a sleep: it makes no timing assumption, and AC-18's
// "no test sleeps" rule is about the TIMEOUT being driven by injected deltaSeconds, which it still is.
constexpr int MAX_POLL_ITERATIONS = 5000000;

struct PollOutcome {
    ProcessState state = ProcessState::Running;
    int exitCode = 0;
    int iterations = 0;
};

[[nodiscard]] PollOutcome pollUntilExit(BlenderProcess& process) {
    PollOutcome outcome;
    for (outcome.iterations = 1; outcome.iterations <= MAX_POLL_ITERATIONS; ++outcome.iterations) {
        outcome.state = process.poll(outcome.exitCode);
        if (outcome.state != ProcessState::Running) {
            return outcome;
        }
        std::this_thread::yield();
    }
    return outcome;
}

// ---- fake-tool support ---------------------------------------------------------------------
//
// The version-probe and export cases need a program whose OUTPUT and EXIT CODE we choose. There is no
// portable way to synthesize one: SDL spawns through CreateProcessW on Windows, which cannot execute
// a .bat or .cmd file, and `cmake -E` has no mode that prints arbitrary text when the only argument
// it is handed is `--version` (buildVersionArgs is exactly two entries, by contract).
//
// So the cases split three ways, and the split is stated rather than hidden:
//   * everything that needs no output at all -- resolution, ToolMissing, the spawn-failure path, the
//     preferences file, the request-drop rules, and both exit-NON-zero export rows -- runs on EVERY
//     lane, driven by the real `cmake` executable (which exits non-zero when handed Blender's argv);
//   * the version BANNER cases and the exit-ZERO export rows need a scripted fake and are therefore
//     POSIX-only, guarded below and reported as such;
//   * nothing anywhere sleeps, and nothing reads or writes a real environment variable.
#if !defined(_WIN32)
void writeExecutable(std::string_view absolutePathUtf8, std::string_view text) {
    writeBytes(absolutePathUtf8, text);
    std::error_code ec;
    std::filesystem::permissions(pathOf(absolutePathUtf8), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    REQUIRE_FALSE(ec);
}

// A fake `blender --version`: prints `banner` and exits with `exitCode`.
void makeFakeVersionTool(std::string_view absolutePathUtf8, std::string_view banner, int exitCode) {
    std::string script = "#!/bin/sh\nprintf '%s\\n' \"";
    script += banner;
    script += "\"\nexit ";
    script += std::to_string(exitCode);
    script += "\n";
    writeExecutable(absolutePathUtf8, script);
}

// Every fake below answers `--version` FIRST and separately. The service probes before it converts,
// so a fake that treated the probe like an export would run the export body with unset arguments --
// which is how an earlier draft of this file passed BS24 for entirely the wrong reason (a failed
// redirection that sh reports and then walks past, leaving exit 0 and an empty banner).
constexpr std::string_view FAKE_VERSION_BRANCH =
    "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then printf '%s\\n' 'Blender 5.2.0 LTS'; exit 0; fi\n";

// A fake Blender export. buildExportArgs' shape is FIXED (AC-11), so the out path is argument 12 and
// the status path is argument 14 -- which is exactly what makes this three lines instead of a parser.
// An empty `statusJson` writes no status file at all (E23's row); an empty `glbBytes` writes no
// artifact.
void makeFakeExportTool(std::string_view absolutePathUtf8, std::string_view glbBytes, std::string_view statusJson,
                        int exitCode) {
    std::string script(FAKE_VERSION_BRANCH);
    if (!glbBytes.empty()) {
        script += "printf '%s' '";
        script += glbBytes;
        script += "' > \"${12}\"\n";
    }
    if (!statusJson.empty()) {
        script += "printf '%s' '";
        script += statusJson;
        script += "' > \"${14}\"\n";
    }
    script += "exit ";
    script += std::to_string(exitCode);
    script += "\n";
    writeExecutable(absolutePathUtf8, script);
}

// Answers the probe immediately and then holds an EXPORT open, so cancel and timeout have something
// to act on. The CHILD sleeps; the TEST never does.
void makeFakeSlowExportTool(std::string_view absolutePathUtf8) {
    writeExecutable(absolutePathUtf8, std::string(FAKE_VERSION_BRANCH) + "sleep 30\n");
}

// task 3.2.4, the session cases: a fake export that COPIES a real, pre-staged GLB into place. The
// `printf '%s'` form above cannot be reused for a GLB -- a GLB is BINARY and carries NUL bytes, which
// no shell single-quoted string can hold -- so the artifact is written by the TEST, in C++, with
// buildGlb, and the fake merely moves it where Blender would have written it. That keeps AC-42 intact
// (no binary file is committed) while still producing a genuinely parseable artifact.
void makeFakeCopyExportTool(std::string_view absolutePathUtf8, std::string_view stagedGlbPath,
                            std::string_view statusJson, int exitCode) {
    std::string script(FAKE_VERSION_BRANCH);
    if (!stagedGlbPath.empty()) {
        script += "cp '";
        script += stagedGlbPath;
        script += "' \"${12}\"\n";
    }
    if (!statusJson.empty()) {
        script += "printf '%s' '";
        script += statusJson;
        script += "' > \"${14}\"\n";
    }
    script += "exit ";
    script += std::to_string(exitCode);
    script += "\n";
    writeExecutable(absolutePathUtf8, script);
}
#endif

// An env that resolves to exactly ONE candidate: the override, alone (AC-3). Every service case uses
// this rather than the machine's real environment, which is what keeps them hermetic.
[[nodiscard]] BlenderEnv overrideEnv(std::string_view binary) {
    BlenderEnv env;
    env.overridePath = std::string(binary);
    return env;
}

// Drives poll() to a settled state without sleeping and without a wall clock. `dt` is INJECTED, so a
// case that wants a timeout supplies a large one and a case that does not supplies zero (AC-18).
void pollUntilSettled(BlenderService& service, float dt = 0.0F) {
    for (int i = 0; i < MAX_POLL_ITERATIONS; ++i) {
        const BlenderState before = service.state();
        service.poll(dt);
        const BlenderState now = service.state();
        if (now != BlenderState::Probing && now != BlenderState::Converting) {
            return;
        }
        // Probing/Converting with a child that has not moved is the ordinary case; keep polling.
        static_cast<void>(before);
        std::this_thread::yield();
    }
    FAIL("poll() never settled within the iteration budget");
}

[[nodiscard]] Guid testGuid() { return *parseGuid("0123456789abcdef0123456789abcdef"); }
[[nodiscard]] ContentHash testHash() { return *parseContentHash("fedcba9876543210fedcba9876543210"); }

// ---- task 3.2.4: the SESSION cases (BS31-BS45) ---------------------------------------------------
//
// These drive the REAL ModelImportSession against a REAL AssetDatabase over a real scratch project --
// the .blend arm's cache, staleness, artifact and identity rules end to end, not the service in
// isolation. The service's own transition table is BS11-BS30 above.

// model_import_test.cpp's own buildGlb, copied rather than shared -- this suite's standing rule
// (scaffolding is copied, the ASSERTION is shared). AC-42: the GLB is assembled here, in memory, from
// a JSON string; NO BINARY FILE IS COMMITTED TO THE REPOSITORY.
void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::string buildGlb(std::string_view json) {
    std::string paddedJson(json);
    while (paddedJson.size() % 4U != 0U) {
        paddedJson += ' ';  // JSON chunk padding is SPACE, per the GLB container spec
    }
    const auto jsonChunkLength = static_cast<std::uint32_t>(paddedJson.size());
    std::string glb;
    glb += "glTF";       // magic
    appendU32(glb, 2U);  // version
    appendU32(glb, 12U + 8U + jsonChunkLength);
    appendU32(glb, jsonChunkLength);
    appendU32(glb, 0x4E4F534AU);  // 'JSON'
    glb += paddedJson;
    return glb;
}

constexpr std::string_view MINIMAL_GLB_JSON = R"({"asset":{"version":"2.0"}})";
// AC-44's DISCRIMINATOR: a GLB that DOES declare an external image URI. An ordinary GLB names none, so
// routing the artifact through the two-pass driver would produce an identical result and seed S26
// would discriminate nothing -- this fixture is the whole reason it does.
constexpr std::string_view EXTERNAL_URI_GLB_JSON = R"({"asset":{"version":"2.0"},"images":[{"uri":"wood.png"}]})";

// A scratch PROJECT: <root>/assets/<name>.blend, scanned into a real AssetDatabase, with Library/
// deliberately OUTSIDE the assets tree exactly as a real project has it.
struct BlendProject {
    std::string projectRoot;
    std::string assetsRoot;
    std::string exportDir;
    engine::editor::AssetDatabase db;
    Guid guid;

    [[nodiscard]] std::string artifactPath() const { return exportDir + '/' + formatGuid(guid) + ".glb"; }
    [[nodiscard]] std::string provenancePath() const { return exportDir + '/' + formatGuid(guid) + ".json"; }
};

void makeDirectories(std::string_view absolutePathUtf8) {
    std::error_code ec;
    std::filesystem::create_directories(pathOf(absolutePathUtf8), ec);
    REQUIRE_FALSE(ec);
}

[[nodiscard]] BlendProject makeBlendProject(const TempDir& tmp, unsigned seed, std::string_view blendBytes = "opaque",
                                            std::uint64_t hashBudgetBytes = engine::editor::MAX_HASH_BYTES_PER_SCAN) {
    BlendProject project;
    project.projectRoot = tmp.utf8();
    project.assetsRoot = tmp.join("assets");
    project.exportDir = project.projectRoot + '/' + std::string(engine::editor::ASSET_CACHE_DIR_NAME) + '/' +
                        std::string(engine::editor::BLENDER_EXPORT_DIR_NAME);
    makeDirectories(project.assetsRoot);
    writeBytes(project.assetsRoot + "/statue.blend", blendBytes);
    engine::GuidGenerator generator(seed);
    project.db.rescan(project.projectRoot, project.assetsRoot, generator, hashBudgetBytes);
    const std::optional<Guid> guid = project.db.guidForPath("statue.blend");
    REQUIRE(guid.has_value());
    project.guid = *guid;
    return project;
}

// The settings fingerprint, RE-DERIVED from the two public primitives rather than by calling the
// production helper -- so a change to what goes into it reddens these cases instead of moving with them.
[[nodiscard]] std::string fingerprintOfDefaults() {
    const std::string text = engine::editor::writeMetaText(Guid{}, engine::editor::ImportSettings{});
    return engine::formatContentHash(engine::hashBytes(std::as_bytes(std::span<const char>(text))));
}

// A provenance record that MATCHES what the arm computes for `project` -- the four compared fields,
// with the two informational ones deliberately set to values nothing compares (E24/BT57's property,
// re-exercised end to end).
[[nodiscard]] engine::editor::ExportProvenance matchingProvenance(const BlendProject& project) {
    engine::editor::ExportProvenance record;
    record.guid = project.guid;
    record.sourcePath = "somewhere/else/statue.blend";  // INFORMATIONAL -- never compared
    record.blenderPath = "/nonexistent/blender";        // INFORMATIONAL -- never compared
    record.blenderVersion = "5.2.0 LTS";
    record.scriptVersion = engine::editor::BLENDER_SCRIPT_VERSION;
    record.settingsFingerprint = fingerprintOfDefaults();
    const engine::editor::AssetRecord* const asset = project.db.findByPath("statue.blend");
    REQUIRE(asset != nullptr);
    record.sourceHash = asset->contentHash;
    record.artifactBytes = 0;
    return record;
}

// Stage a cache HIT: the provenance record plus a real, parseable artifact.
void stageCacheHit(const BlendProject& project, std::string_view glbJson = MINIMAL_GLB_JSON) {
    makeDirectories(project.exportDir);
    writeBytes(project.artifactPath(), buildGlb(glbJson));
    writeBytes(project.provenancePath(), engine::editor::writeExportProvenanceText(matchingProvenance(project)));
}

// Every regular file under `root`, with its size -- the "not one byte changed" observable BS31 needs.
// Sizes rather than mtimes: a filesystem whose mtime granularity is a second (FAT32, some network
// mounts) would make an mtime comparison a coin flip, while a size comparison plus the explicit
// file-SET comparison catches a write, a truncation, an addition and a deletion alike.
[[nodiscard]] std::vector<std::string> listWithSizes(std::string_view rootUtf8) {
    std::vector<std::string> entries;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(pathOf(rootUtf8), ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::u8string name = it->path().u8string();
        entries.emplace_back(std::string(reinterpret_cast<const char*>(name.data()), name.size()) + '|' +
                             std::to_string(static_cast<std::uint64_t>(it->file_size(ec))));
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The process layer (BS1-BS10)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_service: a successful run reports Running then Exited(0) (BS1, AC-15)") {
    const TempDir tmp;
    BlenderProcess process;
    CHECK_FALSE(process.running());

    const std::vector<std::string> args = cmakeArgs({"true"});
    const std::string failure = process.start(args, tmp.utf8(), tmp.join("run.log"));
    REQUIRE(failure.empty());
    CHECK(process.running());

    const PollOutcome outcome = pollUntilExit(process);
    REQUIRE(outcome.state == ProcessState::Exited);
    CHECK(outcome.exitCode == 0);
    CHECK_FALSE(process.running());
    // The loop terminated on its own budget rather than being cut short -- proof that a non-blocking
    // poll actually converges rather than that the assertion above was reached by a blocking wait.
    CHECK(outcome.iterations <= MAX_POLL_ITERATIONS);
}

TEST_CASE("blender_service: a failing run reports Exited(1), not a spawn failure (BS2, AC-16)") {
    const TempDir tmp;
    BlenderProcess process;
    const std::vector<std::string> args = cmakeArgs({"false"});
    REQUIRE(process.start(args, tmp.utf8(), tmp.join("run.log")).empty());

    const PollOutcome outcome = pollUntilExit(process);
    REQUIRE(outcome.state == ProcessState::Exited);
    // A NON-ZERO exit that is NOT a spawn failure. Conflating the two is what makes "Blender rejected
    // your file" indistinguishable from "Blender is not installed", which is the whole of AC-31.
    CHECK(outcome.exitCode == 1);
    CHECK(outcome.exitCode != 0);
    CHECK_FALSE(process.running());
}

TEST_CASE("blender_service: a long-running child can be killed, and the handle destroys cleanly (BS3, AC-17)") {
    const TempDir tmp;
    {
        BlenderProcess process;
        const std::vector<std::string> args = cmakeArgs({"sleep", "30"});
        REQUIRE(process.start(args, tmp.utf8(), tmp.join("run.log")).empty());
        CHECK(process.running());

        process.kill(false);
        const PollOutcome outcome = pollUntilExit(process);
        // "no longer running", NEVER "received a signal": force == false is SIGTERM on POSIX and
        // TerminateProcess on Windows, which has no graceful signal (R5), so only the OUTCOME is
        // portable. The exit code is deliberately not asserted for the same reason.
        REQUIRE(outcome.state == ProcessState::Exited);
        CHECK_FALSE(process.running());
    }
    // The scope closed with the object destroyed. Under ASan on both Debug lanes, a leaked SDL_Process
    // or a leaked redirect SDL_IOStream would be reported here rather than passing silently.
    CHECK(std::filesystem::exists(pathOf(tmp.join("run.log"))));
}

TEST_CASE("blender_service: a nonexistent binary is a spawn failure with a non-empty reason (BS4, AC-21)") {
    const TempDir tmp;
    BlenderProcess process;
    const std::vector<std::string> args = {tmp.join("no-such-tool-anywhere"), "--version"};

    const std::string failure = process.start(args, tmp.utf8(), tmp.join("run.log"));
    // NON-EMPTY: the panel shows this text, so an empty one would be a failure state with nothing to
    // say. It does not abort, does not throw, and does not leave the object claiming a live child.
    CHECK_FALSE(failure.empty());
    CHECK_FALSE(process.running());

    int exitCode = -12345;
    CHECK(process.poll(exitCode) == ProcessState::SpawnFailed);
    CHECK(exitCode == -12345);  // untouched -- there was no process to have an exit code
}

TEST_CASE("blender_service: stdout AND stderr both land in the redirect file, in ONE spawn (BS5, AC-19)") {
    // `cmake -E cat <existing> <missing>` is the only single-command way to make a child write to BOTH
    // streams: the existing file's contents go to stdout, and "no such file or directory (ignoring)"
    // goes to stderr. Without this recipe, seed S34 (leaving STDERR_TO_STDOUT unset) would be a
    // NON-DISCRIMINATOR, because every other `cmake -E` mode writes to stdout only.
    const TempDir tmp;
    const std::string existing = tmp.join("present.txt");
    const std::string missing = tmp.join("absent-on-purpose.txt");
    writeBytes(existing, "AERO_STDOUT_MARKER\n");

    BlenderProcess process;
    const std::string logPath = tmp.join("both.log");
    const std::vector<std::string> args = cmakeArgs({"cat", existing, missing});
    REQUIRE(process.start(args, tmp.utf8(), logPath).empty());
    const PollOutcome outcome = pollUntilExit(process);
    REQUIRE(outcome.state == ProcessState::Exited);

    const std::string log = readAll(logPath);
    CHECK_FALSE(log.empty());
    // The STDOUT half...
    CHECK(log.find("AERO_STDOUT_MARKER") != std::string::npos);
    // ...and the STDERR half, which is the assertion that actually discriminates. "the log is
    // non-empty" would pass with stderr going to the terminal.
    CHECK(log.find("absent-on-purpose.txt") != std::string::npos);
}

TEST_CASE("blender_service: the child's working directory is the one we set (BS6, AC-20)") {
    // `cmake -E touch <relative-name>` creates the file wherever the CHILD's working directory is. The
    // discriminator is not "the spawn worked" but "it landed HERE and nowhere else" -- an export tool
    // that wrote a relative path into the project root or the assets root would be exactly the
    // INV-A1 violation four tasks have kept out of the assets tree.
    const TempDir working;
    const TempDir elsewhere;
    BlenderProcess process;
    const std::vector<std::string> args = cmakeArgs({"touch", "cwdprobe.txt"});
    REQUIRE(process.start(args, working.utf8(), working.join("run.log")).empty());
    REQUIRE(pollUntilExit(process).state == ProcessState::Exited);

    CHECK(std::filesystem::exists(pathOf(working.join("cwdprobe.txt"))));
    CHECK_FALSE(std::filesystem::exists(pathOf(elsewhere.join("cwdprobe.txt"))));
    CHECK_FALSE(std::filesystem::exists(pathOf("cwdprobe.txt")));  // and not in the TEST's own cwd
}

TEST_CASE("blender_service: the redirect stream is closed before start() returns (BS7, AC-19)") {
    // Proven by OVERWRITING the same log path in an immediately following run. On Windows that is a
    // hard requirement -- an open handle blocks the replace -- and everywhere it proves the stream is
    // ours to close and we closed it. The second run's own output must be all that remains.
    const TempDir tmp;
    const std::string logPath = tmp.join("shared.log");
    const std::string first = tmp.join("first.txt");
    const std::string second = tmp.join("second.txt");
    writeBytes(first, "FIRST_RUN_MARKER\n");
    writeBytes(second, "SECOND_RUN_MARKER\n");

    {
        BlenderProcess process;
        const std::vector<std::string> args = cmakeArgs({"cat", first});
        REQUIRE(process.start(args, tmp.utf8(), logPath).empty());
        REQUIRE(pollUntilExit(process).state == ProcessState::Exited);
    }
    CHECK(readAll(logPath).find("FIRST_RUN_MARKER") != std::string::npos);

    {
        BlenderProcess process;
        const std::vector<std::string> args = cmakeArgs({"cat", second});
        REQUIRE(process.start(args, tmp.utf8(), logPath).empty());  // "wb" TRUNCATES the same path
        REQUIRE(pollUntilExit(process).state == ProcessState::Exited);
    }
    const std::string log = readAll(logPath);
    CHECK(log.find("SECOND_RUN_MARKER") != std::string::npos);
    CHECK(log.find("FIRST_RUN_MARKER") == std::string::npos);  // truncated, not appended to
}

TEST_CASE("blender_service: destroying a STILL-RUNNING process leaves nothing behind (BS8, E9)") {
    const TempDir tmp;
    const std::string logPath = tmp.join("orphan.log");
    {
        BlenderProcess process;
        const std::vector<std::string> args = cmakeArgs({"sleep", "30"});
        REQUIRE(process.start(args, tmp.utf8(), logPath).empty());
        CHECK(process.running());
        // Deliberately NOT killed and NOT polled: the destructor is the only thing that runs. SDL's
        // own destroy does NOT stop a process, so a destructor that merely destroyed the handle would
        // leave an orphan -- which is what makes "quit mid-conversion" safe or not.
    }
    // The child really did start, so this is not a vacuous scope.
    CHECK(std::filesystem::exists(pathOf(logPath)));
    // And the redirect handle was released: re-opening the SAME path for truncating write succeeds.
    // On Windows a leaked handle makes this fail outright; elsewhere it is a weaker but still real
    // statement. The SDL_Process handle itself is covered by ASan on both Debug lanes.
    writeBytes(logPath, "reopened after the destructor\n");
    CHECK(readAll(logPath) == "reopened after the destructor\n");
}

TEST_CASE("blender_service: a live process can be move-constructed and move-assigned (BS9)") {
    const TempDir tmp;
    const std::vector<std::string> args = cmakeArgs({"sleep", "30"});

    BlenderProcess source;
    REQUIRE(source.start(args, tmp.utf8(), tmp.join("a.log")).empty());
    REQUIRE(source.running());

    BlenderProcess moved(std::move(source));
    CHECK(moved.running());
    CHECK_FALSE(source.running());  // NOLINT(bugprone-use-after-move) -- the moved-from state IS the assertion

    BlenderProcess target;
    REQUIRE(target.start(cmakeArgs({"sleep", "30"}), tmp.utf8(), tmp.join("b.log")).empty());
    REQUIRE(target.running());
    target = std::move(moved);  // target's OWN child must be killed and destroyed, never leaked
    CHECK(target.running());
    CHECK_FALSE(moved.running());  // NOLINT(bugprone-use-after-move)

    target.kill(true);
    CHECK(pollUntilExit(target).state == ProcessState::Exited);
}

TEST_CASE("blender_service: running() is false before start() and after Exited (BS10)") {
    const TempDir tmp;
    BlenderProcess process;
    CHECK_FALSE(process.running());  // before start()

    int exitCode = 0;
    CHECK(process.poll(exitCode) == ProcessState::SpawnFailed);  // nothing started == nothing to poll

    REQUIRE(process.start(cmakeArgs({"true"}), tmp.utf8(), tmp.join("run.log")).empty());
    REQUIRE(pollUntilExit(process).state == ProcessState::Exited);
    CHECK_FALSE(process.running());  // after Exited

    // Polling again after the exit re-reports the SAME verdict rather than re-waiting on a reaped
    // child -- the service polls every tick and must not depend on catching the transition exactly.
    int again = -1;
    CHECK(process.poll(again) == ProcessState::Exited);
    CHECK(again == 0);
}

// ---------------------------------------------------------------------------------------------
// Resolve + probe (BS11-BS20)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_service: readBlenderEnv splits PATH per host and takes the override from the FILE (BS11)") {
    const TempDir tmp;
    const std::string prefsPath = tmp.join("editor_tools.json");
    ToolPrefs prefs;
    prefs.blenderPath = "/from/the/preferences/file";
    REQUIRE(engine::editor::writeTextFileAtomic(prefsPath, writeToolPrefsText(prefs)).empty());

    bool corrupt = true;
    const BlenderEnv windowsEnv = readBlenderEnv(HostOs::Windows, prefsPath, corrupt);
    CHECK_FALSE(corrupt);
    // The override comes from the PREFERENCES FILE, never from the machine's own environment -- which
    // is what makes every candidate case above hermetic.
    CHECK(windowsEnv.overridePath == "/from/the/preferences/file");

    const BlenderEnv posixEnv = readBlenderEnv(HostOs::Linux, prefsPath, corrupt);
    CHECK(posixEnv.overridePath == "/from/the/preferences/file");

    // The SEPARATOR is the discriminator, and asserting it needs both hosts from ONE process. A real
    // POSIX PATH is colon-separated, so splitting it on ';' yields ONE entry that still contains ':',
    // while splitting it on ':' yields several that contain none.
    if (posixEnv.pathEntries.size() <= 1) {
        MESSAGE("skipped: this process has no multi-entry PATH to split");
    } else {
#if defined(_WIN32)
        MESSAGE("skipped on Windows: PATH is ';'-separated there, so the roles of the two hosts swap");
#else
        CHECK(windowsEnv.pathEntries.size() == 1);
        CHECK(windowsEnv.pathEntries[0].find(':') != std::string::npos);
        CHECK(posixEnv.pathEntries.size() > 1);
        for (const std::string& entry : posixEnv.pathEntries) {
            CHECK(entry.find(':') == std::string::npos);
        }
#endif
    }
}

TEST_CASE("blender_service: no candidate on disk is ToolMissing with a retained list and ZERO spawns (BS12, AC-30)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(tmp.join("definitely-not-here")), tmp.utf8());

    CHECK(service.state() == BlenderState::ToolMissing);
    CHECK_FALSE(service.searchedPaths().empty());  // the panel LISTS every path it looked in (AC-30)
    CHECK(service.binaryPath().empty());
    CHECK_FALSE(service.message().empty());
    // INV-B15: resolution stats candidates and spawns NOTHING. Both counters, because "zero
    // processes" is two numbers in this design, not one.
    CHECK(service.probeRunCount() == 0);
    CHECK(service.exportRunCount() == 0);
}

TEST_CASE("blender_service: a candidate that exists is Probing, and STILL zero spawns (BS13, INV-B15)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());

    CHECK(service.state() == BlenderState::Probing);
    CHECK(service.binaryPath() == std::string(CMAKE_COMMAND));
    // resolve() has stat'ed a candidate and chosen it, and has spawned NOTHING. The probe is poll()'s
    // job and poll()'s alone.
    CHECK(service.probeRunCount() == 0);
    CHECK(service.exportRunCount() == 0);
}

TEST_CASE("blender_service: a parseable supported version reaches Ready (BS14, AC-7)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: CreateProcessW cannot execute a script, so no fake tool can print a banner there");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeVersionTool(fake, "Blender 5.2.0 LTS", 0);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    REQUIRE(service.state() == BlenderState::Probing);
    pollUntilSettled(service);

    CHECK(service.state() == BlenderState::Ready);
    CHECK(service.probeRunCount() == 1);  // poll() spawned it, and exactly once
    CHECK(service.exportRunCount() == 0);
    REQUIRE(service.version().has_value());
    CHECK(service.version()->major == 5);
    CHECK(service.version()->minor == 2);
    // The STRING is what the provenance record carries and what bpy.app.version_string reports from
    // inside the script -- the banner prefix is stripped, the rest is kept verbatim.
    CHECK(service.versionString() == "5.2.0 LTS");
    CHECK(service.message().empty());  // Supported records no warning
#endif
}

TEST_CASE("blender_service: an UNPARSEABLE version still reaches Ready (BS15, D14/E4)") {
    // Portable on every lane: the real cmake executable prints "cmake version 3.x.y", which
    // parseBlenderVersion refuses -- and D14 ATTEMPTS rather than refuses, because an unreadable
    // banner is far likelier to be a locale, a build suffix or a fork than a Blender from 2011.
    // Seed S8 inverts this and must redden here as well as at BT28.
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    REQUIRE(service.state() == BlenderState::Probing);
    pollUntilSettled(service);

    CHECK(service.state() == BlenderState::Ready);
    CHECK(service.probeRunCount() == 1);
    CHECK_FALSE(service.version().has_value());  // genuinely unparseable, not a lucky parse
    CHECK_FALSE(service.logTail().empty());      // and the probe really did produce output
}

TEST_CASE("blender_service: a REFUSED version is ToolUnusable, never Ready (BS16, AC-7/D14)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can print a banner there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeVersionTool(fake, "Blender 2.79b", 0);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);

    // Below 2.80 there is nothing to attempt: bpy.ops.export_scene.gltf did not exist. Seed S7 returns
    // Ready here instead and must redden.
    CHECK(service.state() == BlenderState::ToolUnusable);
    CHECK_FALSE(service.message().empty());
    CHECK(service.probeRunCount() == 1);
    CHECK(service.exportRunCount() == 0);
#endif
}

TEST_CASE("blender_service: a NON-ZERO probe exit is ToolUnusable (BS17)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can control an exit code there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeVersionTool(fake, "Blender 5.2.0 LTS", 3);  // a perfectly good banner, a bad exit code

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);

    // The EXIT CODE decides, not the output: a tool that prints a version and then fails is not one
    // this editor should hand a .blend to.
    CHECK(service.state() == BlenderState::ToolUnusable);
    CHECK(service.message().find('3') != std::string::npos);
    CHECK(service.probeRunCount() == 1);
#endif
}

TEST_CASE("blender_service: a SPAWN FAILURE during the probe is ToolUnusable (BS18, AC-21)") {
    // A file that EXISTS, is accepted by isExecutableFile, and yet CANNOT be spawned on ANY platform.
    // The shebang names an interpreter that does not exist, so POSIX reports ENOENT out of the exec;
    // on Windows the file is not a PE image, so CreateProcessW refuses it. A file of mere GARBAGE does
    // NOT work here and an earlier draft used one: macOS happily runs a header-less executable file
    // through /bin/sh, so the spawn SUCCEEDS and the case measured the wrong branch.
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    writeBytes(fake, "#!/aero/no/such/interpreter\nexit 0\n");
#if !defined(_WIN32)
    std::error_code ec;
    std::filesystem::permissions(pathOf(fake), std::filesystem::perms::owner_all, std::filesystem::perm_options::add,
                                 ec);
    REQUIRE_FALSE(ec);
#endif

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    REQUIRE(service.state() == BlenderState::Probing);
    service.poll(0.0F);

    CHECK(service.state() == BlenderState::ToolUnusable);
    CHECK_FALSE(service.message().empty());  // carries SDL's own reason; never a bare state
    // An ATTEMPT counts, whether or not it started: AC-22 forbids the attempt, not only the success.
    CHECK(service.probeRunCount() == 1);
    CHECK(service.exportRunCount() == 0);
}

TEST_CASE("blender_service: setOverridePath writes the prefs file THERE and resets to Unknown (BS19, AC-25)") {
    const TempDir tmp;
    const TempDir realPrefsWouldBeElsewhere;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    REQUIRE(service.state() == BlenderState::Probing);

    const std::string prefsPath = tmp.join("editor_tools.json");
    CHECK(service.setOverridePath("/chosen/by/the/user", prefsPath).empty());

    // Written to the GIVEN path, and to nothing else. This is the one writeTextFileAtomic call in the
    // whole tree built from the tool-preferences path (AC-25), and a test that let it fall through to
    // the machine-wide default would rewrite the developer's own file -- 2.6.1's BLOCKING-2 (R10).
    REQUIRE(std::filesystem::exists(pathOf(prefsPath)));
    CHECK_FALSE(std::filesystem::exists(pathOf(realPrefsWouldBeElsewhere.join("editor_tools.json"))));
    const engine::editor::FileReadResult written = readTextFile(prefsPath);
    REQUIRE(written.text.has_value());
    const std::optional<ToolPrefs> parsed = parseToolPrefs(*written.text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->blenderPath == "/chosen/by/the/user");

    // Back to Unknown so the NEXT resolve() re-runs against the new preference -- and still no spawn.
    CHECK(service.state() == BlenderState::Unknown);
    CHECK(service.probeRunCount() == 0);
    CHECK(service.exportRunCount() == 0);
}

TEST_CASE("blender_service: a MISSING prefs file is silent; a CORRUPT one raises the out-flag (BS20, AC-8)") {
    const TempDir tmp;
    bool corrupt = true;
    const BlenderEnv missing = readBlenderEnv(HostOs::Linux, tmp.join("not-written-yet.json"), corrupt);
    // SILENT: a machine that has never chosen a Blender must not be warned at on every resolve.
    CHECK_FALSE(corrupt);
    CHECK(missing.overridePath.empty());

    const std::string damaged = tmp.join("damaged.json");
    writeBytes(damaged, R"({"version": 1, "blenderPath": )");
    const BlenderEnv broken = readBlenderEnv(HostOs::Linux, damaged, corrupt);
    // A file that EXISTS but does not parse is empty preferences PLUS the flag -- an OUT PARAMETER,
    // never a log line, because this TU never logs (INV-B10). editor_app.cpp emits the one WARN.
    CHECK(corrupt);
    CHECK(broken.overridePath.empty());
}

// ---------------------------------------------------------------------------------------------
// Conversion transitions (BS21-BS30)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_service: requestConversion in Ready starts exactly one export (BS21)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);

    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    CHECK(service.exportRunCount() == 0);  // RECORDS ONLY -- requestConversion spawns nothing (INV-B15)

    service.poll(0.0F);
    CHECK(service.state() == BlenderState::Converting);
    CHECK(service.exportRunCount() == 1);
    // The script is written into the export directory before the run, and the log path is the one the
    // panel offers the user.
    const std::string scriptPath = tmp.join(std::string(BLENDER_SCRIPT_FILE_NAME));
    REQUIRE(std::filesystem::exists(pathOf(scriptPath)));
    // Its CONTENT, not merely its existence: the run must be handed the exact compile-time constant,
    // byte for byte, or the RNA filter and export_yup's absence stop meaning anything (BT39-BT43).
    CHECK(readAll(scriptPath) == std::string(blenderExportScriptText()));
    CHECK_FALSE(service.logPath().empty());
}

TEST_CASE("blender_service: requestConversion while ToolMissing NEVER spawns (BS22, AC-30)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(tmp.join("definitely-not-here")), tmp.utf8());
    REQUIRE(service.state() == BlenderState::ToolMissing);

    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    for (int i = 0; i < 8; ++i) {
        service.poll(0.016F);
    }
    // The request is DROPPED, not queued: pressing a button when there is no Blender must produce a
    // message, never a spawn that cannot work. The state is unchanged and BOTH counters stay at zero.
    CHECK(service.state() == BlenderState::ToolMissing);
    CHECK(service.exportRunCount() == 0);
    CHECK(service.probeRunCount() == 0);
    CHECK_FALSE(std::filesystem::exists(pathOf(tmp.join(std::string(BLENDER_SCRIPT_FILE_NAME)))));
}

TEST_CASE("blender_service: a second requestConversion while Converting is a NO-OP (BS23, E15)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);

    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    service.poll(0.0F);
    REQUIRE(service.state() == BlenderState::Converting);
    REQUIRE(service.exportRunCount() == 1);

    // Refused at the REQUEST, not at the drain: refusing at the drain would make this property depend
    // on whether the child happened to exit within the same tick, which passes on a fast machine and
    // fails on a slow one.
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    CHECK(service.exportRunCount() == 1);
    pollUntilSettled(service);
    CHECK(service.exportRunCount() == 1);  // and still one after the run finished
}

TEST_CASE("blender_service: exit 0 with an ok status reaches Converted (BS24)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can exit zero for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeExportTool(fake, "glTFDATA", R"({"ok": true, "blender": "5.2.0 LTS", "bytes": 8})", 0);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    pollUntilSettled(service);

    // Converted means "the run exited, its status file exists and reports ok: true" -- and NOTHING
    // more. The artifact has not been read or imported; only ModelImportSession can do that, which is
    // why the service alone can never reach Imported.
    CHECK(service.state() == BlenderState::Converted);
    CHECK(service.failure() == BlenderFailure::None);
    CHECK(service.exportRunCount() == 1);
    CHECK_FALSE(service.artifactPath().empty());
    CHECK(service.artifactPath().find(formatGuid(testGuid())) != std::string::npos);
    CHECK(std::filesystem::exists(pathOf(service.artifactPath())));
#endif
}

TEST_CASE("blender_service: a non-zero exit with NO status file is SourceRejected (BS25, AC-31)") {
    // Portable: handed Blender's fifteen arguments, the real cmake exits non-zero and writes no status
    // file -- exactly the shape MEASURED for a .blend Blender cannot open (F4), where it aborts BEFORE
    // running the export script. That is what makes "non-zero AND no status" a reliable discriminator.
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    pollUntilSettled(service);

    CHECK(service.state() == BlenderState::Failed);
    CHECK(service.failure() == BlenderFailure::SourceRejected);
    CHECK_FALSE(service.message().empty());
    // The message must be SPECIFIC: it names the file Blender could not open, so the user is not told
    // "the export failed" when the export never started.
    CHECK(service.message().find("chair.blend") != std::string::npos);
    CHECK(service.message().find("could not open") != std::string::npos);
    CHECK(service.artifactPath().empty());
}

TEST_CASE("blender_service: a non-zero exit WITH a status file lets the status message win (BS26, AC-32)") {
    const TempDir tmp;
    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(CMAKE_COMMAND), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);

    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    service.poll(0.0F);
    REQUIRE(service.state() == BlenderState::Converting);
    // The status file the script would have written, placed where the run will look for it. cmake
    // never touches it, so it survives to be read at the exit -- which is precisely the case where a
    // run failed AFTER the script had already reported why.
    writeBytes(tmp.join(formatGuid(testGuid()) + ".json"),
               R"({"ok": false, "error": "AERO_STATUS_MESSAGE_WINS", "bytes": 0})");
    pollUntilSettled(service);

    CHECK(service.state() == BlenderState::Failed);
    CHECK(service.failure() == BlenderFailure::ExportFailed);
    // The STATUS FILE, when it exists, is the authority -- not the exit code, and not a generic
    // sentence this tree made up.
    CHECK(service.message() == "AERO_STATUS_MESSAGE_WINS");
    CHECK(service.failure() != BlenderFailure::SourceRejected);
}

TEST_CASE("blender_service: exit 0 with ok:false is ExportFailed carrying that message (BS27, AC-33)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can exit zero for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeExportTool(fake, "", R"({"ok": false, "error": "AERO_EXPORTER_SAID_NO", "bytes": 0})", 0);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    pollUntilSettled(service);

    // The exporter reported its own failure through the status file rather than through an exit code,
    // which is the row the script's try/except exists to make reachable at all.
    CHECK(service.state() == BlenderState::Failed);
    CHECK(service.failure() == BlenderFailure::ExportFailed);
    CHECK(service.message() == "AERO_EXPORTER_SAID_NO");
    CHECK(service.artifactPath().empty());
#endif
}

TEST_CASE("blender_service: exit 0 with NO status file names the missing file (BS28, E23)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can exit zero for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeExportTool(fake, "glTFDATA", "", 0);  // writes an artifact but NO status file

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    pollUntilSettled(service);

    // A .glb exists on disk, and it is STILL a failure. Declaring Converted from the artifact's mere
    // existence would bless a half-written file from a killed run -- seed S36's whole point.
    CHECK(service.state() == BlenderState::Failed);
    CHECK(service.failure() == BlenderFailure::ExportFailed);
    CHECK(service.message().find(".json") != std::string::npos);  // it NAMES the file that is missing
    CHECK(service.artifactPath().empty());
#endif
}

TEST_CASE("blender_service: a run past the timeout is killed and reported TimedOut (BS29, AC-18)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can hold a process open for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeSlowExportTool(fake);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);
    REQUIRE(service.state() == BlenderState::Ready);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    service.poll(0.0F);
    REQUIRE(service.state() == BlenderState::Converting);

    // NO TEST SLEEPS. The clock is INJECTED: one tick claiming more than the whole timeout is what
    // makes a five-minute rule assertable in milliseconds.
    service.poll(BLENDER_TIMEOUT_SECONDS + 1.0F);
    pollUntilSettled(service, PROCESS_FORCE_KILL_SECONDS + 1.0F);

    CHECK(service.state() == BlenderState::Failed);
    // KILLED, not merely reported: seed S16 reports without killing and must redden on the state, and
    // the process is provably gone because poll() settled at all.
    CHECK(service.failure() == BlenderFailure::TimedOut);
    CHECK_FALSE(service.message().empty());
    CHECK(service.artifactPath().empty());
#endif
}

TEST_CASE("blender_service: cancel() kills the run and reports Cancelled (BS30)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can hold a process open for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const std::string fake = tmp.join("blender");
    makeFakeSlowExportTool(fake);

    BlenderService service;
    service.resolve(HostOs::Linux, overrideEnv(fake), tmp.utf8());
    pollUntilSettled(service);
    service.requestConversion(testGuid(), tmp.join("chair.blend"), testHash(), tmp.utf8());
    service.poll(0.0F);
    REQUIRE(service.state() == BlenderState::Converting);

    service.cancel();  // GRACEFUL at once; poll() owns the escalation
    pollUntilSettled(service, PROCESS_FORCE_KILL_SECONDS + 1.0F);

    CHECK(service.state() == BlenderState::Failed);
    CHECK(service.failure() == BlenderFailure::Cancelled);
    // A cancelled run is told apart from a timed-out one and from a rejected source: three distinct
    // reasons, three distinct messages (AC-36).
    CHECK(service.failure() != BlenderFailure::TimedOut);
    CHECK_FALSE(service.message().empty());
    CHECK(service.artifactPath().empty());
#endif
}

// ---------------------------------------------------------------------------------------------
// Cache, staleness and provenance, END TO END through ModelImportSession (BS31-BS40)
// ---------------------------------------------------------------------------------------------

using engine::editor::ModelImportSession;
using engine::editor::SessionState;

TEST_CASE(
    "blender_service: a valid provenance record serves the model with ZERO processes and ZERO bytes "
    "written (BS31, task 3.2.4 AC-22)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 301);
    stageCacheHit(project);

    // The baseline is taken AFTER staging, so anything the selection writes shows up as a difference.
    const std::vector<std::string> before = listWithSizes(project.projectRoot);
    REQUIRE_FALSE(before.empty());

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db, 0.016F);

    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == engine::editor::ImportStatus::Ok);
    // BOTH counters (§A-9): "zero processes" as ONE number is unsatisfiable while the version probe is
    // itself a process, so the observable is split and AC-22 asserts both halves.
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
    // Nothing was resolved either: no environment variable read, no candidate path stat'ed.
    CHECK(session.blender().state() == BlenderState::Unknown);
    CHECK(session.blender().searchedPaths().empty());

    // NOT ONE BYTE under the project changed -- a full listing, so a write ANYWHERE (a re-written
    // script, an artifact under the assets root, a touched .meta) is caught, not only a write at the
    // path this case happens to expect (seeds S12 and S32).
    CHECK(listWithSizes(project.projectRoot) == before);

    // Ten further ticks are ten early returns: still no process, still no write.
    for (int i = 0; i < 10; ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
    }
    CHECK(session.importCount() == 1);
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
    CHECK(listWithSizes(project.projectRoot) == before);
}

TEST_CASE("blender_service: a changed sourceHash invalidates the cached artifact (BS32, task 3.2.4 AC-23)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 302);
    engine::editor::ExportProvenance stale = matchingProvenance(project);
    stale.sourceHash = testHash();  // the ONE axis flipped
    makeDirectories(project.exportDir);
    writeBytes(project.artifactPath(), buildGlb(MINIMAL_GLB_JSON));
    writeBytes(project.provenancePath(), engine::editor::writeExportProvenanceText(stale));

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.blender().exportRunCount() == 0);  // a miss OFFERS a conversion; it never starts one
}

TEST_CASE("blender_service: a changed scriptVersion invalidates the cached artifact (BS33, task 3.2.4 AC-23)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 303);
    engine::editor::ExportProvenance stale = matchingProvenance(project);
    stale.scriptVersion = engine::editor::BLENDER_SCRIPT_VERSION + 1U;
    makeDirectories(project.exportDir);
    writeBytes(project.artifactPath(), buildGlb(MINIMAL_GLB_JSON));
    writeBytes(project.provenancePath(), engine::editor::writeExportProvenanceText(stale));

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
}

TEST_CASE("blender_service: a changed settingsFingerprint invalidates the cached artifact (BS34, task 3.2.4 AC-23)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 304);
    engine::editor::ExportProvenance stale = matchingProvenance(project);
    stale.settingsFingerprint = "00000000000000000000000000000000";
    makeDirectories(project.exportDir);
    writeBytes(project.artifactPath(), buildGlb(MINIMAL_GLB_JSON));
    writeBytes(project.provenancePath(), engine::editor::writeExportProvenanceText(stale));

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
}

TEST_CASE(
    "blender_service: blenderVersion is compared ONLY once a version is known -- an unprobed session "
    "hits, a probed one with a different version misses (BS35, task 3.2.4 AC-23/§A-9, seed S28)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 305);
    stageCacheHit(project);  // records blenderVersion "5.2.0 LTS"

    // Half 1 -- NOTHING has probed, so versionString() is "" and the field is NOT compared. This is
    // the half that makes AC-22's "zero processes" literally true.
    ModelImportSession unprobed;
    unprobed.setTarget("statue.blend", project.db.generation());
    unprobed.service(project.assetsRoot, project.db);
    CHECK(unprobed.state() == SessionState::Imported);
    CHECK(unprobed.blender().probeRunCount() == 0);

    // Half 2 -- a session that HAS probed compares it. `cmake --version` prints its own banner, which
    // is emphatically not "5.2.0 LTS", so the SAME on-disk record is now stale.
    ModelImportSession probed;
    probed.setTarget("statue.blend", project.db.generation());
    probed.service(project.assetsRoot, project.db);
    REQUIRE(probed.state() == SessionState::Imported);
    probed.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(CMAKE_COMMAND), project.exportDir);
    for (int i = 0; i < MAX_POLL_ITERATIONS; ++i) {
        probed.service(project.assetsRoot, project.db, 0.016F);
        if (probed.blender().state() != BlenderState::Probing) {
            break;
        }
        std::this_thread::yield();
    }
    REQUIRE(probed.blender().state() == BlenderState::Ready);
    REQUIRE_FALSE(probed.blender().versionString().empty());
    REQUIRE(probed.blender().versionString() != "5.2.0 LTS");

    // Re-select (a new generation) so the arm re-runs the probe against the now-known version.
    probed.setTarget("statue.blend", project.db.generation() + 1U);
    probed.service(project.assetsRoot, project.db, 0.016F);
    CHECK(probed.state() == SessionState::NeedsConversion);
}

TEST_CASE(
    "blender_service: a provenance file with a wrong envelope version is a MISS, never a repair "
    "(BS36, task 3.2.4 AC-23/E10)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 306);
    stageCacheHit(project);
    // Overwrite with a document whose four compared fields are all correct but whose ENVELOPE is not.
    std::string text = engine::editor::writeExportProvenanceText(matchingProvenance(project));
    const std::size_t versionAt = text.find("\"version\": 1");
    REQUIRE(versionAt != std::string::npos);
    text.replace(versionAt, std::string_view("\"version\": 1").size(), "\"version\": 2");
    writeBytes(project.provenancePath(), text);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
    // The file is LEFT EXACTLY AS IT IS: derived data is disposable, and a miss never repairs.
    CHECK(readAll(project.provenancePath()) == text);
}

TEST_CASE("blender_service: no provenance file at all is a MISS, even with an artifact present (BS37)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 307);
    makeDirectories(project.exportDir);
    writeBytes(project.artifactPath(), buildGlb(MINIMAL_GLB_JSON));  // artifact, but no record

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.importCount() == 0);  // the artifact was never even read
}

TEST_CASE(
    "blender_service: a valid provenance record whose ARTIFACT is missing is a MISS, not a failure "
    "(BS38, task 3.2.4 E11)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 308);
    stageCacheHit(project);
    std::error_code ec;
    std::filesystem::remove(pathOf(project.artifactPath()), ec);
    REQUIRE_FALSE(ec);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.importCount() == 1);  // the read WAS attempted -- that is how the miss was learned
    // Offering a conversion, not showing a stale failure the user cannot act on.
    CHECK(session.state() != SessionState::ConversionFailed);
}

TEST_CASE(
    "blender_service: a cancelled run writes NO provenance and leaves a previous record BYTE-IDENTICAL "
    "(BS39, task 3.2.4 AC-24, seed S11)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can hold a process open for Blender's argv there (see BS14)");
#else
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 309);
    stageCacheHit(project);
    const std::string recordBefore = readAll(project.provenancePath());
    REQUIRE_FALSE(recordBefore.empty());

    const std::string fake = tmp.join("blender");
    makeFakeSlowExportTool(fake);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    REQUIRE(session.state() == SessionState::Imported);  // the cache hit, first

    session.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(fake), project.exportDir);
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.blender().state() == BlenderState::Probing; ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    REQUIRE(session.blender().state() == BlenderState::Ready);

    session.requestConversion();
    session.service(project.assetsRoot, project.db, 0.016F);
    REQUIRE(session.state() == SessionState::Converting);
    session.service(project.assetsRoot, project.db, 0.016F);  // the tick that spawns
    REQUIRE(session.blender().exportRunCount() == 1);

    session.cancelConversion();
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.state() == SessionState::Converting; ++i) {
        session.service(project.assetsRoot, project.db, PROCESS_FORCE_KILL_SECONDS + 1.0F);
        std::this_thread::yield();
    }
    CHECK(session.state() == SessionState::ConversionFailed);
    CHECK(session.blender().failure() == BlenderFailure::Cancelled);
    // THE ASSERTION THIS CASE EXISTS FOR: the previous record survives BYTE-IDENTICAL. Seed S11 writes
    // the provenance before the import succeeds and must redden here.
    CHECK(readAll(project.provenancePath()) == recordBefore);
#endif
}

TEST_CASE(
    "blender_service: a successful run is imported, cached, and hit on the NEXT selection with no "
    "further spawn (BS40, task 3.2.4 AC-24/AC-22, seed S15)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can produce exit 0 plus a status file for Blender's argv (see BS14)");
#else
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 310);
    const std::string staged = tmp.join("staged.glb");
    writeBytes(staged, buildGlb(MINIMAL_GLB_JSON));
    const std::string fake = tmp.join("blender");
    makeFakeCopyExportTool(fake, staged, R"({"ok": true, "blender": "5.2.0 LTS", "error": "", "bytes": 1})", 0);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    REQUIRE(session.state() == SessionState::NeedsConversion);

    session.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(fake), project.exportDir);
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.blender().state() == BlenderState::Probing; ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    REQUIRE(session.blender().state() == BlenderState::Ready);
    CHECK(session.blender().version().has_value());  // the fake prints a real Blender banner

    session.requestConversion();
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.state() != SessionState::Imported &&
                    session.state() != SessionState::ConversionFailed;
         ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    REQUIRE(session.state() == SessionState::Imported);
    CHECK(session.blender().exportRunCount() == 1);
    CHECK(session.result().status == engine::editor::ImportStatus::Ok);

    // AC-24: the provenance record exists ONLY NOW, after a successful import, and it round-trips.
    const std::string record = readAll(project.provenancePath());
    REQUIRE_FALSE(record.empty());
    const std::optional<engine::editor::ExportProvenance> parsed = engine::editor::parseExportProvenance(record);
    REQUIRE(parsed.has_value());
    CHECK(parsed->guid == project.guid);
    CHECK(parsed->scriptVersion == engine::editor::BLENDER_SCRIPT_VERSION);
    CHECK(parsed->blenderVersion == "5.2.0 LTS");
    CHECK(parsed->artifactBytes > 0);

    // The SECOND selection hits the cache: exportRunCount does NOT move (seed S15 drops the
    // short-circuit and must redden exactly here).
    const std::size_t exportsAfterFirst = session.blender().exportRunCount();
    ModelImportSession second;
    second.setTarget("statue.blend", project.db.generation());
    second.service(project.assetsRoot, project.db, 0.016F);
    CHECK(second.state() == SessionState::Imported);
    CHECK(second.blender().exportRunCount() == 0);  // a FRESH session: it never ran anything
    CHECK(second.blender().probeRunCount() == 0);
    CHECK(session.blender().exportRunCount() == exportsAfterFirst);
#endif
}

// ---------------------------------------------------------------------------------------------
// The artifact contract (BS41-BS45)
// ---------------------------------------------------------------------------------------------

TEST_CASE(
    "blender_service: an artifact that DOES declare an external URI imports with exactly one warning "
    "naming the count, and the list is CLEARED (BS41, task 3.2.4 AC-44, seeds S26/S27)") {
    // THE DISCRIMINATOR FOR AC-44, and the reason its fixture is authored the way it is: an ORDINARY
    // GLB names no external URI at all, so routing the artifact through the two-pass driver would
    // produce an identical result and seed S26 would discriminate nothing. "A GLB is self-contained" is
    // a property of Blender's EXPORTER, not of the glTF specification -- this case is what turns that
    // structural assumption into a rendered, assertable fact.
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 311);
    stageCacheHit(project, EXTERNAL_URI_GLB_JSON);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);

    REQUIRE(session.state() == SessionState::Imported);  // the GEOMETRY is real; this is not a failure
    CHECK(session.result().status == engine::editor::ImportStatus::Ok);
    // The list is CLEARED, so nothing downstream can mistake "wood.png" for an assets-relative path --
    // it names a file in Library/, which has no assets-relative directory at all.
    CHECK(session.result().externalUris.empty());
    // EXACTLY ONE warning, and it names the COUNT (seed S27 removes it and must redden both halves).
    REQUIRE(session.result().warnings.size() == 1);
    CHECK(session.result().warnings[0].find("1 external file(s)") != std::string::npos);
    CHECK(session.result().warnings[0].find("self-contained") != std::string::npos);
    CHECK(session.result().warningTotal == 1);

    // The control: the SAME staging with an ordinary GLB produces NO warning at all, which is what
    // proves the warning is a statement about the artifact rather than an unconditional line.
    const TempDir plainTmp;
    const BlendProject plain = makeBlendProject(plainTmp, 312);
    stageCacheHit(plain, MINIMAL_GLB_JSON);
    ModelImportSession plainSession;
    plainSession.setTarget("statue.blend", plain.db.generation());
    plainSession.service(plain.assetsRoot, plain.db);
    REQUIRE(plainSession.state() == SessionState::Imported);
    CHECK(plainSession.result().warnings.empty());
    CHECK(plainSession.result().warningTotal == 0);
}

TEST_CASE(
    "blender_service: an artifact above MAX_ARTIFACT_BYTES is refused from its SIZE ALONE, never read "
    "(BS42, task 3.2.4 AC-35/E12)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 313);
    stageCacheHit(project);
    // Sparse: resize_file allocates no blocks on APFS/ext4/NTFS, so this costs nothing to create and
    // would cost 256 MiB of RSS to READ -- which is exactly the point. readFileBytes decides from
    // file_size BEFORE opening.
    std::error_code ec;
    std::filesystem::resize_file(pathOf(project.artifactPath()), engine::editor::MAX_ARTIFACT_BYTES + 1U, ec);
    REQUIRE_FALSE(ec);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);

    CHECK(session.state() == SessionState::NeedsConversion);  // offered again, not shown as a model
    CHECK(session.importCount() == 1);                        // the attempt happened
    // `size` is filled EVEN ON REFUSAL, which is what lets the panel show a byte count instead of a
    // partial read presented as the whole file.
    CHECK(session.fileSizeBytes() == engine::editor::MAX_ARTIFACT_BYTES + 1U);
}

TEST_CASE(
    "blender_service: an unparseable artifact after a SUCCESSFUL run is ArtifactUnusable, carrying the "
    "importer's own status label and message (BS43, task 3.2.4 AC-34)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can produce exit 0 plus a status file for Blender's argv (see BS14)");
#else
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 314);
    const std::string staged = tmp.join("staged.glb");
    writeBytes(staged, "this is not a GLB at all");  // exit 0, status ok:true, but a junk artifact
    const std::string fake = tmp.join("blender");
    makeFakeCopyExportTool(fake, staged, R"({"ok": true, "blender": "5.2.0 LTS", "error": "", "bytes": 1})", 0);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    session.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(fake), project.exportDir);
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.blender().state() == BlenderState::Probing; ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    REQUIRE(session.blender().state() == BlenderState::Ready);

    session.requestConversion();
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.state() != SessionState::Imported &&
                    session.state() != SessionState::ConversionFailed;
         ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    CHECK(session.state() == SessionState::ConversionFailed);
    // The SESSION composed the message and handed it back, because only the session imports -- the
    // service alone cannot reach this state (§A-11).
    CHECK(session.blender().state() == BlenderState::Failed);
    CHECK(session.blender().failure() == BlenderFailure::ArtifactUnusable);
    CHECK_FALSE(session.blender().message().empty());
    CHECK(session.blender().message().find(engine::editor::importStatusLabel(session.result().status)) !=
          std::string::npos);
    // AC-24: a run whose artifact is unusable writes NO PROVENANCE RECORD. The file at that path is
    // NOT empty -- it holds the run's own STATUS document, which shares the path DELIBERATELY (the
    // provenance record OVERWRITES it on success). So the assertion is about what the document IS, not
    // about whether one exists: a half-finished run leaves a document parseExportProvenance REJECTS,
    // which is precisely the "no valid cache entry" answer the next selection needs (E10).
    const std::string atPath = readAll(project.provenancePath());
    REQUIRE_FALSE(atPath.empty());
    CHECK_FALSE(engine::editor::parseExportProvenance(atPath).has_value());
    CHECK(engine::editor::parseExportStatus(atPath).has_value());  // it IS the status document
#endif
}

TEST_CASE(
    "blender_service: a record with no usable content hash is never served from cache and is never "
    "cached after a run (BS44, task 3.2.4 §D-10 step 2)") {
    const TempDir tmp;
    // A hash budget of ZERO makes every record NotHashed -- 3.1.2's own defaulted-parameter test seam,
    // reused. An unhashed record has NO CACHE KEY at all.
    const BlendProject project = makeBlendProject(tmp, 315, "opaque", /*hashBudgetBytes=*/0U);
    const engine::editor::AssetRecord* const record = project.db.findByPath("statue.blend");
    REQUIRE(record != nullptr);
    REQUIRE(record->change == engine::editor::ImportChange::NotHashed);
    stageCacheHit(project);  // a PERFECTLY matching record on disk

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    // The artifact is treated as STALE rather than compared against a hash that means nothing.
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.importCount() == 0);   // it was not even read
    CHECK(session.targetHasIdentity());  // the IDENTITY is fine -- only the hash is unavailable
}

TEST_CASE(
    "blender_service: a nil-GUID .blend spawns nothing and writes nothing at all (BS45, task 3.2.4 "
    "AC-27, seed S29)") {
    const TempDir tmp;
    const std::string assets = tmp.join("assets");
    makeDirectories(assets);
    writeBytes(assets + "/statue.blend", "opaque");
    writeBytes(assets + "/statue.blend.meta", "{ not valid json");
    engine::editor::AssetDatabase db;
    engine::GuidGenerator generator(316);
    db.rescan(tmp.utf8(), assets, generator);
    const engine::editor::AssetRecord* const record = db.findByPath("statue.blend");
    REQUIRE(record != nullptr);
    REQUIRE_FALSE(record->guid.valid());

    const std::vector<std::string> before = listWithSizes(tmp.utf8());

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db, 0.016F);
    session.requestConversion();
    session.service(assets, db, 0.016F);

    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK_FALSE(session.targetHasIdentity());
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
    CHECK(session.importCount() == 0);
    CHECK(listWithSizes(tmp.utf8()) == before);  // ZERO bytes written, anywhere under the project
}

// ---------------------------------------------------------------------------------------------
// BS46-BS47: the two coverage gaps the sabotage matrix found (seeds S12 and S32)
// ---------------------------------------------------------------------------------------------
//
// Both exist because BS31 -- the case the plan predicted would catch S12 and S32 -- is a PURE CACHE
// HIT, and neither seed's code is on that path: startExport() never runs, so an assets-root write and
// an unconditional script write are both unreachable from it. Both seeds ran GREEN against the whole
// suite, which is a finding, not a pass. These two close it on the path that actually converts.
//
// Neither needs a scripted fake, so both run on EVERY lane: `cmake` handed Blender's own argv exits
// non-zero without writing a status file, which is a perfectly good FAILED conversion -- and
// startExport() (which is where both seeds live) has already done all of its writing by then.

// Drive the session until the Blender service leaves a transient state. Bounded, yielding, never a
// clock and never a sleep.
void sessionUntilBlenderSettled(ModelImportSession& session, std::string_view assetsRoot,
                                const engine::editor::AssetDatabase& db) {
    for (int i = 0; i < MAX_POLL_ITERATIONS; ++i) {
        session.service(assetsRoot, db, 0.016F);
        const BlenderState state = session.blender().state();
        if (state != BlenderState::Probing && state != BlenderState::Converting) {
            return;
        }
        std::this_thread::yield();
    }
    FAIL("the Blender service never settled within the iteration budget");
}

TEST_CASE(
    "blender_service: a REAL conversion writes NOTHING under the assets root (BS46, task 3.2.4 "
    "INV-B8/AC-25, seed S12)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 317);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    REQUIRE(session.state() == SessionState::NeedsConversion);
    session.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(CMAKE_COMMAND), project.exportDir);
    sessionUntilBlenderSettled(session, project.assetsRoot, project.db);
    REQUIRE(session.blender().state() == BlenderState::Ready);

    // The baseline is taken AFTER resolution and BEFORE the run, so every byte the CONVERSION writes
    // shows up -- the script, the log, the artifact and the status file all land in Library/, and the
    // assets tree must be untouched by all of it.
    const std::vector<std::string> assetsBefore = listWithSizes(project.assetsRoot);
    REQUIRE_FALSE(assetsBefore.empty());

    session.requestConversion();
    for (int i = 0; i < MAX_POLL_ITERATIONS && session.state() != SessionState::ConversionFailed &&
                    session.state() != SessionState::Imported;
         ++i) {
        session.service(project.assetsRoot, project.db, 0.016F);
        std::this_thread::yield();
    }
    REQUIRE(session.blender().exportRunCount() == 1);  // the run really happened
    CHECK(listWithSizes(project.assetsRoot) == assetsBefore);
    // and the derived data DID land -- otherwise the assertion above would hold vacuously.
    CHECK(engine::editor::fileExists(project.exportDir + '/' + std::string(engine::editor::BLENDER_SCRIPT_FILE_NAME)));
}

TEST_CASE(
    "blender_service: a SECOND run leaves export_gltf.py byte- and mtime-identical (BS47, task 3.2.4 "
    "INV-B12, seed S32)") {
    const TempDir tmp;
    const BlendProject project = makeBlendProject(tmp, 318);
    const std::string scriptPath = project.exportDir + '/' + std::string(engine::editor::BLENDER_SCRIPT_FILE_NAME);

    ModelImportSession session;
    session.setTarget("statue.blend", project.db.generation());
    session.service(project.assetsRoot, project.db);
    session.blenderMutable().resolve(engine::editor::currentHostOs(), overrideEnv(CMAKE_COMMAND), project.exportDir);
    sessionUntilBlenderSettled(session, project.assetsRoot, project.db);
    REQUIRE(session.blender().state() == BlenderState::Ready);

    // NOT "loop until the state leaves ConversionFailed": after the first run the session ALREADY sits
    // in ConversionFailed, so such a loop would exit before its own body ran even once and the second
    // conversion would never start -- a case that passes while proving nothing. Drive the two
    // deterministic ticks explicitly (drain the request into Converting; spawn), then wait it out.
    const auto convertOnce = [&session, &project]() {
        session.requestConversion();
        session.service(project.assetsRoot, project.db, 0.016F);  // -> Converting (records, no spawn)
        REQUIRE(session.state() == SessionState::Converting);
        for (int i = 0; i < MAX_POLL_ITERATIONS && session.state() == SessionState::Converting; ++i) {
            session.service(project.assetsRoot, project.db, 0.016F);
            std::this_thread::yield();
        }
    };

    convertOnce();
    REQUIRE(session.blender().exportRunCount() == 1);
    // The FIRST run writes it, and its content is the constant byte for byte -- no interpolation site
    // anywhere, which is what makes it assertable at all (AC-13).
    REQUIRE(engine::editor::fileExists(scriptPath));
    CHECK(readAll(scriptPath) == std::string(blenderExportScriptText()));
    std::error_code ec;
    const std::filesystem::file_time_type firstWrite = std::filesystem::last_write_time(pathOf(scriptPath), ec);
    REQUIRE_FALSE(ec);

    convertOnce();
    REQUIRE(session.blender().exportRunCount() == 2);  // a SECOND run really happened
    const std::filesystem::file_time_type secondWrite = std::filesystem::last_write_time(pathOf(scriptPath), ec);
    REQUIRE_FALSE(ec);
    // D6's "write only what differs", a third application: an unchanged script costs ZERO BYTES.
    CHECK(secondWrite == firstWrite);
    CHECK(readAll(scriptPath) == std::string(blenderExportScriptText()));
}
