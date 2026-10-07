// Aero Engine — the shell's chrome (task E.6.2): the toolbar, the status bar and the breadcrumb. The ONE ImGui TU
// of the three strips. It MEASURES with ImGui, asks the pure models, and DRAWS their answer; it decides nothing a
// tier-0 case cannot see. Every colour is an EDITOR_THEME role, read once in chromeColors() (I287 pins the reads);
// every length is a theme metric times the UI scale; every user-derived string is drawn with TextUnformatted,
// AddText or a "%s" format (I307).
#include "shell_chrome_ui.hpp"

#include <aero/editor/breadcrumb.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/shell_chrome_record.hpp>
#include <aero/editor/shortcut_hint.hpp>
#include <aero/editor/status_bar.hpp>
#include <aero/editor/toolbar_model.hpp>

#include "editor_fonts.hpp"        // editorFonts(): Body, Strong, Mono
#include "editor_theme_imgui.hpp"  // toImVec4
#include "editor_theme_ui.hpp"     // currentUiScale

#include <algorithm>
#include <cfloat>  // FLT_MAX -- ImFont::CalcTextSizeA's "no limit" width
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>  // BeginViewportSideBar, GetTopMostPopupModal, IsWindowNavFocusable, the NoFocus flag
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// D1: both bars. NoSavedSettings is NOT forced by BeginViewportSideBar (imgui_widgets.cpp:9278) -- without it a
// bar enters aero_editor.ini. NoNavFocus keeps Ctrl+Tab, which runs even with NavEnableKeyboard off
// (imgui.cpp:15334), from listing either bar as "(Untitled)"; NoNavInputs keeps Tab-to-focus, which is always on
// (imgui.cpp:14861), out of the snap field after a click on the bar.
constexpr ImGuiWindowFlags BAR_FLAGS = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoNavInputs;

// EVERY palette role this TU reads, each read ONCE, here -- D19's map, and the exact set I287 pins.
struct ChromeColors {
    ImVec4 chrome;
    ImVec4 raised;
    ImVec4 border;
    ImVec4 active;
    ImVec4 textBright;
    ImVec4 accent;
    ImVec4 onAccent;
    ImVec4 textSecondary;
    ImVec4 textMuted;
    ImVec4 textLabel;
    ImVec4 text;
    ImVec4 textFaint;
    ImVec4 warning;
    ImVec4 divider;
};

[[nodiscard]] ChromeColors chromeColors() noexcept {
    return ChromeColors{.chrome = toImVec4(EDITOR_THEME.palette.chrome),
                        .raised = toImVec4(EDITOR_THEME.palette.raised),
                        .border = toImVec4(EDITOR_THEME.palette.border),
                        .active = toImVec4(EDITOR_THEME.palette.active),
                        .textBright = toImVec4(EDITOR_THEME.palette.textBright),
                        .accent = toImVec4(EDITOR_THEME.palette.accent),
                        .onAccent = toImVec4(EDITOR_THEME.palette.onAccent),
                        .textSecondary = toImVec4(EDITOR_THEME.palette.textSecondary),
                        .textMuted = toImVec4(EDITOR_THEME.palette.textMuted),
                        .textLabel = toImVec4(EDITOR_THEME.palette.textLabel),
                        .text = toImVec4(EDITOR_THEME.palette.text),
                        .textFaint = toImVec4(EDITOR_THEME.palette.textFaint),
                        .warning = toImVec4(EDITOR_THEME.palette.warning),
                        .divider = toImVec4(EDITOR_THEME.palette.divider)};
}

[[nodiscard]] Vec2 toVec2(ImVec2 v) noexcept { return Vec2{v.x, v.y}; }
[[nodiscard]] ImU32 packed(const ImVec4& colour) { return ImGui::ColorConvertFloat4ToU32(colour); }

