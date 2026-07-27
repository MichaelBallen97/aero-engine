// tests/editor/console_model_test.cpp — task 2.2.5: the console panel's log model, tier-0 and
// UNGATED. The fourth TU of aero_editor_shell_test (which supplies main() from shell_test.cpp -- do
// NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here). No GPU, no window, no ImGui context: it must
// pass identically with AERO_REQUIRE_GPU unset and set.
//
// Cases that emit REAL records install a TU-local LogFixture, copied from tests/log_test.cpp:19-22
// rather than shared (the 2.2.4/F27 rule: no new header, no new include dir). The logger is a
// process-wide singleton and doctest runs every case in one process, so the fixture is what keeps a
// case from leaking global state into shell_test.cpp / hierarchy_test.cpp / project_files_test.cpp.
// Declare the fixture FIRST in every case so it is destroyed LAST -- after any LogSinkScope.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using engine::LogLevel;
using engine::LogLocation;
using namespace engine::editor;  // test TU, not a header -- docs/04 forbids this only in headers

// console=false keeps the suite's output clean; Trace opens the runtime floor fully so each case
// controls filtering itself (tests/log_test.cpp:19-22).
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

LogEntry makeEntry(LogLevel level, std::string message, std::string file = "t.cpp", std::uint32_t line = 1) {
    LogEntry entry;
    entry.level = level;
    entry.message = std::move(message);
    entry.sourceFile = std::move(file);
    entry.line = line;
    return entry;
}

}  // namespace

TEST_CASE("console: logLevelLabel covers every enumerator") {
    CHECK(std::string_view(logLevelLabel(LogLevel::Trace)) == "TRACE");
    CHECK(std::string_view(logLevelLabel(LogLevel::Debug)) == "DEBUG");
    CHECK(std::string_view(logLevelLabel(LogLevel::Info)) == "INFO");
    CHECK(std::string_view(logLevelLabel(LogLevel::Warn)) == "WARN");
    CHECK(std::string_view(logLevelLabel(LogLevel::Error)) == "ERROR");
    CHECK(std::string_view(logLevelLabel(LogLevel::Critical)) == "CRITICAL");
    CHECK(std::string_view(logLevelLabel(LogLevel::Off)) == "OFF");

    for (const LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error,
                                 LogLevel::Critical, LogLevel::Off}) {
        CHECK(logLevelLabel(level) != nullptr);
    }

    CHECK(LOG_LEVEL_LABEL_WIDTH == std::string_view("CRITICAL").size());
    for (const LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error,
                                 LogLevel::Critical, LogLevel::Off}) {
        CHECK(std::string_view(logLevelLabel(level)).size() <= LOG_LEVEL_LABEL_WIDTH);
    }
}

TEST_CASE("console: formatElapsed pins every boundary") {
    CHECK(formatElapsed(0) == "00:00:00.000");
    CHECK(formatElapsed(999) == "00:00:00.999");
    CHECK(formatElapsed(1000) == "00:00:01.000");
    CHECK(formatElapsed(59999) == "00:00:59.999");
    CHECK(formatElapsed(60000) == "00:01:00.000");
    CHECK(formatElapsed(3599999) == "00:59:59.999");
    CHECK(formatElapsed(3600000) == "01:00:00.000");
    CHECK(formatElapsed(359999999) == "99:59:59.999");
    CHECK(formatElapsed(360000000) == "100:00:00.000");
}

TEST_CASE("console: sanitizeLogMessage flattens every control byte") {
    CHECK(sanitizeLogMessage("a\nb") == "a b");
    CHECK(sanitizeLogMessage("a\r\nb") == "a  b");
    CHECK(sanitizeLogMessage("a\tb") == "a b");

    const std::array<char, 5> raw = {'a', '\0', 'b', '\0', 'c'};
    const std::string_view withNul(raw.data(), raw.size());
    const std::string sanitized = sanitizeLogMessage(withNul);
    CHECK(sanitized == "a b c");
    CHECK(sanitized.size() == 5);

    for (int byte = 0x00; byte <= 0x1F; ++byte) {
        const char c = static_cast<char>(byte);
        CHECK(sanitizeLogMessage(std::string_view(&c, 1)) == " ");
    }
    {
        const char c = static_cast<char>(0x7F);
        CHECK(sanitizeLogMessage(std::string_view(&c, 1)) == " ");
    }
    {
        const char c = static_cast<char>(0x20);
        CHECK(sanitizeLogMessage(std::string_view(&c, 1)) == " ");
    }
    {
        const char c = static_cast<char>(0x7E);
        CHECK(sanitizeLogMessage(std::string_view(&c, 1)) == "~");
    }
    CHECK(sanitizeLogMessage("") == "");
    {
        const char highByte = static_cast<char>(0x80);
        const std::string_view view(&highByte, 1);
        CHECK(sanitizeLogMessage(view) == view);
    }
}

