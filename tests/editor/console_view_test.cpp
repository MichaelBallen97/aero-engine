// Aero Engine -- task E.6.3: the Console's chrome as VALUES (CS1-CS10). Tier 0: no ImGui, no GPU, no #if.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>  // formatElapsed, logLevelLabel -- the cross-checks
#include <aero/editor/console_view.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using ed::ChromeRole;
using engine::LogLevel;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

struct SegmentRow {
    std::string_view text;
    ChromeRole role;
};

void checkSegments(const std::vector<ed::ConsoleFooterSegment>& actual, const std::vector<SegmentRow>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i].text == expected[i].text);
        CHECK((actual[i].role == expected[i].role));
    }
}

}  // namespace

TEST_CASE("console view: a token prefix splits off as the channel, as views into the input (task E.6.3, CS1)") {
    const std::string_view m = "rhi: device lost";
    const ed::LogChannelSplit a = ed::splitLogChannel(m);
    CHECK(a.channel == "rhi");
    CHECK(a.message == "device lost");
    CHECK(a.channel.data() == m.data());      // INTO the input: no allocation
    CHECK(a.message.data() == m.data() + 5);  // past "rhi: "
    const ed::LogChannelSplit b = ed::splitLogChannel("component_ops: x");
    CHECK(b.channel == "component_ops");
    CHECK(b.message == "x");
    const ed::LogChannelSplit c = ed::splitLogChannel("a: b");  // the minimal accept
    CHECK(c.channel == "a");
    CHECK(c.message == "b");
}

TEST_CASE("console view: a channel is at most 24 identifier bytes (task E.6.3, CS2)") {
    const std::string at24 = std::string(24U, 'a') + ": x";
    const std::string at25 = std::string(25U, 'a') + ": x";
    CHECK(ed::splitLogChannel(at24).channel.size() == 24U);
    CHECK(ed::splitLogChannel(at24).message == "x");
    CHECK(ed::splitLogChannel(at25).channel.empty());
    CHECK(ed::splitLogChannel(at25).message == at25);  // the whole message, untouched
}

TEST_CASE("console view: a malformed prefix leaves the message whole (task E.6.3, CS3)") {
    for (const std::string_view m : {"rhi:", "rhi: ", "rhi:x", " rhi: x", "9rhi: x", "AudioDevice::open: x",
                                     "render::DebugDraw::create - x", "", "no prefix at all"}) {
        CAPTURE(m);
        const ed::LogChannelSplit split = ed::splitLogChannel(m);
        CHECK(split.channel.empty());
        CHECK(split.message == m);
    }
}

TEST_CASE("console view: the stamp is mm:ss.mmm, widening to h:mm:ss.mmm from an hour (task E.6.3, CS4)") {
    std::string out = "stale";
    ed::formatConsoleStamp(0U, out);
    CHECK(out == "00:00.000");  // and `out` was cleared first
    ed::formatConsoleStamp(59999U, out);
    CHECK(out == "00:59.999");
    ed::formatConsoleStamp(3599999U, out);
    CHECK(out == "59:59.999");
    ed::formatConsoleStamp(3600000U, out);
    CHECK(out == "1:00:00.000");
    ed::formatConsoleStamp(360000000U, out);
    CHECK(out == "100:00:00.000");
    // THE FIT, stated: the stamp column reserves 11 advances, and 36 000 000 ms (10 h) is the first stamp past them.
    for (const std::uint64_t ms : {0ULL, 3599999ULL, 3600000ULL, 35999999ULL}) {
        CAPTURE(ms);
        ed::formatConsoleStamp(ms, out);
        CHECK(static_cast<float>(out.size()) <= ed::CONSOLE_STAMP_COLUMN_ADVANCES);
    }
    ed::formatConsoleStamp(36000000ULL, out);
    CHECK(out == "10:00:00.000");
    CHECK(static_cast<float>(out.size()) > ed::CONSOLE_STAMP_COLUMN_ADVANCES);
    // Copy keeps formatElapsed: two of console_model_test.cpp's own literals, restated.
    CHECK(ed::formatElapsed(0U) == "00:00:00.000");
    CHECK(ed::formatElapsed(3600000U) == "01:00:00.000");
}

