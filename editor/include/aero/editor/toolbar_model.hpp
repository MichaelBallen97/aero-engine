#pragma once
// Aero Engine — the toolbar's PURE half (task E.6.2, D6, D8, D9, D19, D20). No ImGui, no SDL, no GPU: the one
// ImGui TU (editor/src/shell_chrome_ui.cpp) MEASURES with ImGui, calls these, and draws their answer, so every
// decision here is a tier-0 table (toolbar_model_test.cpp). Every user-facing string the toolbar draws is a
// constant here, 7-bit except through the AERO_GLYPH_* / AERO_ICON_* macros (GL6).
#include <aero/editor/asset_view.hpp>     // TextWidth, elideCaptionRight
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_DEGREE
#include <aero/editor/editor_icons.hpp>   // the Tools group of the roster
#include <aero/editor/gizmo.hpp>          // TransformTool, GizmoOperation, SnapSettings
#include <aero/editor/shortcut_hint.hpp>  // HostOs, for the snap tooltip's modifier

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

// ---- the tables (D6, D8) ------------------------------------------------------------------------------------
struct ToolbarTool {
    TransformTool tool;
    std::string_view icon;       // Compact: the icon alone
    std::string_view iconLabel;  // Full: icon + label
    std::string_view tooltip;    // names the key; a bare letter needs no chordHint
};
inline constexpr std::array<ToolbarTool, 4> TOOLBAR_TOOLS{{
    {TransformTool::Select, AERO_ICON_MOUSE_POINTER_2, AERO_ICON_MOUSE_POINTER_2 " Select", "Select (Q)"},
    {TransformTool::Move, AERO_ICON_MOVE, AERO_ICON_MOVE " Move", "Move (W)"},
    {TransformTool::Rotate, AERO_ICON_ROTATE_CW, AERO_ICON_ROTATE_CW " Rotate", "Rotate (E)"},
    {TransformTool::Scale, AERO_ICON_SCALING, AERO_ICON_SCALING " Scale", "Scale (R)"},
}};

inline constexpr std::string_view LOCAL_SEGMENT_LABEL = "Local";
inline constexpr std::string_view WORLD_SEGMENT_LABEL = "World";
inline constexpr std::string_view LOCAL_SEGMENT_TOOLTIP = "Local space (X toggles)";
inline constexpr std::string_view WORLD_SEGMENT_TOOLTIP = "World space (X toggles)";
inline constexpr std::string_view SCALE_IS_LOCAL_TOOLTIP = "Scale is always Local";  // 2.3.3 AC-4's wording
inline constexpr std::string_view SNAP_TOGGLE_ICON = AERO_ICON_MAGNET;
inline constexpr std::string_view SNAP_TOGGLE_LABEL = AERO_ICON_MAGNET " Snap";
inline constexpr std::string_view SNAP_FIELD_SELECT_TOOLTIP = "Select draws no gizmo";

struct ToolbarPlayControl {
    std::string_view icon;
    std::string_view iconLabel;
};
inline constexpr std::array<ToolbarPlayControl, 3> TOOLBAR_PLAY_CONTROLS{{
    {AERO_ICON_PLAY, AERO_ICON_PLAY " Play"},
    {AERO_ICON_PAUSE, AERO_ICON_PAUSE " Pause"},
    {AERO_ICON_STEP_FORWARD, AERO_ICON_STEP_FORWARD " Step"},
}};
// D8: the convention's first live use since 2.5.1 deleted the last stub -- 2970d9a's wording, its em dash
// spelled " -- ". docs/tasks/phase-4.md's 4.7.1 lists Play, Pause and Step, which is what makes it true.
inline constexpr std::string_view PLAY_CONTROLS_TOOLTIP = "Not implemented yet -- task 4.7.1";

inline constexpr std::string_view UNDO_BUTTON_LABEL = AERO_ICON_UNDO_2 " Undo";
inline constexpr std::string_view NOTHING_TO_UNDO_TOOLTIP = "Nothing to undo";

// Font multiples (D9, D20 -- "a width that holds text is a font multiple"): the undo label's FIXED slot, so the
// toolbar's mode can never change with the user's last action, and the snap step field's FLOOR, which holds
// "0.001 m". The chrome widens the field to SNAP_FIELD_WIDEST_TEXT measured in Mono, the face it draws in.
inline constexpr float UNDO_LABEL_SLOT_EM = 10.0F;
inline constexpr float SNAP_FIELD_WIDTH_EM = 4.0F;
// The widest text a step format can produce, in code points: "%.4g m" of a value in [0.001, 0.01) with four
// significant digits ("0.001234 m"). Every Plex Mono glyph has one advance, so ANY ten code points measure it; TB12
// pins that no step value formats wider.
inline constexpr std::string_view SNAP_FIELD_WIDEST_TEXT = "0.000000 m";

// "Snap while dragging (hold Cmd to invert)" on macOS, "... Ctrl ..." elsewhere (D7, D10).
[[nodiscard]] std::string snapToggleTooltip(HostOs os);

