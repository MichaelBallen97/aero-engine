// Aero Engine -- the console panel's log model (task 2.2.5). ZERO ImGui: this TU includes no ImGui
// header and makes no ImGui call (INV-2, grep-asserted in §V6). NO RECURSION anywhere (INV-8).
// NOTHING HERE LOGS (INV-5): push() runs inside engine::setLogCallback's own invocation, so a record
// raised here would re-enter it (log.hpp:87-90).
#include <aero/editor/console_model.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// D4: which LogSink the process-wide callback currently points at. setLogCallback has no
// compare-and-swap (log.cpp:109-115), so this is how a destructing scope knows whether the
// installation it is about to clear is still ITS OWN. Constant-initialised; NEVER dereferenced, only
// compared -- the pointer may already be dangling when a stale scope tests it, and comparing a
// dangling pointer value is fine. Named activeSink, not g_activeSink: .clang-tidy has no
// GlobalVariableCase override (the log.cpp:76-78 precedent).
std::atomic<LogSink*> activeSink{nullptr};

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char on every target, which is UB and trips bugprone-signed-char-misuse, --warnings-as-errors in
// CI (F34; it already cost task 2.2.2 a fix). Copied TU-locally from project_files.cpp's foldAscii
// rather than shared -- keeping it local costs 3 lines and avoids a new header (the 2.2.4/F27 rule).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// __FILE__ is backslash-separated on MSVC, so BOTH separators count. Also TU-local by the same rule.
constexpr bool isPathSeparator(char c) noexcept { return c == '/' || c == '\\'; }

void appendPadded(std::string& out, std::uint64_t value, std::size_t width) {
    const std::string text = std::to_string(value);
    for (std::size_t i = text.size(); i < width; ++i) {
        out.push_back('0');
    }
    out += text;
}

// THE R14 RESOLUTION (D2/INV-9), factored out so LogSinkScope's constructor and its move-assignment
// operator share the ONE lambda that captures a LogSink by value rather than duplicating it. Capturing
// the PARAMETER `sink` (a reference) BY VALUE makes the lambda's closure hold its own independent
// shared_ptr COPY, and logWrite holds that std::function alive across the whole invocation
// (log.cpp:123-133 copies it under the mutex, :153-156 invokes it outside), so the captured LogSink
// stays alive for the whole callback no matter when another thread detaches. NEVER capture `this` or
// `sink.get()`: that is precisely the pattern R14 documents as an ASan-proven use-after-free. This is
// what tier-0 case 21 asserts and sabotage S1 breaks -- it must never be "simplified". Unconditional,
// exactly like setLogCallback itself (F5/D4) -- the caller decides whether that is wanted; this
// function only ever installs.
void installSink(const std::shared_ptr<LogSink>& sink) {
    activeSink.store(sink.get(), std::memory_order_relaxed);
    setLogCallback([sink](const LogRecord& record) { sink->push(record.level, record.message, record.location); });
}

}  // namespace

bool operator==(const LogFilter& a, const LogFilter& b) noexcept {
    return a.minLevel == b.minLevel && a.text == b.text;
}

const char* logLevelLabel(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Critical:
            return "CRITICAL";
        case LogLevel::Off:
            return "OFF";
    }
    return "INFO";  // unreachable; keeps GCC's control-reaches-end quiet (the log.cpp:30 shape)
}

std::string formatElapsed(std::uint64_t millis) {
    std::string out;
    out.reserve(13);
    appendPadded(out, millis / 3600000ULL, 2);  // hours; WIDENS past 100 h rather than wrapping
    out.push_back(':');
    appendPadded(out, (millis / 60000ULL) % 60ULL, 2);
    out.push_back(':');
    appendPadded(out, (millis / 1000ULL) % 60ULL, 2);
    out.push_back('.');
    appendPadded(out, millis % 1000ULL, 3);
    return out;
}

std::string sanitizeLogMessage(std::string_view message) {
    std::size_t n = std::min(message.size(), MAX_LOG_MESSAGE_BYTES);
    const bool cut = n < message.size();
    if (cut) {
        // Back off at most 3 continuation bytes so a multi-byte sequence straddling the cap is
        // dropped WHOLE. Capped at 3 so malformed input terminates at the raw cap instead of walking
        // backwards through the whole message.
        for (int attempts = 0; n > 0 && attempts < 3; ++attempts) {
            if ((static_cast<unsigned char>(message[n]) & 0xC0U) != 0x80U) {
                break;  // the byte AT n starts a sequence -- cutting here is already clean
            }
            --n;
        }
    }
    std::string out;
    out.reserve(n + (cut ? TRUNCATION_MARKER.size() : 0U));
    for (std::size_t i = 0; i < n; ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        out.push_back((byte < 0x20U || byte == 0x7FU) ? ' ' : message[i]);  // D8; >= 0x80 untouched
    }
    if (cut) {
        out.append(TRUNCATION_MARKER);
    }
    return out;
}

