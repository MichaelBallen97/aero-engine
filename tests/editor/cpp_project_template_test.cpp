// Aero Engine -- the C++ project template's pure render (task 4.5.2, CP1-CP9). Tier 0: aero_editor_shell_test, no
// ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING in all three configurations.
// Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
#include <aero/editor/cpp_project_template.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace ed = engine::editor;
namespace fs = std::filesystem;

constexpr std::size_t CMAKE_FILE = 0;
constexpr std::size_t MAIN_FILE = 1;
constexpr std::size_t README_FILE = 2;
constexpr std::size_t IGNORE_FILE = 3;

[[nodiscard]] std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string templatePath(std::string_view leaf) {
    std::string path(AERO_CPP_PROJECT_TEMPLATE_DIR);
    path += '/';
    path += leaf;
    return path;
}

[[nodiscard]] std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

[[nodiscard]] std::size_t countOf(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

[[nodiscard]] bool isIdentifier(std::string_view id) {
    if (!id.starts_with("game_") || id.size() == 5) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// The inverse of cppStringLiteralBody, written separately: \" \\ and \ooo, anything else is itself.
[[nodiscard]] std::string decodeLiteral(std::string_view body) {
    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\') {
            out += body[i];
        } else if (i + 1 < body.size() && (body[i + 1] == '"' || body[i + 1] == '\\')) {
            out += body[++i];
        } else if (i + 3 < body.size() && body[i + 1] >= '0' && body[i + 1] <= '3') {
            const int value = ((body[i + 1] - '0') * 64) + ((body[i + 2] - '0') * 8) + (body[i + 3] - '0');
            out += static_cast<char>(value);
            i += 3;
        } else {
            out += '?';  // not a body this encoder writes
        }
    }
    return out;
}

}  // namespace

TEST_CASE("cpp_project: four files in the stated order, none empty, wired to their own template (CP1)") {
    const auto files = ed::renderCppProject("Demo");
    CHECK(files[CMAKE_FILE].relativePath == "CMakeLists.txt");
    CHECK(files[MAIN_FILE].relativePath == "src/main.cpp");
    CHECK(files[README_FILE].relativePath == "README.md");
    CHECK(files[IGNORE_FILE].relativePath == ".gitignore");
    for (const ed::CppProjectFile& file : files) {
        CHECK_FALSE(file.text.empty());
    }
    CHECK(files[CMAKE_FILE].text.find("find_package(AeroEngine CONFIG REQUIRED)") != std::string::npos);
    CHECK(files[CMAKE_FILE].text.find("aero_add_game(game_Demo src/main.cpp)") != std::string::npos);
    CHECK(files[MAIN_FILE].text.find("int main(") != std::string::npos);
    CHECK(files[README_FILE].text.starts_with("# Demo\n"));
    CHECK(files[IGNORE_FILE].text.find("build/") != std::string::npos);
}

TEST_CASE("cpp_project: every name yields a valid CMake identifier (CP2)") {
    CHECK(ed::cppProjectIdentifier("Demo") == "game_Demo");
    CHECK(ed::cppProjectIdentifier("My Game") == "game_My_Game");
    CHECK(ed::cppProjectIdentifier("test") == "game_test");
    CHECK(ed::cppProjectIdentifier("CMAKE") == "game_CMAKE");
    CHECK(ed::cppProjectIdentifier("1st") == "game_1st");
    CHECK(ed::cppProjectIdentifier("$#;()'@{}%") == "game___________");
    CHECK(ed::cppProjectIdentifier("\xC3\xB1") == "game___");
    const std::vector<std::string> names{"Demo", "My Game",          "test",       "CMAKE",
                                         "1st",  "\xC3\xB1\xC3\xA1", "$#;()'@{}%", "a"};
    for (const std::string& name : names) {
        CAPTURE(name);
        CHECK(isIdentifier(ed::cppProjectIdentifier(name)));
    }
}