TEST_CASE("console view: level names are title case; the tag label stays upper case (task E.6.3, CS5)") {
    CHECK(ed::logLevelName(LogLevel::Trace) == "Trace");
    CHECK(ed::logLevelName(LogLevel::Debug) == "Debug");
    CHECK(ed::logLevelName(LogLevel::Info) == "Info");
    CHECK(ed::logLevelName(LogLevel::Warn) == "Warn");
    CHECK(ed::logLevelName(LogLevel::Error) == "Error");
    CHECK(ed::logLevelName(LogLevel::Critical) == "Critical");
    CHECK(ed::logLevelName(LogLevel::Off) == "Off");
    CHECK(std::string_view(ed::logLevelLabel(LogLevel::Warn)) == "WARN");
    CHECK(std::string_view(ed::logLevelLabel(LogLevel::Critical)) == "CRITICAL");
}

TEST_CASE("console view: each level's tag role (task E.6.3, CS6)") {
    CHECK((ed::logLevelTagRole(LogLevel::Trace) == ChromeRole::TextFaint));
    CHECK((ed::logLevelTagRole(LogLevel::Debug) == ChromeRole::TextMuted));
    CHECK((ed::logLevelTagRole(LogLevel::Info) == ChromeRole::TextLabel));
    CHECK((ed::logLevelTagRole(LogLevel::Warn) == ChromeRole::Warning));
    CHECK((ed::logLevelTagRole(LogLevel::Error) == ChromeRole::Error));
    CHECK((ed::logLevelTagRole(LogLevel::Critical) == ChromeRole::Critical));
    CHECK((ed::logLevelTagRole(LogLevel::Off) == ChromeRole::TextFaint));
}

TEST_CASE("console view: Warn, Error and Critical rows are tinted, the rest are not (task E.6.3, CS7)") {
    const std::optional<ed::Srgb8> warn = ed::consoleRowTint(LogLevel::Warn);
    REQUIRE(warn.has_value());
    CHECK((*warn == ed::Srgb8{35U, 35U, 35U, 255U}));
    for (const LogLevel level : {LogLevel::Error, LogLevel::Critical}) {
        const std::optional<ed::Srgb8> tint = ed::consoleRowTint(level);
        REQUIRE(tint.has_value());
        CHECK((*tint == ed::Srgb8{38U, 33U, 37U, 255U}));
    }
    for (const LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Off}) {
        CHECK_FALSE(ed::consoleRowTint(level).has_value());
    }
}

TEST_CASE("console view: the four columns are advances plus a scaled gap (task E.6.3, CS8)") {
    // 6.462: Plex Mono's advance at 14 (the spec's fontTools measurement). The ascents are illustrative inputs.
    const ed::ConsoleRowLayout one = ed::consoleRowLayout(0.0F, 6.462F, 12.0F, 10.5F, 1.0F);
    CHECK(one.stampX == 0.0F);
    CHECK(one.tagX == doctest::Approx(87.082F).epsilon(1e-5));       // 11 x 6.462 + 16
    CHECK(one.channelX == doctest::Approx(154.778F).epsilon(1e-5));  // + 8 x 6.462 + 16
    CHECK(one.messageX == doctest::Approx(261.246F).epsilon(1e-5));  // + 14 x 6.462 + 16
    CHECK(one.smallTextOffsetY == 1.5F);                             // 12 - 10.5: baseline-aligned
    // Scale 2: the caller's advance is already scaled; the GAPS double.
    const ed::ConsoleRowLayout two = ed::consoleRowLayout(0.0F, 12.924F, 24.0F, 21.0F, 2.0F);
    CHECK(two.tagX == doctest::Approx(174.164F).epsilon(1e-5));  // 11 x 12.924 + 32
    CHECK(ed::consoleSegmentGap(1.0F) == 16.0F);
    CHECK(ed::consoleSegmentGap(2.0F) == 32.0F);
    CHECK(ed::consoleSegmentGap(NAN_F) == 16.0F);
    CHECK((ed::consoleRowLayout(0.0F, 6.462F, 12.0F, 10.5F, NAN_F) == one));
    // THE OVERFLOW, stated (D7): a 12-character stamp (10 h) overruns its column by one advance and still ends left
    // of the tag; 13 (100 h) too; 14 (1000 h) is the first to cross it. The column never widens.
    const float a = 6.462F;
    CHECK(12.0F * a < one.tagX);
    CHECK(13.0F * a < one.tagX);
    CHECK(14.0F * a > one.tagX);
    // The header cluster: right-aligned, or at the row's left edge when the row is narrower than it.
    CHECK(ed::consoleHeaderClusterX(10.0F, 500.0F, 300.0F) == 210.0F);
    CHECK(ed::consoleHeaderClusterX(10.0F, 200.0F, 300.0F) == 10.0F);
    CHECK(ed::consoleHeaderClusterX(10.0F, NAN_F, 300.0F) == 10.0F);
}

