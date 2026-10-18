// Aero Engine -- the Console's chrome as values (task E.6.3). PURE: see console_view.hpp.
#include <aero/editor/console_model.hpp>  // LOG_LEVEL_LABEL_WIDTH
#include <aero/editor/console_view.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>  // std::move -- named, never reached transitively (MSVC)

namespace engine::editor {

static_assert(CONSOLE_TAG_COLUMN_ADVANCES == static_cast<float>(LOG_LEVEL_LABEL_WIDTH),
              "the tag column is the widest upper-case label");

namespace {

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

// ASCII only, by range: std::isalpha(char) is UB for a negative char (a UTF-8 continuation byte).
[[nodiscard]] bool isIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
[[nodiscard]] bool isIdentifierByte(char c) noexcept { return isIdentifierStart(c) || (c >= '0' && c <= '9'); }

// Integer-only, locale-free zero padding (formatElapsed's posture): `value` in at least `width` digits.
void appendPadded(std::string& out, std::uint64_t value, std::size_t width) {
    std::array<char, 20> digits{};  // UINT64_MAX has 20 digits
    std::size_t count = 0;
    do {
        digits[count] = static_cast<char>('0' + static_cast<int>(value % 10U));
        ++count;
        value /= 10U;
    } while (value > 0U && count < digits.size());
    for (std::size_t i = count; i < width; ++i) {
        out.push_back('0');
    }
    while (count > 0U) {
        --count;
        out.push_back(digits[count]);
    }
}

void appendSegment(std::vector<ConsoleFooterSegment>& out, std::uint64_t n, std::string_view singular,
                   std::string_view plural, ChromeRole role) {
    std::string text = std::to_string(n);
    text += ' ';
    text += n == 1U ? singular : plural;
    out.push_back(ConsoleFooterSegment{.text = std::move(text), .role = role});
}

}  // namespace

LogChannelSplit splitLogChannel(std::string_view message) noexcept {
    const LogChannelSplit whole{.channel = std::string_view{}, .message = message};
    if (message.empty() || !isIdentifierStart(message.front())) {
        return whole;
    }
    std::size_t end = 1;
    while (end < message.size() && isIdentifierByte(message[end])) {
        ++end;
    }
    if (end > LOG_CHANNEL_MAX_BYTES) {
        return whole;  // a 25-byte token is not a channel (CS2)
    }
    // `: ` and at least one byte after it. Pointer + size, never substr (which is not noexcept).
    if (end + 2U >= message.size() || message[end] != ':' || message[end + 1U] != ' ') {
        return whole;
    }
    return LogChannelSplit{.channel = std::string_view(message.data(), end),
                           .message = std::string_view(message.data() + end + 2U, message.size() - end - 2U)};
}

void formatConsoleStamp(std::uint64_t millis, std::string& out) {
    const std::uint64_t ms = millis % 1000U;
    const std::uint64_t totalSeconds = millis / 1000U;
    const std::uint64_t seconds = totalSeconds % 60U;
    const std::uint64_t totalMinutes = totalSeconds / 60U;
    const std::uint64_t minutes = totalMinutes % 60U;
    const std::uint64_t hours = totalMinutes / 60U;
    out.clear();
    if (hours > 0U) {
        appendPadded(out, hours, 1U);
        out.push_back(':');
    }
    appendPadded(out, minutes, 2U);
    out.push_back(':');
    appendPadded(out, seconds, 2U);
    out.push_back('.');
    appendPadded(out, ms, 3U);
}

std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "Trace";
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Info:
            return "Info";
        case LogLevel::Warn:
            return "Warn";
        case LogLevel::Error:
            return "Error";
        case LogLevel::Critical:
            return "Critical";
        case LogLevel::Off:
            return "Off";
    }
    return "Off";
}

ChromeRole logLevelTagRole(LogLevel level) noexcept {
    switch (level) {  // identical arms share ONE label (bugprone-branch-clone, the logLevelColor precedent)
        case LogLevel::Trace:
        case LogLevel::Off:
            return ChromeRole::TextFaint;
        case LogLevel::Debug:
            return ChromeRole::TextMuted;
        case LogLevel::Info:
            return ChromeRole::TextLabel;
        case LogLevel::Warn:
            return ChromeRole::Warning;
        case LogLevel::Error:
            return ChromeRole::Error;
        case LogLevel::Critical:
            return ChromeRole::Critical;
    }
    return ChromeRole::TextFaint;
}

std::optional<Srgb8> consoleRowTint(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Warn:
            return EDITOR_THEME.panels.consoleWarnRowTint;
        case LogLevel::Error:
        case LogLevel::Critical:
            return EDITOR_THEME.panels.consoleErrorRowTint;
        case LogLevel::Trace:
        case LogLevel::Debug:
        case LogLevel::Info:
        case LogLevel::Off:
            return std::nullopt;
    }
    return std::nullopt;
}

ConsoleRowLayout consoleRowLayout(float rowMinX, float smallAdvance, float bodyAscent, float smallAscent,
                                  float uiScale) noexcept {
    const float gap = consoleSegmentGap(uiScale);
    ConsoleRowLayout out{};
    out.stampX = rowMinX;
    out.tagX = out.stampX + (CONSOLE_STAMP_COLUMN_ADVANCES * smallAdvance) + gap;
    out.channelX = out.tagX + (CONSOLE_TAG_COLUMN_ADVANCES * smallAdvance) + gap;
    out.messageX = out.channelX + (CONSOLE_CHANNEL_COLUMN_ADVANCES * smallAdvance) + gap;
    out.smallTextOffsetY = bodyAscent - smallAscent;
    return out;
}

float consoleSegmentGap(float uiScale) noexcept { return EDITOR_THEME.panels.consoleColumnGap * layoutScale(uiScale); }

float consoleHeaderClusterX(float rowMinX, float rowWidth, float clusterWidth) noexcept {
    const float right = rowMinX + rowWidth - clusterWidth;
    if (!std::isfinite(right) || !std::isfinite(rowMinX)) {
        return std::isfinite(rowMinX) ? rowMinX : 0.0F;
    }
    return std::max(rowMinX, right);
}

void consoleFooterSegments(const ConsoleFooterCounts& counts, std::vector<ConsoleFooterSegment>& out) {
    out.clear();
    std::string messages;
    if (counts.filtered) {
        messages = std::to_string(counts.visible) + " of ";
    }
    messages += std::to_string(counts.held);
    messages += counts.held == 1U ? " message" : " messages";
    out.push_back(ConsoleFooterSegment{.text = std::move(messages), .role = ChromeRole::TextMuted});
    appendSegment(out, counts.warnings, "warning", "warnings",
                  counts.warnings > 0U ? ChromeRole::Warning : ChromeRole::TextMuted);
    appendSegment(out, counts.errors, "error", "errors",
                  counts.errors > 0U ? ChromeRole::Error : ChromeRole::TextMuted);
    if (counts.critical > 0U) {
        appendSegment(out, counts.critical, "critical", "critical", ChromeRole::Critical);
    }
    std::string floor = "engine floor: ";
    floor += logLevelName(counts.engineFloor);
    out.push_back(ConsoleFooterSegment{.text = std::move(floor), .role = ChromeRole::TextMuted});
    if (counts.agedOut > 0U) {
        appendSegment(out, counts.agedOut, "aged out", "aged out", ChromeRole::TextMuted);
    }
    appendSegment(out, counts.dropped, "dropped", "dropped", ChromeRole::TextFaint);  // ALWAYS (D7)
}

}  // namespace engine::editor