// ---- the snap field (D7) -------------------------------------------------------------------------------------
// The operation whose step the field edits: the tool's own, and Move's under Select (the field is disabled there).
[[nodiscard]] GizmoOperation snapFieldOperation(TransformTool tool) noexcept;
// THE ONE SOURCE of the field's format and value: "%.4g m", "%.4g" AERO_GLYPH_DEGREE, "%.4g" -- so 1000 reads
// "1000", never "1e+03". NUL-terminated literals, because ImGui::DragFloat takes a const char*.
[[nodiscard]] const char* snapStepFormat(TransformTool tool) noexcept;
[[nodiscard]] float snapStepValue(TransformTool tool, const SnapSettings& snap) noexcept;
// What DragFloat draws for that pair: the same format through std::snprintf (ImGui's DataTypeFormatString is
// ImFormatString, vsnprintf, here -- the tree defines no IMGUI_USE_STB_SPRINTF).
[[nodiscard]] std::string snapStepText(TransformTool tool, const SnapSettings& snap);

// ---- the undo affordance (D9) --------------------------------------------------------------------------------
// The label as drawn in a slot `slotWidth` wide: "" with nothing to undo; the label VERBATIM when it fits (a '%'
// or a '##' is text -- the chrome draws it with TextUnformatted); else right-elided on a code-point boundary.
[[nodiscard]] std::string undoAffordanceLabel(std::string_view undoLabel, float slotWidth, const TextWidth& measure);

// ---- the bars' heights (D19) ---------------------------------------------------------------------------------
// The ONE conversion of a bar height to points: round(heightDp x uiScale) -- a WHOLE point, because
// BeginViewportSideBar insets the raw float while Begin truncates the window's Pos and SizeFull (imgui.cpp:9128,
// :9182), so 26 x 1.25 = 32.5 would leave a one-point unpainted row. A non-finite or non-positive scale is 1.
[[nodiscard]] float shellBarHeight(float heightDp, float uiScale) noexcept;

// ---- the layout (D20) ----------------------------------------------------------------------------------------
// What the chrome MEASURED, in ImGui units (already scaled). The undo LABEL is deliberately absent: the undo
// group's width uses the fixed slot, never the label (seed S45).
struct ToolbarMetrics {
    std::array<float, 4> toolFull{};     // each tool button, icon + label
    std::array<float, 4> toolCompact{};  // icon only
    float localSegment = 0.0F;
    float worldSegment = 0.0F;
    float snapToggleFull = 0.0F;
    float snapToggleCompact = 0.0F;
    float snapField = 0.0F;
    std::array<float, 3> playFull{};
    std::array<float, 3> playCompact{};
    float undoButton = 0.0F;
    float undoChip = 0.0F;
    float itemSpacing = 0.0F;   // style.ItemSpacing.x
    float groupPadding = 0.0F;  // EDITOR_THEME.shell.toolbarGroupPadding x the UI scale
    float fontSize = 0.0F;      // the body font's size, scaled -- the slot's em
    bool operator==(const ToolbarMetrics&) const noexcept = default;
};

// Each group's outer width.
struct ToolbarWidths {
    float tools = 0.0F;
    float space = 0.0F;
    float snap = 0.0F;
    float play = 0.0F;
    float undo = 0.0F;
    bool operator==(const ToolbarWidths&) const noexcept = default;
};
struct ToolbarWidthPair {
    ToolbarWidths full;
    ToolbarWidths compact;
};
// The groups' widths: a group of abutting buttons is 2 x groupPadding + its buttons; the snap group is that
// around the toggle, + itemSpacing + the field; the play controls are three buttons itemSpacing apart; the undo
// group is button + itemSpacing + chip, + itemSpacing + UNDO_LABEL_SLOT_EM x fontSize in Full only.
[[nodiscard]] ToolbarWidthPair toolbarWidths(const ToolbarMetrics& metrics) noexcept;

// Full: icons + labels, the undo slot. Compact: icons only, no undo slot, the chip kept. Minimal: Compact without
// the play group (it is disabled and carries no action). The WIDEST that fits; at the exact boundary both fit.
enum class ToolbarMode : std::uint8_t { Full = 0, Compact, Minimal };

// Every x is BAR-LOCAL (from the toolbar window's left edge). The left groups pack from toolbarPaddingX; the undo
// group sits at max(availableRight - undo, leftGroupsEnd + gap) -- right-aligned while it clears the left groups,
// packed after them (and clipped by the window) otherwise; the play group, when drawn, is CENTRED in the free span
// [leftGroupsEnd + gap, undoX - gap] (the mock's placement).
struct ToolbarLayout {
    ToolbarMode mode = ToolbarMode::Minimal;
    float toolsX = 0.0F;
    float spaceX = 0.0F;
    float snapX = 0.0F;
    float leftGroupsEnd = 0.0F;
    bool playDrawn = false;
    float playX = 0.0F;
    float undoX = 0.0F;
    bool operator==(const ToolbarLayout&) const noexcept = default;
};
// `uiScale` is NON-DEFAULTED and multiplies toolbarGroupGap and toolbarPaddingX exactly once (US9); a NaN or
// negative width is sanitised to 0 first (std::max returns NaN when its first argument is NaN).
[[nodiscard]] ToolbarLayout toolbarLayout(float barWidth, const ToolbarWidths& full, const ToolbarWidths& compact,
                                          float uiScale) noexcept;

}  // namespace engine::editor