std::string_view logSourceBasename(const char* file) noexcept {
    if (file == nullptr) {
        return {};  // log.cpp:144 proves a null `file` is a real, already-handled caller shape
    }
    const std::string_view path(file);
    for (std::size_t i = path.size(); i > 0; --i) {
        if (isPathSeparator(path[i - 1U])) {
            // The (pointer, length) constructor, NEVER substr(): substr() can throw
            // std::out_of_range (bugprone-exception-escape flags it inside a noexcept function even
            // though i <= path.size() always holds here), while this constructor cannot throw at all.
            return std::string_view(path.data() + i, path.size() - i);
        }
    }
    return path;
}

bool containsAsciiCaseInsensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t i = 0;
        while (i < needle.size() && foldAscii(static_cast<unsigned char>(haystack[start + i])) ==
                                        foldAscii(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

bool matchesFilter(const LogEntry& entry, const LogFilter& filter) noexcept {
    if (entry.level < filter.minLevel) {
        return false;
    }
    return containsAsciiCaseInsensitive(entry.message, filter.text);  // D12: MESSAGE only
}

std::string buildClipboardText(const LogHistory& history) {
    std::string out;
    for (std::size_t i = 0; i < history.visibleCount(); ++i) {
        const LogEntry& entry = history.visibleAt(i);
        out += formatElapsed(entry.elapsedMs);
        out += "  ";
        const std::string_view label(logLevelLabel(entry.level));
        out += label;
        out.append(LOG_LEVEL_LABEL_WIDTH - label.size(), ' ');  // every label is <= 8 bytes
        out += "  ";
        out += entry.message;
        if (!entry.sourceFile.empty()) {
            out += "  (";
            out += entry.sourceFile;
            out.push_back(':');
            out += std::to_string(entry.line);
            out.push_back(')');
        }
        out.push_back('\n');
    }
    return out;
}

// ---- LogSink ------------------------------------------------------------------------------------

LogSink::LogSink(std::size_t stagingCapacity) noexcept
    : cap(stagingCapacity), origin(std::chrono::steady_clock::now()) {}

void LogSink::push(LogLevel level, std::string_view message, const LogLocation& location) {
    // Built entirely OUTSIDE the lock: the string copies, the sanitisation pass and the clock read
    // are the only costly parts, and a logging thread must never hold this mutex across them.
    LogEntry entry;
    entry.level = level;
    entry.elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count());
    entry.message = sanitizeLogMessage(message);                       // D8 -- copies; F2 demands it
    entry.sourceFile = std::string(logSourceBasename(location.file));  // D6 -- copies before the view dies
    entry.line = location.line;

    const std::lock_guard<std::mutex> lock(mutex);
    if (staged.size() >= cap) {
        ++dropped;  // refuse the NEWEST: the start of a burst is where the cause usually is (D11).
        return;     // NEVER log from here -- INV-5 / log.hpp:87-90's recursion warning.
    }
    staged.push_back(std::move(entry));
}

std::uint64_t LogSink::take(std::vector<LogEntry>& out) {
    assert(out.empty() &&
           "LogSink::take(): `out` must be empty -- the swap would hand the sink a "
           "non-empty staging buffer");
    const std::lock_guard<std::mutex> lock(mutex);
    staged.swap(out);  // O(1): three pointer swaps, no allocation, no element touched
    const std::uint64_t lost = dropped;
    dropped = 0;
    return lost;
}

std::size_t LogSink::stagingCapacity() const noexcept { return cap; }

// ---- LogSinkScope -- the R14 resolution and the D4 token ----------------------------------------

LogSinkScope::LogSinkScope() : LogSinkScope(DEFAULT_LOG_STAGING_CAPACITY) {}

LogSinkScope::LogSinkScope(std::size_t stagingCapacity) : sinkPtr(std::make_shared<LogSink>(stagingCapacity)) {
    installSink(sinkPtr);  // see installSink's own comment for the R14 resolution (D2/INV-9)
}

LogSinkScope::~LogSinkScope() { detach(); }

LogSinkScope::LogSinkScope(LogSinkScope&& other) noexcept : sinkPtr(std::move(other.sinkPtr)) {
    // The token records a LogSink*, and the sink did not move -- so activeSink stays correct and the
    // moved-from scope's destructor becomes a no-op (its sinkPtr is null).
}

LogSinkScope& LogSinkScope::operator=(LogSinkScope&& other) noexcept {
    if (this != &other) {
        detach();  // our own installation goes first, or it would leak past the assignment
        sinkPtr = std::move(other.sinkPtr);
        if (sinkPtr) {
            // `other` may already have been displaced by a LATER scope's construction before this
            // assignment ran (F5/E16 -- setLogCallback has no compare-and-swap), so its routing cannot
            // simply be "inherited" by moving the pointer alone. Re-install for the sink we now own,
            // exactly as the constructor does, so the assignee becomes (or remains) the active
            // installation for whatever sink it ends up holding.
            installSink(sinkPtr);
        }
    }
    return *this;
}

const std::shared_ptr<LogSink>& LogSinkScope::sink() const noexcept { return sinkPtr; }

bool LogSinkScope::installed() const noexcept {
    return sinkPtr != nullptr && activeSink.load(std::memory_order_relaxed) == sinkPtr.get();
}

void LogSinkScope::detach() noexcept {
    if (!sinkPtr) {
        return;  // moved-from, or already detached -- idempotent by contract
    }
    LogSink* expected = sinkPtr.get();
    if (activeSink.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed)) {
        setLogCallback({});  // only OUR installation is cleared; a LATER scope's survives (D4/E16)
    }
    sinkPtr.reset();
}

