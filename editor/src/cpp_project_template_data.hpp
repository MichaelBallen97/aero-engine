#pragma once
// Aero Engine -- the editor's embedded C++ project template (task 4.5.2). SRC-PRIVATE. Each accessor is DEFINED by a
// TU that cmake/embed_run.cmake generates in its text mode at build time (never committed, never linted): the four
// files of editor/templates/cpp-project/, byte for byte, tokens included. Static storage for the program's life.
#include <string_view>

namespace engine::editor::embedded {
[[nodiscard]] std::string_view cppProjectCmakeTemplate() noexcept;
[[nodiscard]] std::string_view cppProjectMainTemplate() noexcept;
[[nodiscard]] std::string_view cppProjectReadmeTemplate() noexcept;
[[nodiscard]] std::string_view cppProjectGitignoreTemplate() noexcept;
}  // namespace engine::editor::embedded
