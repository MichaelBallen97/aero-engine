#pragma once
// Aero Engine -- the console panel's log model (task 2.2.5). PUBLIC and ImGui-FREE by rule: it speaks
// engine + std types only, so the tier-0 aero_editor_shell_test can exercise the sink, the ring, the
// filter and every formatter with no ImGui context, no window and no GPU. All ImGui lives in
// console_panel.cpp. The editor's "public headers stay ImGui-free" rule is held by FILE PLACEMENT,
// not by a guard (R12) -- do not claim enforcement that does not exist.
//
// R14 (docs/08-risks.md), RESOLVED HERE, AT THE CALL SITE: LogSinkScope installs a callback that
// captures a shared_ptr<LogSink> BY VALUE. engine/core/src/log.cpp:123-133 copies the stored callback
// under its mutex and :153-156 invokes it OUTSIDE that mutex, so the std::function -- and therefore
// every object it captures -- is alive for the whole invocation no matter when another thread
// detaches. Detach-then-destroy, the use-after-free R14 names, is structurally impossible. core needs
// no drain, and engine/ is byte-identical after this task. NEVER capture `this` or a raw LogSink*.
//
// NOTHING REACHABLE FROM LogSink::push MAY LOG. push() runs INSIDE the engine's own callback, so a
// log record raised from it re-enters the callback that called it (log.hpp:87-90's "mind unbounded
// recursion"). Overflow is a COUNTER, never a warning.

#include <aero/core/log.hpp>

// Include what you use: libstdc++ does NOT supply transitively what libc++ does, and the Linux lane
// is where that shows up. <array> is for `counts`, <chrono> for the elapsed-time origin.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// How many records the panel keeps. Oldest are EVICTED past this, and the count is surfaced (D11).
inline constexpr std::size_t DEFAULT_LOG_HISTORY_CAPACITY = 10000;
// How many records may sit in the sink between two pumps. Beyond this they are DROPPED and counted
// SEPARATELY -- eviction is a long session, dropping is something spewing (D11).
inline constexpr std::size_t DEFAULT_LOG_STAGING_CAPACITY = 4096;
// A single record's stored message is capped here. Truncation happens on a UTF-8 boundary and is
// marked with TRUNCATION_MARKER, which is appended AFTER the cap (the cap bounds the payload).
inline constexpr std::size_t MAX_LOG_MESSAGE_BYTES = 4096;
inline constexpr std::string_view TRUNCATION_MARKER = " ...[truncated]";

// One captured record. OWNS its strings: LogRecord::message dies with the callback (log.hpp:43-46),
// and LogLocation::file's static-storage guarantee is the MACROS' contract, not logWrite's -- which
// is reachable with a hand-built location (tests/log_test.cpp:284) and already had to be hardened
// against a null one (log.cpp:144). F3/D6.
struct LogEntry {
    std::uint64_t sequence = 0;   // assigned by LogHistory::append; unique and strictly increasing
    std::uint64_t elapsedMs = 0;  // since the owning LogSink was constructed (steady_clock, D7)
    std::string message;          // sanitised to ONE line and length-capped at capture time (D8)
    std::string sourceFile;       // BASENAME only ("editor_app.cpp"); empty when unknown
    std::uint32_t line = 0;       // 0 when unknown
    LogLevel level = LogLevel::Info;
};

struct LogFilter {
    LogLevel minLevel = LogLevel::Trace;  // >= this passes (D10); LogLevel is ordered, log.hpp:25-33
    std::string text;                     // ASCII-case-insensitive substring of MESSAGE only; "" passes all
};
[[nodiscard]] bool operator==(const LogFilter& a, const LogFilter& b) noexcept;

// ---- pure helpers: no I/O, no ImGui, no clock; every one is a tier-0 test case ----

// "TRACE" / "DEBUG" / "INFO" / "WARN" / "ERROR" / "CRITICAL" / "OFF". Never null.
[[nodiscard]] const char* logLevelLabel(LogLevel level) noexcept;