TEST_CASE("console: sanitizeLogMessage truncates on a UTF-8 boundary") {
    const std::string exact(MAX_LOG_MESSAGE_BYTES, 'x');
    CHECK(sanitizeLogMessage(exact) == exact);

    const std::string overByOne = exact + "x";
    const std::string truncated = sanitizeLogMessage(overByOne);
    CHECK(truncated.size() == MAX_LOG_MESSAGE_BYTES + TRUNCATION_MARKER.size());
    CHECK(truncated.ends_with(TRUNCATION_MARKER));

    // A 3-byte UTF-8 sequence (the euro sign) straddling the cap must be dropped WHOLE.
    const std::string capMinusOne(MAX_LOG_MESSAGE_BYTES - 1, 'x');
    const std::string straddling = capMinusOne + "\xE2\x82\xAC";
    const std::string straddlingResult = sanitizeLogMessage(straddling);
    const std::string expectedStraddling = capMinusOne + std::string(TRUNCATION_MARKER);
    CHECK(straddlingResult == expectedStraddling);

    // Malformed input: three continuation-looking bytes straddle the cap (at indices MAX-2, MAX-1 and
    // MAX), so the back-off decrements three times and the attempts<3 guard exhausts BEFORE it finds a
    // lead byte, cutting cleanly at MAX-3 rather than walking further back through the whole message.
    // (A single trailing continuation byte immediately preceded by an ASCII byte -- the OTHER shape --
    // is already covered by the straddling-euro-sign arm above, which backs off exactly once.)
    const std::string malformed = std::string(MAX_LOG_MESSAGE_BYTES - 3, 'x') + std::string(11, '\x80');
    const std::string malformedResult = sanitizeLogMessage(malformed);
    const std::string expectedMalformed = std::string(MAX_LOG_MESSAGE_BYTES - 3, 'x') + std::string(TRUNCATION_MARKER);
    CHECK(malformedResult == expectedMalformed);
}

TEST_CASE("console: logSourceBasename handles both separators") {
    CHECK(logSourceBasename("/a/b/c.cpp") == "c.cpp");
    const std::string windowsPath = "C:\\src\\a.cpp";  // hoisted: MSVC's legacy preprocessor + \" in macros
    CHECK(logSourceBasename(windowsPath.c_str()) == "a.cpp");
    CHECK(logSourceBasename("a.cpp") == "a.cpp");
    CHECK(logSourceBasename("/a/b/") == "");
    CHECK(logSourceBasename("") == "");
    CHECK(logSourceBasename(nullptr) == "");
}

TEST_CASE("console: containsAsciiCaseInsensitive folds only ASCII") {
    CHECK(containsAsciiCaseInsensitive("Hello World", "world"));
    CHECK_FALSE(containsAsciiCaseInsensitive("Hello", "xyz"));
    CHECK(containsAsciiCaseInsensitive("ABC", "abc"));
    CHECK(containsAsciiCaseInsensitive("abc", "ABC"));
    CHECK(containsAsciiCaseInsensitive("anything", ""));
    CHECK_FALSE(containsAsciiCaseInsensitive("ab", "abc"));
    CHECK(containsAsciiCaseInsensitive("", ""));
    CHECK(containsAsciiCaseInsensitive("\xC3\xA9x", "\xC3\xA9"));       // high bytes compare raw
    CHECK_FALSE(containsAsciiCaseInsensitive("\xC3\xA9", "\xC3\x89"));  // no Unicode folding -- the documented limit
}

