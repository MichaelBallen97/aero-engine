// Exercises the logging API (task 0.2.4): levels, formatting, call-site capture, the
// callback seam, compile-time gating, and thread safety. The logger is a process-wide
// singleton and doctest runs every case in one process, so LogFixture restores global state.
#include <aero/core/log.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// console=false keeps the suite's output clean; Trace opens the runtime floor fully so each
// case controls filtering itself.
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
};

struct Captured {
    engine::LogLevel level = engine::LogLevel::Info;
    std::string message;
    std::string file;
    std::uint32_t line = 0;
};

// A named free function so __func__ has a stable, assertable spelling. AERO_LOG_* captures
// __func__ at the CALL SITE, so a record raised here must name this function rather than
// anything inside the logging implementation. It lives outside a TEST_CASE because a
// TEST_CASE body's own __func__ is a doctest-generated name.
void emitInfoFromNamedFunction() { AERO_LOG_INFO("raised from a named function"); }

}  // namespace

TEST_CASE("log: the runtime level gates records") {
    const LogFixture fixture;
    engine::setLogLevel(engine::LogLevel::Warn);
    CHECK(engine::logLevel() == engine::LogLevel::Warn);
    CHECK_FALSE(engine::logEnabled(engine::LogLevel::Trace));
    CHECK_FALSE(engine::logEnabled(engine::LogLevel::Info));
    CHECK(engine::logEnabled(engine::LogLevel::Warn));
    CHECK(engine::logEnabled(engine::LogLevel::Critical));

    // Off is a floor value: it silences even Critical.
    engine::setLogLevel(engine::LogLevel::Off);
    CHECK_FALSE(engine::logEnabled(engine::LogLevel::Critical));
}

TEST_CASE("log: the callback receives the formatted message, level, and call site") {
    const LogFixture fixture;
    std::vector<Captured> records;
    engine::setLogCallback([&records](const engine::LogRecord& record) {
        records.push_back({record.level, std::string{record.message}, record.location.file, record.location.line});
    });

    const auto expectedLine = static_cast<std::uint32_t>(__LINE__) + 1;
    AERO_LOG_INFO("player {} at {:.2f}", "kai", 1.5);

    REQUIRE(records.size() == 1);
    CHECK(records[0].level == engine::LogLevel::Info);
    CHECK(records[0].message == "player kai at 1.50");
    CHECK(records[0].line == expectedLine);
    CHECK(records[0].file.find("log_test.cpp") != std::string::npos);
}

TEST_CASE("log: records below the runtime level never reach the callback") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1); });

    engine::setLogLevel(engine::LogLevel::Error);
    AERO_LOG_INFO("dropped");
    AERO_LOG_WARN("dropped");
    AERO_LOG_ERROR("kept");
    AERO_LOG_CRITICAL("kept");
    CHECK(count.load() == 2);
}

// Proves D5's choice of the non-formatting spdlog overload: a message that already contains
// braces must survive verbatim and must not be reparsed as a format string.
TEST_CASE("log: braces in a formatted message are not reinterpreted") {
    const LogFixture fixture;
    std::string captured;
    engine::setLogCallback([&captured](const engine::LogRecord& record) { captured = std::string{record.message}; });

    AERO_LOG_INFO("{}", "literal {braces} and {0} survive");
    CHECK(captured == "literal {braces} and {0} survive");
}

// Proves D6: below AERO_LOG_ACTIVE_LEVEL nothing is emitted even with the runtime floor wide
// open. Debug presets compile Trace in; Release presets (NDEBUG -> Info) discard both.
TEST_CASE("log: AERO_LOG_ACTIVE_LEVEL discards records at compile time") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1); });
    engine::setLogLevel(engine::LogLevel::Trace);

    AERO_LOG_TRACE("trace");
    AERO_LOG_DEBUG("debug");