// ---- LogHistory ---------------------------------------------------------------------------------

LogHistory::LogHistory(std::size_t capacity) : cap(std::max<std::size_t>(capacity, 1U)) {}

void LogHistory::append(LogEntry entry) {
    entry.sequence = nextSeq++;
    // PLAN C1 -- LOAD-BEARING. `counts` has LOG_LEVEL_RECORD_COUNT (6) slots, but LogLevel::Off is 6
    // and logEnabled(Off) is true against EVERY floor (log.hpp:83), so a record CAN arrive carrying
    // it via detail::logWrite. An unguarded ++counts[6] is an out-of-bounds write into a heap object.
    // Off deliberately gets no counter slot (levelCount(Off) == 0); the record itself is still kept
    // and still shown. Tier-0 case 25 is the ASan proof; sabotage S17 removes this guard.
    const auto index = static_cast<std::size_t>(entry.level);
    if (index < counts.size()) {
        ++counts[index];
    }
    const bool visible = matchesFilter(entry, activeFilter);
    const std::uint64_t seq = entry.sequence;
    entries.push_back(std::move(entry));
    if (visible) {
        visibleSeq.push_back(seq);  // ascending by construction
    }
    while (entries.size() > cap) {  // evict AFTER the push, so cap == 1 still works
        const auto frontIndex = static_cast<std::size_t>(entries.front().level);
        if (frontIndex < counts.size()) {
            --counts[frontIndex];  // C1's mirror -- must match the increment above exactly
        }
        entries.pop_front();
        ++evicted;
    }
    // Recomputed AFTER eviction -- D13. Pruning here is what keeps visibleAt()'s O(1) subtraction
    // in range; sabotage S9 removes this loop and case 11's precondition assert fires.
    const std::uint64_t oldest = oldestSequence();
    while (!visibleSeq.empty() && visibleSeq.front() < oldest) {
        visibleSeq.pop_front();
    }
}

void LogHistory::appendAll(std::vector<LogEntry>& batch) {
    for (LogEntry& entry : batch) {
        append(std::move(entry));
    }
    batch.clear();  // INV-10: take()'s precondition is that its `out` is empty
}

void LogHistory::noteDropped(std::uint64_t count) noexcept { dropped += count; }

void LogHistory::clear() noexcept {
    entries.clear();
    visibleSeq.clear();
    counts.fill(0U);
    evicted = 0;
    dropped = 0;
    // nextSeq is DELIBERATELY NOT reset (E23): a sequence number from before the clear must never
    // alias a record after it. activeFilter is kept so the view stays consistent with the widgets.
}

void LogHistory::setFilter(LogFilter next) {
    if (next == activeFilter) {
        return;  // the ONLY thing standing between a filter diff and a rebuild every frame
    }
    activeFilter = std::move(next);
    visibleSeq.clear();
    for (const LogEntry& entry : entries) {  // the ONE full rebuild in the whole design (D13)
        if (matchesFilter(entry, activeFilter)) {
            visibleSeq.push_back(entry.sequence);
        }
    }
}

const LogFilter& LogHistory::filter() const noexcept { return activeFilter; }
std::size_t LogHistory::capacity() const noexcept { return cap; }
std::size_t LogHistory::size() const noexcept { return entries.size(); }
std::uint64_t LogHistory::evictedCount() const noexcept { return evicted; }
std::uint64_t LogHistory::droppedCount() const noexcept { return dropped; }
std::size_t LogHistory::visibleCount() const noexcept { return visibleSeq.size(); }
std::uint64_t LogHistory::nextSequence() const noexcept { return nextSeq; }

std::uint32_t LogHistory::levelCount(LogLevel level) const noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < counts.size() ? counts[index] : 0U;  // Off -> 0, by contract (C1)
}

std::uint64_t LogHistory::oldestSequence() const noexcept { return nextSeq - entries.size(); }

const LogEntry& LogHistory::visibleAt(std::size_t index) const noexcept {
    assert(index < visibleSeq.size() && "LogHistory::visibleAt(): index out of range");
    // O(1): `entries` is contiguous in SEQUENCE space by construction, and visibleSeq was pruned to
    // the same window by append(). Sabotage S9 makes this assert fire.
    return entries[static_cast<std::size_t>(visibleSeq[index] - oldestSequence())];
}

}  // namespace engine::editor
