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
