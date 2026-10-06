// Aero Engine — the theme's ImGui half (task E.6.1, spec D8, D9, D13): the 63-slot colour table, the style
// builder, the ONE style writer in the editor (I286) and the ImGui-free snapshots the tests read.
#include "editor_theme_ui.hpp"

#include <aero/editor/editor_theme.hpp>

#include "editor_theme_imgui.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

static_assert(ImGuiCol_COUNT == static_cast<int>(EDITOR_STYLE_COLOR_COUNT),
              "ImGui gained or lost a colour slot: map it in editorStyleColors (D13) and update "
              "EDITOR_STYLE_COLOR_COUNT");

namespace {

[[nodiscard]] ImVec2 toImVec2(Dp2 value) noexcept { return {value.x, value.y}; }

[[nodiscard]] constexpr Srgb8 withAlpha(Srgb8 color, std::uint8_t alpha) noexcept {
    color.a = alpha;
    return color;
}

// THE FLOOR: no member ScaleAllSizes scales whose theme value is >= 1 is ever truncated below 1.
// ScaleAllSizes (imgui.cpp:1602-1651 at the pinned 1.92.8) rounds each size DOWN to a whole point, so at any
// UI scale in [0.5, 1) a theme value in [1, 2) becomes 0 -- the case imgui.cpp:1599-1600 warns about
// ("Consider not calling this if your initial scale factor if <1.0"). A scale below 1 is reachable (X11 at
// Xft.dpi 90 resolves to 0.95, and UI_SCALE_MIN is 0.5), and a 0 there is a defect rather than a smaller
// size: a 1-dp border or separator vanishes, a 0 MouseCursorScale puts every tooltip and drag preview under
// the cursor (imgui.cpp:12777, :13503-13518), and a 0 TabMinWidthBase drops the minimum tab width. So
// buildEditorStyle floors each member below at 1 when the THEME states it as >= 1; one the theme states as 0
// stays 0, and at a scale >= 1 the floor changes nothing. The set is every scaled scalar whose theme value is
// in [1, 2) -- the members that CAN truncate below 1 at 0.5 or more -- plus the borders and lines that held
// the floor first, each paired with the theme member it is built from: the ONE spelling of the set. No Dp2
// component is in [1, 2) today. I280's universal reads every scaled member at 0.5, 0.75 and 0.95, so a
// theme change that puts a member there is red until it joins this table.
struct FlooredSize {
    float ImGuiStyle::*style;
    float ThemeMetrics::*theme;
};
constexpr std::array<FlooredSize, 15> FLOORED_SIZES{{
    {&ImGuiStyle::WindowBorderSize, &ThemeMetrics::windowBorderSize},
    {&ImGuiStyle::ChildBorderSize, &ThemeMetrics::childBorderSize},
    {&ImGuiStyle::PopupBorderSize, &ThemeMetrics::popupBorderSize},
    {&ImGuiStyle::FrameBorderSize, &ThemeMetrics::frameBorderSize},
    {&ImGuiStyle::ImageBorderSize, &ThemeMetrics::imageBorderSize},
    {&ImGuiStyle::TabBorderSize, &ThemeMetrics::tabBorderSize},
    {&ImGuiStyle::TabMinWidthBase, &ThemeMetrics::tabMinWidthBase},
    {&ImGuiStyle::TabBarBorderSize, &ThemeMetrics::tabBarBorderSize},
    {&ImGuiStyle::TabBarOverlineSize, &ThemeMetrics::tabBarOverlineSize},
    {&ImGuiStyle::TreeLinesSize, &ThemeMetrics::treeLinesSize},
    {&ImGuiStyle::DragDropTargetBorderSize, &ThemeMetrics::dragDropTargetBorderSize},
    {&ImGuiStyle::SeparatorSize, &ThemeMetrics::separatorSize},
    {&ImGuiStyle::SeparatorTextBorderSize, &ThemeMetrics::separatorTextBorderSize},
    {&ImGuiStyle::DockingSeparatorSize, &ThemeMetrics::dockingSeparatorSize},
    {&ImGuiStyle::MouseCursorScale, &ThemeMetrics::mouseCursorScale},
}};

// A private ImGui context for the length of a scope -- the E.4.4 font harness's pattern: no window, no GPU,
// no ini or log file -- restoring whichever context was current, so a snapshot is safe beside a live app.
class PrivateContext {
public:
    PrivateContext() : previous(ImGui::GetCurrentContext()), context(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(context);  // CreateContext restores the previous one when there was one
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
    }
    ~PrivateContext() {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    PrivateContext(const PrivateContext&) = delete;
    PrivateContext& operator=(const PrivateContext&) = delete;
    PrivateContext(PrivateContext&&) = delete;
    PrivateContext& operator=(PrivateContext&&) = delete;

private:
    ImGuiContext* previous = nullptr;
    ImGuiContext* context = nullptr;
};

// Every value an ImGui-free test needs, copied by name, in the constructor's order (imgui.cpp:1512-1590).
[[nodiscard]] EditorStyleSnapshot snapshotOf(const ImGuiStyle& style, const ImGuiIO& io) {
    EditorStyleSnapshot snap;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const auto index = static_cast<std::size_t>(i);
        snap.colorNames[index] = ImGui::GetStyleColorName(i);
        const ImVec4& color = style.Colors[i];
        snap.colorFloats[index] = {color.x, color.y, color.z, color.w};
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);  // ImGui's own byte rounding
        snap.colorBytes[index] = Srgb8{.r = static_cast<std::uint8_t>((packed >> IM_COL32_R_SHIFT) & 0xFFU),
                                       .g = static_cast<std::uint8_t>((packed >> IM_COL32_G_SHIFT) & 0xFFU),
                                       .b = static_cast<std::uint8_t>((packed >> IM_COL32_B_SHIFT) & 0xFFU),
                                       .a = static_cast<std::uint8_t>((packed >> IM_COL32_A_SHIFT) & 0xFFU)};
    }
    std::vector<StyleMemberValue>& m = snap.members;
    m.reserve(69);
    const auto scalar = [&m](std::string_view name, float value) {
        m.push_back(StyleMemberValue{.name = name, .x = value, .y = 0.0F});
    };
    const auto pair = [&m](std::string_view name, ImVec2 value) {
        m.push_back(StyleMemberValue{.name = name, .x = value.x, .y = value.y});
    };
    const auto integer = [&m](std::string_view name, int value) {
        m.push_back(StyleMemberValue{.name = name, .x = static_cast<float>(value), .y = 0.0F});
    };
    scalar("Alpha", style.Alpha);
    scalar("DisabledAlpha", style.DisabledAlpha);
    pair("WindowPadding", style.WindowPadding);
    scalar("WindowRounding", style.WindowRounding);
    scalar("WindowBorderSize", style.WindowBorderSize);
    scalar("WindowBorderHoverPadding", style.WindowBorderHoverPadding);
    pair("WindowMinSize", style.WindowMinSize);
    pair("WindowTitleAlign", style.WindowTitleAlign);
    integer("WindowMenuButtonPosition", static_cast<int>(style.WindowMenuButtonPosition));
    scalar("ChildRounding", style.ChildRounding);
    scalar("ChildBorderSize", style.ChildBorderSize);
    scalar("PopupRounding", style.PopupRounding);
    scalar("PopupBorderSize", style.PopupBorderSize);
    pair("FramePadding", style.FramePadding);
    scalar("FrameRounding", style.FrameRounding);
    scalar("FrameBorderSize", style.FrameBorderSize);
    pair("ItemSpacing", style.ItemSpacing);
    pair("ItemInnerSpacing", style.ItemInnerSpacing);
    pair("CellPadding", style.CellPadding);
    pair("TouchExtraPadding", style.TouchExtraPadding);
    scalar("IndentSpacing", style.IndentSpacing);
    scalar("ColumnsMinSpacing", style.ColumnsMinSpacing);
    scalar("ScrollbarSize", style.ScrollbarSize);
    scalar("ScrollbarRounding", style.ScrollbarRounding);
    scalar("ScrollbarPadding", style.ScrollbarPadding);
    scalar("GrabMinSize", style.GrabMinSize);
    scalar("GrabRounding", style.GrabRounding);
    scalar("LogSliderDeadzone", style.LogSliderDeadzone);
    scalar("ImageRounding", style.ImageRounding);
    scalar("ImageBorderSize", style.ImageBorderSize);
    scalar("TabRounding", style.TabRounding);
    scalar("TabBorderSize", style.TabBorderSize);
    scalar("TabMinWidthBase", style.TabMinWidthBase);
    scalar("TabMinWidthShrink", style.TabMinWidthShrink);
    scalar("TabCloseButtonMinWidthSelected", style.TabCloseButtonMinWidthSelected);
    scalar("TabCloseButtonMinWidthUnselected", style.TabCloseButtonMinWidthUnselected);
    scalar("TabBarBorderSize", style.TabBarBorderSize);
    scalar("TabBarOverlineSize", style.TabBarOverlineSize);
    scalar("TableAngledHeadersAngle", style.TableAngledHeadersAngle);
    pair("TableAngledHeadersTextAlign", style.TableAngledHeadersTextAlign);
    integer("TreeLinesFlags", style.TreeLinesFlags);
    scalar("TreeLinesSize", style.TreeLinesSize);
    scalar("TreeLinesRounding", style.TreeLinesRounding);
    scalar("DragDropTargetRounding", style.DragDropTargetRounding);
    scalar("DragDropTargetBorderSize", style.DragDropTargetBorderSize);
    scalar("DragDropTargetPadding", style.DragDropTargetPadding);
    scalar("ColorMarkerSize", style.ColorMarkerSize);
    integer("ColorButtonPosition", static_cast<int>(style.ColorButtonPosition));
    pair("ButtonTextAlign", style.ButtonTextAlign);
    pair("SelectableTextAlign", style.SelectableTextAlign);
    scalar("SeparatorSize", style.SeparatorSize);
    scalar("SeparatorTextBorderSize", style.SeparatorTextBorderSize);
    pair("SeparatorTextAlign", style.SeparatorTextAlign);
    pair("SeparatorTextPadding", style.SeparatorTextPadding);
    pair("DisplayWindowPadding", style.DisplayWindowPadding);
    pair("DisplaySafeAreaPadding", style.DisplaySafeAreaPadding);
    integer("DockingNodeHasCloseButton", style.DockingNodeHasCloseButton ? 1 : 0);
    scalar("DockingSeparatorSize", style.DockingSeparatorSize);
    scalar("MouseCursorScale", style.MouseCursorScale);
    integer("AntiAliasedLines", style.AntiAliasedLines ? 1 : 0);
    integer("AntiAliasedLinesUseTex", style.AntiAliasedLinesUseTex ? 1 : 0);
    integer("AntiAliasedFill", style.AntiAliasedFill ? 1 : 0);
    scalar("CurveTessellationTol", style.CurveTessellationTol);
    scalar("CircleTessellationMaxError", style.CircleTessellationMaxError);
    scalar("HoverStationaryDelay", style.HoverStationaryDelay);
    scalar("HoverDelayShort", style.HoverDelayShort);
    scalar("HoverDelayNormal", style.HoverDelayNormal);
    integer("HoverFlagsForTooltipMouse", style.HoverFlagsForTooltipMouse);
    integer("HoverFlagsForTooltipNav", style.HoverFlagsForTooltipNav);
    snap.fontSizeBase = style.FontSizeBase;
    snap.fontScaleMain = style.FontScaleMain;
    snap.fontScaleDpi = style.FontScaleDpi;
    snap.configDpiScaleFonts = io.ConfigDpiScaleFonts;
    return snap;
}

