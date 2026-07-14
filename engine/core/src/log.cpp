// Aero Engine — logging implementation (task 0.2.4).
// The ONLY translation unit that knows spdlog exists. Nothing named here may appear in a
// public header (docs/04 boundary rule); the `lint` job's boundary step enforces it.
#include <aero/core/log.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace engine {
namespace {

spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
    // clang-format off
    switch (level) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warn:     return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off:      return spdlog::level::off;
    }
    // clang-format on
    return spdlog::level::info;  // unreachable; keeps GCC's control-reaches-end quiet
}

// Constructed directly rather than via spdlog::stdout_color_mt (spec D13): the global
// registry would collide on the duplicate name across initLogging() calls and would couple
// our lifetime to spdlog's registry statics.
std::shared_ptr<spdlog::logger> makeLogger(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        // Detects a non-tty (CI, redirected output) and emits no colour escapes there.
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    auto logger = std::make_shared<spdlog::logger>("aero", sinks.begin(), sinks.end());
    // engine::detail::activeLogLevel is the SINGLE filter (spec D10) — spdlog passes all.
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::err);
    logger->set_pattern("[%T.%e] [%^%l%$] [%t] %v (%s:%#)");
    return logger;
}

struct LogState {
    std::mutex mutex;
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<const LogCallback> callback;
};

// Immortal by design (spec D12): never destroyed, so late logging cannot touch a destroyed
// object. It stays reachable from this function-local static pointer for the whole process,
// so LeakSanitizer (Linux Debug lane) does not report it — LSan flags only unreachable
// blocks. The magic static makes construction thread-safe.
LogState& state() {
    static auto* instance = new LogState();
    return *instance;
}

}  // namespace

namespace detail {
// Constant-initialised: no static-init-order hazard, so logEnabled() is safe during static
// initialisation. Named activeLogLevel, NOT g_logLevel — .clang-tidy's VariableCase is
// camelBack with no GlobalVariableCase override (task 0.1.6 D8), so g_logLevel fails CI.
std::atomic<LogLevel> activeLogLevel{DEFAULT_LOG_LEVEL};
}  // namespace detail

void initLogging(const LogConfig& config) {
    LogState& logState = state();
    {
        const std::lock_guard<std::mutex> lock(logState.mutex);
        logState.logger = makeLogger(config);
    }
    setLogLevel(config.level);
}

void shutdownLogging() {
    LogState& logState = state();
    std::shared_ptr<spdlog::logger> logger;
    {
        const std::lock_guard<std::mutex> lock(logState.mutex);
        logger = std::move(logState.logger);
        logState.logger.reset();
        logState.callback.reset();
    }
    if (logger) {
        logger->flush();
    }
    setLogLevel(DEFAULT_LOG_LEVEL);
}

void setLogLevel(LogLevel level) { detail::activeLogLevel.store(level, std::memory_order_relaxed); }

LogLevel logLevel() { return detail::activeLogLevel.load(std::memory_order_relaxed); }

void setLogCallback(LogCallback callback) {
    // shared_ptr<const> so logWrite copies a refcount, not a std::function (spec D9).
    auto stored = callback ? std::make_shared<const LogCallback>(std::move(callback)) : nullptr;
    LogState& logState = state();
    const std::lock_guard<std::mutex> lock(logState.mutex);
    logState.callback = std::move(stored);
}

namespace detail {
void logWrite(LogLevel level, const LogLocation& location, std::string_view message) {
    if (!logEnabled(level)) {
        return;
    }

    LogState& logState = state();
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<const LogCallback> callback;
    {
        const std::lock_guard<std::mutex> lock(logState.mutex);
        if (!logState.logger) {
            logState.logger = makeLogger(LogConfig{});  // lazy default init (spec D12)
        }
        logger = logState.logger;
        callback = logState.callback;
    }

    // The non-formatting overload (logger.h:115): `message` is written verbatim, so braces
    // inside it are never reinterpreted as a format string and cannot throw.
    logger->log(spdlog::source_loc{location.file, static_cast<int>(location.line), location.function},
                toSpdlogLevel(level), spdlog::string_view_t{message.data(), message.size()});

    // Invoked OUTSIDE the lock (spec D9): a callback that logs cannot deadlock.
    if (callback && *callback) {
        (*callback)(LogRecord{level, message, location});
    }
}
}  // namespace detail
}  // namespace engine