// The width of `text` in the CURRENT font -- the one measurer the layouts are handed.
[[nodiscard]] float currentTextWidth(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

// A button's width for `label` in the current font: ButtonEx's own size, text + 2 x FramePadding.x.
[[nodiscard]] float buttonWidth(std::string_view label) {
    return currentTextWidth(label) + (2.0F * ImGui::GetStyle().FramePadding.x);
}

// Shown on hover, disabled or not, at once -- the editor's per-item idiom (shell_ui.cpp's ioTooltip), never
// SetItemTooltip's delayed path. "%s": a tooltip string is never a format string.
void itemTooltip(std::string_view text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", std::string(text).c_str());
    }
}

void recordItem(ToolbarButtonRecord& record, bool drawnActive) {
    record.min = toVec2(ImGui::GetItemRectMin());
    record.max = toVec2(ImGui::GetItemRectMax());
    record.drawnActive = drawnActive;
    record.enabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) == 0;
}

// D19 (a)-(d) and D1: one button INSIDE a `raised` group. No border of its own -- the theme's frameBorderSize
// would draw a ring inside the group's outline, so FrameBorderSize 0 is pushed around THIS Button alone. Clicking
// it leaves the keyboard where it was: ImGuiItemFlags_NoFocus makes ButtonBehavior skip both FocusWindow arms
// (imgui_widgets.cpp:553-554), so the Hierarchy's Delete / Cmd+D / F2 keep working after a tool click. ACTIVE
// pushes all three button slots and the text slot together: onAccent on `hover` is 1.34:1 and on `active` 1.49:1,
// so a Button-only push would lose the pair on hover and press (TH13). Inactive keeps Button = raised.
[[nodiscard]] bool groupButton(const char* label, bool active, const ImVec4& activeFill, const ImVec4& activeText,
                               const ImVec4& idleText, ToolbarButtonRecord& record) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeFill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeFill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeFill);
        ImGui::PushStyleColor(ImGuiCol_Text, activeText);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, idleText);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true);
    const bool pressed = ImGui::Button(label);
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(active ? 4 : 1);
    recordItem(record, active);
    return pressed;
}

// A group's `raised` body and its 1-dp `border`, drawn BEHIND its buttons (submission order is draw order).
void groupBackground(ImDrawList* list, float x, float y, float width, float height, const ChromeColors& c) {
    const float rounding = ImGui::GetStyle().FrameRounding;
    list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), packed(c.raised), rounding);
    list->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), packed(c.border), rounding, 0,
                  ImGui::GetStyle().FrameBorderSize);
}

// A rule `ruleThickness` x the UI scale thick, floored at one device pixel.
[[nodiscard]] float ruleThickness(float uiScale) {
    const float pixel = 1.0F / std::max(ImGui::GetIO().DisplayFramebufferScale.y, 1.0F);
    return std::max(EDITOR_THEME.shell.ruleThickness * uiScale, pixel);
}

// Opens a side bar with the chrome's own window style (1:1 pushes, popped right after Begin, which has read them).
// The CALLER calls ImGui::End() unconditionally: BeginViewportSideBar does not (only BeginMainMenuBar wraps it).
[[nodiscard]] bool beginBar(const char* name, ImGuiDir dir, float height, const ChromeColors& c) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, c.chrome);
    const bool open = ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), dir, height, BAR_FLAGS);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    return open;
}

}  // namespace