// NOT a UI colour: the purity poison below, magenta, which no palette token is (TH1 restates all of them).
constexpr Srgb8 STYLE_POISON_COLOR{255U, 0U, 255U, 255U};

// Everything the builder must overwrite, set to something it would never produce: a builder that started
// from GetStyle() would carry these through (seed S10).
void poisonStyle(ImGuiStyle& style) {
    for (ImVec4& color : style.Colors) {
        color = toImVec4(STYLE_POISON_COLOR);
    }
    style.ScaleAllSizes(999.0F);
    style.FontSizeBase = 999.0F;
    style.FontScaleMain = 999.0F;
    style.FontScaleDpi = 999.0F;
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ColorButtonPosition = ImGuiDir_Left;
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesFull;
    style.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_None;
    style.HoverFlagsForTooltipNav = ImGuiHoveredFlags_None;
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
    style.DockingNodeHasCloseButton = false;
}

}  // namespace

ImVec4 toImVec4(Srgb8 color) noexcept {
    // A DIVISION, never `k * (1 / 255.0F)`: the reciprocal form is bit-unequal for 126 of the 256 bytes.
    return {static_cast<float>(color.r) / 255.0F, static_cast<float>(color.g) / 255.0F,
            static_cast<float>(color.b) / 255.0F, static_cast<float>(color.a) / 255.0F};
}