#if AERO_LOG_ACTIVE_LEVEL <= AERO_LOG_LEVEL_TRACE
    CHECK(count.load() == 2);
#elif AERO_LOG_ACTIVE_LEVEL <= AERO_LOG_LEVEL_DEBUG
    CHECK(count.load() == 1);
#else
    CHECK(count.load() == 0);
#endif
}

TEST_CASE("log: macros are single statements usable after a braceless if/else") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1); });

    // Must compile AND bind the else to this if — that is the do/while(false) wrapper.
    const bool flag = true;
    if (flag)
        AERO_LOG_INFO("then-branch");
    else
        AERO_LOG_ERROR("else-branch");

    CHECK(count.load() == 1);
}

TEST_CASE("log: setLogCallback({}) detaches the callback") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1); });
    AERO_LOG_INFO("delivered");
    engine::setLogCallback({});
    AERO_LOG_INFO("not delivered");
    CHECK(count.load() == 1);
}

TEST_CASE("log: concurrent logging from many threads is safe") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1, std::memory_order_relaxed); });

    constexpr int THREAD_COUNT = 8;
    constexpr int PER_THREAD = 50;
    std::vector<std::thread> threads;
    threads.reserve(THREAD_COUNT);
    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                AERO_LOG_INFO("thread {} message {}", t, i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(count.load() == THREAD_COUNT * PER_THREAD);
}

// Also the console sink's end-to-end smoke test: the lazy reinit restores console=true, so
// this case intentionally prints ONE line. Test presets set outputOnFailure, so it stays
// hidden unless the suite fails.
TEST_CASE("log: logging after shutdownLogging lazily reinitialises") {
    engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false});
    engine::shutdownLogging();
    CHECK(engine::logLevel() == engine::DEFAULT_LOG_LEVEL);

    AERO_LOG_INFO("logging after shutdown must not crash");

    engine::shutdownLogging();  // idempotent
}

// AC-6 names file, function AND line, but the call-site case above asserts only file and line.
// __func__ is captured by a different macro argument than __FILE__/__LINE__, so it can be wrong
// on its own.
TEST_CASE("log: the callback receives the call-site function name") {
    const LogFixture fixture;
    std::string function;
    engine::setLogCallback([&function](const engine::LogRecord& record) {
        // Guarded: LogLocation::function is a raw pointer, and std::string{nullptr} is UB.
        function = record.location.function != nullptr ? record.location.function : "<null>";
    });

    emitInfoFromNamedFunction();

    // find(), not ==: __func__'s exact spelling is implementation-defined. Clang, GCC and MSVC
    // all embed the bare name, but none of them promise the absence of decoration.
    CHECK(function.find("emitInfoFromNamedFunction") != std::string::npos);
}

// detail::logWrite is the header's documented entry point for a message whose text is only
// known at runtime (std::format_string needs a compile-time-constant format), and it is what
// every AERO_LOG_* call ultimately funnels into. Nothing else covers it directly.
TEST_CASE("log: logWrite delivers a runtime-built message and re-checks the level") {
    const LogFixture fixture;
    std::vector<Captured> records;
    engine::setLogCallback([&records](const engine::LogRecord& record) {
        records.push_back({record.level, std::string{record.message}, record.location.file, record.location.line});
    });

    // Long enough to defeat std::string's small-string optimisation, so LogRecord::message
    // views heap memory: were the callback ever invoked after the caller's buffer died, ASan
    // would fault here rather than silently comparing freed bytes. This guards the lifetime
    // contract that the planned std::format_to optimisation would most easily break.
    const std::string runtimeBuilt = "runtime-built " + std::string(64, 'x');
    const engine::LogLocation location{"synthetic.cpp", "syntheticFunction", 4242};

    engine::detail::logWrite(engine::LogLevel::Warn, location, runtimeBuilt);

    REQUIRE(records.size() == 1);
    CHECK(records[0].level == engine::LogLevel::Warn);
    CHECK(records[0].message == runtimeBuilt);
    CHECK(records[0].file == "synthetic.cpp");
    CHECK(records[0].line == 4242);

    // The documented re-check, exercised at the strictest floor: a direct caller is filtered
    // exactly as a macro call would be, so Off silences even Critical end-to-end.
    engine::setLogLevel(engine::LogLevel::Off);
    engine::detail::logWrite(engine::LogLevel::Critical, location, runtimeBuilt);
    CHECK(records.size() == 1);
}