void drawToolbar(ShellUiState& state, const CommandStack& commands, bool fileEnabled) {
    ShellChromeRecord discard;
    ShellChromeRecord& rec = state.chromeRecord != nullptr ? *state.chromeRecord : discard;
    // A null tool state draws the tool, space and snap groups DISABLED, writing a scratch copy -- never a crash.
    TransformToolState scratch;
    const bool haveTools = state.tools != nullptr;
    TransformToolState& tools = haveTools ? *state.tools : scratch;
    const ChromeColors c = chromeColors();
    const float s = currentUiScale();
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ++rec.framesRecorded;
    rec.uiScaleAtDraw = s;
    rec.viewportPos = toVec2(viewport->Pos);
    rec.viewportSize = toVec2(viewport->Size);
    rec.workPos = toVec2(viewport->WorkPos);
    rec.workSize = toVec2(viewport->WorkSize);

    const float height = shellBarHeight(toolbarHeightDp(EDITOR_THEME), s);  // a WHOLE point (D1, TB11)
    rec.toolbarAxis = height;
    const bool open = beginBar("##AeroToolbar", ImGuiDir_Up, height, c);
    rec.toolbarDrawn = open;
    if (open) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        rec.toolbarMin = toVec2(pos);
        rec.toolbarMax = Vec2{pos.x + size.x, pos.y + size.y};
        rec.toolbarNavFocusable = ImGui::IsWindowNavFocusable(ImGui::GetCurrentWindow());
        ImDrawList* const list = ImGui::GetWindowDrawList();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float fontSize = ImGui::GetFontSize();
        const float frame = ImGui::GetFrameHeight();
        const float pad = EDITOR_THEME.shell.toolbarGroupPadding * s;
        const float groupTop = pos.y + (EDITOR_THEME.shell.toolbarPaddingY * s);
        const float groupHeight = frame + (2.0F * pad);
        const float buttonY = groupTop + pad;
        // A modal blocks every click on this bar (imgui.cpp:5620-5623), so it blocks every request too (D21).
        const bool noModal = ImGui::GetTopMostPopupModal() == nullptr;
        const std::string chip = chordHint(currentHostOs(), ChordModifiers{.ctrl = true}, "Z");

        // ---- measure (the layout's exact inputs, recorded) ------------------------------------------------
        ToolbarMetrics metrics{};
        for (std::size_t i = 0; i < TOOLBAR_TOOLS.size(); ++i) {
            metrics.toolFull[i] = buttonWidth(TOOLBAR_TOOLS[i].iconLabel);
            metrics.toolCompact[i] = buttonWidth(TOOLBAR_TOOLS[i].icon);
        }
        metrics.localSegment = buttonWidth(LOCAL_SEGMENT_LABEL);
        metrics.worldSegment = buttonWidth(WORLD_SEGMENT_LABEL);
        metrics.snapToggleFull = buttonWidth(SNAP_TOGGLE_LABEL);
        metrics.snapToggleCompact = buttonWidth(SNAP_TOGGLE_ICON);
        for (std::size_t i = 0; i < TOOLBAR_PLAY_CONTROLS.size(); ++i) {
            metrics.playFull[i] = buttonWidth(TOOLBAR_PLAY_CONTROLS[i].iconLabel);
            metrics.playCompact[i] = buttonWidth(TOOLBAR_PLAY_CONTROLS[i].icon);
        }
        metrics.undoButton = buttonWidth(UNDO_BUTTON_LABEL);
        // The chip's face (D9), at an UNSCALED size -- never GetFontSize().
        ImGui::PushFont(editorFonts().mono, EDITOR_THEME.type.smallSize);
        metrics.undoChip = buttonWidth(chip);
        const float monoFrame = ImGui::GetFrameHeight();
        // The step field draws in Mono too, so its width is measured in Mono: a FLOOR of SNAP_FIELD_WIDTH_EM Body
        // em, widened to the widest text its formats produce. Body and Mono sizes round independently (IM_ROUND),
        // so at a fractional UI scale a Body-em width alone clips a ten-glyph "0.001234 m".
        const float widestStep = currentTextWidth(SNAP_FIELD_WIDEST_TEXT);
        ImGui::PopFont();
        metrics.snapField = std::max(SNAP_FIELD_WIDTH_EM * fontSize, widestStep);
        rec.snapFieldTextWidth = widestStep;
        metrics.itemSpacing = style.ItemSpacing.x;
        metrics.groupPadding = pad;
        metrics.fontSize = fontSize;
        const ToolbarWidthPair widths = toolbarWidths(metrics);
        const float available = size.x;
        const ToolbarLayout layout = toolbarLayout(available, widths.full, widths.compact, s);
        const bool full = layout.mode == ToolbarMode::Full;
        const ToolbarWidths& w = full ? widths.full : widths.compact;
        rec.toolbarAvailableWidth = available;
        rec.metrics = metrics;
        rec.fullWidths = widths.full;
        rec.compactWidths = widths.compact;
        rec.layout = layout;

        // ---- the tools (D6: always live; Q W E R) -----------------------------------------------------------
        float groupX = pos.x + layout.toolsX;
        groupBackground(list, groupX, groupTop, w.tools, groupHeight, c);
        ImGui::SetCursorScreenPos(ImVec2(groupX + pad, buttonY));
        const std::optional<TransformTool> toolRequest = state.toolbarToolRequest;
        const bool toolsEnabled = haveTools;  // D21: the tools' predicate -- live whenever there is a state
        ImGui::BeginDisabled(!toolsEnabled);
        for (std::size_t i = 0; i < TOOLBAR_TOOLS.size(); ++i) {
            const ToolbarTool& row = TOOLBAR_TOOLS[i];
            if (i > 0U) {
                ImGui::SameLine(0.0F, 0.0F);  // in-group buttons abut (the mock)
            }
            const char* const label = full ? row.iconLabel.data() : row.icon.data();
            const bool active = tools.mode.tool == row.tool;
            ImGui::PushID(static_cast<int>(i));
            const bool pressed = groupButton(label, active, c.accent, c.onAccent, c.textSecondary, rec.tools[i]);
            ImGui::PopID();
            itemTooltip(row.tooltip);
            const bool requested = toolRequest == row.tool;
            if (pressed || (requested && toolsEnabled && noModal)) {
                tools.mode.tool = row.tool;
            }
        }
        ImGui::EndDisabled();

        // ---- the space (D6: segmented; Scale shows Local, disabled) ------------------------------------------
        groupX = pos.x + layout.spaceX;
        groupBackground(list, groupX, groupTop, w.space, groupHeight, c);
        ImGui::SetCursorScreenPos(ImVec2(groupX + pad, buttonY));
        // The space Manipulate receives, by the SAME rule (effectiveSpace): the segment can never show a space the
        // gizmo does not use. Select draws no gizmo, so it shows the stored space.
        const std::optional<GizmoOperation> toolOperation = gizmoOperationFor(tools.mode.tool);
        const bool scaleForcesLocal =
            toolOperation.has_value() && effectiveSpace(*toolOperation, GizmoSpace::World) == GizmoSpace::Local;
        const GizmoSpace shown =
            toolOperation.has_value() ? effectiveSpace(*toolOperation, tools.mode.space) : tools.mode.space;
        const std::optional<GizmoSpace> spaceRequest = state.toolbarSpaceRequest;
        const bool spaceEnabled = haveTools && !scaleForcesLocal;  // D21: the segments' predicate
        ImGui::BeginDisabled(!spaceEnabled);
        for (std::size_t k = 0; k < rec.space.size(); ++k) {
            const GizmoSpace segment = k == 0U ? GizmoSpace::Local : GizmoSpace::World;
            if (k > 0U) {
                ImGui::SameLine(0.0F, 0.0F);
            }
            const std::string_view label = k == 0U ? LOCAL_SEGMENT_LABEL : WORLD_SEGMENT_LABEL;
            const bool pressed =
                groupButton(label.data(), shown == segment, c.active, c.textBright, c.textSecondary, rec.space[k]);
            itemTooltip(scaleForcesLocal ? SCALE_IS_LOCAL_TOOLTIP
                                         : (k == 0U ? LOCAL_SEGMENT_TOOLTIP : WORLD_SEGMENT_TOOLTIP));
            const bool requested = spaceRequest == segment;
            if (pressed || (requested && spaceEnabled && noModal)) {
                tools.mode.space = segment;
            }
        }
        ImGui::EndDisabled();

        // ---- snap (D7): the toggle in its group, then the active tool's step field ---------------------------
        groupX = pos.x + layout.snapX;
        const float toggleWidth = full ? metrics.snapToggleFull : metrics.snapToggleCompact;
        groupBackground(list, groupX, groupTop, (2.0F * pad) + toggleWidth, groupHeight, c);
        ImGui::SetCursorScreenPos(ImVec2(groupX + pad, buttonY));
        ImGui::BeginDisabled(!haveTools);  // D21: the toggle's predicate -- live whenever there is a state
        const char* const toggleLabel = full ? SNAP_TOGGLE_LABEL.data() : SNAP_TOGGLE_ICON.data();
        const bool togglePressed =
            groupButton(toggleLabel, tools.snap.enabled, c.active, c.textBright, c.textMuted, rec.snapToggle);
        ImGui::EndDisabled();
        itemTooltip(snapToggleTooltip(currentHostOs()));
        const bool toggleClicked = togglePressed || (state.toolbarSnapToggleRequest && haveTools && noModal);
        if (toggleClicked) {
            tools.snap.enabled = !tools.snap.enabled;
        }
        const TransformTool tool = tools.mode.tool;
        const bool fieldEnabled = haveTools && tool != TransformTool::Select;  // D21: the step's predicate
        const GizmoOperation fieldOperation = snapFieldOperation(tool);
        const SnapRange range = snapStepRange(fieldOperation);
        float fieldValue = snapStepValue(tool, tools.snap);
        rec.snapFormat = snapStepFormat(tool);  // what DragFloat is HANDED (the record's stated exception)
        rec.snapValue = fieldValue;
        rec.snapFieldEnabled = fieldEnabled;
        const float monoY = buttonY + ((frame - monoFrame) * 0.5F);  // a Mono frame centred on the group's buttons
        ImGui::PushFont(editorFonts().mono, EDITOR_THEME.type.smallSize);
        ImGui::SetCursorScreenPos(ImVec2(groupX + (2.0F * pad) + toggleWidth + style.ItemSpacing.x, monoY));
        ImGui::SetNextItemWidth(metrics.snapField);
        ImGui::BeginDisabled(!fieldEnabled);
        // D19: the field is a `raised` box like the chip beside it, not the theme's `inset` input fill; hover and
        // active follow the button slots, so the three states stay distinct.
        ImGui::PushStyleColor(ImGuiCol_FrameBg, c.raised);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, c.active);
        // Speed 0 with Logarithmic moves ~1 % of the log span per pixel (imgui_widgets.cpp:2573), so six decades
        // take ~100 px; double-click or Cmd/Ctrl-click types a value, which snapStepUpdate clamps.
        const bool changed = ImGui::DragFloat("##snapstep", &fieldValue, 0.0F, range.min, range.max,
                                              snapStepFormat(tool), ImGuiSliderFlags_Logarithmic);
        const bool deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::PopFont();
        if (tool == TransformTool::Select) {
            itemTooltip(SNAP_FIELD_SELECT_TOOLTIP);
        }
        const bool stepRequested = state.toolbarSnapStepRequest.has_value() && fieldEnabled && noModal;
        const std::optional<float> stepRequest = stepRequested ? state.toolbarSnapStepRequest : std::nullopt;
        // THE ONE CALL of the pure commit rule, once per frame: a live drag frame changes the step and never
        // commits; the end of the edit, or a request, does (G27).
        const SnapStepUpdate update =
            snapStepUpdate(tools.snap, fieldOperation, changed ? std::optional<float>(fieldValue) : std::nullopt,
                           deactivatedAfterEdit, stepRequest);
        tools.snap = update.next;
        state.snapCommitted = toggleClicked || update.commit;

        // ---- play / pause / step (D8): drawn, disabled, naming the task that owns them -----------------------
        rec.playGroupDrawn = layout.playDrawn;
        if (layout.playDrawn) {
            ImGui::SetCursorScreenPos(ImVec2(pos.x + layout.playX, buttonY));
            ImGui::BeginDisabled();  // ONE pair, whatever the mode: nothing here has an action yet
            for (std::size_t i = 0; i < TOOLBAR_PLAY_CONTROLS.size(); ++i) {
                const ToolbarPlayControl& control = TOOLBAR_PLAY_CONTROLS[i];
                if (i > 0U) {
                    ImGui::SameLine();
                }
                ImGui::Button(full ? control.iconLabel.data() : control.icon.data());
                rec.playDisabled[i] = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", PLAY_CONTROLS_TOOLTIP.data());
                }
            }
            ImGui::EndDisabled();
        }

        // ---- undo (D9): the button, the chip, the current label in its fixed slot -----------------------------
        const float undoX = pos.x + layout.undoX;
        const bool undoEnabled = fileEnabled && commands.canUndo();  // the Edit menu item's predicate, ONCE
        ImGui::SetCursorScreenPos(ImVec2(undoX, buttonY));
        // The disabled pair OUTERMOST, so the FrameBorderSize push/pop sit four lines apart -- I307(b)'s window,
        // groupButton's shape. GetItemFlags() after EndDisabled still reads the button's flags.
        ImGui::BeginDisabled(!undoEnabled);
        ImGui::PushStyleColor(ImGuiCol_Button, c.chrome);
        ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true);
        const bool undoPressed = ImGui::Button(UNDO_BUTTON_LABEL.data());
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        recordItem(rec.undo, false);
        if (!commands.canUndo()) {
            itemTooltip(NOTHING_TO_UNDO_TOOLTIP);
        }
        if (undoPressed || (state.toolbarUndoRequest && undoEnabled && noModal)) {
            state.undoRequested = true;  // the chord's own flag: applied by the ONE applyHistoryRequests
        }
        const float chipX = undoX + metrics.undoButton + style.ItemSpacing.x;
        const float chipY = monoY;
        const ImVec2 chipMin(chipX, chipY);
        const ImVec2 chipMax(chipX + metrics.undoChip, chipY + monoFrame);
        list->AddRectFilled(chipMin, chipMax, packed(c.raised), style.FrameRounding);
        list->AddRect(chipMin, chipMax, packed(c.border), style.FrameRounding, 0, style.FrameBorderSize);
        ImGui::PushFont(editorFonts().mono, EDITOR_THEME.type.smallSize);
        list->AddText(ImVec2(chipX + style.FramePadding.x, chipY + style.FramePadding.y), packed(c.textSecondary),
                      chip.c_str());
        ImGui::PopFont();
        rec.undoChip = chip;
        rec.undoLabel.clear();
        if (full) {
            // COPIED before anything can push, undo or clear (command_stack.hpp:86-87), and drawn as TEXT.
            const std::string label(commands.undoLabel());
            const float slotWidth = UNDO_LABEL_SLOT_EM * fontSize;
            const std::string drawn = undoAffordanceLabel(label, slotWidth, currentTextWidth);
            if (!drawn.empty()) {
                ImGui::SetCursorScreenPos(
                    ImVec2(chipX + metrics.undoChip + style.ItemSpacing.x, buttonY + style.FramePadding.y));
                // De-emphasised ENABLED text: the textMuted role, never the disabled-text slot.
                ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
                ImGui::TextUnformatted(drawn.c_str());
                ImGui::PopStyleColor();
                if (drawn != label && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::EndTooltip();  // only after a true BeginTooltip (imgui.h's contract)
                }
            }
            rec.undoLabel = drawn;
        }

        // ---- the bottom rule, drawn INSIDE the bar on its last row (D19) ----------------------------------------
        const float rule = ruleThickness(s);
        list->AddRectFilled(ImVec2(pos.x, pos.y + size.y - rule), ImVec2(pos.x + size.x, pos.y + size.y),
                            packed(c.divider));
    }
    ImGui::End();  // ALWAYS -- BeginViewportSideBar returns Begin()'s value and never ends the window itself
}

