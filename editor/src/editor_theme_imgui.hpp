#pragma once
// Aero Engine — the theme's ImGui-typed half (task E.6.1, spec D9, D13). SRC-PRIVATE and INCLUDES
// <imgui.h>, so only ImGui TUs include it; tests and ImGui-free callers use editor_theme_ui.hpp instead.
#include <aero/editor/editor_theme.hpp>

#include <array>
#include <imgui.h>
#include <utility>

namespace engine::editor {

// D13: the 63 (slot, colour) pairs, in ImGuiCol order, from the palette (alpha-bearing entries carry theirs).
using EditorStyleColors = std::array<std::pair<ImGuiCol, Srgb8>, ImGuiCol_COUNT>;
[[nodiscard]] EditorStyleColors editorStyleColors(const ThemePalette& palette);
// D9: PURE in its two arguments -- starts from a value-initialised ImGuiStyle, NEVER from GetStyle().
[[nodiscard]] ImGuiStyle buildEditorStyle(const EditorTheme& theme, float uiScale);
// A DIVISION per channel, so ImGui's IM_F32_TO_INT8_SAT returns the original byte for all 256 values (I280).
[[nodiscard]] ImVec4 toImVec4(Srgb8 color) noexcept;
[[nodiscard]] ImVec4 toImVec4(Rgbaf color) noexcept;

}  // namespace engine::editor