TEST_CASE("cpp_project: the literal body escapes with three octal digits and nothing wider (CP3)") {
    CHECK(ed::cppStringLiteralBody("\"") == "\\\"");
    CHECK(ed::cppStringLiteralBody("\\") == "\\\\");
    CHECK(ed::cppStringLiteralBody("\xC3\xB1") == "\\303\\261");
    CHECK(ed::cppStringLiteralBody("\xC3\xB1"
                                   "1") == "\\303\\2611");
    CHECK(ed::cppStringLiteralBody(std::string_view("\x01", 1)) == "\\001");
    CHECK(ed::cppStringLiteralBody("\x7F") == "\\177");
    CHECK(ed::cppStringLiteralBody("?%{") == "?%{");
    CHECK(ed::cppStringLiteralBody("Plain name") == "Plain name");
}

TEST_CASE("cpp_project: all 256 byte values survive the literal body (CP4)") {
    std::string all;
    for (int value = 0; value < 256; ++value) {
        all += static_cast<char>(value);
    }
    const std::string body = ed::cppStringLiteralBody(all);
    CHECK(decodeLiteral(body) == all);
    for (const char c : body) {
        const auto byte = static_cast<unsigned char>(c);
        CHECK(byte >= 0x20U);
        CHECK(byte <= 0x7EU);
    }
}

TEST_CASE("cpp_project: a project named after a token is not expanded a second time (CP5)") {
    const std::vector<std::string> names{"@AERO_PROJECT_ID@", "@AERO_PROJECT_TITLE@", "@AERO_PROJECT_NAME@"};
    for (const std::string& name : names) {
        CAPTURE(name);
        const auto files = ed::renderCppProject(name);
        CHECK(countOf(files[README_FILE].text, name) == 1);  // the heading
        CHECK(countOf(files[MAIN_FILE].text, name) == 1);    // the title
        CHECK(countOf(files[CMAKE_FILE].text, name) == 0);   // only the identifier reaches CMake
        CHECK(files[CMAKE_FILE].text.find('@') == std::string::npos);
    }
}

TEST_CASE("cpp_project: the render is pure and differs between names only where a token sat (CP6)") {
    const auto first = ed::renderCppProject("Alpha");
    const auto again = ed::renderCppProject("Alpha");
    const auto other = ed::renderCppProject("Omega");
    for (std::size_t i = 0; i < ed::CPP_PROJECT_FILE_COUNT; ++i) {
        CHECK(first[i].text == again[i].text);
        const std::string a = replaceAll(replaceAll(first[i].text, "game_Alpha", "X"), "Alpha", "X");
        const std::string b = replaceAll(replaceAll(other[i].text, "game_Omega", "X"), "Omega", "X");
        CHECK(a == b);
    }
    CHECK(first[CMAKE_FILE].text != other[CMAKE_FILE].text);
    CHECK(first[IGNORE_FILE].text == other[IGNORE_FILE].text);
}

TEST_CASE("cpp_project: no token survives an ordinary name and the README holds the build commands (CP7)") {
    const auto files = ed::renderCppProject("Demo");
    for (const ed::CppProjectFile& file : files) {
        CHECK(file.text.find("@AERO_PROJECT_") == std::string::npos);
    }
    const std::string& readme = files[README_FILE].text;
    CHECK(readme.find("cmake --preset <os>-sdk\n"
                      "cmake --build --preset <os>-sdk\n"
                      "cmake --install build/<os>-sdk\n") != std::string::npos);
    CHECK(readme.find("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<engine>/build/sdk/<os>-sdk\n"
                      "cmake --build build --config Release\n") != std::string::npos);
    CHECK(readme.find("./build/game_Demo\n") != std::string::npos);
    CHECK(readme.find("build\\Release\\game_Demo.exe") != std::string::npos);
    CHECK(readme.find("--frames <n>") != std::string::npos);
    CHECK(readme.find("world.registerComponent<T>") != std::string::npos);
    CHECK(readme.find("AERO_COMPONENT") != std::string::npos);
    CHECK(readme.find("MultiThreadedDLL") != std::string::npos);
}