void drawStatusBar(ShellUiState& state) {
    ShellChromeRecord discard;
    ShellChromeRecord& rec = state.chromeRecord != nullptr ? *state.chromeRecord : discard;
    const ChromeColors c = chromeColors();
    const float s = currentUiScale();
    const float height = shellBarHeight(statusBarHeightDp(EDITOR_THEME), s);
    rec.statusAxis = height;
    const bool open = beginBar("##AeroStatusBar", ImGuiDir_Down, height, c);
    rec.statusDrawn = open;
    if (open) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        rec.statusMin = toVec2(pos);
        rec.statusMax = Vec2{pos.x + size.x, pos.y + size.y};
        rec.statusNavFocusable = ImGui::IsWindowNavFocusable(ImGui::GetCurrentWindow());
        ImDrawList* const list = ImGui::GetWindowDrawList();
        const float rule = ruleThickness(s);
        list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + rule), packed(c.divider));  // the top rule
        if (state.statusText != nullptr) {
            const StatusBarText& text = *state.statusText;
            // D12: Plex Mono at the SMALL size -- an unscaled size, which FontScaleDpi scales; never GetFontSize().
            ImGui::PushFont(editorFonts().mono, EDITOR_THEME.type.smallSize);
            rec.statusFont = ImGui::GetFont()->GetDebugName();
            rec.statusFontSize = ImGui::GetFontSize();
            rec.statusBarWidth = size.x;
            const StatusBarLayout layout = statusBarLayout(size.x, text, currentTextWidth, s);
            const float textY = pos.y + ((size.y - ImGui::GetFontSize()) * 0.5F);
            const auto drawText = [textY, &pos](const std::string& value, float x, const ImVec4& colour) {
                ImGui::SetCursorScreenPos(ImVec2(pos.x + x, textY));
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                ImGui::TextUnformatted(value.c_str());
                ImGui::PopStyleColor();
            };
            if (!layout.root.empty()) {
                drawText(layout.root, layout.rootX, c.textMuted);
                if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                    ImGui::TextUnformatted(text.rootTooltip.c_str());  // the full root, never a format
                    ImGui::EndTooltip();
                }
            }
            if (!layout.watch.empty()) {
                drawText(layout.watch, layout.watchX, c.textMuted);
            }
            drawText(layout.frame, layout.frameX, c.textMuted);
            if (!layout.backend.empty()) {
                drawText(layout.backend, layout.backendX, c.accent);
            }
            ImGui::PopFont();
            rec.statusRoot = layout.root;
            rec.statusWatch = layout.watch;
            rec.statusFrame = layout.frame;
            rec.statusBackend = layout.backend;
        }
    }
    ImGui::End();  // ALWAYS (beginBar's note)
}

