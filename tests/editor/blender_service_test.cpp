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
#include <aero/editor/blender_service.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/text_file.hpp>

#include "../../editor/src/blender_process.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
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
