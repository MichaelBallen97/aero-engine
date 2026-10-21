#pragma once
// Aero Engine -- the JavaScript VM (task 4.1.1; ADR-007). One Vm = one quickjs-ng runtime + one context,
// created together, used on ONE thread, torn down together. Nothing is script-visible: a VM offers the
// ECMAScript language and the backend's core globals only -- no engine global, no binding, no host
// function (those arrive with 4.2.1 and 4.4.x).
//
// The backend is an implementation detail of engine/script/src/ and never crosses this header (project
// rule #3). .github/scripts/check-script-boundary.sh and tests/script_boundary_probe.cpp enforce that.
//
// Errors never cross this API as exceptions: every outcome is a status, and every failure is also logged
// ONCE, at ERROR, in the fixed shape listed above evaluateScript below.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace engine::script {

// ---- limits --------------------------------------------------------------------------------------------
// The JavaScript heap: every byte the backend allocates, header included, is charged against it. There
// is deliberately no "unlimited": 0 is refused rather than read as the backend's own "no limit".
inline constexpr std::size_t DEFAULT_HEAP_LIMIT_BYTES = std::size_t{64} * 1024U * 1024U;  // 64 MiB
// The smallest heap a VM may be given. An empty VM needs about 110 KB (100 828 B Release, 110 188 B Debug), so no
// create runs out of heap at or above this; below it the backend's context-creation failure path is not safe to
// take, so such a limit is refused up front instead of being tried.
inline constexpr std::size_t MIN_HEAP_LIMIT_BYTES = std::size_t{1024} * 1024U;  // 1 MiB

// The C stack a call may use below its entry point. 512 KiB fits inside the SMALLEST main-thread stack of
// the five platforms (1 MiB on Windows and iOS) with room left for the frames above the entry and for the
// one interpreter frame that may overshoot between two checks. The calling thread must have at least
// stackLimitBytes + 256 KiB of free stack -- a default 512 KiB macOS/iOS secondary thread does NOT.
inline constexpr std::size_t DEFAULT_STACK_LIMIT_BYTES = std::size_t{512} * 1024U;
inline constexpr std::size_t MIN_STACK_LIMIT_BYTES = std::size_t{64} * 1024U;
inline constexpr std::size_t MAX_STACK_LIMIT_BYTES = std::size_t{8} * 1024U * 1024U;