// The compile-time gate and the runtime default are driven by the same NDEBUG condition and
// must not drift apart. static_assert, not CHECK: a wrong default must fail the BUILD. The
// "lazily reinitialises" case compares logLevel() against DEFAULT_LOG_LEVEL — the same constant
// on both sides — so it cannot catch a wrong value; these can.
TEST_CASE("log: DEFAULT_LOG_LEVEL and AERO_LOG_ACTIVE_LEVEL follow the NDEBUG split") {
#if defined(NDEBUG)
    static_assert(engine::DEFAULT_LOG_LEVEL == engine::LogLevel::Info, "NDEBUG builds default to Info");
    static_assert(AERO_LOG_ACTIVE_LEVEL == AERO_LOG_LEVEL_INFO, "NDEBUG builds gate at Info");
#else
    static_assert(engine::DEFAULT_LOG_LEVEL == engine::LogLevel::Trace, "debug builds default to Trace");
    static_assert(AERO_LOG_ACTIVE_LEVEL == AERO_LOG_LEVEL_TRACE, "debug builds gate at Trace");
#endif

    // logEnabled() is a `>=` against the floor, so both ends of the enum are boundaries: Trace
    // admits everything, Off admits nothing. The existing level case covers Warn and Off only.
    const LogFixture fixture;
    engine::setLogLevel(engine::LogLevel::Trace);
    CHECK(engine::logEnabled(engine::LogLevel::Trace));
    CHECK(engine::logEnabled(engine::LogLevel::Critical));
    engine::setLogLevel(engine::LogLevel::Off);
    CHECK_FALSE(engine::logEnabled(engine::LogLevel::Trace));
}

// shutdownLogging()'s contract is "flushes, clears the callback, releases the logger, resets
// the level". Callback-clearing was only implicitly covered: a stale callback holds a dangling
// capture, so ASan would fault in a LATER case — a confusing failure far from the cause.
TEST_CASE("log: shutdownLogging clears the callback") {
    const LogFixture fixture;
    std::atomic<int> count{0};
    engine::setLogCallback([&count](const engine::LogRecord&) { count.fetch_add(1); });
    AERO_LOG_INFO("delivered");
    REQUIRE(count.load() == 1);

    engine::shutdownLogging();
    // Re-init only to keep the console quiet (the lazy reinit would restore console=true); it
    // must not resurrect the cleared callback.
    engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false});
    AERO_LOG_INFO("must not reach the cleared callback");

    CHECK(count.load() == 1);
}

// initLogging()'s default argument is what a subsystem calls when it wants the engine defaults.
// Every other case passes an explicit config, so this is the only cover for the default-argument
// path, for LogConfig's own defaults, and for D12's "last call wins".
TEST_CASE("log: initLogging() applies LogConfig's defaults") {
    const LogFixture fixture;
    static_assert(engine::LogConfig{}.level == engine::DEFAULT_LOG_LEVEL, "LogConfig defaults to the engine level");
    static_assert(engine::LogConfig{}.console, "LogConfig defaults to a console sink");

    // Move the level off the default first, so the assertion below cannot pass trivially.
    engine::setLogLevel(engine::LogLevel::Off);
    REQUIRE(engine::logLevel() == engine::LogLevel::Off);

    engine::initLogging();  // default argument; re-init is allowed and the last call wins

    CHECK(engine::logLevel() == engine::DEFAULT_LOG_LEVEL);
}