TEST_CASE("console: matchesFilter composes level and text") {
    const LogEntry entry = makeEntry(LogLevel::Warn, "the disk is nearly full", "disk.cpp");
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Trace, ""}));
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Debug, ""}));
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Info, ""}));
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Warn, ""}));
    CHECK_FALSE(matchesFilter(entry, LogFilter{LogLevel::Error, ""}));
    CHECK_FALSE(matchesFilter(entry, LogFilter{LogLevel::Critical, ""}));

    CHECK(matchesFilter(entry, LogFilter{LogLevel::Trace, "disk"}));
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Trace, "DISK"}));
    CHECK_FALSE(matchesFilter(entry, LogFilter{LogLevel::Trace, "database"}));
    CHECK(matchesFilter(entry, LogFilter{LogLevel::Trace, ""}));

    // D12: the source file is not searched.
    CHECK_FALSE(matchesFilter(entry, LogFilter{LogLevel::Trace, "disk.cpp"}));
}

TEST_CASE("console: LogHistory appends and counts per level") {
    LogHistory h;
    const std::array<LogLevel, 6> levels = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                            LogLevel::Warn,  LogLevel::Error, LogLevel::Critical};
    for (const LogLevel level : levels) {
        h.append(makeEntry(level, "m"));
    }
    CHECK(h.size() == 6);
    for (const LogLevel level : levels) {
        CHECK(h.levelCount(level) == 1);
    }
    CHECK(h.levelCount(LogLevel::Off) == 0);
    CHECK(h.nextSequence() == 6);
    CHECK(h.visibleCount() == 6);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(h.visibleAt(i).sequence == i);
    }
    CHECK(h.capacity() == DEFAULT_LOG_HISTORY_CAPACITY);
}

TEST_CASE("console: LogHistory evicts the oldest past capacity") {
    LogHistory h(4);
    for (int i = 0; i < 10; ++i) {
        h.append(makeEntry(LogLevel::Info, "m" + std::to_string(i)));
    }
    CHECK(h.size() == 4);
    CHECK(h.evictedCount() == 6);
    CHECK(h.visibleAt(0).message == "m6");
    CHECK(h.visibleAt(3).message == "m9");
    CHECK(h.visibleAt(0).sequence == 6);

    LogHistory split(4);
    for (int i = 0; i < 6; ++i) {
        split.append(makeEntry(LogLevel::Info, "i" + std::to_string(i)));
    }
    for (int i = 0; i < 4; ++i) {
        split.append(makeEntry(LogLevel::Warn, "w" + std::to_string(i)));
    }
    CHECK(split.levelCount(LogLevel::Info) == 0);
    CHECK(split.levelCount(LogLevel::Warn) == 4);
}

TEST_CASE("console: LogHistory::setFilter rebuilds and is idempotent") {
    LogHistory h;
    h.append(makeEntry(LogLevel::Trace, "alpha trace"));
    h.append(makeEntry(LogLevel::Info, "beta info"));
    h.append(makeEntry(LogLevel::Warn, "gamma warn"));
    h.append(makeEntry(LogLevel::Error, "alpha error"));

    h.setFilter(LogFilter{LogLevel::Warn, ""});
    CHECK(h.visibleCount() == 2);
    CHECK(h.visibleAt(0).message == "gamma warn");
    CHECK(h.visibleAt(1).message == "alpha error");

    h.setFilter(LogFilter{LogLevel::Trace, "alpha"});
    CHECK(h.visibleCount() == 2);
    CHECK(h.visibleAt(0).message == "alpha trace");
    CHECK(h.visibleAt(1).message == "alpha error");

    const std::size_t before = h.visibleCount();
    const std::uint64_t firstSeq = h.visibleAt(0).sequence;
    h.setFilter(LogFilter{LogLevel::Trace, "alpha"});  // identical filter -- no-op
    CHECK(h.visibleCount() == before);
    CHECK(h.visibleAt(0).sequence == firstSeq);

    h.setFilter(LogFilter{});
    CHECK(h.visibleCount() == 4);
    CHECK(h.visibleAt(0).message == "alpha trace");
    CHECK(h.visibleAt(3).message == "alpha error");
}

TEST_CASE("console: the filtered view survives eviction (D13)") {
    LogHistory h(8);
    h.setFilter(LogFilter{LogLevel::Trace, "keep"});
    for (int i = 0; i < 100; ++i) {
        h.append(makeEntry(LogLevel::Info, ((i % 2 == 0) ? "keep " : "drop ") + std::to_string(i)));
    }
    CHECK(h.size() == 8);
    CHECK(h.visibleCount() == 4);

    std::uint64_t previousSeq = 0;
    for (std::size_t i = 0; i < h.visibleCount(); ++i) {
        const LogEntry& entry = h.visibleAt(i);
        CHECK(matchesFilter(entry, h.filter()));
        if (i > 0) {
            CHECK(entry.sequence > previousSeq);
        }
        previousSeq = entry.sequence;
        CHECK(entry.sequence >= h.nextSequence() - 8);
    }

    const std::vector<std::string> expected = {"keep 92", "keep 94", "keep 96", "keep 98"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(h.visibleAt(i).message == expected[i]);
    }
}

