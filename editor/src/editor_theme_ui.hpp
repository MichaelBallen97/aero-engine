#pragma once
// Aero Engine — the theme's ImGui half, declared ImGui-FREE (task E.6.1, spec D8, D9, D13). SRC-PRIVATE,
// and it names no ImGui type at all, so tests/editor/imgui_layer_test.cpp -- ImGui-free at source -- can
// drive the style builder and read the live style through ImGui-free snapshots. The ImGui-typed half is
// editor_theme_imgui.hpp, which only ImGui TUs include.
#include <aero/editor/editor_theme.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr std::size_t EDITOR_STYLE_COLOR_COUNT = 63;  // == ImGuiCol_COUNT, asserted in the .cpp

// D9: ImGui::GetStyle() = buildEditorStyle(EDITOR_THEME, uiScale) -- THE one style write in the editor
// (I286). Rebuilt whole from a fresh ImGuiStyle at every call, never scaled in place, so it never compounds.
void applyEditorStyle(float uiScale);
// The live style's FontScaleDpi -- the ONE stored copy of the UI scale (D8). Needs a current context.
[[nodiscard]] float currentUiScale();
// IM_COL32's packing of a theme colour, for ImDrawList calls; declared ImGui-free so any caller can use it.
[[nodiscard]] std::uint32_t toImU32(Srgb8 color) noexcept;

// ---- snapshots, for tests: ImGui-free copies of a style, read through ImGui's own conversions --------
// A scalar member has y == 0; an enum, flag or bool member is its integer value in x.
struct StyleMemberValue {
    std::string_view name;  // the ImGuiStyle member's own name, e.g. "FramePadding"
    float x = 0.0F;
    float y = 0.0F;
};

struct EditorStyleSnapshot {
    std::array<std::string_view, EDITOR_STYLE_COLOR_COUNT> colorNames{};       // GetStyleColorName(i)
    std::array<Srgb8, EDITOR_STYLE_COLOR_COUNT> colorBytes{};                  // via ColorConvertFloat4ToU32
    std::array<std::array<float, 4>, EDITOR_STYLE_COLOR_COUNT> colorFloats{};  // the floats ImGui holds
    // EVERY size, alignment, behaviour, enum and flag member, in the constructor's order.
    std::vector<StyleMemberValue> members;
    float fontSizeBase = 0.0F;
    float fontScaleMain = 0.0F;
    float fontScaleDpi = 0.0F;
    bool configDpiScaleFonts = true;  // the context's io flag
};

// Each builds its result in a PRIVATE ImGui context through applyEditorStyle -- the production path -- and
// restores whichever context was current, so each is safe beside a live EditorApp.
[[nodiscard]] EditorStyleSnapshot snapshotEditorStyle(float uiScale);
[[nodiscard]] EditorStyleSnapshot snapshotStyleAfter(std::span<const float> scalesInOrder);
// Poisons the private context's style first -- every colour magenta, ScaleAllSizes(999), FontSizeBase,
// FontScaleMain and FontScaleDpi 999, and every enum and flag member set to a non-default value -- then
// applies.
[[nodiscard]] EditorStyleSnapshot snapshotStyleBuiltOverPoison(float uiScale);
// The CURRENT context's live style and io flag (the GPU tier).
[[nodiscard]] EditorStyleSnapshot snapshotLiveStyle();

}  // namespace engine::editor