// The width the level column must reserve: strlen("CRITICAL").
inline constexpr std::size_t LOG_LEVEL_LABEL_WIDTH = 8;
// How many LogLevel values can actually BE a record's level: Trace..Critical. `Off` is a FLOOR value
// (log.hpp:22-24), so it gets no counter slot and is not selectable in the filter -- but a record
// CAN still carry it (logEnabled(Off) is true against every floor), which is exactly why
// LogHistory::append range-checks the index instead of trusting it (plan C1).
inline constexpr std::size_t LOG_LEVEL_RECORD_COUNT = 6;

// "00:00:00.000" .. "99:59:59.999", widening past 100 h. Integer-only, locale-free, no <format>
// chrono and no timezone (D7) -- which is what makes it assert identically on three OSes.
[[nodiscard]] std::string formatElapsed(std::uint64_t millis);

// One line, always: every byte < 0x20 or == 0x7F becomes ' ' (newline, CR, tab and NUL included --
// ImGuiListClipper needs uniform row height, D8). Bytes >= 0x80 pass through, so UTF-8 survives.
// Longer than MAX_LOG_MESSAGE_BYTES -> cut at a UTF-8 boundary (back off at most 3 continuation
// bytes; malformed input cuts at the raw cap) + TRUNCATION_MARKER.
[[nodiscard]] std::string sanitizeLogMessage(std::string_view message);

// The text after the last '/' or '\\'. Handles BOTH separators because __FILE__ is backslashed on
// MSVC. nullptr / "" / a trailing separator all yield "". POINTS INTO `file` -- copy before it dies.
[[nodiscard]] std::string_view logSourceBasename(const char* file) noexcept;

// ASCII-case-insensitive substring test. Empty needle -> true. NEVER std::tolower(char): UTF-8
// continuation bytes are negative as char on every target, which is UB and trips
// bugprone-signed-char-misuse (F34). Bytes >= 0x80 compare raw, so non-ASCII matches byte-exactly.
[[nodiscard]] bool containsAsciiCaseInsensitive(std::string_view haystack, std::string_view needle) noexcept;

// entry.level >= filter.minLevel AND (filter.text empty OR it occurs in entry.message). The source
// file and the level label are deliberately NOT searched (D12).
[[nodiscard]] bool matchesFilter(const LogEntry& entry, const LogFilter& filter) noexcept;

class LogHistory;
// "<elapsed>  <LEVEL   >  <message>  (<file>:<line>)\n" per VISIBLE row, in order; the source suffix
// is omitted when sourceFile is empty. Empty view -> "". Bounded by the history's capacity (D20).
[[nodiscard]] std::string buildClipboardText(const LogHistory& history);

// ---- the thread-safe staging sink ----
//
// Holds only records NOT YET CONSUMED. push() runs on ARBITRARY threads; take() runs on the owner
// thread and swaps the staged vector out in O(1), so the mutex never covers a container walk, a
// filter pass or a draw (D3). NOTHING reachable from push() may log -- INV-5.
class LogSink {
public:
    explicit LogSink(std::size_t stagingCapacity = DEFAULT_LOG_STAGING_CAPACITY) noexcept;
    ~LogSink() = default;  // MAY RUN ON A LOGGING THREAD (E14): touches only its own members. It must
                           // never grow a dependency on the frame thread, ImGui, or the panel.
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    // ANY THREAD. Timestamps, sanitises and copies OUTSIDE the lock, then enqueues under it.
    // At capacity the record is dropped (the NEWEST is refused, so the START of a burst -- where the
    // cause usually is -- survives) and counted.
    void push(LogLevel level, std::string_view message, const LogLocation& location);

    // OWNER THREAD. PRECONDITION: `out` is empty (asserted in debug). Swaps in O(1) and returns how
    // many records were dropped since the previous take(), resetting that counter.
    std::uint64_t take(std::vector<LogEntry>& out);

    [[nodiscard]] std::size_t stagingCapacity() const noexcept;

private:
    std::mutex mutex;              // NOT `mutable`: no const member function locks it (plan C9)
    std::vector<LogEntry> staged;  // NOT pre-reserved: both buffers reach steady capacity by swapping
    std::uint64_t dropped = 0;
    std::size_t cap;
    std::chrono::steady_clock::time_point origin;  // D7's elapsed-time zero
};