// The wall-clock budget of ONE call: compile + run + the job drain + error reporting. A call that is still
// executing JavaScript when it runs out is interrupted with an error the script cannot catch, and ends
// Interrupted even when the backend converts that error (a Promise executor, Promise.try and a thenable job all
// do); no new JavaScript call starts in it after that. There is no "unlimited": the engine must never hang on
// a script.
inline constexpr std::chrono::milliseconds DEFAULT_CALL_TIME_BUDGET{1000};
inline constexpr std::chrono::milliseconds MIN_CALL_TIME_BUDGET{1};
inline constexpr std::chrono::milliseconds MAX_CALL_TIME_BUDGET{60'000};

struct VmConfig {
    std::size_t heapLimitBytes = DEFAULT_HEAP_LIMIT_BYTES;    // >= MIN_HEAP_LIMIT_BYTES
    std::size_t stackLimitBytes = DEFAULT_STACK_LIMIT_BYTES;  // [MIN_STACK_LIMIT_BYTES, MAX_STACK_LIMIT_BYTES]
    std::chrono::milliseconds callTimeBudget = DEFAULT_CALL_TIME_BUDGET;  // [MIN_, MAX_CALL_TIME_BUDGET]
};

// ---- statuses ------------------------------------------------------------------------------------------
enum class VmCreateStatus : std::uint8_t {
    Ok = 0,
    InvalidHeapLimit,   // heapLimitBytes < MIN_HEAP_LIMIT_BYTES
    InvalidStackLimit,  // outside [MIN_STACK_LIMIT_BYTES, MAX_STACK_LIMIT_BYTES]
    InvalidTimeBudget,  // outside [MIN_CALL_TIME_BUDGET, MAX_CALL_TIME_BUDGET]
    OutOfMemory,        // the system allocator failed (the heap limit alone can no longer starve an empty VM)
};
// Never named toString: doctest's DOCTEST_STRINGIFY finds an unqualified toString by ADL (CLAUDE.md).
[[nodiscard]] std::string_view vmCreateStatusLabel(VmCreateStatus status) noexcept;

enum class EvalStatus : std::uint8_t {
    Ok = 0,
    CompileError,  // the source did not compile; nothing ran
    Exception,     // a value was thrown and not caught -- an Error, any other value, or a stack overflow
    OutOfMemory,   // the heap limit refused an allocation and the failure was not caught
    Interrupted,   // the call's time budget ran out; the script could not catch it, nor start new calls
    InvalidVm,     // a failed create's Vm, or a moved-from one
    WrongThread,   // called from a thread that is not the VM's; nothing was touched
};
[[nodiscard]] std::string_view evalStatusLabel(EvalStatus status) noexcept;

// What was thrown. For an Error object: its name, message and stack text (the backend's frames, one per
// line, no trailing newline; may be empty). For any other value: name and stack are empty and message
// is String(value), or "<unprintable value>" when that conversion itself throws.
struct ScriptError {
    std::string name;
    std::string message;
    std::string stack;
};

struct EvalResult {
    EvalStatus status = EvalStatus::InvalidVm;  // the FIRST failure of the call (the script before its jobs)
    ScriptError error;       // filled iff status is CompileError, Exception, OutOfMemory or Interrupted
    std::string completion;  // String(completion value) iff the script ran to completion -- DIAGNOSTIC text,
                             // not a value bridge; "<unprintable value>" when the conversion throws
};

struct VmStats {
    std::size_t heapLimitBytes = 0;
    std::size_t heapBytesInUse = 0;  // live blocks, each with its 16-byte header
    std::size_t heapBytesPeak = 0;
    std::size_t liveAllocations = 0;
    std::uint64_t refusedAllocations = 0;     // requests the heap limit refused, over the VM's life
    std::uint64_t unhandledRejections = 0;    // reported at the end of a call
    std::uint64_t rejectionsHandledLate = 0;  // reported, then handled in a later call
};

// What teardown found. A leak-free VM reports zeros. NOTE: in a build whose backend has assertions on
// (every *-debug preset), a leaked object or function aborts inside the backend's teardown before this
// report exists; a leaked string is still reported here.
struct VmTeardownReport {
    std::size_t leakedAllocations = 0;
    std::size_t leakedBytes = 0;
};

struct VmCreateResult;

class Vm {
public:
    [[nodiscard]] static VmCreateResult create(const VmConfig& config = {});

    Vm() noexcept;  // an INVALID Vm: what a failed create returns and what a move leaves behind
    Vm(Vm&& other) noexcept;
    Vm& operator=(Vm&& other) noexcept;  // tears the assigned-to VM down first
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    ~Vm();  // == shutdown(), report discarded (its ERROR record is not)

    [[nodiscard]] bool valid() const noexcept;

    // THE entry point. Compiles `source` as strict global code under `fileName` (stack traces and every
    // log record name it), runs it, drains the job queue, reports unhandled promise rejections, and
    // returns. Both strings are copied: neither needs a terminator nor outlives the call.
    //
    // Every failure is logged once, at ERROR, as "script: <fileName>: " followed by:
    //   CompileError   "compile error: <name>: <message>"                       + "\n<stack>"
    //   Exception      "uncaught <name>: <message>"                             + "\n<stack>"  (an Error)
    //                  "uncaught exception: <String(value)>"                                   (anything else)
    //   OutOfMemory    "out of memory (heap limit <L> bytes, <U> bytes in use)" + "\n<stack>"
    //   Interrupted    "interrupted after exceeding the <B> ms time budget"     + "\n<stack>"
    //                  "... time budget with promise jobs still queued"        (the drain was cut short)
    //   a rejection    "unhandled promise rejection: <name>: <message>"         + "\n<stack>"  (status unchanged)
    // and, at INFO, "a promise rejection reported as unhandled was handled later". A "\n<stack>" tail is
    // present only when the stack text is non-empty.
    [[nodiscard]] EvalResult evaluateScript(std::string_view source, std::string_view fileName);

    // Runs the cycle collector now. Reference counting frees everything else immediately.
    void collectGarbage() noexcept;

    // The VM's thread only (a debug assert says so).
    [[nodiscard]] VmStats stats() const noexcept;

    // Frees the context and the runtime and reports what did not come back. Idempotent; leaves the Vm
    // invalid. May run on any thread provided no call is in flight.
    [[nodiscard]] VmTeardownReport shutdown() noexcept;

private:
    struct Impl;  // holds the backend's runtime + context; defined in src/vm_impl.hpp
    explicit Vm(std::unique_ptr<Impl> owned) noexcept;
    std::unique_ptr<Impl> impl;  // heap-pinned: the backend's callbacks hold Impl*, never Vm*
};

struct VmCreateResult {
    VmCreateStatus status = VmCreateStatus::OutOfMemory;
    Vm vm;  // valid() iff status == Ok
};

// The backend's version string ("0.15.1" at the pinned vcpkg baseline).
[[nodiscard]] std::string_view scriptBackendVersion() noexcept;

}  // namespace engine::script