TEST_CASE("cpp_project: each embedded template is exactly the file it was built from (CP8)") {
    const auto files = ed::renderCppProject("Demo");
    const std::array<std::string_view, ed::CPP_PROJECT_FILE_COUNT> leaves{"CMakeLists.txt.in", "main.cpp.in",
                                                                          "README.md.in", "gitignore.in"};
    for (std::size_t i = 0; i < ed::CPP_PROJECT_FILE_COUNT; ++i) {
        CAPTURE(leaves[i]);
        std::string expected = slurp(templatePath(leaves[i]));
        REQUIRE_FALSE(expected.empty());
        expected = replaceAll(expected, "@AERO_PROJECT_ID@", "game_Demo");
        expected = replaceAll(expected, "@AERO_PROJECT_TITLE@", "Demo");
        expected = replaceAll(expected, "@AERO_PROJECT_NAME@", "Demo");
        CHECK(files[i].text == expected);
    }
}

TEST_CASE("cpp_project: the first file a new author reads names only engine and standard headers (CP9)") {
    const std::string text = slurp(templatePath("main.cpp.in"));
    REQUIRE_FALSE(text.empty());
    std::size_t includes = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string_view line = std::string_view(text).substr(at, end - at);
        at = end + 1;
        if (!line.starts_with("#include")) {
            continue;
        }
        ++includes;
        CAPTURE(line);
        REQUIRE(line.size() > 11);
        REQUIRE(line[9] == '<');
        REQUIRE(line.back() == '>');
        const std::string_view name = line.substr(10, line.size() - 11);
        if (name.starts_with("aero/")) {
            continue;
        }
        for (const char c : name) {
            CHECK(((c >= 'a' && c <= 'z') || c == '_'));
        }
    }
    CHECK(includes > 0);
    REQUIRE(countOf(text, "@AERO_PROJECT_TITLE@") == 1);
    const std::size_t token = text.find("@AERO_PROJECT_TITLE@");
    const std::size_t lineStart = text.rfind('\n', token) + 1;
    const std::size_t lineEnd = text.find('\n', token);
    CHECK(text.substr(lineStart, lineEnd - lineStart).find(".title = \"@AERO_PROJECT_TITLE@\"") != std::string::npos);
}

// ---- The writer (CP10-CP16) ------------------------------------------------------------------------------------