// ---- the RAII installer over engine::setLogCallback ----
//
// Move-only. Construction installs; destruction detaches -- but ONLY if this scope is still the
// active installation (D4): setLogCallback has no compare-and-swap (F5), so an unconditional detach
// would clear a LATER installer's callback.
class LogSinkScope {
public:
    LogSinkScope();
    explicit LogSinkScope(std::size_t stagingCapacity);
    ~LogSinkScope();
    // Moving TRANSFERS the sink but never seizes the routing slot from a third scope. The token records
    // a LogSink* and the sink itself does not move, so a moved-to scope inherits `other`'s active status
    // exactly: active if `other` was, displaced if `other` was displaced.
    LogSinkScope(LogSinkScope&& other) noexcept;
    // Assignment additionally detaches this scope's own prior installation first, and re-installs for
    // the incoming sink ONLY when the routing slot was ours or was free -- assigning never kills an
    // unrelated scope's console. The re-install allocates, so an out-of-memory failure THERE terminates
    // (noexcept is mandatory on a move-assignment here); see the .cpp for why each alternative is
    // worse. Nothing under editor/ move-assigns a scope, so that path is test-only in practice.
    LogSinkScope& operator=(LogSinkScope&& other) noexcept;
    LogSinkScope(const LogSinkScope&) = delete;
    LogSinkScope& operator=(const LogSinkScope&) = delete;

    // The sink, shared with the installed callback. Null only after a move-from. Exposed as the
    // shared_ptr (not a reference) so a test can assert use_count() >= 2 while installed -- the
    // mechanical discriminator for D2's shared-ownership rule (tier-0 case 21 / sabotage S1).
    [[nodiscard]] const std::shared_ptr<LogSink>& sink() const noexcept;
    [[nodiscard]] bool installed() const noexcept;  // true iff THIS scope is the active installation
    void detach() noexcept;                         // idempotent; also called by the destructor

private:
    std::shared_ptr<LogSink> sinkPtr;
};

// ---- the panel-owned, single-threaded history ----
//
// The bounded ring, the per-level counts, and the filtered view. NOT thread-safe and deliberately so:
// everything here runs on the frame thread, which is what makes every rule below a tier-0 test with
// no threads at all (D3).
class LogHistory {
public:
    explicit LogHistory(std::size_t capacity = DEFAULT_LOG_HISTORY_CAPACITY);

    void append(LogEntry entry);                   // assigns entry.sequence; evicts past capacity
    void appendAll(std::vector<LogEntry>& batch);  // moves each element, then batch.clear()
    void noteDropped(std::uint64_t count) noexcept;
    void clear() noexcept;           // records, visible view, counters. KEEPS capacity, filter, and the
                                     // sequence counter -- so a stale sequence can never alias a new record
    void setFilter(LogFilter next);  // no-op when equal; otherwise ONE full rebuild (D13)

    [[nodiscard]] const LogFilter& filter() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;                        // records currently HELD
    [[nodiscard]] std::uint64_t evictedCount() const noexcept;              // aged out since the last clear()
    [[nodiscard]] std::uint64_t droppedCount() const noexcept;              // lost at the sink since the last clear()
    [[nodiscard]] std::uint32_t levelCount(LogLevel level) const noexcept;  // held records; Off -> 0
    [[nodiscard]] std::size_t visibleCount() const noexcept;
    // PRECONDITION index < visibleCount(), asserted in debug (docs/04).
    [[nodiscard]] const LogEntry& visibleAt(std::size_t index) const noexcept;
    [[nodiscard]] std::uint64_t nextSequence() const noexcept;

private:
    [[nodiscard]] std::uint64_t oldestSequence() const noexcept;  // nextSeq - entries.size()

    std::deque<LogEntry> entries;                                // oldest at the front; contiguous in SEQUENCE space
    std::deque<std::uint64_t> visibleSeq;                        // ascending; D13's incremental filtered view
    std::array<std::uint32_t, LOG_LEVEL_RECORD_COUNT> counts{};  // indexed by LogLevel 0..5
    LogFilter activeFilter;
    std::uint64_t nextSeq = 0;
    std::uint64_t evicted = 0;
    std::uint64_t dropped = 0;
    std::size_t cap;  // clamped to >= 1 by the constructor
};

}  // namespace engine::editor
