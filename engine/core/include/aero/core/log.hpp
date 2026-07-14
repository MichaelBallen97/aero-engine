#pragma once
// Aero Engine — logging API (task 0.2.4).
// The engine's own logging surface. The spdlog backend is an implementation detail of
// src/log.cpp and never crosses this boundary (project boundary rule, docs/04); the `lint`
// job's boundary step enforces that mechanically. Call through the AERO_LOG_* macros: they
// capture the call site and compile out below AERO_LOG_ACTIVE_LEVEL.
//
// Thread safety: every function here is safe to call from any thread.
// Lifetime: logging works without any init call (the first record initialises defaults).
// Logging from a static destructor AFTER main() returns is NOT supported — spdlog owns
// process-lifetime statics of its own.

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace engine {

// Ordered by increasing severity. `Off` is a FLOOR value (setLogLevel(Off) silences
// everything) — it is never the level of a record. Values are contiguous from 0 and MUST
// stay in sync with the AERO_LOG_LEVEL_* macros below, which drive the compile-time gate.
enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
};

// A record's call site. The pointers are the compiler's own literals (__FILE__/__func__),
// so they have static storage duration and always outlive the record.
struct LogLocation {
    const char* file = nullptr;
    const char* function = nullptr;
    std::uint32_t line = 0;
};

// One record, as handed to a LogCallback. `message` is the formatted text WITHOUT any
// pattern decoration (no timestamp or level prefix) so a consumer can present it its own
// way. It is a view onto a buffer owned by the caller and is INVALID once the callback
// returns — copy it if you need to keep it.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view message;
    LogLocation location;
};

// Verbose by default in debug builds, quieter once NDEBUG is defined.
#if defined(NDEBUG)
inline constexpr LogLevel DEFAULT_LOG_LEVEL = LogLevel::Info;
#else
inline constexpr LogLevel DEFAULT_LOG_LEVEL = LogLevel::Trace;
#endif

struct LogConfig {
    LogLevel level = DEFAULT_LOG_LEVEL;
    bool console = true;
};

// Optional: the first log record initialises with defaults if this is never called.
// May be called more than once — the last call wins (tests use it to silence the console).
void initLogging(const LogConfig& config = {});

// Flushes sinks, clears the callback, releases the logger, and resets the level to
// DEFAULT_LOG_LEVEL. Idempotent; a later log record lazily reinitialises with defaults.
void shutdownLogging();

void setLogLevel(LogLevel level);
LogLevel logLevel();

namespace detail {
// NOT API. Exposed only so logEnabled() inlines at the call site. Constant-initialised, so
// it is immune to static-initialisation order — logEnabled() is safe during static init.
extern std::atomic<LogLevel> activeLogLevel;
}  // namespace detail

// True when `level` passes the runtime floor. Cheap: one relaxed load, no lock.
inline bool logEnabled(LogLevel level) { return level >= detail::activeLogLevel.load(std::memory_order_relaxed); }

using LogCallback = std::function<void(const LogRecord&)>;

// Installs a callback receiving every record that passes the runtime floor, in addition to
// the console sink. Pass {} to clear. The callback MAY BE INVOKED FROM ANY THREAD and must
// be thread-safe. It is invoked outside the internal lock, so it may log without
// deadlocking (mind unbounded recursion).
void setLogCallback(LogCallback callback);

namespace detail {
// The single non-template sink entry: `message` is ALREADY formatted. Re-checks the level so
// direct callers behave exactly like the macros. spdlog lives entirely behind this
// declaration. Use this directly for a runtime-built string (std::format_string requires a
// compile-time-constant format).
void logWrite(LogLevel level, const LogLocation& location, std::string_view message);

// Formats at the call site with std::format — a STANDARD type, so the public header stays
// free of third-party types while keeping compile-time format checking. The level check
// happens BEFORE formatting: a disabled record costs one relaxed atomic load.
template <typename... Args>
void logFormat(LogLevel level, const LogLocation& location, std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(level)) {
        return;
    }
    logWrite(level, location, std::format(fmt, std::forward<Args>(args)...));
}
}  // namespace detail
}  // namespace engine

// ---- Compile-time gate -------------------------------------------------------------------
// Records below AERO_LOG_ACTIVE_LEVEL generate NO code (the `if constexpr` branch is
// discarded, so nothing is emitted even at -O0) yet are still fully type-checked — a
// disabled call site cannot bit-rot. Override per target with
// target_compile_definitions(<tgt> PRIVATE AERO_LOG_ACTIVE_LEVEL=<n>).
#define AERO_LOG_LEVEL_TRACE 0
#define AERO_LOG_LEVEL_DEBUG 1
#define AERO_LOG_LEVEL_INFO 2
#define AERO_LOG_LEVEL_WARN 3
#define AERO_LOG_LEVEL_ERROR 4
#define AERO_LOG_LEVEL_CRITICAL 5
#define AERO_LOG_LEVEL_OFF 6

#if !defined(AERO_LOG_ACTIVE_LEVEL)
    #if defined(NDEBUG)
        #define AERO_LOG_ACTIVE_LEVEL AERO_LOG_LEVEL_INFO
    #else
        #define AERO_LOG_ACTIVE_LEVEL AERO_LOG_LEVEL_TRACE
    #endif
#endif

#define AERO_LOG_LOCATION() \
    ::engine::LogLocation { __FILE__, __func__, static_cast<std::uint32_t>(__LINE__) }

// do/while(false): keeps each macro a single statement, so it is valid after a braceless
// if/else (the statement-safety lesson profiler.hpp documents).
#define AERO_LOG_IMPL(levelEnum, levelNumber, ...)                                      \
    do {                                                                                \
        if constexpr ((levelNumber) >= AERO_LOG_ACTIVE_LEVEL) {                         \
            ::engine::detail::logFormat((levelEnum), AERO_LOG_LOCATION(), __VA_ARGS__); \
        }                                                                               \
    } while (false)

#define AERO_LOG_TRACE(...) AERO_LOG_IMPL(::engine::LogLevel::Trace, AERO_LOG_LEVEL_TRACE, __VA_ARGS__)
#define AERO_LOG_DEBUG(...) AERO_LOG_IMPL(::engine::LogLevel::Debug, AERO_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define AERO_LOG_INFO(...) AERO_LOG_IMPL(::engine::LogLevel::Info, AERO_LOG_LEVEL_INFO, __VA_ARGS__)
#define AERO_LOG_WARN(...) AERO_LOG_IMPL(::engine::LogLevel::Warn, AERO_LOG_LEVEL_WARN, __VA_ARGS__)
#define AERO_LOG_ERROR(...) AERO_LOG_IMPL(::engine::LogLevel::Error, AERO_LOG_LEVEL_ERROR, __VA_ARGS__)
#define AERO_LOG_CRITICAL(...) AERO_LOG_IMPL(::engine::LogLevel::Critical, AERO_LOG_LEVEL_CRITICAL, __VA_ARGS__)
