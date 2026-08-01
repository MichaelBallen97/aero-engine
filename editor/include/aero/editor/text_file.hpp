#pragma once
// Aero Engine — the editor's text-file bytes (task 2.6.1, D12; the declarations promoted verbatim
// from scene_session.hpp, where they landed at task 2.5.1). PUBLIC, and free of ImGui, SDL, entt and
// <filesystem> -- ALL of the latter lives in text_file.cpp, exactly as it did in scene_file.cpp.
// Held by FILE PLACEMENT (R12), like every other header under editor/include.
//
// Promoted because the project writer needs all three and the atomic write is subtle enough (binary
// on BOTH sides, a SCOPED stream, `.aero-tmp` + rename, Windows's refusal to replace an open file)
// that a second hand-rolled copy in this tree would be a defect waiting to happen.
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

struct FileReadResult {
    std::optional<std::string> text;  // engaged == success
    std::string error;                // OS reason; empty iff `text` is engaged
};
[[nodiscard]] FileReadResult readTextFile(std::string_view absolutePathUtf8);

// "" == success. ATOMIC: writes <path>.aero-tmp, CLOSES it, renames over `path`.
[[nodiscard]] std::string writeTextFileAtomic(std::string_view absolutePathUtf8, std::string_view text);
[[nodiscard]] bool fileExists(std::string_view absolutePathUtf8);

}  // namespace engine::editor