TEST_CASE("console: LogHistory::clear keeps the filter and the sequence") {
    LogHistory h;
    h.setFilter(LogFilter{LogLevel::Warn, ""});
    h.append(makeEntry(LogLevel::Info, "info"));
    h.append(makeEntry(LogLevel::Warn, "warn"));
    h.noteDropped(5);

    h.clear();
    CHECK(h.size() == 0);
    CHECK(h.visibleCount() == 0);
    CHECK(h.evictedCount() == 0);
    CHECK(h.droppedCount() == 0);
    for (const LogLevel level :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical}) {
        CHECK(h.levelCount(level) == 0);
    }
    CHECK(h.filter() == LogFilter{LogLevel::Warn, ""});
    const std::uint64_t nextSeqAfterClear = h.nextSequence();
    CHECK(nextSeqAfterClear == 2);

    h.append(makeEntry(LogLevel::Warn, "post-clear"));
    CHECK(h.size() == 1);
    CHECK(h.visibleAt(0).sequence >= nextSeqAfterClear);
}

TEST_CASE("console: noteDropped accumulates and clears") {
    LogHistory h;
    h.noteDropped(3);
    h.noteDropped(4);
    CHECK(h.droppedCount() == 7);
    h.clear();
    CHECK(h.droppedCount() == 0);
    h.noteDropped(0);
    CHECK(h.droppedCount() == 0);
}

TEST_CASE("console: LogHistory clamps a zero capacity") {
    LogHistory h(0);
    CHECK(h.capacity() == 1);
    for (int i = 0; i < 5; ++i) {
        h.append(makeEntry(LogLevel::Info, "m" + std::to_string(i)));
    }
    CHECK(h.size() == 1);
    CHECK(h.evictedCount() == 4);
    CHECK(h.visibleAt(0).message == "m4");
    CHECK(h.visibleCount() == 1);
}

TEST_CASE("console: buildClipboardText renders the visible view") {
    LogHistory h;
    h.append(makeEntry(LogLevel::Info, "hello", "a.cpp", 10));
    h.append(makeEntry(LogLevel::Warn, "world", "", 0));
    h.append(makeEntry(LogLevel::Error, "boom", "b.cpp", 42));

    const std::string text = buildClipboardText(h);
    std::string expected;
    expected += formatElapsed(h.visibleAt(0).elapsedMs) + "  INFO      hello  (a.cpp:10)\n";
    expected += formatElapsed(h.visibleAt(1).elapsedMs) + "  WARN      world\n";
    expected += formatElapsed(h.visibleAt(2).elapsedMs) + "  ERROR     boom  (b.cpp:42)\n";
    CHECK(text == expected);

    const LogHistory empty;
    CHECK(buildClipboardText(empty) == "");

    h.setFilter(LogFilter{LogLevel::Warn, ""});
    const std::string filteredText = buildClipboardText(h);
    CHECK(filteredText.find("hello") == std::string::npos);
    const auto lineCount = static_cast<std::size_t>(std::count(filteredText.begin(), filteredText.end(), '\n'));
    CHECK(lineCount == h.visibleCount());
}

TEST_CASE("console: LogSink push/take round-trips in order") {
    LogSink sink;
    const LogLocation loc{"f.cpp", "fn", 7};
    sink.push(LogLevel::Info, "one", loc);
    sink.push(LogLevel::Warn, "two", loc);
    sink.push(LogLevel::Error, "three", loc);

    std::vector<LogEntry> out;
    const std::uint64_t droppedFirst = sink.take(out);
    CHECK(droppedFirst == 0);
    CHECK(out.size() == 3);
    CHECK(out[0].message == "one");
    CHECK(out[0].level == LogLevel::Info);
    CHECK(out[0].line == 7);
    CHECK(out[1].message == "two");
    CHECK(out[1].level == LogLevel::Warn);
    CHECK(out[2].message == "three");
    CHECK(out[2].level == LogLevel::Error);
    CHECK(out[1].elapsedMs >= out[0].elapsedMs);
    CHECK(out[2].elapsedMs >= out[1].elapsedMs);

    out.clear();
    const std::uint64_t droppedSecond = sink.take(out);
    CHECK(out.empty());
    CHECK(droppedSecond == 0);
}