namespace {

constexpr std::string_view DEMO_NAME = "Demo";

// A unique temp directory whose name carries the case id, removing itself. A tree this case made, in the tests only.
class TempDir {
public:
    explicit TempDir(std::string_view caseId, const std::u8string& leaf = u8"") {
        std::error_code ec;
        dirPath = fs::temp_directory_path(ec) / ("aero_cpp_project_" + std::string(caseId));
        if (!leaf.empty()) {
            dirPath /= fs::path(leaf);
        }
        fs::remove_all(dirPath, ec);
        fs::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] static std::string utf8Of(const fs::path& path) {
        const std::u8string bytes = path.u8string();
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::ranges::replace(text, '\\', '/');
        return text;
    }
    [[nodiscard]] std::string utf8() const { return utf8Of(dirPath); }
    [[nodiscard]] std::string join(std::string_view leaf) const { return utf8() + '/' + std::string(leaf); }
    [[nodiscard]] const fs::path& path() const noexcept { return dirPath; }

private:
    fs::path dirPath;
};

// CP15 only. The process working directory is global state, so it is restored by a destructor that runs on a thrown
// REQUIRE as well as on success.
class ScopedCwd {
public:
    explicit ScopedCwd(const fs::path& to) : previous(fs::current_path()) { fs::current_path(to); }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
    ScopedCwd(ScopedCwd&&) = delete;
    ScopedCwd& operator=(ScopedCwd&&) = delete;

private:
    fs::path previous;
};

[[nodiscard]] std::string readAll(const std::string& path) {
    const ed::FileReadResult read = ed::readTextFile(path);
    return read.text.value_or(std::string("<unreadable>"));
}

void putBytes(const std::string& path, std::string_view bytes) {
    std::ofstream out(fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                      std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::size_t countEntries(const fs::path& folder) {
    std::error_code ec;
    std::size_t count = 0;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        ++count;
    }
    return count;
}

[[nodiscard]] bool anyTempLeft(const fs::path& folder) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().filename().string().find(".aero-tmp") != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("cpp_project: an empty folder is given the four files, bytes as rendered (CP10)") {
    const TempDir dir("CP10");
    const auto rendered = ed::renderCppProject(DEMO_NAME);
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    for (std::size_t i = 0; i < ed::CPP_PROJECT_FILE_COUNT; ++i) {
        CAPTURE(rendered[i].relativePath);
        CHECK(report.files[i].relativePath == rendered[i].relativePath);
        CHECK(report.files[i].state == ed::CppProjectWrite::Written);
        CHECK(report.files[i].error.empty());
        CHECK(readAll(dir.join(rendered[i].relativePath)) == rendered[i].text);
    }
    CHECK(fs::is_directory(dir.path() / "src"));
}

TEST_CASE("cpp_project: a second run changes nothing, bytes and modification times alike (CP11)") {
    const TempDir dir("CP11");
    REQUIRE(ed::writeCppProject(dir.utf8(), DEMO_NAME).ok());
    const auto rendered = ed::renderCppProject(DEMO_NAME);
    // Move every time into the past, so a rewrite could not hide behind a clock tick.
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code ec;
    for (const ed::CppProjectFile& file : rendered) {
        fs::last_write_time(dir.path() / std::string(file.relativePath), past, ec);
        REQUIRE_FALSE(ec);
    }
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    for (std::size_t i = 0; i < ed::CPP_PROJECT_FILE_COUNT; ++i) {
        CAPTURE(rendered[i].relativePath);
        CHECK(report.files[i].state == ed::CppProjectWrite::Unchanged);
        CHECK(readAll(dir.join(rendered[i].relativePath)) == rendered[i].text);
        CHECK(fs::last_write_time(dir.path() / std::string(rendered[i].relativePath), ec) == past);
    }
}

TEST_CASE("cpp_project: a hand-edited main.cpp survives while the others are created (CP12)") {
    const TempDir dir("CP12");
    REQUIRE(ed::ensureDirectory(dir.join("src")).empty());
    putBytes(dir.join("src/main.cpp"), "int main() { return 7; }\n");
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    CHECK(report.files[0].state == ed::CppProjectWrite::Written);
    CHECK(report.files[1].state == ed::CppProjectWrite::Unchanged);
    CHECK(report.files[2].state == ed::CppProjectWrite::Written);
    CHECK(report.files[3].state == ed::CppProjectWrite::Written);
    CHECK(readAll(dir.join("src/main.cpp")) == "int main() { return 7; }\n");
}

TEST_CASE("cpp_project: a directory wearing a file's name counts as present and is left alone (CP13)") {
    const TempDir dir("CP13");
    REQUIRE(ed::ensureDirectory(dir.join("README.md")).empty());
    putBytes(dir.join("README.md/keep.txt"), "kept");
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    CHECK(report.files[0].state == ed::CppProjectWrite::Written);
    CHECK(report.files[1].state == ed::CppProjectWrite::Written);
    CHECK(report.files[2].state == ed::CppProjectWrite::Unchanged);
    CHECK(report.files[3].state == ed::CppProjectWrite::Written);
    CHECK(fs::is_directory(dir.path() / "README.md"));
    CHECK(readAll(dir.join("README.md/keep.txt")) == "kept");
}

TEST_CASE("cpp_project: a file named src fails src/main.cpp alone and is not removed (CP14)") {
    const TempDir dir("CP14");
    putBytes(dir.join("src"), "not a folder");
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK_FALSE(report.ok());
    CHECK(report.files[0].state == ed::CppProjectWrite::Written);
    CHECK(report.files[1].state == ed::CppProjectWrite::Failed);
    CHECK_FALSE(report.files[1].error.empty());
    CHECK(report.files[2].state == ed::CppProjectWrite::Written);
    CHECK(report.files[3].state == ed::CppProjectWrite::Written);
    CHECK(fs::is_regular_file(dir.path() / "src"));
    CHECK(readAll(dir.join("src")) == "not a folder");
}

TEST_CASE("cpp_project: a root that is a file, or empty, fails every file and writes nothing (CP15)") {
    const TempDir dir("CP15");
    putBytes(dir.join("root-is-a-file"), "x");
    // The working directory is a folder this case owns, so no other test or ctest entry writes into what is counted.
    const TempDir workDir("CP15_cwd");
    const ScopedCwd cwd(workDir.path());
    const std::size_t before = countEntries(fs::current_path());
    REQUIRE(before == 0);

    const ed::CppProjectReport onFile = ed::writeCppProject(dir.join("root-is-a-file"), DEMO_NAME);
    CHECK_FALSE(onFile.ok());
    for (const ed::CppProjectFileOutcome& outcome : onFile.files) {
        CAPTURE(outcome.relativePath);
        CHECK(outcome.state == ed::CppProjectWrite::Failed);
        CHECK_FALSE(outcome.error.empty());
    }
    CHECK(readAll(dir.join("root-is-a-file")) == "x");

    const ed::CppProjectReport onEmpty = ed::writeCppProject("", DEMO_NAME);
    CHECK_FALSE(onEmpty.ok());
    for (const ed::CppProjectFileOutcome& outcome : onEmpty.files) {
        CAPTURE(outcome.relativePath);
        CHECK(outcome.state == ed::CppProjectWrite::Failed);
        CHECK(outcome.error == "no project root");
    }
    CHECK(countEntries(fs::current_path()) == before);
}

TEST_CASE("cpp_project: a root holding a space and non-ASCII bytes succeeds and leaves no temp behind (CP16)") {
    const TempDir dir("CP16", u8"sp ace \u00f1 dir");
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    const auto rendered = ed::renderCppProject(DEMO_NAME);
    for (std::size_t i = 0; i < ed::CPP_PROJECT_FILE_COUNT; ++i) {
        CAPTURE(rendered[i].relativePath);
        CHECK(report.files[i].state == ed::CppProjectWrite::Written);
        CHECK(readAll(dir.join(rendered[i].relativePath)) == rendered[i].text);
    }
    CHECK_FALSE(anyTempLeft(dir.path().parent_path()));
}

TEST_CASE("cpp_project: a dangling symlink at a template path is present and stays a symlink (CP17)") {
    const TempDir dir("CP17");
    const fs::path link = dir.path() / "CMakeLists.txt";
    const fs::path target = "missing-target";
    std::error_code makeEc;
    fs::create_symlink(target, link, makeEc);
    if (makeEc) {
        MESSAGE("CP17: no symlink can be made here (" << makeEc.message() << "), so it asserts nothing more");
        return;
    }
    const ed::CppProjectReport report = ed::writeCppProject(dir.utf8(), DEMO_NAME);
    CHECK(report.ok());
    CHECK(report.files[0].state == ed::CppProjectWrite::Unchanged);
    CHECK(report.files[1].state == ed::CppProjectWrite::Written);
    CHECK(report.files[2].state == ed::CppProjectWrite::Written);
    CHECK(report.files[3].state == ed::CppProjectWrite::Written);

    std::error_code statusEc;
    CHECK(fs::is_symlink(fs::symlink_status(link, statusEc)));
    std::error_code readEc;
    CHECK(fs::read_symlink(link, readEc) == target);
    std::error_code targetEc;
    CHECK_FALSE(fs::exists(fs::symlink_status(dir.path() / target, targetEc)));
}
