#pragma once
// Aero Engine -- src-private: the log/console panel (task 2.2.5). This HEADER is ImGui-free
// (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// console_panel.cpp. All log access goes through <aero/editor/console_model.hpp> (INV-3, §V6).
//
// FRAME SHAPE (D17) -- onDraw is four phases:
//   1. header   Clear, Copy, the level combo, the text filter, Auto-scroll -- RECORDED only
//   2. log      the clipper-driven row list, strictly READ-ONLY
//   3. footer   counts, the engine floor, the aged-out / dropped notices, strictly READ-ONLY
//   apply       one switch over `pending` plus one filter diff -- the ONLY place LogHistory is
//               mutated during a frame (INV-4)
// This is not ceremony: clear() and setFilter() both restructure the very deque ImGuiListClipper is
// walking, so doing either mid-draw is a use-after-invalidate.
//
// pumpLog() is called by EditorApp::tick() EVERY frame, visible or not (D14) -- NEVER from onDraw.
// shell_ui.cpp:74-79 skips onDraw entirely for a hidden or tabbed-away panel, and Console shares its
// dock node with Assets, so it is behind another tab a great deal of the time (AC-6).
#include <aero/core/log.hpp>  // LogLevel
#include <aero/editor/console_model.hpp>
#include <aero/editor/console_view.hpp>  // task E.6.3: ConsoleDrawRecord, ConsoleFooterSegment
#include <aero/editor/panel.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

class ConsolePanel final : public Panel {
public:
    explicit ConsolePanel(LogSinkScope scope);

    // FROZEN (D23/INV-1): "Console" is the ImGui window name AND the imgui.ini settings key
    // (panel.hpp:45-48), written into every aero_editor.ini since 2.1.3. Renaming it orphans every
    // existing user's saved layout for this panel.
    [[nodiscard]] const char* id() const noexcept override { return "Console"; }
    // Bottom, shared as a TAB with "Assets" (2.2.4). Console registers FIRST, so it is the selected
    // tab in a fresh layout -- tests/editor/imgui_layer_test.cpp:336-340 depends on that (F11).
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Bottom; }
    // `context` is IGNORED: no World read, no Selection read or write. A console has nothing to say
    // about the scene, and shipping a dead coupling now would be exactly the dead code
    // shell_ui.cpp:19-24 refuses to ship for menu items. options() is deliberately NOT overridden.
    void onDraw(PanelContext& context) override;

    // Drains the sink into the history. Called once per frame by EditorApp::tick() BEFORE the draw
    // walk, whether or not this panel is visible (D14/AC-6). Not an ImGui call.
    void pumpLog();

    [[nodiscard]] const LogHistory& history() const noexcept { return logHistory; }

    // task E.6.1 / E.6.3: what ImGui DREW this frame (D14), read back from the record -- consequences the widget
    // produced, never a request's round trip; empty / 0 until a row draws. The tag is Mono small now, so
    // lastLevelFontName() reads Mono; the SCOPED-PUSH witness is lastAfterMessageFontName(), taken inside the row
    // right after the message's PopFont -- any read outside the row is blind, the child's End() pops a leaked font.
    [[nodiscard]] const std::string& lastMessageFontName() const noexcept {
        return drawRecordValue.lastMessageFontName;
    }
    [[nodiscard]] const std::string& lastLevelFontName() const noexcept { return drawRecordValue.lastLevelFontName; }
    [[nodiscard]] float lastLevelFontSize() const noexcept { return drawRecordValue.lastLevelFontSize; }
    [[nodiscard]] const std::string& lastAfterMessageFontName() const noexcept {
        return drawRecordValue.lastAfterMessageFontName;
    }
    [[nodiscard]] std::uint32_t messageRowsSubmitted() const noexcept { return drawRecordValue.messageRowsSubmitted; }
    [[nodiscard]] const ConsoleDrawRecord& drawRecord() const noexcept { return drawRecordValue; }

private:
    enum class ActionKind : std::uint8_t { None = 0, Clear, Copy };  // performance-enum-size (F33)

    void drawHeader();                       // phase 1
    void drawLogChild(float footerReserve);  // phase 2
    void drawFooter(float band);             // phase 3
    void applyPending();                     // the ONE mutating step

    LogSinkScope sinkScope;  // owns the installation; detaches in ~ConsolePanel (D15/E13)
    LogHistory logHistory;
    std::vector<LogEntry> pumpScratch;  // reused across frames; EMPTY between them (INV-10)
    LogFilter editedFilter;             // the widgets' mirror; diffed against logHistory.filter()
    ActionKind pending = ActionKind::None;
    bool autoScroll = true;
    ConsoleDrawRecord drawRecordValue;                // task E.6.3 -- drawRecord()'s value, named apart from it
    std::vector<ConsoleFooterSegment> footerScratch;  // task E.6.3 -- per-frame scratch, capacity kept
    std::string stampScratch;                         // task E.6.3 -- formatConsoleStamp's out string
    std::string levelPreviewScratch;                  // task E.6.3 -- the combo's "Name <chevron>" preview
};

}  // namespace engine::editor
