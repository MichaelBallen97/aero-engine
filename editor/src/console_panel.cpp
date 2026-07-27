// Aero Engine -- the log/console panel (task 2.2.5). THE only new ImGui TU. It reaches the log stream
// ONLY through <aero/editor/console_model.hpp>: no setLogCallback, no LogCallback, no
// engine::detail:: anything (INV-3, grep-asserted in §V6).
//
// FIVE ImGui rules this file lives or dies by. Getting one wrong is an IM_ASSERT ABORT in the Debug
// build, not a visual glitch:
//   * BeginChild/EndChild are 1:1 like Begin/End -- EndChild() ALWAYS runs, whatever BeginChild
//     returned (imgui.h:458-463).
//   * BeginCombo/EndCombo are the OPPOSITE -- EndCombo() ONLY when BeginCombo() returned true
//     (imgui.h:670). The asymmetry below is deliberate; do not "tidy" it into symmetry.
//   * PushID/PopID and PushStyleColor/PopStyleColor are 1:1 -- and there is NO continue, break or
//     return between any pair here (duplicate ImGui ids silently MERGE widgets).
//   * ImGuiListClipper (imgui.h:2994-3012) requires UNIFORM ROW HEIGHT, which is why a record is
//     flattened to one line at CAPTURE time (D8), not at draw time.
//   * NEVER ImGui::Text(runtimeString). ImGui::Text is a printf format (imgui.h:625, IM_FMTARGS(1))
//     and this project enables NO -Wformat* warning and has no clang-tidy format check -- so
//     ImGui::Text(entry.message.c_str()) would compile silently and read the varargs stack on any
//     message containing '%'. Every runtime string here goes through TextUnformatted, or through a
//     LITERAL "%s". INV-6 is held by the §V6 grep and by review. CI would NOT catch a violation.
#include "console_panel.hpp"

#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/panel_context.hpp>

#include "text_input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string>
#include <utility>

namespace engine::editor {

namespace {

// DPI-proportional, never a pixel constant.
constexpr float LEVEL_COMBO_FONT_MULTIPLE = 8.0F;
constexpr float FILTER_INPUT_FONT_MULTIPLE = 14.0F;
// The widest elapsed stamp the column must reserve before it starts widening (D7 / formatElapsed).
constexpr const char* ELAPSED_COLUMN_SAMPLE = "00:00:00.000";

// Plan C4: the levels a record can carry, and therefore the only ones the filter offers. `Off` is a
// FLOOR value (log.hpp:22-24), so selecting it would mean "show nothing" -- excluded structurally
// rather than by an off-by-one loop bound, and sized by the same constant the counters use.
constexpr std::array<LogLevel, LOG_LEVEL_RECORD_COUNT> SELECTABLE_LEVELS = {
    LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical};

// Plan C2: identical arms share ONE label. Separate token-identical case bodies are what
// bugprone-branch-clone reports, and that check is --warnings-as-errors on the Linux Debug lane.
// Trace/Debug and Info follow the ACTIVE THEME via GetStyleColorVec4 (imgui.h:563) instead of
// hardcoding a palette; only the three attention levels carry literal colours.
ImVec4 logLevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug:
            return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        case LogLevel::Info:
        case LogLevel::Off:  // unreachable in practice: Off is a floor value, never a record level
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        case LogLevel::Warn:
            return ImVec4(1.00F, 0.80F, 0.35F, 1.00F);
        case LogLevel::Error:
            return ImVec4(1.00F, 0.45F, 0.40F, 1.00F);
        case LogLevel::Critical:
            return ImVec4(1.00F, 0.25F, 0.25F, 1.00F);
    }
    // Unreachable; keeps GCC's control-reaches-end quiet -- the log.cpp:30 shape. NOTE: this is a
    // CONVENTION, not enforcement. The project enables neither -Wall nor -Wswitch, so a new
    // enumerator would not fail the build; shell_ui.cpp:109-111's static_assert is the pattern to
    // copy if that ever needs to be mechanical. LogLevel is a settled 0.2.4 enum.
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

}  // namespace

ConsolePanel::ConsolePanel(LogSinkScope scope) : sinkScope(std::move(scope)) {}

// ---- the pump: called from EditorApp::tick(), NEVER from onDraw (D14) --------------------------
void ConsolePanel::pumpLog() {
    LogSink* const sink = sinkScope.sink().get();
    if (sink == nullptr) {
        return;  // a moved-from scope; impossible for a registered panel, but the check is free
    }
    logHistory.noteDropped(sink->take(pumpScratch));  // pumpScratch is empty here by INV-10
    logHistory.appendAll(pumpScratch);                // moves each element, then clears
}