TEST_CASE("console: LogSink drops the newest at capacity") {
    LogSink sink(4);
    const LogLocation loc{"f.cpp", "fn", 1};
    for (int i = 0; i < 9; ++i) {
        sink.push(LogLevel::Info, "p" + std::to_string(i), loc);
    }
    std::vector<LogEntry> out;
    const std::uint64_t droppedFirst = sink.take(out);
    REQUIRE(out.size() == 4);
    CHECK(out[0].message == "p0");
    CHECK(out[1].message == "p1");
    CHECK(out[2].message == "p2");
    CHECK(out[3].message == "p3");
    CHECK(droppedFirst == 5);

    out.clear();
    const std::uint64_t droppedSecond = sink.take(out);
    CHECK(out.empty());
    CHECK(droppedSecond == 0);
    CHECK(sink.stagingCapacity() == 4);
}

TEST_CASE("console: LogSink is safe under 8 concurrent producers") {
    LogSink sink;  // default capacity (4096) > 2000, so nothing can be dropped
    constexpr int THREAD_COUNT = 8;
    constexpr int PUSHES_PER_THREAD = 250;
    constexpr int EXPECTED_TOTAL = THREAD_COUNT * PUSHES_PER_THREAD;

    // Each producer decrements this once its pushes are all issued. The main thread take()s in a loop
    // while any producer is still running (so the mutex never has to hold a full 2000-record burst),
    // then joins, then does ONE final take() to catch anything pushed between the last decrement it
    // observed and the corresponding thread actually finishing.
    std::atomic<int> threadsRemaining{THREAD_COUNT};
    std::vector<std::thread> producers;
    producers.reserve(THREAD_COUNT);
    for (int t = 0; t < THREAD_COUNT; ++t) {
        producers.emplace_back([&sink, &threadsRemaining, t]() {
            const LogLocation loc{"stress.cpp", "producer", static_cast<std::uint32_t>(t)};
            for (int i = 0; i < PUSHES_PER_THREAD; ++i) {
                sink.push(LogLevel::Info, "t" + std::to_string(t) + " m" + std::to_string(i), loc);
            }
            threadsRemaining.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    std::vector<std::string> received;
    received.reserve(static_cast<std::size_t>(EXPECTED_TOTAL));
    std::uint64_t droppedTotal = 0;
    std::vector<LogEntry> batch;
    while (threadsRemaining.load(std::memory_order_relaxed) > 0) {
        batch.clear();
        droppedTotal += sink.take(batch);
        for (LogEntry& entry : batch) {
            received.push_back(std::move(entry.message));
        }
    }
    for (std::thread& th : producers) {
        th.join();
    }
    batch.clear();
    droppedTotal += sink.take(batch);
    for (LogEntry& entry : batch) {
        received.push_back(std::move(entry.message));
    }

    CHECK(received.size() == static_cast<std::size_t>(EXPECTED_TOTAL));
    CHECK(droppedTotal == 0);

    std::vector<std::string> expected;
    expected.reserve(static_cast<std::size_t>(EXPECTED_TOTAL));
    for (int t = 0; t < THREAD_COUNT; ++t) {
        for (int i = 0; i < PUSHES_PER_THREAD; ++i) {
            expected.push_back("t" + std::to_string(t) + " m" + std::to_string(i));
        }
    }
    std::sort(received.begin(), received.end());
    std::sort(expected.begin(), expected.end());
    CHECK(received == expected);
}

TEST_CASE("console: LogSinkScope captures real engine records") {
    const LogFixture fixture;
    const LogSinkScope scope;
    const auto expectedLine = static_cast<std::uint32_t>(__LINE__) + 1U;
    AERO_LOG_WARN("console: captured record {}", 7);

    std::vector<LogEntry> out;
    scope.sink()->take(out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].level == LogLevel::Warn);
    CHECK(out[0].message == "console: captured record 7");
    CHECK(out[0].sourceFile == "console_model_test.cpp");
    CHECK(out[0].line == expectedLine);

    engine::setLogLevel(LogLevel::Error);
    AERO_LOG_INFO("below the floor");
    out.clear();
    scope.sink()->take(out);
    CHECK(out.empty());
}

TEST_CASE("console: detaching releases the callback's reference") {
    const LogFixture fixture;
    {
        LogSinkScope scope;
        std::shared_ptr<LogSink> observer = scope.sink();
        const std::weak_ptr<LogSink> weak = observer;
        CHECK(observer.use_count() >= 3);

        scope.detach();
        CHECK(observer.use_count() == 1);
        CHECK_FALSE(scope.installed());

        AERO_LOG_ERROR("nobody should receive this");
        std::vector<LogEntry> out;
        observer->take(out);
        CHECK(out.empty());

        observer.reset();
        CHECK(weak.expired());
    }
    {
        // Repeat the whole arm with DESTRUCTION instead of an explicit detach(): `observer` and `weak`
        // outlive the inner block so `scope`'s destructor (which detaches via ~LogSinkScope) can run
        // while a separate reference is still held, then observer.reset() proves the sink is truly
        // gone rather than merely unreachable through `scope`.
        std::shared_ptr<LogSink> observer;
        std::weak_ptr<LogSink> weak;
        {
            const LogSinkScope scope;
            observer = scope.sink();
            weak = observer;
            CHECK(observer.use_count() >= 3);
        }  // scope destructs here -- detaches via ~LogSinkScope
        CHECK(observer.use_count() == 1);
        CHECK_FALSE(weak.expired());  // not yet -- `observer` still holds it alive
        observer.reset();
        CHECK(weak.expired());  // NOW the sink is truly gone, not leaked
    }
}

TEST_CASE("console: the installed callback holds a shared reference (R14)") {
    const LogFixture fixture;
    const LogSinkScope scope;
    CHECK(scope.installed());
    CHECK(scope.sink().use_count() >= 2);
}

TEST_CASE("console: a second scope displaces the first without breaking it (E16)") {
    const LogFixture fixture;
    LogSinkScope a;
    LogSinkScope b;
    CHECK_FALSE(a.installed());
    CHECK(b.installed());

    AERO_LOG_INFO("routed to b");
    std::vector<LogEntry> outA;
    std::vector<LogEntry> outB;
    a.sink()->take(outA);
    b.sink()->take(outB);
    CHECK(outA.empty());
    CHECK(outB.size() == 1);

    a.detach();
    AERO_LOG_INFO("still routed to b");
    outB.clear();
    b.sink()->take(outB);
    CHECK(outB.size() == 1);

    // b.sink() is about to become null once detached; keep an observer alive so the "routed to
    // nobody" check below has something to call take() on.
    const std::shared_ptr<LogSink> bObserver = b.sink();
    b.detach();
    AERO_LOG_INFO("routed to nobody");
    outB.clear();
    bObserver->take(outB);
    CHECK(outB.empty());
}

TEST_CASE("console: LogSinkScope moves cleanly") {
    const LogFixture fixture;
    // Wrapped in std::optional (the shell_test.cpp/transform_test.cpp/rhi_device_test.cpp precedent):
    // moving *a rather than a bare local, and reading the moved-from state back through a-> afterward,
    // is what keeps bugprone-use-after-move from flagging a deliberate moved-from-state assertion.
    std::optional<LogSinkScope> a;
    a.emplace();
    LogSink* const raw = a->sink().get();
    LogSinkScope b(std::move(*a));
    CHECK(a->sink() == nullptr);
    CHECK_FALSE(a->installed());
    CHECK(b.installed());
    CHECK(b.sink().get() == raw);

    AERO_LOG_INFO("reaches b");
    std::vector<LogEntry> out;
    b.sink()->take(out);
    CHECK(out.size() == 1);

    LogSinkScope c;
    const std::shared_ptr<LogSink> observer = c.sink();
    CHECK(observer.use_count() >= 2);
    c = std::move(b);
    CHECK(observer.use_count() == 1);  // c's OWN prior installation was detached first
    CHECK(c.sink().get() == raw);

    out.clear();
    AERO_LOG_INFO("still reaches the moved-in sink");
    c.sink()->take(out);
    CHECK(out.size() == 1);

    // A moved-from scope's destructor detaches nothing. The moved-from `d` must therefore be DESTROYED
    // while the observer and the moved-to scope are still alive -- asserting before any destructor has
    // run would only restate that moving a shared_ptr preserves the total count, which is true by
    // definition and would hold even if ~d wrongly detached.
    {
        std::optional<LogSinkScope> movedTo;
        std::shared_ptr<LogSink> dObserver;
        {
            std::optional<LogSinkScope> d;
            d.emplace();
            dObserver = d->sink();
            movedTo.emplace(std::move(*d));
            d.reset();  // the moved-from scope dies HERE, with movedTo and dObserver outliving it
        }
        CHECK(movedTo->sink() == dObserver);
        CHECK(movedTo->installed());        // ~d did not clear the routing movedTo owns
        CHECK(dObserver.use_count() >= 2);  // ... and did not drop the callback's reference
    }
}

TEST_CASE("console: move-assignment never seizes a third scope's installation") {
    const LogFixture fixture;
    // a and b are both displaced by x, which is the active installation. Assigning b into a must move
    // the sink WITHOUT stealing routing from x: no scope was constructed or destroyed, so x's console
    // has no reason to go dead. Re-installing unconditionally here would silently kill it.
    std::optional<LogSinkScope> a;
    a.emplace();
    std::optional<LogSinkScope> b;
    b.emplace();
    const LogSinkScope x;
    CHECK(x.installed());
    CHECK_FALSE(a->installed());
    CHECK_FALSE(b->installed());

    LogSink* const bRaw = b->sink().get();
    *a = std::move(*b);
    CHECK(a->sink().get() == bRaw);  // the sink moved
    CHECK(x.installed());            // ... and x still owns the routing
    CHECK_FALSE(a->installed());

    std::vector<LogEntry> out;
    AERO_LOG_INFO("still reaches x");
    x.sink()->take(out);
    CHECK(out.size() == 1);

    out.clear();
    a->sink()->take(out);
    CHECK(out.empty());  // a holds a sink nothing routes to, exactly as before the assignment
}

TEST_CASE("console: install/destroy under concurrent logging (R14 stress)") {
    const LogFixture fixture;
    std::atomic<bool> stop{false};
    constexpr int LOGGER_THREAD_COUNT = 4;
    std::vector<std::thread> loggers;
    loggers.reserve(LOGGER_THREAD_COUNT);
    for (int t = 0; t < LOGGER_THREAD_COUNT; ++t) {
        loggers.emplace_back([&stop, t]() {
            while (!stop.load(std::memory_order_relaxed)) {
                AERO_LOG_INFO("stress thread {} tick", t);
            }
        });
    }

    constexpr int ROUNDS = 50;
    for (int round = 0; round < ROUNDS; ++round) {
        const LogSinkScope s;
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& th : loggers) {
        th.join();
    }
}

TEST_CASE("console: an Off-level record is stored but gets no counter slot (plan C1)") {
    LogHistory h;
    h.append(makeEntry(LogLevel::Off, "floor value"));
    CHECK(h.size() == 1);
    CHECK(h.visibleCount() == 1);  // Off >= Trace
    CHECK(h.visibleAt(0).message == "floor value");
    CHECK(std::string_view(logLevelLabel(h.visibleAt(0).level)) == "OFF");
    CHECK(h.levelCount(LogLevel::Off) == 0);
    // THIS is the assertion that actually discriminates a missing guard, and it is not decoration.
    // levelCount() has its own independent range check, so it reads 0 whatever the write side did --
    // it cannot witness the overflow. `counts` is a 6-slot array followed immediately by activeFilter,
    // so an unguarded ++counts[Off] lands on activeFilter.minLevel and silently promotes the filter
    // from Trace to Debug. Neither a sanitizer nor any other assertion here sees that: it is an
    // intra-object write, and std::array::operator[] is not instrumented. Comparing the filter against
    // a default-constructed one is what turns the C1 guard from asserted-in-prose into tested.
    CHECK(h.filter() == LogFilter{});

    LogHistory small(1);
    small.append(makeEntry(LogLevel::Off, "evicted"));
    small.append(makeEntry(LogLevel::Info, "survivor"));
    CHECK(small.size() == 1);
    CHECK(small.evictedCount() == 1);
    CHECK(small.levelCount(LogLevel::Info) == 1);
    // The mirror for the EVICTION decrement: unguarded, --counts[Off] on the evicted record underflows
    // the same neighbouring byte to 255 instead of promoting it to 1.
    CHECK(small.filter() == LogFilter{});
}