ImVec4 toImVec4(Rgbaf color) noexcept { return {color.r, color.g, color.b, color.a}; }

std::uint32_t toImU32(Srgb8 color) noexcept { return IM_COL32(color.r, color.g, color.b, color.a); }

EditorStyleColors editorStyleColors(const ThemePalette& palette) {
    const ThemePalette& p = palette;
    // spec section 6.4's table, in ImGuiCol order. The alpha-bearing entries are the ONLY colours in the
    // editor not sampled from the mock -- which is opaque wherever it is visible -- and each says why.
    return EditorStyleColors{{
        {ImGuiCol_Text, p.text},
        {ImGuiCol_TextDisabled, p.textDisabled},
        {ImGuiCol_WindowBg, p.panel},
        {ImGuiCol_ChildBg, withAlpha(p.panel, 0)},  // transparent: a child shows the window it sits in
        {ImGuiCol_PopupBg, p.raised},
        {ImGuiCol_Border, p.border},
        {ImGuiCol_BorderShadow, withAlpha(p.canvas, 0)},  // no shadow under a border
        {ImGuiCol_FrameBg, p.inset},
        {ImGuiCol_FrameBgHovered, p.raised},
        {ImGuiCol_FrameBgActive, p.raisedHeader},
        {ImGuiCol_TitleBg, p.chrome},
        {ImGuiCol_TitleBgActive, p.chrome},
        {ImGuiCol_TitleBgCollapsed, p.chrome},
        {ImGuiCol_MenuBarBg, p.panel},
        {ImGuiCol_ScrollbarBg, withAlpha(p.panel, 0)},  // transparent: the bar floats over its panel
        {ImGuiCol_ScrollbarGrab, p.active},
        {ImGuiCol_ScrollbarGrabHovered, p.textDisabled},
        {ImGuiCol_ScrollbarGrabActive, p.textMuted},
        // D13: a RadioButton draws its dot in CheckMark on FrameBg (imgui_widgets.cpp:1407), so CheckMark is
        // the accent -- onAccent would make every radio dot invisible -- and a checked box, filled with
        // CheckboxSelectedBg (:1298), is a solid accent square with its tick on top.
        {ImGuiCol_CheckMark, p.accent},
        {ImGuiCol_CheckboxSelectedBg, p.accent},
        {ImGuiCol_SliderGrab, p.accent},
        {ImGuiCol_SliderGrabActive, p.textBright},
        {ImGuiCol_Button, p.raised},
        {ImGuiCol_ButtonHovered, p.hover},
        // D13: two sites read ButtonActive as an EMPHASIS fill under light text (the Material panel's Apply
        // and the viewport's active tool) -- the mock's light-text emphasis surface, never an accent fill.
        {ImGuiCol_ButtonActive, p.active},
        {ImGuiCol_Header, p.selection},
        {ImGuiCol_HeaderHovered, p.hover},
        {ImGuiCol_HeaderActive, p.active},
        {ImGuiCol_Separator, p.divider},
        {ImGuiCol_SeparatorHovered, p.accentBorder},
        {ImGuiCol_SeparatorActive, p.accent},
        {ImGuiCol_ResizeGrip, p.border},
        {ImGuiCol_ResizeGripHovered, p.accentBorder},
        {ImGuiCol_ResizeGripActive, p.accent},
        {ImGuiCol_InputTextCursor, p.text},
        {ImGuiCol_TabHovered, p.hover},
        {ImGuiCol_Tab, p.chrome},
        {ImGuiCol_TabSelected, p.panel},
        {ImGuiCol_TabSelectedOverline, p.accent},
        {ImGuiCol_TabDimmed, p.chrome},
        {ImGuiCol_TabDimmedSelected, p.panel},
        {ImGuiCol_TabDimmedSelectedOverline, p.accentBorder},
        {ImGuiCol_DockingPreview, withAlpha(p.accent, 102)},  // 40 %: the layout shows through the preview
        {ImGuiCol_DockingEmptyBg, p.canvas},
        {ImGuiCol_PlotLines, p.textSecondary},
        {ImGuiCol_PlotLinesHovered, p.warning},
        {ImGuiCol_PlotHistogram, p.accent},
        {ImGuiCol_PlotHistogramHovered, p.warning},
        {ImGuiCol_TableHeaderBg, p.raised},
        {ImGuiCol_TableBorderStrong, p.border},
        {ImGuiCol_TableBorderLight, p.divider},
        {ImGuiCol_TableRowBg, withAlpha(p.panel, 0)},    // transparent: a row shows its panel
        {ImGuiCol_TableRowBgAlt, withAlpha(p.text, 8)},  // a 3 % lift for every other row
        {ImGuiCol_TextLink, p.accent},
        {ImGuiCol_TextSelectedBg, withAlpha(p.accent, 89)},  // 35 %: selected text stays readable
        {ImGuiCol_TreeLines, p.divider},
        {ImGuiCol_DragDropTarget, p.accent},
        {ImGuiCol_DragDropTargetBg, withAlpha(p.accent, 31)},  // 12 %: a tint, not a fill
        {ImGuiCol_UnsavedMarker, p.warning},                   // D13: the mock's dirty dot is the warning hue
        {ImGuiCol_NavCursor, p.accent},
        {ImGuiCol_NavWindowingHighlight, withAlpha(p.accent, 179)},  // 70 %
        {ImGuiCol_NavWindowingDimBg, withAlpha(p.canvas, 128)},      // 50 %: a dim, not a blackout
        {ImGuiCol_ModalWindowDimBg, withAlpha(p.canvas, 153)},       // 60 %: a modal dims what is behind it
    }};
}