// ---- phase 1: header (Clear, Copy, level, filter, auto-scroll) -- RECORDED only ----------------
void ConsolePanel::drawHeader() {
    if (ImGui::Button("Clear")) {
        pending = ActionKind::Clear;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        pending = ActionKind::Copy;
    }
    ImGui::SameLine();
    // Plan C8: the combo's preview is "WARN", not ">= WARN", so the >= semantics need this static
    // prefix. Cheaper than a second label helper, and it keeps logLevelLabel the single tier-0-tested
    // label function. Symmetric with "Filter" below.
    ImGui::TextUnformatted("Level >=");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * LEVEL_COMBO_FONT_MULTIPLE);
    // imgui.h:670 -- EndCombo ONLY when BeginCombo returned true. The OPPOSITE of EndChild below.
    if (ImGui::BeginCombo("##level", logLevelLabel(editedFilter.minLevel))) {
        for (const LogLevel level : SELECTABLE_LEVELS) {
            // logLevelLabel returns a string literal, so these labels are unique and stable -- no
            // PushID needed here.
            if (ImGui::Selectable(logLevelLabel(level), level == editedFilter.minLevel)) {
                editedFilter.minLevel = level;  // UI mirror ONLY; applied in applyPending()
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Filter");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * FILTER_INPUT_FONT_MULTIPLE);
    // Reused AS IS from task 2.2.2 -- no hint variant is added, so text_input.{hpp,cpp} stays
    // byte-identical (it is shared with two other panels). The return value is deliberately unused:
    // applyPending() diffs editedFilter against logHistory.filter() unconditionally.
    inputTextString("##filter", editedFilter.text, ImGuiInputTextFlags_None);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
}

// ---- phase 2: the log child -- strictly READ-ONLY ----------------------------------------------
void ConsolePanel::drawLogChild(float footerHeight) {
    // imgui.h:452-455 -- the NEGATIVE height means "all remaining height minus footerHeight". A
    // hand-computed `avail.y - footerHeight` that evaluated to exactly 0.0f would instead mean "fill
    // the parent" and silently eat the footer. This is the documented reserve-a-footer idiom.
    const ImVec2 logSize(0.0F, -footerHeight);
    ImGui::BeginChild("##log", logSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

    // Column offsets from CalcTextSize, not from a padded string: alignment must not silently depend
    // on the default font being monospaced (F30's is; a future Unicode font would not be). D18.
    const float levelColumnX = ImGui::CalcTextSize(ELAPSED_COLUMN_SAMPLE).x + ImGui::GetFontSize();
    const float messageColumnX = levelColumnX + ImGui::CalcTextSize("CRITICAL").x + ImGui::GetFontSize();

    ImGuiListClipper clipper;  // imgui.h:2994-3012 -- only the visible rows are submitted (AC-13)
    clipper.Begin(static_cast<int>(logHistory.visibleCount()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogEntry& entry = logHistory.visibleAt(static_cast<std::size_t>(i));
            const ImVec4 color = logLevelColor(entry.level);
            // Plan C6: keyed by SEQUENCE, not by row position -- so hover/tooltip state follows the
            // RECORD when the ring evicts under a stationary mouse. Two simultaneously visible
            // sequences differ by far less than 2^31, so the truncation cannot collide in a frame.
            ImGui::PushID(static_cast<int>(entry.sequence));
            // An EMPTY label plus separate TextUnformatted segments -- the fix commit cd4caab shipped
            // for 2.2.4's filenames, applied to content FAR more likely to contain "##". Selectable
            // has no format overload and runs its label through FindRenderedTextEnd(), so a message
            // containing "##" would display truncated; TextUnformatted goes through TextEx, which
            // never calls FindRenderedTextEnd and renders "##" literally (both verified at upstream
            // v1.92.8-docking). The layout is exact, not approximate: Selectable calls ItemSize()
            // with the LABEL-derived size before the span widening, and CalcTextSize returns
            // (0, fontSize) for an empty display range -- so the row keeps full height while
            // CursorPosPrevLine.x stays at the row origin, and SameLine(0,0) puts the first segment
            // exactly where Selectable would have drawn its label. The return value is unused:
            // a selected log line has nothing to do (D18).
            ImGui::Selectable("##row", false);
            // imgui_internal.h:1034,1036 -- DelayNormal IS inside AllowedMaskForIsItemHovered, so
            // this cannot trip IsItemHovered's IM_ASSERT. SetTooltip (imgui.h:835) needs NO
            // balancing, unlike BeginItemTooltip/EndTooltip (:842/:834) -- that is why it is used.
            // The format string is a LITERAL; the runtime string is an argument (INV-6).
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !entry.sourceFile.empty()) {
                ImGui::SetTooltip("%s:%u", entry.sourceFile.c_str(), static_cast<unsigned>(entry.line));
            }
            ImGui::SameLine(0.0F, 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            lineScratch = formatElapsed(entry.elapsedMs);
            ImGui::TextUnformatted(lineScratch.c_str());  // NEVER ImGui::Text -- INV-6
            ImGui::PopStyleColor();
            ImGui::SameLine(levelColumnX, 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(logLevelLabel(entry.level));
            ImGui::PopStyleColor();
            ImGui::SameLine(messageColumnX, 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();  // no continue/break/return anywhere between any Push and its Pop
        }
    }
    // imgui.h:505,509,511 -- pin to the bottom ONLY when the user is ALREADY there, so scrolling up
    // to read history is never yanked away. Must run after the last submitted item (the clipper has
    // seeked the cursor past the whole virtual list by now) and before EndChild.
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();  // UNCONDITIONAL -- imgui.h:458-463, and the opposite of the combo rule above
}

// ---- phase 3: footer -- the honest-truncation surface, strictly READ-ONLY ----------------------
void ConsolePanel::drawFooter() {
    // Plan C7: ASCII separators only -- the default font is ASCII/Latin-1 (F30) and this task already
    // owes one ?-glyph validation row for message text; a second one for our own chrome would be
    // self-inflicted. Same shape as asset_browser_panel.cpp:349-361.
    lineScratch = "showing " + std::to_string(logHistory.visibleCount()) + " of " + std::to_string(logHistory.size());
    lineScratch += " | warn " + std::to_string(logHistory.levelCount(LogLevel::Warn));
    lineScratch += " | err " + std::to_string(logHistory.levelCount(LogLevel::Error));
    lineScratch += " | crit " + std::to_string(logHistory.levelCount(LogLevel::Critical));
    // Read FRESH every frame so it can never go stale, and stated at all so an empty >= Trace view in
    // a Release build is explained rather than mysterious (D9/AC-9: Trace and Debug emit no code at
    // all under NDEBUG -- log.hpp:137-143 -- so they can NEVER appear there).
    lineScratch += " | engine floor ";
    lineScratch += logLevelLabel(engine::logLevel());
    // Two DISTINCT causes, reported separately and never silently (D11): aging out is a long session,
    // dropping is something spewing.
    if (logHistory.evictedCount() > 0) {
        lineScratch += " | " + std::to_string(logHistory.evictedCount()) + " aged out";
    }
    if (logHistory.droppedCount() > 0) {
        lineScratch += " | " + std::to_string(logHistory.droppedCount()) + " dropped (burst)";
    }
    ImGui::TextDisabled("%s", lineScratch.c_str());  // a LITERAL format with a %s -- INV-6
}

// ---- apply: the ONE mutating step (D17/INV-4) ---------------------------------------------------
void ConsolePanel::applyPending() {
    const ActionKind action = pending;
    pending = ActionKind::None;
    switch (action) {
        case ActionKind::None:
            break;
        case ActionKind::Clear:
            // Restructures visibleSeq -- the very deque the clipper was walking. THIS is why the
            // phase split exists.
            logHistory.clear();
            break;
        case ActionKind::Copy: {
            // The CURRENTLY FILTERED view (D20): you filter to the errors, then paste the errors.
            // Bounded by DEFAULT_LOG_HISTORY_CAPACITY, not by hope.
            const std::string text = buildClipboardText(logHistory);
            ImGui::SetClipboardText(text.c_str());  // imgui.h:1162
            break;
        }
    }
    // Unconditional, and AFTER the switch, so Clear-then-filter-edit in one frame lands in a defined
    // order. setFilter() is a no-op when the filters compare equal, so this costs one comparison per
    // frame in the common case.
    if (!(editedFilter == logHistory.filter())) {
        logHistory.setFilter(editedFilter);
    }
}

// ---- the frame ---------------------------------------------------------------------------------
void ConsolePanel::onDraw(PanelContext& /*context*/) {  // the context is IGNORED, by design
    drawHeader();  // 1 -- records an action or edits the UI-side filter mirror only
    // Reserve one framed line for the footer. GetFrameHeightWithSpacing() (imgui.h:600) is more than
    // a text line, so the footer always fits and the outer window never grows a scrollbar.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing();
    drawLogChild(footerHeight);  // 2 -- strictly read-only
    drawFooter();                // 3 -- strictly read-only
    applyPending();              // the ONLY place logHistory is mutated during a frame
}

}  // namespace engine::editor
