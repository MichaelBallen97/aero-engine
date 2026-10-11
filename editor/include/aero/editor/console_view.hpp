#pragma once
// Aero Engine -- the Console's chrome as VALUES (task E.6.3, spec D7). PUBLIC and PURE: no ImGui, no logging calls,
// so tier 0 reaches every decision console_panel.cpp only draws -- the channel split, the stamp, the level's name,
// tag role and row tint, the four columns and the footer's counted segments.
//
// THE CHANNEL IS DISPLAY ONLY: the row shows a `token: ` prefix in its own column and the message without it, while
// Copy (buildClipboardText) and the filter (matchesFilter) keep reading the WHOLE message -- no LogRecord change.
#include <aero/core/log.hpp>  // LogLevel
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/panel_chrome_model.hpp>  // ChromeRole

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// `^[A-Za-z_][A-Za-z0-9_]{0,23}: ` followed by a non-empty rest; otherwise the whole message and an empty channel.
// Both members are views INTO `message` (no allocation).
struct LogChannelSplit {
    std::string_view channel;
    std::string_view message;
};
inline constexpr std::size_t LOG_CHANNEL_MAX_BYTES = 24;
[[nodiscard]] LogChannelSplit splitLogChannel(std::string_view message) noexcept;

// "mm:ss.mmm" under an hour, "h:mm:ss.mmm" from one hour -- the hour field widening without bound. DISPLAY only:
// Copy keeps formatElapsed's "HH:MM:SS.mmm". Clears `out`, then appends (a reused scratch keeps its capacity).
void formatConsoleStamp(std::uint64_t millis, std::string& out);

