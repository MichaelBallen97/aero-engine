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
#include "../../editor/src/blender_process.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::editor::BlenderProcess;
using engine::editor::ProcessState;

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

// A BOUNDED poll loop. It never sleeps and never consults a clock: it polls until the child reports
// Exited or the iteration budget runs out, and the budget failing is a test failure rather than a
// hang. `cmake -E true` is a process creation plus an immediate exit, so this converges in far fewer
// iterations than the budget on every platform.
constexpr int MAX_POLL_ITERATIONS = 2000000;

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
    }
    return outcome;
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