TEST_CASE("console view: the footer's counts, roles and singulars (task E.6.3, CS9)") {
    std::vector<ed::ConsoleFooterSegment> out;
    ed::ConsoleFooterCounts counts{.held = 418U, .visible = 418U, .warnings = 3U, .engineFloor = LogLevel::Info};
    ed::consoleFooterSegments(counts, out);
    checkSegments(out, {{"418 messages", ChromeRole::TextMuted},
                        {"3 warnings", ChromeRole::Warning},
                        {"0 errors", ChromeRole::TextMuted},
                        {"engine floor: Info", ChromeRole::TextMuted},
                        {"0 dropped", ChromeRole::TextFaint}});
    counts.filtered = true;
    counts.visible = 12U;
    counts.warnings = 0U;
    ed::consoleFooterSegments(counts, out);
    CHECK(out.front().text == "12 of 418 messages");
    CHECK((out[1].role == ChromeRole::TextMuted));  // 0 warnings: muted, not the warning hue
    const ed::ConsoleFooterCounts singular{
        .held = 1U, .visible = 1U, .warnings = 1U, .errors = 1U, .critical = 2U, .engineFloor = LogLevel::Trace};
    ed::consoleFooterSegments(singular, out);
    checkSegments(out, {{"1 message", ChromeRole::TextMuted},
                        {"1 warning", ChromeRole::Warning},
                        {"1 error", ChromeRole::Error},
                        {"2 critical", ChromeRole::Critical},
                        {"engine floor: Trace", ChromeRole::TextMuted},
                        {"0 dropped", ChromeRole::TextFaint}});
}

TEST_CASE("console view: the footer's tail and its order (task E.6.3, CS10)") {
    std::vector<ed::ConsoleFooterSegment> out;
    const ed::ConsoleFooterCounts all{.held = 9U,
                                      .visible = 9U,
                                      .warnings = 2U,
                                      .errors = 3U,
                                      .critical = 4U,
                                      .engineFloor = LogLevel::Info,
                                      .agedOut = 5U,
                                      .dropped = 6U};
    ed::consoleFooterSegments(all, out);
    checkSegments(out, {{"9 messages", ChromeRole::TextMuted},
                        {"2 warnings", ChromeRole::Warning},
                        {"3 errors", ChromeRole::Error},
                        {"4 critical", ChromeRole::Critical},
                        {"engine floor: Info", ChromeRole::TextMuted},
                        {"5 aged out", ChromeRole::TextMuted},
                        {"6 dropped", ChromeRole::TextFaint}});
    const ed::ConsoleFooterCounts quiet{.held = 0U, .visible = 0U, .engineFloor = LogLevel::Info};
    ed::consoleFooterSegments(quiet, out);
    REQUIRE_FALSE(out.empty());
    CHECK(out.back().text == "0 dropped");  // ALWAYS present (seed S20)
    for (const ed::ConsoleFooterSegment& segment : out) {
        CHECK(segment.text.find("aged out") == std::string::npos);  // omitted at 0
        CHECK(segment.text.find("critical") == std::string::npos);  // omitted at 0
    }
}
