// tests/editor/esbuild_tool_test.cpp -- task 4.3.1: the pinned esbuild and its pure rules. Tier-0, UNGATED, no
// GPU, no window, no ImGui context.
//
// Step 2 holds one case: the binary the configure fetched is the pinned version and runs. Every later case of the
// task's pure half lands in this file.
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. tool_process.hpp is SRC-PRIVATE and is reached by an explicit relative
// path (the blender_service_test.cpp precedent).
#include "../../editor/src/tool_process.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using engine::editor::ProcessState;
using engine::editor::ToolProcess;

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