void drawBreadcrumb(const ProjectSession& project, const SceneSession& session, const CommandStack& commands,
                    ShellChromeRecord* record) {
    ShellChromeRecord discard;
    ShellChromeRecord& rec = record != nullptr ? *record : discard;
    const ChromeColors c = chromeColors();
    const float s = currentUiScale();
    // SCREEN space, read before anything moves the cursor (D11; GetWindowContentRegionMax is obsolete since
    // 1.91.0 -- imgui.cpp:657-658 names this replacement).
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float menusEnd = cursor.x;
    const float barRight = cursor.x + ImGui::GetContentRegionAvail().x;
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    rec.menuMin = toVec2(windowPos);
    rec.menuMax = Vec2{windowPos.x + windowSize.x, windowPos.y + windowSize.y};
    rec.menuNavFocusable = ImGui::IsWindowNavFocusable(ImGui::GetCurrentWindow());
    rec.breadcrumbBarLeft = windowPos.x;
    rec.breadcrumbBarRight = barRight;
    rec.breadcrumbMenusEnd = menusEnd;

    // One frame behind a swap, a save or an undo (D11): this runs before applyFileRequests/applyHistoryRequests.
    const BreadcrumbText text = breadcrumbText(project.name(), session.documentName(), !commands.isClean());
    const float fontSize = ImGui::GetFontSize();
    ImFont* const strong = editorFonts().strong;
    const TextWidth strongWidth = [strong, fontSize](std::string_view t) {
        return strong->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, t.data(), t.data() + t.size()).x;
    };
    const BreadcrumbMeasure measure{.body = currentTextWidth, .strong = strongWidth};
    const BreadcrumbLayout layout = breadcrumbLayout(windowPos.x, barRight, menusEnd, text, measure, s);
    const float textY = cursor.y;
    rec.breadcrumbProject = layout.project;
    rec.breadcrumbScene = layout.scene;
    rec.breadcrumbSeparatorDrawn = layout.separatorDrawn;
    rec.dirtyDotDrawn = layout.dotDrawn;
    rec.breadcrumbProjectFont.clear();
    rec.breadcrumbSceneFont.clear();
    if (!layout.project.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(layout.projectX, textY));
        ImGui::PushStyleColor(ImGuiCol_Text, c.textLabel);
        ImGui::TextUnformatted(layout.project.c_str());  // a project name can hold '%' or '##'
        ImGui::PopStyleColor();
        rec.breadcrumbProjectX = ImGui::GetItemRectMin().x;
        rec.breadcrumbProjectFont = ImGui::GetFont()->GetDebugName();
    }
    if (layout.separatorDrawn) {
        ImGui::SetCursorScreenPos(ImVec2(layout.separatorX, textY));
        ImGui::PushStyleColor(ImGuiCol_Text, c.textFaint);  // decorative, no floor (TH13)
        ImGui::TextUnformatted(text.separator.c_str());
        ImGui::PopStyleColor();
    }
    if (!layout.scene.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(layout.sceneX, textY));
        ImGui::PushFont(strong, 0.0F);  // keep the size: Strong at Body's size (D11)
        ImGui::PushStyleColor(ImGuiCol_Text, c.text);
        ImGui::TextUnformatted(layout.scene.c_str());
        ImGui::PopStyleColor();
        rec.breadcrumbSceneX = ImGui::GetItemRectMin().x;
        rec.breadcrumbSceneCenterY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5F;
        rec.breadcrumbSceneFont = ImGui::GetFont()->GetDebugName();
        ImGui::PopFont();
    }
    if (layout.dotDrawn) {
        // A filled circle, never a glyph: a glyph's ink position depends on the face and the bake (I278).
        // Centred on the TEXT line: the menu bar aligns its text to the frame padding (BeginMenuBar ->
        // AlignTextToFramePadding), so the text's centre is FramePadding.y + half the font below textY.
        const float dotCenterY = textY + ImGui::GetStyle().FramePadding.y + (fontSize * 0.5F);
        ImDrawList* const list = ImGui::GetWindowDrawList();
        list->AddCircleFilled(ImVec2(layout.dotCenterX, dotCenterY), layout.dotDiameter * 0.5F, packed(c.warning));
        rec.breadcrumbDotCenterY = dotCenterY;
    }
    // The tooltip: the scene's full path (a user path can hold '%': TextUnformatted), and the dirty state.
    if (layout.runEnd > layout.runStart && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(layout.runStart, windowPos.y),
                                   ImVec2(layout.runEnd, windowPos.y + windowSize.y))) {
        if (ImGui::BeginTooltip()) {
            const std::string path(session.path());
            ImGui::TextUnformatted(path.empty() ? "Untitled -- not saved yet" : path.c_str());
            if (text.dirty) {
                ImGui::TextUnformatted("Unsaved changes");
            }
            ImGui::EndTooltip();
        }
    }
}

}  // namespace engine::editor
