#pragma once
// Aero Engine -- the C++ project template (task 4.5.2): the four files a new C++ project is given, rendered from the
// project's name. PURE: no filesystem include, no logging; the same name yields the same bytes.
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr std::size_t CPP_PROJECT_FILE_COUNT = 4;

struct CppProjectFile {
    std::string_view relativePath;  // "CMakeLists.txt", "src/main.cpp", "README.md", ".gitignore"
    std::string text;
};

// "game_" + projectName with every byte outside [A-Za-z0-9_] replaced by '_'. Always a valid CMake target and project
// name, whatever the project is called.
[[nodiscard]] std::string cppProjectIdentifier(std::string_view projectName);
// The bytes of a C string literal's body: 7-bit, printable, every other byte a three-digit octal escape, so MSVC's
// code page cannot change it.
[[nodiscard]] std::string cppStringLiteralBody(std::string_view bytes);
// PURE: the same name yields the same bytes. One left-to-right pass: replaced text is never rescanned.
[[nodiscard]] std::array<CppProjectFile, CPP_PROJECT_FILE_COUNT> renderCppProject(std::string_view projectName);

}  // namespace engine::editor