ImGuiStyle buildEditorStyle(const EditorTheme& theme, float uiScale) {
    ImGuiStyle style{};  // its constructor calls StyleColorsDark(this), which needs no context
    const ThemeMetrics& m = theme.metrics;
    style.Alpha = m.alpha;
    style.DisabledAlpha = m.disabledAlpha;
    style.WindowPadding = toImVec2(m.windowPadding);
    style.WindowRounding = m.windowRounding;
    style.WindowBorderSize = m.windowBorderSize;
    style.WindowBorderHoverPadding = m.windowBorderHoverPadding;
    style.WindowMinSize = toImVec2(m.windowMinSize);
    style.WindowTitleAlign = toImVec2(m.windowTitleAlign);
    style.ChildRounding = m.childRounding;
    style.ChildBorderSize = m.childBorderSize;
    style.PopupRounding = m.popupRounding;
    style.PopupBorderSize = m.popupBorderSize;
    style.FramePadding = toImVec2(m.framePadding);
    style.FrameRounding = m.frameRounding;
    style.FrameBorderSize = m.frameBorderSize;
    style.ItemSpacing = toImVec2(m.itemSpacing);
    style.ItemInnerSpacing = toImVec2(m.itemInnerSpacing);
    style.CellPadding = toImVec2(m.cellPadding);
    style.TouchExtraPadding = toImVec2(m.touchExtraPadding);
    style.IndentSpacing = m.indentSpacing;
    style.ColumnsMinSpacing = m.columnsMinSpacing;
    style.ScrollbarSize = m.scrollbarSize;
    style.ScrollbarRounding = m.scrollbarRounding;
    style.ScrollbarPadding = m.scrollbarPadding;
    style.GrabMinSize = m.grabMinSize;
    style.GrabRounding = m.grabRounding;
    style.LogSliderDeadzone = m.logSliderDeadzone;
    style.ImageRounding = m.imageRounding;
    style.ImageBorderSize = m.imageBorderSize;
    style.TabRounding = m.tabRounding;
    style.TabBorderSize = m.tabBorderSize;
    style.TabMinWidthBase = m.tabMinWidthBase;
    style.TabMinWidthShrink = m.tabMinWidthShrink;
    style.TabCloseButtonMinWidthSelected = m.tabCloseButtonMinWidthSelected;
    style.TabCloseButtonMinWidthUnselected = m.tabCloseButtonMinWidthUnselected;
    style.TabBarBorderSize = m.tabBarBorderSize;
    style.TabBarOverlineSize = m.tabBarOverlineSize;
    // The constructor's own expression, 35 * (IM_PI / 180): std::numbers::pi_v<float> is the same float as
    // IM_PI, which lives in imgui_internal.h -- a header this TU has no other reason to include.
    style.TableAngledHeadersAngle = m.tableAngledHeadersAngleDegrees * (std::numbers::pi_v<float> / 180.0F);
    style.TableAngledHeadersTextAlign = toImVec2(m.tableAngledHeadersTextAlign);
    style.TreeLinesSize = m.treeLinesSize;
    style.TreeLinesRounding = m.treeLinesRounding;
    style.DragDropTargetRounding = m.dragDropTargetRounding;
    style.DragDropTargetBorderSize = m.dragDropTargetBorderSize;
    style.DragDropTargetPadding = m.dragDropTargetPadding;
    style.ColorMarkerSize = m.colorMarkerSize;
    style.ButtonTextAlign = toImVec2(m.buttonTextAlign);
    style.SelectableTextAlign = toImVec2(m.selectableTextAlign);
    style.SeparatorSize = m.separatorSize;
    style.SeparatorTextBorderSize = m.separatorTextBorderSize;
    style.SeparatorTextAlign = toImVec2(m.separatorTextAlign);
    style.SeparatorTextPadding = toImVec2(m.separatorTextPadding);
    style.DisplayWindowPadding = toImVec2(m.displayWindowPadding);
    style.DisplaySafeAreaPadding = toImVec2(m.displaySafeAreaPadding);
    style.DockingNodeHasCloseButton = m.dockingNodeHasCloseButton;
    style.DockingSeparatorSize = m.dockingSeparatorSize;
    style.MouseCursorScale = m.mouseCursorScale;
    style.AntiAliasedLines = m.antiAliasedLines;
    style.AntiAliasedLinesUseTex = m.antiAliasedLinesUseTex;
    style.AntiAliasedFill = m.antiAliasedFill;
    style.CurveTessellationTol = m.curveTessellationTol;
    style.CircleTessellationMaxError = m.circleTessellationMaxError;
    style.HoverStationaryDelay = m.hoverStationaryDelay;
    style.HoverDelayShort = m.hoverDelayShort;
    style.HoverDelayNormal = m.hoverDelayNormal;
    // WindowMenuButtonPosition, ColorButtonPosition, TreeLinesFlags and the two HoverFlagsForTooltip* are
    // behaviour flags, not visual tokens: they stay at the constructor's values.
    for (const auto& [slot, color] : editorStyleColors(theme.palette)) {
        style.Colors[slot] = toImVec4(color);
    }
    style.ScaleAllSizes(uiScale);  // on a FRESH style, so it never compounds (imgui.cpp:1599-1601)
    // THE FLOOR: a size that is >= 1 at 1x is never truncated below 1 (FLOORED_SIZES' comment).
    for (const FlooredSize& floored : FLOORED_SIZES) {
        if (m.*floored.theme >= 1.0F) {
            style.*floored.style = std::max(style.*floored.style, 1.0F);
        }
    }
    style.FontSizeBase = theme.type.bodySize;
    style.FontScaleMain = 1.0F;
    style.FontScaleDpi = uiScale;  // the ONE stored copy of the UI scale (D8)
    return style;
}

void applyEditorStyle(float uiScale) { ImGui::GetStyle() = buildEditorStyle(EDITOR_THEME, uiScale); }

float currentUiScale() { return ImGui::GetStyle().FontScaleDpi; }

EditorStyleSnapshot snapshotEditorStyle(float uiScale) {
    const PrivateContext scope;
    applyEditorStyle(uiScale);
    return snapshotOf(ImGui::GetStyle(), ImGui::GetIO());
}

EditorStyleSnapshot snapshotStyleAfter(std::span<const float> scalesInOrder) {
    const PrivateContext scope;
    for (const float scale : scalesInOrder) {
        applyEditorStyle(scale);
    }
    return snapshotOf(ImGui::GetStyle(), ImGui::GetIO());
}

EditorStyleSnapshot snapshotStyleBuiltOverPoison(float uiScale) {
    const PrivateContext scope;
    poisonStyle(ImGui::GetStyle());
    applyEditorStyle(uiScale);
    return snapshotOf(ImGui::GetStyle(), ImGui::GetIO());
}

EditorStyleSnapshot snapshotLiveStyle() { return snapshotOf(ImGui::GetStyle(), ImGui::GetIO()); }

}  // namespace engine::editor