// "Trace" .. "Critical", "Off": the level combo and the footer. The TAG draws logLevelLabel (upper case). Views of
// string literals, so .data() is NUL-terminated.
[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;
// The tag's role: Trace textFaint, Debug textMuted, Info textLabel, Warn warning, Error error, Critical critical;
// Off (a floor, never a record's level in practice) textFaint.
[[nodiscard]] ChromeRole logLevelTagRole(LogLevel level) noexcept;
// The row tint behind a Warn / Error / Critical row (Critical takes the error tint); none for the other four.
[[nodiscard]] std::optional<Srgb8> consoleRowTint(LogLevel level) noexcept;

// The columns, in advances of the SMALL Mono face (constant per glyph, so a column never depends on its text).
inline constexpr float CONSOLE_STAMP_COLUMN_ADVANCES = 11.0F;    // "9:59:59.999", the widest stamp under 10 h; past
                                                                 // it the stamp runs into the gap, never the tag
inline constexpr float CONSOLE_TAG_COLUMN_ADVANCES = 8.0F;       // "CRITICAL" (LOG_LEVEL_LABEL_WIDTH)
inline constexpr float CONSOLE_CHANNEL_COLUMN_ADVANCES = 14.0F;  // "component_ops" and one to spare
// D1: the filter field's width, a NAMED font multiple (moved from console_panel.cpp:43).
inline constexpr float CONSOLE_FILTER_FONT_MULTIPLE = 14.0F;
inline constexpr std::string_view CONSOLE_LEVEL_TOOLTIP = "Show records at or above this level";

struct ConsoleRowLayout {
    float stampX = 0.0F;
    float tagX = 0.0F;
    float channelX = 0.0F;
    float messageX = 0.0F;
    float smallTextOffsetY = 0.0F;  // where a small-face cell starts below the row's top, baseline-aligned
    friend constexpr bool operator==(const ConsoleRowLayout&, const ConsoleRowLayout&) = default;
};
// `smallAdvance` is the small Mono face's advance at the live scale (already scaled); the gaps are
// ThemePanels::consoleColumnGap x uiScale. The two ascents are the Mono face's at the body and small point sizes.
[[nodiscard]] ConsoleRowLayout consoleRowLayout(float rowMinX, float smallAdvance, float bodyAscent, float smallAscent,
                                                float uiScale) noexcept;
// The space between two footer segments (no separator glyph, D7): consoleColumnGap x uiScale.
[[nodiscard]] float consoleSegmentGap(float uiScale) noexcept;
// The header cluster's left edge: right-aligned in the row, or the row's left edge when the row is narrower than the
// cluster (the cluster then clips at its right end). A non-finite input answers rowMinX.
[[nodiscard]] float consoleHeaderClusterX(float rowMinX, float rowWidth, float clusterWidth) noexcept;

struct ConsoleFooterCounts {
    std::size_t held = 0;     // records in the ring
    std::size_t visible = 0;  // records passing the filter
    bool filtered = false;    // a filter is set: "V of N messages"
    std::uint32_t warnings = 0;
    std::uint32_t errors = 0;
    std::uint32_t critical = 0;
    LogLevel engineFloor = LogLevel::Info;
    std::uint64_t agedOut = 0;
    std::uint64_t dropped = 0;
};
struct ConsoleFooterSegment {
    std::string text;
    ChromeRole role = ChromeRole::TextMuted;
};
// In order: [V of ]N messages, W warnings (warning when > 0), E errors (error when > 0), C critical (only when > 0),
// engine floor: <Name>, A aged out (only when > 0), D dropped (textFaint, ALWAYS). Singular at 1. Clears `out`.
void consoleFooterSegments(const ConsoleFooterCounts& counts, std::vector<ConsoleFooterSegment>& out);

// What the Console DREW this frame (D14), read back for the GPU tier.
struct ConsoleDrawRecord {
    std::uint64_t framesRecorded = 0;        // + 1 per onDraw
    std::uint32_t messageRowsSubmitted = 0;  // this frame
    std::uint32_t rowsTinted = 0;            // this frame
    ChromeRole lastTagRole = ChromeRole::TextFaint;
    std::string lastTagText;               // the upper-case label the tag drew
    std::string lastStampDrawn;            // the stamp text drawn
    std::string lastChannelDrawn;          // the channel cell's text ("" for an unprefixed record)
    std::string lastMessageDrawn;          // the message cell's text, prefix stripped
    std::string lastMessageFontName;       // GetFont()->GetDebugName() inside the message's push
    std::string lastLevelFontName;         // the face the tag was drawn in (the ImFont* handed to AddText)
    float lastLevelFontSize = 0.0F;        // ...and its size
    std::string lastAfterMessageFontName;  // GetFont() right after the message's PopFont, inside the row (F10)
    float headerRowMinX = 0.0F;            // the content region's left edge at the header
    float headerRowMaxX = 0.0F;            // ...and its right edge (rowMin.x + rowWidth)
    float headerClusterWidth = 0.0F;       // the cluster width the panel computed (the fit test reads it, #111)
    float headerControlsMinX = 0.0F;       // GetItemRectMin().x of the Clear button, as ImGui placed it
    float headerControlsMaxX = 0.0F;       // GetItemRectMax().x of the Auto-scroll checkbox, likewise
    bool headerRuleDrawn = false;          // the rule's AddRectFilled added vertices (none at alpha 0)
    float headerRuleY = 0.0F;              // the rule's top edge (the log child starts one rule below it)
    bool logChildBordered = false;         // the border Begin APPLIED to the log child (its WindowBorderSize > 0)
    float logChildRequestedY = 0.0F;       // GetContentRegionAvail().y - footerReserve before BeginChild (unclamped)
    float logChildMaxY = 0.0F;
    float footerHeight = 0.0F;  // the reservation as computed: the band + ItemSpacing.y
    float footerBandMinY = 0.0F;
    float footerBandMaxY = 0.0F;
    std::uint32_t footerSegmentsDrawn = 0;
    std::string footerTextDrawn;    // every segment text handed to the draw this frame, each followed by '|'
    float windowScrollMaxY = 0.0F;  // the Console window's own GetScrollMaxY(), read inside onDraw
};

}  // namespace engine::editor
