#pragma once
// Aero Engine -- the editor's embedded script texts (task 4.2.2). SRC-PRIVATE. Each accessor is DEFINED by a TU that
// cmake/embed_run.cmake generates in its text mode at build time (never committed, never linted): the build's
// aero-engine.d.ts and editor/templates/tsconfig.json.in, byte for byte. Static storage for the program's life.
// Functions rather than `extern const` arrays, for editor_font_data.hpp's reason.
#include <string_view>

namespace engine::editor::embedded {
[[nodiscard]] std::string_view scriptDeclarations() noexcept;
[[nodiscard]] std::string_view tsconfigTemplate() noexcept;
}  // namespace engine::editor::embedded
