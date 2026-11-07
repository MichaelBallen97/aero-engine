#pragma once
// Aero Engine -- the C++ project template (task 4.5.2): the four files a new C++ project is given, rendered from the
// project's name. The render is PURE: no filesystem include, no logging; the same name yields the same bytes. The
// writer puts the four files into a project folder and never deletes, renames or copies (check A's ninth file).
#include <array>
#include <cstddef>
#include <cstdint>
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

enum class CppProjectWrite : std::uint8_t { Written, Unchanged, Failed };

struct CppProjectFileOutcome {
    std::string_view relativePath;
    CppProjectWrite state = CppProjectWrite::Failed;
    std::string error;  // "" unless Failed
};

struct CppProjectReport {
    std::array<CppProjectFileOutcome, CPP_PROJECT_FILE_COUNT> files;
    [[nodiscard]] bool ok() const noexcept;  // no file Failed
};

// Creates each file only when nothing is at its path: a directory wearing the name counts, and so does a symlink
// whose target is missing. Overwrites nothing and removes nothing. One file's failure does not stop the others. An
// empty root fails every file and touches nothing. Never logs: the caller turns a report into a WARN.
[[nodiscard]] CppProjectReport writeCppProject(std::string_view projectRootUtf8, std::string_view projectName);

}  // namespace engine::editor
