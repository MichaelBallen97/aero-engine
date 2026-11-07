// Aero Engine -- the C++ project template's pure render (task 4.5.2, CP1-CP9). Tier 0: aero_editor_shell_test, no
// ImGui, no GPU, no window, and UNGATED -- every case here must be PRESENT and PASSING in all three configurations.
// Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp supplies main().
#include <aero/editor/cpp_project_template.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;

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
