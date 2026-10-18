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
#include <aero/editor/console_view.hpp>
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/toolbar_model.hpp>  // shellBarHeight: the ONE dp -> point conversion

#include "editor_fonts.hpp"     // task E.6.1: editorFonts().mono
#include "editor_theme_ui.hpp"  // currentUiScale, toImU32
#include "panel_chrome_ui.hpp"
#include "text_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>  // task E.6.3: GetCurrentWindow -- the log child's APPLIED border (I313)
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {

// Plan C4: the levels a record can carry, and therefore the only ones the filter offers. `Off` is a
// FLOOR value (log.hpp:22-24), so selecting it would mean "show nothing" -- excluded structurally
// rather than by an off-by-one loop bound, and sized by the same constant the counters use.
constexpr std::array<LogLevel, LOG_LEVEL_RECORD_COUNT> SELECTABLE_LEVELS = {
    LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical};

// task E.6.3: the scroll a requestLogScrollX may set -- the scrollbar's reach as last drawn, [0, scrollMaxX], the only
// values a drag can produce. Begin clamps a target into ScrollMax too, but only AFTER ImRound64 has cast it to a
// 64-bit integer (imgui.cpp:12539-12541, imgui_internal.h:544), which an infinite or huge target would overflow. A
// non-finite request answers -1, which SetNextWindowScroll reads as "keep this axis" (imgui.cpp:8005).
[[nodiscard]] float logScrollTarget(float wanted, float scrollMaxX) noexcept {
    const float reach = std::isfinite(scrollMaxX) ? std::max(scrollMaxX, 0.0F) : 0.0F;
    return std::isfinite(wanted) ? std::clamp(wanted, 0.0F, reach) : -1.0F;
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

// ---- phase 1: header -- RECORDED only. D7: an UNFILLED row (ImGui's own tab bar already paints the chrome band
// above it) over a divider rule, its controls right-aligned: Clear, Copy, the level combo, the filter, Auto-scroll.
void ConsolePanel::drawHeader() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = currentUiScale();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float padY = EDITOR_THEME.panels.panelHeaderPaddingY * s;
    const float frame = ImGui::GetFrameHeight();
    // The cluster's width, measured in the faces it draws in (no decision here: consoleHeaderClusterX places it).
    const float clearWidth = ImGui::CalcTextSize("Clear").x + (2.0F * style.FramePadding.x);
    const float copyWidth = ImGui::CalcTextSize("Copy").x + (2.0F * style.FramePadding.x);
    const float comboWidth = ImGui::CalcTextSize("Critical " AERO_ICON_CHEVRON_DOWN).x + (2.0F * style.FramePadding.x);
    const float filterWidth = ImGui::GetFontSize() * CONSOLE_FILTER_FONT_MULTIPLE;
    const float checkboxWidth = frame + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto-scroll").x;
    const float controls = clearWidth + copyWidth + comboWidth + filterWidth + checkboxWidth;
    const float cluster = controls + (4.0F * style.ItemSpacing.x);  // four SameLine gaps between five controls
    const float clusterX = consoleHeaderClusterX(rowMin.x, rowWidth, cluster);
    drawRecordValue.headerRowMinX = rowMin.x;
    drawRecordValue.headerRowMaxX = rowMin.x + rowWidth;
    drawRecordValue.headerClusterWidth = cluster;

    ImGui::SetCursorScreenPos(ImVec2(clusterX, rowMin.y + padY));
    const bool clearPressed = ghostButton("Clear");
    drawRecordValue.headerControlsMinX = ImGui::GetItemRectMin().x;  // where ImGui PLACED the cluster (I314)
    if (clearPressed) {
        pending = ActionKind::Clear;
    }
    ImGui::SameLine();
    if (ghostButton("Copy")) {
        pending = ActionKind::Copy;
    }
    ImGui::SameLine();
    // D7: the combo names the level in title case with the chevron icon, and states its meaning in a tooltip rather
    // than in a "Level >=" label beside it.
    levelPreviewScratch.assign(logLevelName(editedFilter.minLevel));
    levelPreviewScratch += " " AERO_ICON_CHEVRON_DOWN;
    ImGui::SetNextItemWidth(comboWidth);
    // imgui.h:670 -- EndCombo ONLY when BeginCombo returned true. The OPPOSITE of EndChild below.
    if (ImGui::BeginCombo("##level", levelPreviewScratch.c_str(), ImGuiComboFlags_NoArrowButton)) {
        for (const LogLevel level : SELECTABLE_LEVELS) {
            // logLevelName returns views of string literals, so .data() is NUL-terminated and the labels unique.
            if (ImGui::Selectable(logLevelName(level).data(), level == editedFilter.minLevel)) {
                editedFilter.minLevel = level;  // UI mirror ONLY; applied in applyPending()
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {  // the combo: EndCombo hands LastItemData back to it (imgui.cpp:8848)
        ImGui::SetTooltip("%s", CONSOLE_LEVEL_TOOLTIP.data());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(filterWidth);
    // The hint overload (task E.6.3): ImGui draws "Filter" in its own placeholder slot. The return value is
    // deliberately unused: applyPending() diffs editedFilter against logHistory.filter() unconditionally.
    inputTextString("##filter", "Filter", editedFilter.text, ImGuiInputTextFlags_None);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    drawRecordValue.headerControlsMaxX = ImGui::GetItemRectMax().x;  // where the cluster's last item ENDED (I314)
    // The divider rule under the row, across the full content width; the log child starts on the row below it. The
    // record reads what the list RECEIVED (AddRectFilled adds no vertex at alpha 0), never a flag set beside it.
    const float ruleY = rowMin.y + frame + (2.0F * padY);
    ImDrawList* const headerList = ImGui::GetWindowDrawList();
    const int ruleVertices = headerList->VtxBuffer.Size;
    horizontalRule(headerList, rowMin.x, rowMin.x + rowWidth, ruleY, ChromeRole::Divider);
    drawRecordValue.headerRuleDrawn = headerList->VtxBuffer.Size > ruleVertices;
    drawRecordValue.headerRuleY = ruleY;
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, ruleY + ruleHeight()));  // the log child is the item that follows
}

// ---- phase 2: the log child -- strictly READ-ONLY. D7: borderless; per row the tint, the empty Selectable, the
// stamp / tag / channel in small Mono on the draw list, and the message in Mono at the body size, `text` on every
// level (the tag carries the level).
void ConsolePanel::drawLogChild(float footerReserve) {
    // imgui.h:452-455 -- the NEGATIVE height means "all remaining height minus footerReserve". A hand-computed
    // `avail.y - footerReserve` that evaluated to exactly 0.0f would instead mean "fill the parent".
    const ImVec2 logSize(0.0F, -footerReserve);
    // The request BEFORE CalcItemSize clamps the child to at least 4 (imgui.cpp:12345): a dock too short for header
    // + footer shows here, never in the child's drawn height (I314).
    drawRecordValue.logChildRequestedY = ImGui::GetContentRegionAvail().y - footerReserve;
    constexpr ImGuiChildFlags LOG_CHILD_FLAGS = ImGuiChildFlags_None;  // D7: no bordered child
    // requestLogScrollX's drain, the request-seam rule's shape: the scrollbar has no disabled state, so the gate is
    // noModal alone -- a modal blocks the drag the seam stands in for (imgui.cpp:5620-5623) -- and the target is the
    // scrollbar's reach as last drawn. SetNextWindowScroll lands in the very next Begin, the child's
    // (imgui.cpp:8003-8008), so this frame's rows draw scrolled. DROPPED when refused, never deferred: the reset runs
    // whichever way the gate answered (I337), and EditorApp's expireLogScrollX() drops one issued while the panel did
    // not draw (I338).
    const bool noModal = ImGui::GetTopMostPopupModal() == nullptr;
    if (pendingLogScrollX.has_value() && noModal) {
        ImGui::SetNextWindowScroll(ImVec2(logScrollTarget(*pendingLogScrollX, latchedLogScrollMaxX), -1.0F));
    }
    pendingLogScrollX.reset();  // one tick: the reset runs whichever way the gate answered
    ImGui::BeginChild("##log", logSize, LOG_CHILD_FLAGS, ImGuiWindowFlags_HorizontalScrollbar);
    // What Begin APPLIED, not what was passed: BeginChildEx zeroes ChildBorderSize for a borderless child
    // (imgui.cpp:6807-6809) and Begin copies it into the child (imgui.cpp:8123); the theme's childBorderSize is 1.
    drawRecordValue.logChildBordered = ImGui::GetCurrentWindow()->WindowBorderSize > 0.0F;
    drawRecordValue.logVisibleWidth = ImGui::GetContentRegionAvail().x;
    drawRecordValue.logContentWidth = ImGui::GetCurrentWindow()->ContentSize.x;  // the previous frame's rows (I341)
    // The scroll Begin APPLIED, read off the child (I341 scrolls it), and the reach the next request is bounded to.
    drawRecordValue.logScrollX = ImGui::GetScrollX();
    latchedLogScrollMaxX = ImGui::GetScrollMaxX();
    drawRecordValue.logScrollMaxX = latchedLogScrollMaxX;

    ImDrawList* const list = ImGui::GetWindowDrawList();
    ImFont* const mono = editorFonts().mono;
    const float s = currentUiScale();
    // The stamp, tag and channel cells' ONE face: every cell draw AND the record read this constant, so the record
    // names the face the draws were handed (I313 pins the three draws to it).
    constexpr PanelFace CELL_FACE = PanelFace::MonoSmall;
    const float smallPoints = panelFacePoints(CELL_FACE);
    // The columns are advances of the SMALL Mono face, constant per glyph; the small cells sit on the body-size
    // message's baseline (two ascents of ONE face, each copied at once -- a bake can reallocate).
    const float smallAdvance = faceTextWidth(CELL_FACE, "0");
    const float bodyAscent = mono != nullptr ? mono->GetFontBaked(panelFacePoints(PanelFace::Mono))->Ascent : 0.0F;
    const float smallAscent = mono != nullptr ? mono->GetFontBaked(smallPoints)->Ascent : 0.0F;
    const float rowMinX = ImGui::GetCursorScreenPos().x;
    // The tint's right edge is the Selectable's: its max_x, the WorkRect's right edge, plus its right half-spacing
    // (imgui_widgets.cpp:7382, :7393, :7397). Under the horizontal scrollbar the WorkRect spans the CONTENT while the
    // content region spans only the visible width (imgui.cpp:8531, :8546), so a GetContentRegionAvail() edge stopped
    // short by the overhang, and moved left with the scroll (I341).
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float rowMaxX = ImGui::GetCurrentWindow()->WorkRect.Max.x + (spacingX - std::trunc(spacingX * 0.5F));
    const ConsoleRowLayout layout = consoleRowLayout(rowMinX, smallAdvance, bodyAscent, smallAscent, s);
    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    const float spacingUp = std::trunc(spacingY * 0.5F);  // the Selectable's own half-spacing split (IM_TRUNC)
    const ImFont* const smallFace = panelFaceFont(CELL_FACE);
    const char* const smallFaceName = smallFace != nullptr ? smallFace->GetDebugName() : "";

    ImGuiListClipper clipper;  // imgui.h:2994-3012 -- only the visible rows are submitted (AC-13)
    clipper.Begin(static_cast<int>(logHistory.visibleCount()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogEntry& entry = logHistory.visibleAt(static_cast<std::size_t>(i));
            const LogChannelSplit split = splitLogChannel(entry.message);  // DISPLAY only: Copy keeps the whole
            // Plan C6: keyed by SEQUENCE, not by row position (2.2.5's comment, unchanged in substance).
            ImGui::PushID(static_cast<int>(entry.sequence));
            const float rowTop = ImGui::GetCursorScreenPos().y;
            const float lineHeight = ImGui::GetTextLineHeight();
            // D7: the tint BEFORE the Selectable, so the hover fill paints on top of it, over the Selectable's own
            // half-spacing-widened box (imgui_widgets.cpp:7388-7398) -- less its left half-spacing, which this
            // borderless child clips anyway (no WindowPadding, imgui.cpp:8127-8128, so it lies left of the clip rect).
            const int tintVertex = list->VtxBuffer.Size;
            bool rowTinted = false;
            if (const std::optional<Srgb8> tint = consoleRowTint(entry.level); tint.has_value()) {
                list->AddRectFilled(ImVec2(rowMinX, rowTop - spacingUp),
                                    ImVec2(rowMaxX, rowTop + lineHeight + (spacingY - spacingUp)), toImU32(*tint));
                // The edge the list RECEIVED: PrimRect's second vertex is (p_max.x, p_min.y) (imgui_draw.cpp:772,
                // :777), and AddRectFilled adds none at alpha 0 (:1532-1533), which records 0.
                const bool tintDrawn = list->VtxBuffer.Size == tintVertex + 4;
                drawRecordValue.lastTintMaxX = tintDrawn ? list->VtxBuffer[tintVertex + 1].pos.x : 0.0F;
                ++drawRecordValue.rowsTinted;
                rowTinted = true;
            }
            // An EMPTY label (2.2.5's cd4caab fix: a message containing "##" must never reach a label).
            ImGui::Selectable("##row", false);
            if (rowTinted) {  // the box ItemAdd recorded for it (imgui.cpp:11979), clipped or not
                drawRecordValue.lastTintedRowItemMaxX = ImGui::GetItemRectMax().x;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !entry.sourceFile.empty()) {
                ImGui::SetTooltip("%s:%u", entry.sourceFile.c_str(), static_cast<unsigned>(entry.line));
            }
            const float smallY = rowTop + layout.smallTextOffsetY;
            formatConsoleStamp(entry.elapsedMs, stampScratch);
            drawRoleText(list, CELL_FACE, ImVec2(layout.stampX, smallY), ChromeRole::TextFaint, stampScratch);
            const std::string_view tag = logLevelLabel(entry.level);
            const ChromeRole tagRole = logLevelTagRole(entry.level);
            drawRoleText(list, CELL_FACE, ImVec2(layout.tagX, smallY), tagRole, tag);
            list->PushClipRect(ImVec2(layout.channelX, rowTop), ImVec2(layout.messageX, rowTop + lineHeight), true);
            drawRoleText(list, CELL_FACE, ImVec2(layout.channelX, smallY), ChromeRole::TextMuted, split.channel);
            list->PopClipRect();
            ImGui::SameLine(0.0F, 0.0F);  // back onto the row's line, then to the message column
            ImGui::SetCursorScreenPos(ImVec2(layout.messageX, ImGui::GetCursorScreenPos().y));
            // The MESSAGE in Mono at size 0 -- the current size, Body's (E.6.1 D6) -- so the row the Selectable sized
            // and the clipper's step stay exact. NO continue, break or return between this push and its pop (I283).
            ImGui::PushFont(mono, 0.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::Text));
            drawRecordValue.lastMessageFontName = ImGui::GetFont()->GetDebugName();
            ImGui::TextUnformatted(split.message.data(), split.message.data() + split.message.size());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            drawRecordValue.lastAfterMessageFontName = ImGui::GetFont()->GetDebugName();  // the push's scope (F10)
            drawRecordValue.lastTagRole = tagRole;
            drawRecordValue.lastTagText.assign(tag);
            drawRecordValue.lastStampDrawn = stampScratch;
            drawRecordValue.lastChannelDrawn.assign(split.channel);
            drawRecordValue.lastMessageDrawn.assign(split.message);
            drawRecordValue.lastLevelFontName = smallFaceName;
            drawRecordValue.lastLevelFontSize = smallPoints;
            ++drawRecordValue.messageRowsSubmitted;
            ImGui::PopID();  // no continue/break/return anywhere between any Push and its Pop
        }
    }
    // imgui.h:505,509,511 -- pin to the bottom ONLY when the user is ALREADY there.
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();  // UNCONDITIONAL -- imgui.h:458-463, and the opposite of the combo rule above
    drawRecordValue.logChildMaxY = ImGui::GetItemRectMax().y;
}

// ---- phase 3: footer -- strictly READ-ONLY. D7: ONE band shellBarHeight(statusBarHeightDp) tall under a divider
// rule; the counted segments from the pure model, consoleSegmentGap apart, with no separator glyph.
void ConsolePanel::drawFooter(float band) {
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ImVec2 bandMin = ImGui::GetCursorScreenPos();
    const float bandMaxX = bandMin.x + ImGui::GetContentRegionAvail().x;
    horizontalRule(list, bandMin.x, bandMaxX, bandMin.y, ChromeRole::Divider);
    // Read FRESH every frame (the engine floor explains an empty Trace view in a Release build, 2.2.5's D9/AC-9).
    const ConsoleFooterCounts counts{.held = logHistory.size(),
                                     .visible = logHistory.visibleCount(),
                                     .filtered = !(logHistory.filter() == LogFilter{}),
                                     .warnings = logHistory.levelCount(LogLevel::Warn),
                                     .errors = logHistory.levelCount(LogLevel::Error),
                                     .critical = logHistory.levelCount(LogLevel::Critical),
                                     .engineFloor = engine::logLevel(),
                                     .agedOut = logHistory.evictedCount(),
                                     .dropped = logHistory.droppedCount()};
    consoleFooterSegments(counts, footerScratch);
    const float textY = bandMin.y + ((band - panelFacePoints(PanelFace::MonoSmall)) * 0.5F);
    const float gap = consoleSegmentGap(currentUiScale());
    float x = bandMin.x;
    for (const ConsoleFooterSegment& segment : footerScratch) {
        drawRoleText(list, PanelFace::MonoSmall, ImVec2(x, textY), segment.role, segment.text);
        drawRecordValue.footerTextDrawn += segment.text;  // the text the draw was handed (I314 joins the model's)
        drawRecordValue.footerTextDrawn += '|';
        x += faceTextWidth(PanelFace::MonoSmall, segment.text) + gap;
        ++drawRecordValue.footerSegmentsDrawn;
    }
    // The band as ONE item: it ends exactly at the content region's bottom, so the window never scrolls.
    ImGui::Dummy(ImVec2(0.0F, band));
    drawRecordValue.footerBandMinY = bandMin.y;
    drawRecordValue.footerBandMaxY = ImGui::GetItemRectMax().y;
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
    ++drawRecordValue.framesRecorded;
    drawRecordValue.messageRowsSubmitted = 0;  // PER-FRAME counts -- what this frame really drew
    drawRecordValue.rowsTinted = 0;
    drawRecordValue.footerSegmentsDrawn = 0;
    drawRecordValue.footerTextDrawn.clear();
    drawHeader();  // 1 -- records an action or edits the UI-side filter mirror only
    // D7: the band is E.6.2's ONE dp -> point conversion, never restated. EndChild's ItemSize adds ItemSpacing.y after
    // the child (imgui.cpp:6860, :12129), so the reservation is the band PLUS that spacing -- the band alone would
    // leave 20 of its 26 dp and push the footer into a scrollbar.
    const float band = shellBarHeight(statusBarHeightDp(EDITOR_THEME), currentUiScale());
    const float footerReserve = band + ImGui::GetStyle().ItemSpacing.y;
    drawRecordValue.footerHeight = footerReserve;
    drawLogChild(footerReserve);  // 2 -- strictly read-only
    drawFooter(band);             // 3 -- strictly read-only
    drawRecordValue.windowScrollMaxY = ImGui::GetScrollMaxY();
    applyPending();  // the ONLY place logHistory is mutated during a frame
}

}  // namespace engine::editor
