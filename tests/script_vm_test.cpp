// Aero Engine -- engine::script::Vm (task 4.1.1): lifecycle, limits and the trapping matrix.
//
// Every VM case drives the PUBLIC API only -- no backend header is included here: a case proves an EFFECT
// (a status, a log record, a value the script left behind), never an internal. The one exception is the
// backend-free allocator, whose contract VM36-VM38 test directly through aero::script_internal. Every case captures log
// records through the one setLogCallback slot and ends with expectCleanShutdown(), so each one is also a
// leak check: an assert-enabled backend aborts in its teardown on a leaked object, and the allocator's
// count reports every other leak in every configuration.
//
// No case asserts an upper bound on a duration (a slow runner must never redden a correct tree), and
// every case that is bounded by memory or by stack depth runs with the MAXIMUM time budget, so a slow
// Debug lane cannot turn an OutOfMemory or a RangeError into an Interrupted.

#include <aero/core/log.hpp>
#include <aero/script/internal/heap_accounting.hpp>  // VM36-VM38: the allocator's contract, white-box
#include <aero/script/script.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace engine::script {
// doctest streams a failing operand through an unqualified operator<< found by ADL, so a printer for an
// engine type must live in that type's own namespace (the render_light_falloff_test.cpp precedent).
std::ostream& operator<<(std::ostream& out, EvalStatus status) { return out << evalStatusLabel(status); }
std::ostream& operator<<(std::ostream& out, VmCreateStatus status) { return out << vmCreateStatusLabel(status); }
}  // namespace engine::script

namespace {

using engine::LogLevel;
using engine::script::EvalResult;
using engine::script::EvalStatus;
using engine::script::Vm;
using engine::script::VmConfig;
using engine::script::VmCreateResult;
using engine::script::VmCreateStatus;
using engine::script::VmStats;
using engine::script::VmTeardownReport;

struct CapturedRecord {
    LogLevel level = LogLevel::Info;
    std::string message;
};

// Every record, from any thread (VM24 logs from a second one), copied under a lock.
class LogCapture {
public:
    LogCapture() {
        engine::setLogCallback([this](const engine::LogRecord& record) {
            const std::lock_guard<std::mutex> lock{mutex};
            records.push_back(CapturedRecord{record.level, std::string{record.message}});
        });
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;
    ~LogCapture() { engine::setLogCallback(nullptr); }

    [[nodiscard]] std::vector<CapturedRecord> atLeast(LogLevel floor) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::vector<CapturedRecord> out;
        for (const CapturedRecord& record : records) {
            if (record.level >= floor) {
                out.push_back(record);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t countAtLeast(LogLevel floor) const { return atLeast(floor).size(); }

    [[nodiscard]] std::size_t countExactly(LogLevel level) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::size_t count = 0;
        for (const CapturedRecord& record : records) {
            if (record.level == level) {
                ++count;
            }
        }
        return count;
    }

    void clear() {
        const std::lock_guard<std::mutex> lock{mutex};
        records.clear();
    }

private:
    mutable std::mutex mutex;
    std::vector<CapturedRecord> records;
};

[[nodiscard]] Vm makeVm(const VmConfig& config = {}) {
    VmCreateResult created = Vm::create(config);
    REQUIRE(created.status == VmCreateStatus::Ok);
    REQUIRE(created.vm.valid());
    return std::move(created.vm);
}

// The tail of EVERY case.
void expectCleanShutdown(Vm& vm) {
    const VmTeardownReport report = vm.shutdown();
    CHECK(report.leakedAllocations == 0U);
    CHECK(report.leakedBytes == 0U);
    CHECK_FALSE(vm.valid());
}

// "One ERROR": exactly one record at WARN or above, so a stray WARN fails too, and it is an ERROR carrying
// exactly `message`.
void expectOnlyError(const LogCapture& log, std::string_view message) {
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[0].message == message);
}

// The one ERROR's text (exactly one record at WARN or above, and it is an ERROR).
[[nodiscard]] std::string onlyErrorMessage(const LogCapture& log) {
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    return records[0].message;
}

[[nodiscard]] std::string_view firstLine(std::string_view text) { return text.substr(0, text.find('\n')); }

// For a case bounded by memory or stack depth: it must never be cut short by the clock.
[[nodiscard]] VmConfig patient(VmConfig config = {}) {
    config.callTimeBudget = engine::script::MAX_CALL_TIME_BUDGET;
    return config;
}

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = KIB * KIB;

}  // namespace

// ---- VM1-VM6: the configuration, create(), the version and the move rules --------------------------------

TEST_CASE("script: VmConfig's defaults are 64 MiB, 512 KiB and 1000 ms, inside their own ranges (VM1)") {
    namespace script = engine::script;
    const VmConfig config{};
    CHECK(config.heapLimitBytes == 64 * MIB);
    CHECK(config.stackLimitBytes == 512 * KIB);
    CHECK(config.callTimeBudget.count() == 1000);
    CHECK(script::MIN_STACK_LIMIT_BYTES <= script::DEFAULT_STACK_LIMIT_BYTES);
    CHECK(script::DEFAULT_STACK_LIMIT_BYTES <= script::MAX_STACK_LIMIT_BYTES);
    CHECK(script::MIN_CALL_TIME_BUDGET <= script::DEFAULT_CALL_TIME_BUDGET);
    CHECK(script::DEFAULT_CALL_TIME_BUDGET <= script::MAX_CALL_TIME_BUDGET);
    CHECK(script::MIN_HEAP_LIMIT_BYTES == 1 * MIB);
    CHECK(script::MIN_STACK_LIMIT_BYTES == 64 * KIB);
    CHECK(script::MAX_STACK_LIMIT_BYTES == 8 * MIB);
    CHECK(script::MIN_CALL_TIME_BUDGET.count() == 1);
    CHECK(script::MAX_CALL_TIME_BUDGET.count() == 60000);
}

TEST_CASE("script: create() with the defaults gives a valid VM and logs nothing at INFO or above (VM2)") {
    const LogCapture log;
    VmCreateResult created = Vm::create();
    REQUIRE(created.status == VmCreateStatus::Ok);
    REQUIRE(created.vm.valid());
    const VmStats stats = created.vm.stats();
    CHECK(stats.heapLimitBytes == 64 * MIB);
    CHECK(stats.heapBytesInUse > 0U);
    CHECK(stats.heapBytesInUse < 64 * MIB);
    CHECK(stats.heapBytesInUse < engine::script::MIN_HEAP_LIMIT_BYTES);  // the minimum really holds an empty VM
    CHECK(stats.liveAllocations > 0U);
    CHECK(stats.heapBytesPeak >= stats.heapBytesInUse);
    CHECK(stats.refusedAllocations == 0U);
    CHECK(stats.unhandledRejections == 0U);
    CHECK(stats.rejectionsHandledLate == 0U);
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);  // the "VM created" record is DEBUG
    MESSAGE("empty VM: " << stats.heapBytesInUse << " bytes in use in " << stats.liveAllocations << " allocations");
    expectCleanShutdown(created.vm);
}

TEST_CASE("script: each invalid VmConfig field is refused with its own status and one ERROR (VM3)") {
    struct Row {
        VmConfig config;
        VmCreateStatus status;
        std::string_view message;
    };
    const auto withStack = [](std::size_t bytes) {
        VmConfig config;
        config.stackLimitBytes = bytes;
        return config;
    };
    const auto withBudget = [](std::chrono::milliseconds budget) {
        VmConfig config;
        config.callTimeBudget = budget;
        return config;
    };
    VmConfig noHeap;
    noHeap.heapLimitBytes = 0;
    VmConfig noHeapNoStack = withStack(0);
    noHeapNoStack.heapLimitBytes = 0;

    constexpr std::string_view HEAP_MESSAGE =
        "script: Vm::create refused: heapLimitBytes 0 is below the minimum 1048576";
    const std::array<Row, 8> rows{{
        {noHeap, VmCreateStatus::InvalidHeapLimit, HEAP_MESSAGE},
        {withStack(0), VmCreateStatus::InvalidStackLimit,
         "script: Vm::create refused: stackLimitBytes 0 is outside [65536, 8388608]"},
        {withStack(64 * KIB - 1), VmCreateStatus::InvalidStackLimit,
         "script: Vm::create refused: stackLimitBytes 65535 is outside [65536, 8388608]"},
        {withStack(8 * MIB + 1), VmCreateStatus::InvalidStackLimit,
         "script: Vm::create refused: stackLimitBytes 8388609 is outside [65536, 8388608]"},
        {withBudget(std::chrono::milliseconds{0}), VmCreateStatus::InvalidTimeBudget,
         "script: Vm::create refused: callTimeBudget 0 ms is outside [1, 60000] ms"},
        {withBudget(std::chrono::milliseconds{-1}), VmCreateStatus::InvalidTimeBudget,
         "script: Vm::create refused: callTimeBudget -1 ms is outside [1, 60000] ms"},
        {withBudget(std::chrono::milliseconds{60001}), VmCreateStatus::InvalidTimeBudget,
         "script: Vm::create refused: callTimeBudget 60001 ms is outside [1, 60000] ms"},
        // the heap is checked first, so a config wrong in two fields is named by the first
        {noHeapNoStack, VmCreateStatus::InvalidHeapLimit, HEAP_MESSAGE},
    }};

    LogCapture log;
    for (const Row& row : rows) {
        INFO("expecting: " << row.message);
        log.clear();
        const VmCreateResult result = Vm::create(row.config);
        CHECK(result.status == row.status);
        CHECK_FALSE(result.vm.valid());
        expectOnlyError(log, row.message);
    }

    // The four boundaries are accepted.
    log.clear();
    const std::array<VmConfig, 4> edges{withStack(64 * KIB), withStack(8 * MIB),
                                        withBudget(std::chrono::milliseconds{1}),
                                        withBudget(std::chrono::milliseconds{60'000})};
    for (const VmConfig& edge : edges) {
        Vm vm = makeVm(edge);
        expectCleanShutdown(vm);
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
}

TEST_CASE("script: a heap limit below 1 MiB is refused; 1 MiB itself creates a working VM (VM4)") {
    LogCapture log;
    VmConfig config;
    config.heapLimitBytes = engine::script::MIN_HEAP_LIMIT_BYTES - 1;
    {
        const VmCreateResult result = Vm::create(config);
        CHECK(result.status == VmCreateStatus::InvalidHeapLimit);
        CHECK_FALSE(result.vm.valid());
        expectOnlyError(log, "script: Vm::create refused: heapLimitBytes 1048575 is below the minimum 1048576");
    }
    for (const std::size_t limit : std::array<std::size_t, 2>{1, 64 * KIB}) {
        log.clear();
        config.heapLimitBytes = limit;
        const VmCreateResult result = Vm::create(config);
        CHECK(result.status == VmCreateStatus::InvalidHeapLimit);
        CHECK_FALSE(result.vm.valid());
        expectOnlyError(log, "script: Vm::create refused: heapLimitBytes " + std::to_string(limit) +
                                 " is below the minimum 1048576");
    }

    // The minimum itself is accepted, and the VM it gives is a working one.
    log.clear();
    config.heapLimitBytes = engine::script::MIN_HEAP_LIMIT_BYTES;
    Vm vm = makeVm(config);
    const EvalResult sum = vm.evaluateScript("1 + 1", "min.js");
    CHECK(sum.status == EvalStatus::Ok);
    CHECK(sum.completion == "2");
    expectCleanShutdown(vm);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
}

TEST_CASE("script: the backend is quickjs-ng 0.15.1 -- a port bump must re-verify its assumptions (VM5)") {
    // A failure here means the pinned port moved. Re-read every backend behaviour this layer relies on (the
    // interrupt cadence, the uncatchable interrupt error, the rejection tracker's two calls, the teardown
    // assert and the allocator contract) against the new source before changing the literal.
    CHECK(engine::script::scriptBackendVersion() == std::string_view{"0.15.1"});
}

TEST_CASE("script: a moved-from VM is invalid and refuses every call; the moved-to VM keeps working (VM6)") {
    LogCapture log;
    Vm a = makeVm();
    Vm b = std::move(a);
    const auto expectMovedFrom = [&log](Vm& moved) {
        CHECK_FALSE(moved.valid());
        log.clear();
        moved.collectGarbage();
        expectOnlyError(log, "script: collectGarbage on an invalid Vm (a failed create or a moved-from Vm)");
        const VmStats stats = moved.stats();
        CHECK(stats.heapLimitBytes == 0U);
        CHECK(stats.heapBytesInUse == 0U);
        CHECK(stats.heapBytesPeak == 0U);
        CHECK(stats.liveAllocations == 0U);
        CHECK(stats.refusedAllocations == 0U);
        CHECK(stats.unhandledRejections == 0U);
        CHECK(stats.rejectionsHandledLate == 0U);
        log.clear();
        const EvalResult evaluated = moved.evaluateScript("1", "x.js");
        CHECK(evaluated.status == EvalStatus::InvalidVm);
        expectOnlyError(log, "script: evaluateScript on an invalid Vm (a failed create or a moved-from Vm)");
        log.clear();
        const VmTeardownReport report = moved.shutdown();
        CHECK(report.leakedAllocations == 0U);
        CHECK(report.leakedBytes == 0U);
        CHECK(log.countAtLeast(LogLevel::Trace) == 0U);
    };
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- that IS the subject
    expectMovedFrom(a);
    CHECK(b.valid());

    // Move-assignment tears the assigned-to VM down first, and finds nothing wrong with it.
    Vm c = makeVm();
    log.clear();
    c = std::move(b);
    CHECK(c.valid());
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- that IS the subject
    expectMovedFrom(b);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);

    // Every allocation of this call goes through the allocator whose address the runtime captured before
    // both moves; ASan would report a dangling one.
    log.clear();
    const EvalResult moved = c.evaluateScript("1 + 1", "moved.js");
    CHECK(moved.status == EvalStatus::Ok);
    CHECK(moved.completion == "2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(c);

    // The VM a failed create returns is invalid too.
    VmConfig tiny;
    tiny.heapLimitBytes = 1;
    VmCreateResult failed = Vm::create(tiny);
    CHECK(failed.status == VmCreateStatus::InvalidHeapLimit);
    log.clear();
    const EvalResult refused = failed.vm.evaluateScript("1", "failed.js");
    CHECK(refused.status == EvalStatus::InvalidVm);
    expectOnlyError(log, "script: evaluateScript on an invalid Vm (a failed create or a moved-from Vm)");
}

// ---- VM7-VM18: evaluateScript and every synchronous arm of the trapping matrix ------------------------------

TEST_CASE("script: evaluateScript returns the completion value as text (VM7)") {
    const LogCapture log;
    Vm vm = makeVm();
    const auto run = [&vm](std::string_view source) { return vm.evaluateScript(source, "v7.js"); };

    const EvalResult sum = run("1 + 1");
    CHECK(sum.status == EvalStatus::Ok);
    CHECK(sum.completion == "2");
    CHECK(sum.error.name.empty());
    CHECK(sum.error.message.empty());
    CHECK(sum.error.stack.empty());

    CHECK(run("\"a\" + \"b\"").completion == "ab");
    CHECK(run("({})").completion == "[object Object]");
    CHECK(run("undefined").completion == "undefined");

    // Rendering the completion never changes the status.
    const EvalResult symbol = run("Symbol(\"s\")");
    CHECK(symbol.status == EvalStatus::Ok);
    CHECK(symbol.completion == "<unprintable value>");

    // An Error whose toString throws: the backend's conversion falls back to the `message` property and reports
    // success with the exception still pending. That is a failed conversion all the same, and nothing may
    // outlive the call: the pre-free object check at teardown would find the pending exception's objects.
    const std::string_view brokenToString =
        R"js((() => { const e = new Error("m"); e.toString = () => { throw new Error("t"); }; return e; })())js";
    const EvalResult throwing = run(brokenToString);
    CHECK(throwing.status == EvalStatus::Ok);
    CHECK(throwing.completion == "<unprintable value>");

    // The same broken toString, but the conversion happens INSIDE the script: parseFloat converts the Error and
    // returns, with the exception still pending. The completion is an ordinary 42, and that stale exception is
    // not its failure.
    const std::string_view staleException =
        R"js((() => { const e = new Error("m"); e.toString = () => { throw new Error("t"); }; )js"
        R"js(parseFloat(e); return 42; })())js";
    const EvalResult stale = run(staleException);
    CHECK(stale.status == EvalStatus::Ok);
    CHECK(stale.completion == "42");

    // The terminator arm: the view's next byte is ';', not '\0', so a wrapper that handed the view to the
    // backend unterminated would let it read on into `throw 1;` and return Exception.
    const std::string_view whole{"1 + 1; throw 1;"};
    const EvalResult view = vm.evaluateScript(whole.substr(0, 5), "view.js");
    CHECK(view.status == EvalStatus::Ok);
    CHECK(view.completion == "2");

    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // ... and the teardown found nothing still referenced
}

TEST_CASE("script: state persists across calls, and the code runs strict (VM8)") {
    const LogCapture log;
    Vm vm = makeVm();
    CHECK(vm.evaluateScript("globalThis.counter = 41;", "s8.js").status == EvalStatus::Ok);
    const EvalResult next = vm.evaluateScript("++counter", "s8.js");
    CHECK(next.status == EvalStatus::Ok);
    CHECK(next.completion == "42");

    // Sloppy code would have created a global: the witness for the strict flag.
    const EvalResult strict = vm.evaluateScript("undeclaredName = 1;", "s8.js");
    CHECK(strict.status == EvalStatus::Exception);
    CHECK(strict.error.name == "ReferenceError");
    CHECK(log.countAtLeast(LogLevel::Warn) == 1U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: nothing engine- or OS-shaped is visible, and no script can block the thread (VM9)") {
    const LogCapture log;
    Vm vm = makeVm();
    const std::string_view globalsProbe =
        "[typeof print, typeof console, typeof std, typeof os, typeof require, typeof scriptEngine].join()";
    const EvalResult absent = vm.evaluateScript(globalsProbe, "v9.js");
    CHECK(absent.status == EvalStatus::Ok);
    CHECK(absent.completion == "undefined,undefined,undefined,undefined,undefined,undefined");

    // The control: the language's own globals are there.
    const EvalResult present =
        vm.evaluateScript("[typeof Promise, typeof queueMicrotask, typeof JSON].join()", "v9.js");
    CHECK(present.completion == "function,function,object");

    // The two stack-trace hooks are gone: a backtrace would run them as script code, and an interrupt raised
    // inside either is thrown away by the backend.
    const std::string_view hookProbe =
        R"js([Object.getOwnPropertyDescriptor(Error, "prepareStackTrace"), )js"
        R"js(Object.getOwnPropertyDescriptor(Error, "stackTraceLimit")].every((d) => d === undefined))js";
    CHECK(vm.evaluateScript(hookProbe, "v9.js").completion == "true");

    // A timeout of 0 means that even a regression that enabled blocking returns at once ("returned" or
    // "timed-out") instead of hanging the case.
    const EvalResult blocking = vm.evaluateScript(
        "typeof Atomics === \"undefined\" ? \"absent\" : (() => { try { Atomics.wait(new Int32Array("
        "new SharedArrayBuffer(4)), 0, 0, 0); return \"returned\"; } catch (e) { return e.name + \": \" + "
        "e.message; } })()",
        "v9.js");
    CHECK(blocking.status == EvalStatus::Ok);
    const bool cannotBlock =
        blocking.completion == "TypeError: cannot block in this thread" || blocking.completion == "absent";
    CHECK(cannotBlock);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: an uncaught Error is Exception with its name, message and stack, logged once (VM10)") {
    LogCapture log;
    Vm vm = makeVm();
    // The file name is a prefix of a longer view, so a wrapper that handed it to the backend unterminated
    // would print `err.jsTRAILING` in both the stack and the record.
    const std::string_view fileName = std::string_view{"err.jsTRAILING"}.substr(0, 6);
    log.clear();
    const std::string_view source = "function f() { throw new TypeError(\"bad thing\"); } f();";
    const EvalResult result = vm.evaluateScript(source, fileName);
    CHECK(result.status == EvalStatus::Exception);
    CHECK(result.error.name == "TypeError");
    CHECK(result.error.message == "bad thing");
    CHECK(firstLine(result.error.stack).starts_with("    at f (err.js:1:"));
    CHECK_FALSE(result.error.stack.empty());
    CHECK(result.error.stack.back() != '\n');
    CHECK(result.completion.empty());
    // The log and the result agree byte for byte.
    expectOnlyError(log, "script: err.js: uncaught TypeError: bad thing\n" + result.error.stack);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a thrown non-Error value is Exception with String(value), or a placeholder (VM11)") {
    struct Row {
        std::string_view script;
        std::string_view message;
    };
    const std::array<Row, 7> rows{{
        {"throw 42;", "42"},
        {"throw \"boom\";", "boom"},
        {"throw null;", "null"},  // Exception, NOT OutOfMemory: no allocation was refused
        {"throw undefined;", "undefined"},
        {"throw { a: 1 };", "[object Object]"},
        {"throw Symbol(\"s\");", "<unprintable value>"},
        {"throw { toString() { throw new Error(\"nested\"); } };", "<unprintable value>"},
    }};
    LogCapture log;
    Vm vm = makeVm();
    for (const Row& row : rows) {
        INFO("script: " << row.script);
        log.clear();
        const EvalResult result = vm.evaluateScript(row.script, "v.js");
        CHECK(result.status == EvalStatus::Exception);
        CHECK(result.error.name.empty());
        CHECK(result.error.stack.empty());
        CHECK(result.error.message == row.message);
        expectOnlyError(log, "script: v.js: uncaught exception: " + std::string{row.message});
    }

    // Nothing the conversions threw survived into the next call.
    log.clear();
    const EvalResult next = vm.evaluateScript("1 + 1", "v.js");
    CHECK(next.status == EvalStatus::Ok);
    CHECK(next.completion == "2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a syntax error is CompileError, runs nothing, and names file, line and column (VM12)") {
    LogCapture log;
    Vm vm = makeVm();
    log.clear();
    const EvalResult result = vm.evaluateScript("globalThis.ran = true; let x = ;", "bad.js");
    CHECK(result.status == EvalStatus::CompileError);
    CHECK(result.error.name == "SyntaxError");
    CHECK_FALSE(result.error.message.empty());
    CHECK(firstLine(result.error.stack).starts_with("    at bad.js:1:"));
    const std::string expected = "script: bad.js: compile error: SyntaxError: " + result.error.message;
    expectOnlyError(log, expected + "\n" + result.error.stack);

    // The first statement never ran.
    const EvalResult ran = vm.evaluateScript("typeof ran", "bad.js");
    CHECK(ran.status == EvalStatus::Ok);
    CHECK(ran.completion == "undefined");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a SyntaxError thrown at run time by eval() is Exception, not CompileError (VM13)") {
    LogCapture log;
    Vm vm = makeVm();
    log.clear();
    const EvalResult result = vm.evaluateScript("eval(\"let y = ;\");", "ev.js");
    CHECK(result.status == EvalStatus::Exception);
    CHECK(result.error.name == "SyntaxError");
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message).starts_with("script: ev.js: uncaught SyntaxError: "));
    expectCleanShutdown(vm);
}

TEST_CASE("script: an uncaught out-of-memory is OutOfMemory, logged with the limit, and the VM recovers (VM14)") {
    LogCapture log;
    // The DEFAULT 64 MiB heap: this is the one case that exercises the shipped limit.
    Vm vm = makeVm(patient());
    log.clear();
    const EvalResult big =
        vm.evaluateScript("(() => { const s = \"x\".repeat(128 * 1024 * 1024); return s.length; })()", "oom.js");
    CHECK(big.status == EvalStatus::OutOfMemory);
    CHECK(big.error.name == "InternalError");
    CHECK(big.error.message == "out of memory");
    CHECK(vm.stats().refusedAllocations > 0U);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message).starts_with("script: oom.js: out of memory (heap limit 67108864 bytes, "));
    CHECK(firstLine(message).ends_with(" bytes in use)"));
    const EvalResult after = vm.evaluateScript("1 + 1", "oom.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");

    // The array arm fills the heap for real; its refusal is a REALLOCATE inside the backend, the path whose
    // keep-the-block contract VM37 pins in isolation.
    log.clear();
    const EvalResult grow = vm.evaluateScript("(() => { const a = []; for (;;) { a.push(1.5); } })()", "grow.js");
    CHECK(grow.status == EvalStatus::OutOfMemory);
    CHECK(grow.error.message == "out of memory");
    const EvalResult again = vm.evaluateScript("1 + 1", "grow.js");
    CHECK(again.status == EvalStatus::Ok);
    CHECK(again.completion == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: an out-of-memory the script catches is Ok and logs nothing (VM15)") {
    const LogCapture log;
    VmConfig config;
    config.heapLimitBytes = 8 * MIB;
    Vm vm = makeVm(patient(config));
    const EvalResult caught = vm.evaluateScript(
        R"js((() => { try { "x".repeat(64 * 1024 * 1024); return "no"; } catch (e) { return e.message; } })())js",
        "caught.js");
    CHECK(caught.status == EvalStatus::Ok);
    CHECK(caught.completion == "out of memory");
    CHECK(vm.stats().refusedAllocations > 0U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // a refusal alone is not an OutOfMemory

    // The refusal belongs to an earlier call, and the witness is per call.
    const EvalResult thrown = vm.evaluateScript("throw null;", "caught.js");
    CHECK(thrown.status == EvalStatus::Exception);

    // A refusal in THIS call and an Error as the thrown value are still not enough: its message must be a
    // string, as every message the backend's own out-of-memory error carries is.
    const std::string_view symbolMessage =
        R"js((() => { try { "x".repeat(64 * 1024 * 1024); } catch (e) {} )js"
        R"js(const err = new Error("x"); err.message = Symbol("s"); throw err; })())js";
    const EvalResult scripted = vm.evaluateScript(symbolMessage, "caught.js");
    CHECK(scripted.status == EvalStatus::Exception);

    // And a message that cannot even be read -- quickjs's own out-of-memory error carries it as a plain own data
    // property, so a getter that throws can only be a script's.
    const std::string_view throwingMessage =
        R"js((() => { try { "x".repeat(64 * 1024 * 1024); } catch (e) {} )js"
        R"js(const err = new Error("x"); )js"
        R"js(Object.defineProperty(err, "message", { get() { throw 1; } }); throw err; })())js";
    const EvalResult unreadable = vm.evaluateScript(throwingMessage, "caught.js");
    CHECK(unreadable.status == EvalStatus::Exception);
    expectCleanShutdown(vm);
}

TEST_CASE("script: small-object exhaustion is OutOfMemory whatever value the backend threw (VM16)") {
    LogCapture log;
    VmConfig config;
    config.heapLimitBytes = 4 * MIB;
    {
        Vm vm = makeVm(patient(config));
        log.clear();
        const EvalResult list =
            vm.evaluateScript("(() => { let head = null; for (;;) { head = { next: head }; } })()", "list.js");
        CHECK(list.status == EvalStatus::OutOfMemory);
        const std::string message = onlyErrorMessage(log);
        CHECK(firstLine(message).starts_with("script: list.js: out of memory (heap limit 4194304 bytes, "));
        // Which of the backend's two failure values this build produced: an InternalError, or `null` when not
        // even the error object fits.
        MESSAGE("thrown value: " << list.error.name << "/" << list.error.message);
        // The list was function-local: unreachable after the unwind.
        const EvalResult after = vm.evaluateScript("1 + 1", "list.js");
        CHECK(after.status == EvalStatus::Ok);
        CHECK(after.completion == "2");
        expectCleanShutdown(vm);
    }

    // The collector arm. Every node references itself, so reference counting frees NOTHING after the
    // unwind; only the collector the trap runs after classifying the failure empties the heap. A follow-up
    // as small as `1 + 1` fits in the slack the failed allocation leaves (about 1 KB on this build), so it
    // cannot tell the two apart: the heap figure right after the call can.
    Vm vm = makeVm(patient(config));
    log.clear();
    const EvalResult cycles = vm.evaluateScript(
        "(() => { let head = null; for (;;) { const a = { next: head }; a.self = a; head = a; } })()", "cycles.js");
    CHECK(cycles.status == EvalStatus::OutOfMemory);
    // trap collected the unreachable cycles; a heap still full here would leave every later call only that slack
    CHECK(vm.stats().heapBytesInUse < vm.stats().heapLimitBytes / 2U);
    const EvalResult after = vm.evaluateScript("1 + 1", "cycles.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a stack overflow is a catchable RangeError, trapped as Exception (VM17)") {
    LogCapture log;
    Vm vm = makeVm(patient());
    log.clear();
    const EvalResult deep = vm.evaluateScript("function r(n) { return r(n + 1) + 1; } r(0);", "deep.js");
    CHECK(deep.status == EvalStatus::Exception);
    CHECK(deep.error.name == "RangeError");
    CHECK(deep.error.message == "Maximum call stack size exceeded");
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: deep.js: uncaught RangeError: Maximum call stack size exceeded");

    // Catchable, unlike the interrupt.
    const EvalResult caught = vm.evaluateScript(
        "(() => { function q() { return q() + 1; } try { q(); return \"no\"; } catch (e) { return e.name; } })()",
        "deep.js");
    CHECK(caught.status == EvalStatus::Ok);
    CHECK(caught.completion == "RangeError");
    const EvalResult after = vm.evaluateScript("1 + 1", "deep.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");
    expectCleanShutdown(vm);
}

namespace {

// Each padded frame's array is past 64 KiB on purpose. ASan's use-after-return instrumentation moves a smaller
// frame's locals onto its heap-allocated fake stack (on by default with Linux's libasan), where they would deepen
// nothing; both compilers leave a larger frame on the real stack (GCC 13 asan.cc: asan_frame_size <= 65536; LLVM 18
// AddressSanitizer.cpp: LocalStackSize <= kMaxStackMallocSize, 1 << 16).
constexpr std::size_t PADDING_FRAME_BYTES = 80 * KIB;

// Where the outermost and the deepest padding arrays sit, so the case measures how deep the VM was created instead
// of assuming it.
struct PaddingSpan {
    std::uintptr_t outermost = 0;
    std::uintptr_t deepest = 0;
};

// Creates a VM `frames` levels below the first padded frame. Every level writes EVERY byte of its volatile array --
// an optimiser drops the bytes a frame never touches, and did: with three of 4096 written, a Release frame came to
// 32 bytes -- and recurses through a volatile function pointer, so no level can be inlined, merged, shrunk or dropped.
Vm createUnderPaddedFrames(const VmConfig& config, int frames, PaddingSpan& span);
using PaddedFrameFn = Vm (*)(const VmConfig&, int, PaddingSpan&);
volatile PaddedFrameFn paddedFrame = &createUnderPaddedFrames;

Vm createUnderPaddedFrames(const VmConfig& config, int frames, PaddingSpan& span) {
    std::array<volatile unsigned char, PADDING_FRAME_BYTES> padding;
    for (volatile unsigned char& byte : padding) {
        byte = 1;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(padding.data());
    if (span.outermost == 0) {
        span.outermost = address;
    }
    if (frames <= 0) {
        span.deepest = address;
    }
    const PaddedFrameFn next = paddedFrame;
    Vm vm = frames <= 0 ? makeVm(config) : next(config, frames - 1, span);
    padding[1] = padding[0];  // the frame is live across the call, and the deepest one's across the create
    return vm;
}

// How deep a script can recurse before the backend's own stack check fires, measured from the call.
[[nodiscard]] std::size_t depthOf(Vm& vm) {
    const EvalResult result = vm.evaluateScript(
        "(() => { let d = 0; function r() { ++d; r(); } try { r(); } catch (e) {} return d; })()", "depth.js");
    REQUIRE(result.status == EvalStatus::Ok);
    return static_cast<std::size_t>(std::stoull(result.completion));
}

}  // namespace

TEST_CASE("script: the stack limit is applied, and measured from the call rather than from creation (VM18)") {
    const LogCapture log;
    VmConfig shortConfig;
    shortConfig.stackLimitBytes = engine::script::MIN_STACK_LIMIT_BYTES;
    VmConfig longConfig;
    longConfig.stackLimitBytes = 512 * KIB;
    Vm shortStack = makeVm(patient(shortConfig));
    Vm longStack = makeVm(patient(longConfig));
    PaddingSpan span;
    Vm deepCreated = createUnderPaddedFrames(patient(shortConfig), 2, span);
    // "Created deep" is measured, not assumed: the deepest padding sits at least 128 KiB below the outermost, which
    // is itself below this frame (the stack grows down on every supported target).
    REQUIRE(span.outermost > span.deepest);
    REQUIRE(span.outermost - span.deepest >= 128 * KIB);

    const std::size_t shortDepth = depthOf(shortStack);
    const std::size_t longDepth = depthOf(longStack);
    const std::size_t deepDepth = depthOf(deepCreated);
    MESSAGE("depths: 64 KiB=" << shortDepth << " 512 KiB=" << longDepth << " (64 KiB, created deep)=" << deepDepth
                              << "; padding span " << (span.outermost - span.deepest) / KIB << " KiB");
    CHECK(shortDepth > 0U);
    // The configured limit is applied: without it both would use the backend's own 1 MiB.
    CHECK(shortDepth < longDepth);
    // Both are entered from the same frame, so both measure the same 64 KiB below the same entry. Without
    // the stack top being re-captured at entry, `deep`'s limit would sit at least 128 KiB lower (the span above)
    // and it would recurse at least three times as far.
    CHECK(deepDepth == shortDepth);

    expectCleanShutdown(shortStack);
    expectCleanShutdown(longStack);
    expectCleanShutdown(deepCreated);
}

// ---- VM19-VM23: the per-call time budget ------------------------------------------------------------------

namespace {

[[nodiscard]] VmConfig withBudget(std::chrono::milliseconds budget) {
    VmConfig config;
    config.callTimeBudget = budget;
    return config;
}

}  // namespace

TEST_CASE("script: a runaway loop is Interrupted no earlier than its budget, logged, and the VM recovers (VM19)") {
    LogCapture log;
    {
        // The DEFAULT 1000 ms budget, the one the engine ships.
        Vm vm = makeVm();
        log.clear();
        const auto start = std::chrono::steady_clock::now();
        const std::string_view spin = "globalThis.n = 0; while (true) { ++globalThis.n; }";
        const EvalResult result = vm.evaluateScript(spin, "spin.js");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(result.status == EvalStatus::Interrupted);
        CHECK(result.error.name == "InternalError");
        CHECK(result.error.message == "interrupted");
        CHECK(result.error.stack.find("spin.js:1:") != std::string::npos);  // the loop's own frame
        // A LOWER bound, which no slow runner can break: the handler fires only once the deadline has passed,
        // and this clock started before the call's.
        CHECK(elapsed >= engine::script::DEFAULT_CALL_TIME_BUDGET);
        const std::string expected = "script: spin.js: interrupted after exceeding the 1000 ms time budget\n";
        expectOnlyError(log, expected + result.error.stack);
        MESSAGE("interrupt latency past the budget: " << std::chrono::duration_cast<std::chrono::milliseconds>(
                                                             elapsed - engine::script::DEFAULT_CALL_TIME_BUDGET)
                                                             .count()
                                                      << " ms");

        // Anti-vacuity: the loop really ran, so the interrupt is the budget's and not, say, a compile failure.
        const EvalResult ran = vm.evaluateScript("n > 0", "spin.js");
        CHECK(ran.status == EvalStatus::Ok);
        CHECK(ran.completion == "true");
        const EvalResult after = vm.evaluateScript("1 + 1", "spin.js");
        CHECK(after.status == EvalStatus::Ok);
        CHECK(after.completion == "2");
        expectCleanShutdown(vm);
    }

    // Queued work behind an interrupt: the drain is skipped for an interrupted script, so there is exactly one
    // record; the next call drains the leftover job silently.
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    const EvalResult queued = vm.evaluateScript("Promise.resolve().then(() => {}); while (true) {}", "queued.js");
    CHECK(queued.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: queued.js: interrupted after exceeding the 100 ms time budget");
    log.clear();
    const EvalResult next = vm.evaluateScript("1 + 1", "queued.js");
    CHECK(next.status == EvalStatus::Ok);
    CHECK(next.completion == "2");
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: the interrupt cannot be caught -- neither catch nor finally runs (VM20)") {
    const LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    const EvalResult result = vm.evaluateScript(
        "try { while (true) {} } catch (e) { globalThis.caught = true; } finally { globalThis.fin = true; }",
        "uncatchable.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const EvalResult probe = vm.evaluateScript("[typeof caught, typeof fin].join()", "uncatchable.js");
    CHECK(probe.status == EvalStatus::Ok);
    CHECK(probe.completion == "undefined,undefined");
    expectCleanShutdown(vm);
}

TEST_CASE("script: catastrophic regex backtracking is Interrupted too (VM21)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // About 2^40 backtracking steps would otherwise run for hours: the regex engine polls the same handler.
    const EvalResult result = vm.evaluateScript(R"js(/(a+)+$/.test("a".repeat(40) + "!"))js", "re.js");
    CHECK(result.status == EvalStatus::Interrupted);
    CHECK(result.error.message == "interrupted");
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: re.js: interrupted after exceeding the 100 ms time budget");
    expectCleanShutdown(vm);
}

TEST_CASE("script: every call gets a fresh deadline (VM23)") {
    const LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{200}));
    CHECK(vm.evaluateScript("while (true) {}", "fresh.js").status == EvalStatus::Interrupted);
    // The backend consults the handler only every 10 000 polls, so a short follow-up (`1 + 1`) would pass even
    // with a stale, already-expired deadline. Each iteration here passes two polls, so this loop forces about
    // six consultations, the first of which a stale deadline fails.
    const EvalResult next =
        vm.evaluateScript("(() => { let i = 0; while (i < 30000) { ++i; } return i; })()", "fresh.js");
    CHECK(next.status == EvalStatus::Ok);
    CHECK(next.completion == "30000");
    expectCleanShutdown(vm);
}

// ---- VM24: the thread check -------------------------------------------------------------------------------

TEST_CASE("script: a call from another thread is refused without touching the backend (VM24)") {
    LogCapture log;
    Vm vm = makeVm();
    const std::size_t before = vm.stats().liveAllocations;
    log.clear();
    std::thread{[&vm] { vm.collectGarbage(); }}.join();
    expectOnlyError(log, "script: collectGarbage called from a thread that is not the VM's; refused");
    CHECK(vm.stats().liveAllocations == before);

    log.clear();
    EvalResult foreign;
    std::thread{[&vm, &foreign] { foreign = vm.evaluateScript("1 + 1", "t.js"); }}.join();
    CHECK(foreign.status == EvalStatus::WrongThread);
    expectOnlyError(log, "script: evaluateScript called from a thread that is not the VM's; refused");
    CHECK(vm.stats().liveAllocations == before);

    // The VM keeps working on its own thread.
    log.clear();
    const EvalResult own = vm.evaluateScript("1 + 1", "t.js");
    CHECK(own.status == EvalStatus::Ok);
    CHECK(own.completion == "2");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

// ---- VM33-VM34: stats() and the cycle collector -------------------------------------------------------------

TEST_CASE("script: stats() follows the heap up and down and keeps the peak (VM33)") {
    const LogCapture log;
    Vm vm = makeVm();
    const VmStats before = vm.stats();
    const std::string_view grow = "globalThis.big = new Array(200000).fill(0.5); big.length";
    const EvalResult built = vm.evaluateScript(grow, "stats.js");
    CHECK(built.status == EvalStatus::Ok);
    CHECK(built.completion == "200000");
    const VmStats grown = vm.stats();
    CHECK(vm.evaluateScript("globalThis.big = undefined;", "stats.js").status == EvalStatus::Ok);
    const VmStats shrunk = vm.stats();

    // 200 000 values of at least 8 bytes each.
    CHECK(grown.heapBytesInUse > before.heapBytesInUse + 1 * MIB);
    // Reference counting freed the array at once, no collector involved.
    CHECK(shrunk.heapBytesInUse < grown.heapBytesInUse);
    CHECK(grown.heapBytesPeak >= grown.heapBytesInUse);
    CHECK(shrunk.heapBytesPeak >= grown.heapBytesPeak);  // the third call compiles before it frees
    CHECK(shrunk.liveAllocations < grown.liveAllocations);
    CHECK(before.heapLimitBytes == 64 * MIB);
    CHECK(grown.heapLimitBytes == 64 * MIB);
    CHECK(shrunk.heapLimitBytes == 64 * MIB);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: collectGarbage() frees an unreachable cycle that reference counting cannot (VM34)") {
    const LogCapture log;
    Vm vm = makeVm();
    // Call 1 builds a two-object cycle held by a global and watched by a WeakRef. Call 2 drops the last strong
    // reference and creates nothing afterwards, so no automatic collection can run between that drop and the
    // observation: the cycle must still be alive, because reference counting cannot free it.
    const std::string_view build =
        "globalThis.keep = (() => { const a = {}; const b = { a }; a.b = b; return a; })(); "
        "globalThis.w = new WeakRef(keep); w.deref() !== undefined";
    CHECK(vm.evaluateScript(build, "gc.js").completion == "true");
    CHECK(vm.evaluateScript("keep = undefined; w.deref() !== undefined", "gc.js").completion == "true");
    vm.collectGarbage();
    CHECK(vm.evaluateScript("w.deref() === undefined", "gc.js").completion == "true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    expectCleanShutdown(vm);
}

// ---- VM22, VM25-VM31, VM39: promise jobs and unhandled rejections --------------------------------------------

TEST_CASE("script: an endless job chain is stopped at the budget, and the VM still tears down clean (VM22)") {
    LogCapture log;
    {
        Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
        log.clear();
        // Two chains: jobs alternate between them, so while one chain's job runs the other's next job is
        // already queued, and an interrupted job can break only its own chain.
        const EvalResult result =
            vm.evaluateScript("function f() { Promise.resolve().then(f); } f(); f();", "chain.js");
        CHECK(result.status == EvalStatus::Interrupted);
        CHECK(result.completion == "undefined");  // the script itself completed
        // Which of the two shapes follows is deliberately not asserted, because it is not deterministic: usually
        // the drain's pre-job check stops the chain between two jobs ("... with promise jobs still queued"), but
        // when the deadline expires inside a job and the handler's consultation lands in that job first, the
        // handler interrupts the job instead (the plain interrupt record, with its stack).
        const std::string message = onlyErrorMessage(log);
        const std::string_view shape = "script: chain.js: interrupted after exceeding the 100 ms time budget";
        CHECK(firstLine(message).starts_with(shape));
        MESSAGE("chain stopped by: " << result.error.message);

        // A surviving chain is drained under the next call's budget: the documented behaviour.
        const EvalResult next = vm.evaluateScript("1 + 1", "next.js");
        CHECK(next.status == EvalStatus::Interrupted);
        CHECK(next.completion == "2");

        // Teardown with a chain still queued: the backend's own assert is a witness here, and so is the zero
        // report. The pre-free object check must stay quiet while jobs are queued.
        log.clear();
        expectCleanShutdown(vm);
        CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    }

    // Part 2: the deadline passes inside a job, where only the handler can act. Exactly one record: the drain
    // returns at once instead of logging "jobs still queued" for the job behind. The record is matched by the
    // prefix both shapes share, so a call whose start took longer than its budget (the drain's own stop, before any
    // job) cannot redden the case.
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    const EvalResult result = vm.evaluateScript(
        "Promise.resolve().then(() => { while (true) {} }); Promise.resolve().then(() => {});", "job.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    const std::string_view prefix = "script: job.js: interrupted after exceeding the 100 ms time budget";
    CHECK(firstLine(message).starts_with(prefix));
    expectCleanShutdown(vm);
}

TEST_CASE("script: an unhandled rejection is reported once, at ERROR, after the drain (VM25)") {
    LogCapture log;
    {
        Vm vm = makeVm();
        log.clear();
        const EvalResult error = vm.evaluateScript("Promise.reject(new Error(\"nope\"));", "rej.js");
        CHECK(error.status == EvalStatus::Ok);
        const std::string message = onlyErrorMessage(log);
        CHECK(firstLine(message) == "script: rej.js: unhandled promise rejection: Error: nope");
        const std::string_view rest = std::string_view{message}.substr(message.find('\n') + 1);
        CHECK(firstLine(rest).starts_with("    at <eval> (rej.js:1:"));
        CHECK(vm.stats().unhandledRejections == 1U);

        // A non-Error reason.
        log.clear();
        CHECK(vm.evaluateScript("Promise.reject(5);", "rej5.js").status == EvalStatus::Ok);
        expectOnlyError(log, "script: rej5.js: unhandled promise rejection: 5");
        CHECK(vm.stats().unhandledRejections == 2U);
        expectCleanShutdown(vm);
    }

    // Part 3: describing can reject. The getter's own rejection, recorded while the list is being reported, is
    // reported at the end of the NEXT call -- the swap's witness (iterating the live vector would reallocate
    // under the loop: an ASan report in Debug).
    const std::string_view getter =
        "(() => { const e = new Error(\"x\"); Object.defineProperty(e, \"message\", "
        "{ get() { Promise.reject(2); return \"m\"; } }); Promise.reject(e); })();";
    {
        Vm vm = makeVm();
        log.clear();
        CHECK(vm.evaluateScript(getter, "getter.js").status == EvalStatus::Ok);
        const std::string message = onlyErrorMessage(log);
        CHECK(firstLine(message) == "script: getter.js: unhandled promise rejection: Error: m");
        log.clear();
        CHECK(vm.evaluateScript("1 + 1", "next.js").status == EvalStatus::Ok);
        expectOnlyError(log, "script: next.js: unhandled promise rejection: 2");
        expectCleanShutdown(vm);
    }

    // Part 4: the same script, then NO further call: teardown must free the one rejection still listed.
    Vm vm = makeVm();
    CHECK(vm.evaluateScript(getter, "getter.js").status == EvalStatus::Ok);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a rejection handled in the same script logs nothing (VM26)") {
    const LogCapture log;
    Vm vm = makeVm();
    const EvalResult result = vm.evaluateScript("Promise.reject(new Error(\"x\")).catch(() => {});", "same.js");
    CHECK(result.status == EvalStatus::Ok);
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(vm.stats().unhandledRejections == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a rejection handled by a later job of the same call logs nothing (VM27)") {
    const LogCapture log;
    Vm vm = makeVm();
    // The report waits for the whole drain, not for the script's last line.
    const EvalResult result = vm.evaluateScript(
        "(() => { const p = Promise.reject(1); Promise.resolve().then(() => p.catch(() => {})); })();", "later.js");
    CHECK(result.status == EvalStatus::Ok);
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(vm.stats().unhandledRejections == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a rejection handled in a later call logs one INFO (VM28)") {
    LogCapture log;
    Vm vm = makeVm();
    log.clear();
    CHECK(vm.evaluateScript("globalThis.p28 = Promise.reject(new Error(\"late\"));", "late1.js").status ==
          EvalStatus::Ok);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: late1.js: unhandled promise rejection: Error: late");

    log.clear();
    CHECK(vm.evaluateScript("p28.catch(() => {});", "late2.js").status == EvalStatus::Ok);
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Info);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Info);
    CHECK(records[0].message == "script: late2.js: a promise rejection reported as unhandled was handled later");
    CHECK(vm.stats().rejectionsHandledLate == 1U);
    CHECK(vm.stats().unhandledRejections == 1U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: an async function that throws is an unhandled rejection (VM29)") {
    LogCapture log;
    Vm vm = makeVm();
    log.clear();
    const EvalResult result =
        vm.evaluateScript("async function f29() { throw new TypeError(\"async\"); } f29();", "async.js");
    CHECK(result.status == EvalStatus::Ok);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: async.js: unhandled promise rejection: TypeError: async");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a job that throws is trapped; the first failure of a call decides its status (VM30)") {
    LogCapture log;
    Vm vm = makeVm();
    // Part 1: the drain continues past the failure.
    log.clear();
    const EvalResult microtask = vm.evaluateScript(
        "globalThis.after30 = []; queueMicrotask(() => { throw new Error(\"tick\"); }); "
        "queueMicrotask(() => { after30.push(\"ran\"); }); \"done\"",
        "mt.js");
    CHECK(microtask.status == EvalStatus::Exception);
    CHECK(microtask.completion == "done");
    CHECK(microtask.error.message == "tick");
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: mt.js: uncaught Error: tick");
    CHECK(vm.evaluateScript("after30.join()", "mt.js").completion == "ran");

    // Part 2: the script's failure came first, so it decides the status; the job's is still logged.
    log.clear();
    const EvalResult both = vm.evaluateScript(
        R"js(queueMicrotask(() => { throw new Error("job"); }); throw new Error("script");)js", "both.js");
    CHECK(both.status == EvalStatus::Exception);
    CHECK(both.error.message == "script");
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[1].level == LogLevel::Error);
    CHECK(firstLine(records[0].message) == "script: both.js: uncaught Error: script");
    CHECK(firstLine(records[1].message) == "script: both.js: uncaught Error: job");
    expectCleanShutdown(vm);
}

TEST_CASE("script: queued jobs run before the call returns (VM31)") {
    const LogCapture log;
    Vm vm = makeVm();
    const EvalResult first = vm.evaluateScript(
        R"js(globalThis.order = []; Promise.resolve().then(() => order.push("job")); order.push("sync");)js",
        "order.js");
    CHECK(first.status == EvalStatus::Ok);
    // Evaluated before the second call's own drain: "sync" alone would mean the first call returned with its
    // job still queued.
    CHECK(vm.evaluateScript("order.join()", "order.js").completion == "sync,job");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a drain cut off by the budget leaves its job queued, deterministically (VM39)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(engine::script::MIN_CALL_TIME_BUDGET));
    // A warm-up whose result is not asserted: a fresh context's poll counter starts at zero, so its first poll
    // may consult the handler; this call spends that consultation, and the next ~10 000 polls consult nothing.
    static_cast<void>(vm.evaluateScript("0", "warm.js"));
    log.clear();
    // The script makes about five polls, far from the next consultation, so the handler cannot fire. The native
    // typed-array join never re-enters bytecode and formats a million elements, so the 1 ms deadline has always
    // passed when the drain begins: only the pre-job check can produce this outcome.
    const std::string_view source =
        "Promise.resolve().then(() => { globalThis.ran39 = true; }); new Int32Array(1000000).join().length";
    const EvalResult stop = vm.evaluateScript(source, "stop.js");
    CHECK(stop.status == EvalStatus::Interrupted);
    CHECK(stop.completion == "1999999");
    CHECK(stop.error.name.empty());
    CHECK(stop.error.stack.empty());
    CHECK(stop.error.message == "the time budget ran out with promise jobs still queued");
    const std::string_view record = "script: stop.js: interrupted after exceeding the 1 ms time budget";
    expectOnlyError(log, std::string{record} + " with promise jobs still queued");

    // A call evaluates its script before its own drain, so this proves the job never ran in the cut-off call.
    // The follow-up's own status and records are not asserted: its 1 ms drain may or may not finish.
    const EvalResult probe = vm.evaluateScript("typeof ran39", "stop.js");
    CHECK(probe.completion == "undefined");
    expectCleanShutdown(vm);
}

// ---- VM40-VM43: an exhausted budget stops every script, through the backend's promise paths too -------------

TEST_CASE("script: an interrupt quickjs turns into a rejection still ends the call Interrupted (VM40)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // The backend's Promise constructor takes the interrupt as an ordinary exception and rejects the promise
    // with it, after which the script would run on. The statement after it must NOT run: once the budget is
    // gone no new JavaScript call starts.
    const std::string_view source = "new Promise(() => { while (true) {} }); globalThis.after40 = String(42);";
    const EvalResult result = vm.evaluateScript(source, "exec.js");
    CHECK(result.status == EvalStatus::Interrupted);
    CHECK(result.error.name == "InternalError");
    CHECK(result.error.message == "interrupted");
    CHECK(result.completion.empty());  // `String(42)` was refused, so the script did not complete
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: exec.js: interrupted after exceeding the 100 ms time budget");
    CHECK(vm.stats().unhandledRejections == 0U);

    // The stack-limit witness: the `String` call never started.
    const EvalResult probe = vm.evaluateScript("typeof after40", "exec.js");
    CHECK(probe.status == EvalStatus::Ok);
    CHECK(probe.completion == "undefined");
    // The next call has its limit back.
    const EvalResult mapped = vm.evaluateScript("[1, 2].map((x) => x * 2).join()", "exec.js");
    CHECK(mapped.status == EvalStatus::Ok);
    CHECK(mapped.completion == "2,4");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a loop that swallows interrupts through promise executors still ends (VM41)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // Each consultation of the interrupt handler lands in an executor's inner loop, the constructor turns it
    // into a rejection, `.catch` handles it, and the outer loop goes on.
    const std::string_view source =
        "for (;;) { try { new Promise(() => { for (;;) {} }).catch(() => {}); } catch (e) {} }";
    const EvalResult result = vm.evaluateScript(source, "swallow.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: swallow.js: interrupted after exceeding the 100 ms time budget");
    const EvalResult after = vm.evaluateScript("1 + 1", "swallow.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: an interrupt inside Promise.try ends the call Interrupted (VM42)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    const EvalResult result =
        vm.evaluateScript("Promise.try(() => { for (;;) {} }); globalThis.after42 = String(1);", "try.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: try.js: interrupted after exceeding the 100 ms time budget");
    const EvalResult probe = vm.evaluateScript("typeof after42", "try.js");
    CHECK(probe.status == EvalStatus::Ok);
    CHECK(probe.completion == "undefined");
    expectCleanShutdown(vm);
}

TEST_CASE("script: an interrupt inside a thenable job run by the drain ends the call Interrupted (VM43)") {
    LogCapture log;
    // A one-second budget: the interrupt must happen INSIDE the drain's job, and a call whose start took longer
    // than a short budget would meet the drain's own pre-job stop instead.
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{1000}));
    log.clear();
    // The thenable job takes the interrupt as an ordinary exception and rejects the promise with it; the call
    // is Interrupted all the same, and the converted interrupt is not reported as an unhandled rejection.
    const EvalResult result = vm.evaluateScript("Promise.resolve({ then() { for (;;) {} } }); \"ok\"", "then.js");
    CHECK(result.status == EvalStatus::Interrupted);
    CHECK(result.completion == "ok");
    CHECK(result.error.message == "interrupted");
    CHECK(result.error.stack.empty());
    expectOnlyError(log, "script: then.js: interrupted after exceeding the 1000 ms time budget");
    CHECK(vm.stats().unhandledRejections == 0U);
    const EvalResult after = vm.evaluateScript("1 + 1", "then.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: an interrupt does not cost the lazily built builtins (VM45)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // Array.fromAsync is built from bytecode on its first read. A first read after the budget ran out used to
    // fail, leave the property undefined for good, and leak the objects holding the realm.
    const std::string_view spin = "new Promise(() => { for (;;) {} }); Array.fromAsync;";
    const EvalResult interrupted = vm.evaluateScript(spin, "lazy.js");
    CHECK(interrupted.status == EvalStatus::Interrupted);
    const std::string_view types = "[typeof Array.fromAsync, typeof Iterator.zip, typeof Iterator.zipKeyed].join()";
    const EvalResult probe = vm.evaluateScript(types, "lazy.js");
    CHECK(probe.status == EvalStatus::Ok);
    CHECK(probe.completion == "function,function,function");
    log.clear();
    expectCleanShutdown(vm);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // the teardown found nothing still referenced
}

TEST_CASE("script: nested catch-alls at the heap limit are still interrupted (VM44)") {
    LogCapture log;
    // The smallest heap fills long before the deadline on every lane.
    VmConfig config = withBudget(std::chrono::milliseconds{1000});
    config.heapLimitBytes = engine::script::MIN_HEAP_LIMIT_BYTES;
    Vm vm = makeVm(config);
    log.clear();
    // Every poll sits inside a try, so the interrupt is raised inside a catch-all at every depth. At the heap limit
    // the interrupt's own error object cannot be allocated, and the backend then throws a plain `null` -- which
    // cannot be marked uncatchable, and which the outer catch takes every time. Only the allocator's grace window,
    // opened each time the interrupt fires, lets the real error and its backtrace be built, and that error leaves
    // every try. (An interrupt thrown at a loop's back-edge outside any try would end the call even as `null`.)
    const std::string_view source =
        "(() => { let head = null; for (;;) { try { for (;;) { try { head = { next: head }; } catch (e) {} } } "
        "catch (e) {} } })()";
    const EvalResult result = vm.evaluateScript(source, "full.js");
    CHECK(result.status == EvalStatus::Interrupted);
    // The canonical InternalError and its backtrace were built at a full heap; a `null` has no stack at all.
    CHECK(result.error.stack.find("full.js:1:") != std::string::npos);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: full.js: interrupted after exceeding the 1000 ms time budget");
    const VmStats stats = vm.stats();
    CHECK(stats.heapBytesPeak <= stats.heapLimitBytes + 16 * KIB);  // the overshoot is bounded
    const EvalResult after = vm.evaluateScript("1 + 1", "full.js");
    CHECK(after.status == EvalStatus::Ok);
    CHECK(after.completion == "2");
    expectCleanShutdown(vm);
}

TEST_CASE("script: work refused after the budget ran out is not reported as rejections (VM47)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // Calling an async function has no stack check of its own: once the budget is gone its first resumption is
    // refused and the function returns a rejected promise. That rejection is the budget's doing, not the script's.
    const std::string_view source = "new Promise(() => { while (true) {} }); (async () => {})();";
    const EvalResult result = vm.evaluateScript(source, "async.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: async.js: interrupted after exceeding the 100 ms time budget");
    CHECK(vm.stats().unhandledRejections == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a budget that runs out after an earlier failure is still logged (VM48)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // The script fails first, so the call is an Exception; the thenable job the drain then runs exhausts the
    // budget, and the backend converts that interrupt. The call's status is the first failure's, but the
    // exhausted budget is still a record of its own.
    const std::string_view source = R"js(Promise.resolve({ then() { for (;;) {} } }); throw new Error("boom");)js";
    const EvalResult result = vm.evaluateScript(source, "boom.js");
    CHECK(result.status == EvalStatus::Exception);
    CHECK(result.error.message == "boom");
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[1].level == LogLevel::Error);
    CHECK(firstLine(records[0].message) == "script: boom.js: uncaught Error: boom");
    // By the prefix the drain's own cut-off record shares, should the call have started past its budget.
    const std::string_view prefix = "script: boom.js: interrupted after exceeding the 100 ms time budget";
    CHECK(firstLine(records[1].message).starts_with(prefix));
    expectCleanShutdown(vm);
}

TEST_CASE("script: an interrupt left over from an earlier call does not make a later call Interrupted (VM49)") {
    LogCapture log;
    // A one-second budget, so that the second call, which must not be interrupted, cannot be on a slow runner.
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{1000}));
    // The converted interrupt outlives its call as a promise's rejection reason.
    const EvalResult first = vm.evaluateScript("globalThis.p49 = new Promise(() => { for (;;) {} });", "left.js");
    CHECK(first.status == EvalStatus::Interrupted);

    // A later call's reaction job passes that object on. It is not THIS call's budget that ran out, so the call is
    // not Interrupted: the skipped rejection has made the object an ordinary error, the job's derived promise is
    // rejected with it, and nothing handles that promise -- a plain unhandled rejection, reported as one.
    log.clear();
    const EvalResult second = vm.evaluateScript("p49.then(() => {}); 1", "left.js");
    CHECK(second.status == EvalStatus::Ok);
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Info);
    REQUIRE(records.size() == 1U);
    CHECK(records[0].level == LogLevel::Error);
    const std::string_view expected = "script: left.js: unhandled promise rejection: InternalError: interrupted";
    CHECK(firstLine(records[0].message) == expected);
    // The promise the handler was attached to was never reported, so its handler is not "handled later".
    CHECK(vm.stats().rejectionsHandledLate == 0U);
    CHECK(vm.stats().unhandledRejections == 1U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: a stack that is not text reads as empty, and stack-trace hooks are inert (VM50)") {
    LogCapture log;
    Vm vm = makeVm();
    log.clear();
    // An Error whose stack is null (the backend builds such errors in some failure paths): no stack, not the text
    // "null".
    const EvalResult nul =
        vm.evaluateScript(R"js((() => { const e = new Error("x"); e.stack = null; throw e; })())js", "nul.js");
    CHECK(nul.status == EvalStatus::Exception);
    CHECK(nul.error.stack.empty());
    expectOnlyError(log, "script: nul.js: uncaught Error: x");

    // Error.prepareStackTrace and Error.stackTraceLimit are V8 extensions the VM removes: assigning them makes plain
    // properties that nothing reads, so a stack is always the backend's own text. (The backend's stackTraceLimit
    // setter overwrote its value without freeing the old one, which a Debug build reports as a leak at teardown:
    // the double assignment below is that witness.)
    const std::string_view hooked =
        R"js(Error.prepareStackTrace = () => "custom"; Error.stackTraceLimit = {}; Error.stackTraceLimit = 1; )js"
        R"js((() => { try { throw new Error("y"); } )js"
        R"js(catch (e) { return e.stack === "custom" ? "hooked" : "plain"; } })())js";
    const EvalResult plain = vm.evaluateScript(hooked, "hook.js");
    CHECK(plain.status == EvalStatus::Ok);
    CHECK(plain.completion == "plain");
    expectCleanShutdown(vm);
}

TEST_CASE("script: an exception storm whose stack-trace hooks spin is still interrupted (VM51)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // Every exception a script raises makes the backend build a backtrace, and a backtrace used to run the script's
    // Error.prepareStackTrace and Error.stackTraceLimit hooks and then throw away whatever exception was pending --
    // an interrupt raised inside a spinning hook included. Both hooks are removed when the VM is created, so
    // assigning them is inert and the storm below reaches the interrupt at its loop's back-edge.
    const std::string_view source = R"js(Error.prepareStackTrace = () => { for (;;) {} }; )js"
                                    R"js(Error.stackTraceLimit = { valueOf() { for (;;) {} } }; )js"
                                    R"js(for (;;) { try { null.x; } catch (e) {} })js";
    const EvalResult result = vm.evaluateScript(source, "hook.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: hook.js: interrupted after exceeding the 100 ms time budget");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a budget that runs out while a rejection is described is recorded (VM52)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    log.clear();
    // The script and its drain finish at once; the report then describes the rejection's reason through its own
    // toString, which spins until the budget runs out. That is a record of its own, written after the report.
    const EvalResult result = vm.evaluateScript("Promise.reject({ toString() { for (;;) {} } });", "r.js");
    CHECK(result.status == EvalStatus::Interrupted);
    const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
    REQUIRE(records.size() == 2U);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[1].level == LogLevel::Error);
    CHECK(records[0].message == "script: r.js: unhandled promise rejection: <unprintable value>");
    CHECK(records[1].message == "script: r.js: interrupted after exceeding the 100 ms time budget");
    expectCleanShutdown(vm);
}

TEST_CASE("script: a rejection skipped after the budget ran out is not \"handled later\" when handled (VM53)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    // The async function's refused first resumption becomes a rejected promise the report skips, because it is the
    // budget's doing. The script keeps that promise.
    const std::string_view source = "new Promise(() => { while (true) {} }); globalThis.q53 = (async () => {})();";
    CHECK(vm.evaluateScript(source, "skip.js").status == EvalStatus::Interrupted);

    // Handling it in a later call is not a late handling of anything that was ever reported.
    log.clear();
    const EvalResult later = vm.evaluateScript("q53.catch(() => {}); 1", "skip.js");
    CHECK(later.status == EvalStatus::Ok);
    CHECK(log.countAtLeast(LogLevel::Info) == 0U);
    CHECK(vm.stats().rejectionsHandledLate == 0U);
    CHECK(vm.stats().unhandledRejections == 0U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: an interrupt object kept from an earlier call is an ordinary error later (VM54)") {
    LogCapture log;
    Vm vm = makeVm(withBudget(std::chrono::milliseconds{100}));
    // The converted interrupt becomes a rejected promise's reason, and the script keeps the promise.
    const std::string_view spin = "globalThis.p54 = new Promise(() => { for (;;) {} });";
    CHECK(vm.evaluateScript(spin, "keep.js").status == EvalStatus::Interrupted);

    // A later call takes the reason out of it...
    const std::string_view take = "p54.catch((e) => { globalThis.left54 = e; }); 1";
    CHECK(vm.evaluateScript(take, "keep.js").status == EvalStatus::Ok);

    // ...and it is an ordinary error now: a catch takes it, where the uncatchable flag would have ended the call.
    const std::string_view thrown = R"js((() => { try { throw left54; } catch (e) { return "caught"; } })())js";
    const EvalResult caught = vm.evaluateScript(thrown, "keep.js");
    CHECK(caught.status == EvalStatus::Ok);
    CHECK(caught.completion == "caught");

    // A genuine rejection with that object as its reason is reported like any other.
    log.clear();
    const EvalResult again = vm.evaluateScript("Promise.reject(left54); 1", "again.js");
    CHECK(again.status == EvalStatus::Ok);
    const std::string message = onlyErrorMessage(log);
    CHECK(firstLine(message) == "script: again.js: unhandled promise rejection: InternalError: interrupted");
    CHECK(vm.stats().unhandledRejections == 1U);
    expectCleanShutdown(vm);
}

TEST_CASE("script: the promises kept for skipped rejections stay bounded across many interrupted calls (VM55)") {
    const LogCapture log;
    // 1 ms budget, default heap: each call spins a Promise executor until the budget runs out, and the promise the
    // backend rejects with the interrupt is one the report skips and keeps. Nothing but the cap bounds that list.
    Vm vm = makeVm(withBudget(engine::script::MIN_CALL_TIME_BUDGET));
    const auto interruptedCalls = [&vm](int count) {
        for (int i = 0; i < count; ++i) {
            const EvalResult result = vm.evaluateScript("new Promise(() => { for (;;) {} }); 1", "keep.js");
            REQUIRE(result.status == EvalStatus::Interrupted);
        }
    };
    interruptedCalls(300);
    vm.collectGarbage();
    const std::size_t before = vm.stats().heapBytesInUse;
    interruptedCalls(300);
    vm.collectGarbage();
    const std::size_t after = vm.stats().heapBytesInUse;
    MESSAGE("heap after 300 interrupted calls: " << before << " bytes; after 300 more: " << after << " bytes");
    // With the cap the list stays at 256 and each eviction frees a promise and its reason. Without it, 300 more
    // entries of about 1 KB each would add roughly 300 KiB.
    CHECK(after < before + 64 * KIB);
    expectCleanShutdown(vm);
}

// ---- VM32: shutdown ---------------------------------------------------------------------------------------

TEST_CASE("script: shutdown() reports zero leaks after a session through every arm, and is idempotent (VM32)") {
    LogCapture log;
    {
        Vm vm = makeVm();
        const VmTeardownReport first = vm.shutdown();
        CHECK(first.leakedAllocations == 0U);
        CHECK(first.leakedBytes == 0U);
        CHECK_FALSE(vm.valid());

        log.clear();
        const VmTeardownReport second = vm.shutdown();
        CHECK(second.leakedAllocations == 0U);
        CHECK(second.leakedBytes == 0U);
        CHECK(log.countAtLeast(LogLevel::Trace) == 0U);

        {
            Vm shutDown = makeVm();
            static_cast<void>(shutDown.shutdown());
            log.clear();
        }  // the destructor of a VM that already shut down is a no-op
        CHECK(log.countAtLeast(LogLevel::Trace) == 0U);
    }

    // A session through every arm that can leave something behind, on one VM...
    VmConfig config;
    config.heapLimitBytes = 8 * MIB;
    Vm vm = makeVm(patient(config));
    const auto status = [&vm](std::string_view source) { return vm.evaluateScript(source, "s32.js").status; };
    CHECK(status("1 + 1") == EvalStatus::Ok);
    CHECK(status("let =") == EvalStatus::CompileError);
    CHECK(status("throw new Error(\"e\")") == EvalStatus::Exception);
    CHECK(status("throw 7") == EvalStatus::Exception);
    CHECK(status("(() => { \"x\".repeat(64 * 1024 * 1024); })()") == EvalStatus::OutOfMemory);
    CHECK(status("(() => { function r() { return r() + 1; } r(); })()") == EvalStatus::Exception);
    log.clear();
    CHECK(status("globalThis.p32 = Promise.reject(new Error(\"u\"));") == EvalStatus::Ok);
    CHECK(log.countExactly(LogLevel::Error) == 1U);
    log.clear();
    CHECK(status("p32.catch(() => {});") == EvalStatus::Ok);
    CHECK(log.countExactly(LogLevel::Info) == 1U);
    CHECK(status("queueMicrotask(() => { throw 1; })") == EvalStatus::Exception);

    // ...and the two interrupting arms, on a second VM with a short budget.
    Vm quick = makeVm(withBudget(std::chrono::milliseconds{100}));
    const auto quickStatus = [&quick](std::string_view source) {
        return quick.evaluateScript(source, "q32.js").status;
    };
    CHECK(quickStatus("while (true) {}") == EvalStatus::Interrupted);
    // Two endless chains, whichever record shape stops them: one is still queued at teardown.
    CHECK(quickStatus("function f() { Promise.resolve().then(f); } f(); f();") == EvalStatus::Interrupted);

    log.clear();
    const VmTeardownReport sessionReport = vm.shutdown();
    CHECK(sessionReport.leakedAllocations == 0U);
    CHECK(sessionReport.leakedBytes == 0U);
    const VmTeardownReport quickReport = quick.shutdown();  // with a job chain still queued
    CHECK(quickReport.leakedAllocations == 0U);
    CHECK(quickReport.leakedBytes == 0U);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
}

// ---- VM35: the four setters the design rules out -------------------------------------------------------

TEST_CASE("script: the script layer never calls the four backend setters its design rules out (VM35)") {
    namespace fs = std::filesystem;
    const auto strippedSource = [](const fs::path& path) {
        std::ifstream file{path};
        std::string out;
        std::string line;
        while (std::getline(file, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) {
                line.erase(comment);
            }
            out += line;
            out += '\n';
        }
        return out;
    };

    // A DERIVED set: every .cpp and .hpp under the script layer's sources, never an enumeration.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator{AERO_SCRIPT_SRC_DIR}) {
        if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") {
            files.push_back(entry.path());
        }
    }
    REQUIRE_FALSE(files.empty());

    bool sawVmCpp = false;
    bool sawRuntimeCreation = false;
    for (const fs::path& file : files) {
        const std::string text = strippedSource(file);
        INFO("file: " << file.filename().string());
        // Why each is banned: the allocator owns the heap limit, blocking would stall the engine thread,
        // the dump flags print past the engine log and free leaked strings the allocator must see, and a GC
        // knob without a measurement is a guess.
        CHECK(text.find("JS_SetMemoryLimit(") == std::string::npos);
        CHECK(text.find("JS_SetCanBlock(") == std::string::npos);
        CHECK(text.find("JS_SetDumpFlags(") == std::string::npos);
        CHECK(text.find("JS_SetGCThreshold(") == std::string::npos);
        sawVmCpp = sawVmCpp || file.filename() == "vm.cpp";
        sawRuntimeCreation = sawRuntimeCreation || text.find("JS_NewRuntime2(") != std::string::npos;
    }
    // Anti-vacuity: the scan reached vm.cpp, and the same stripped scan DOES find a real backend call.
    CHECK(sawVmCpp);
    CHECK(sawRuntimeCreation);
}

// ---- VM36-VM38: the allocator's contract, white-box -------------------------------------------------------

TEST_CASE("script: the allocator charges every block its 16-byte header and refuses past the limit (VM36)") {
    using engine::script::internal::HEAP_HEADER_BYTES;
    using engine::script::internal::HeapAccounting;
    CHECK(HEAP_HEADER_BYTES == 16U);

    HeapAccounting heap{1024};
    void* block = heap.allocate(1024 - HEAP_HEADER_BYTES);
    REQUIRE(block != nullptr);
    CHECK(heap.bytesInUse() == 1024U);
    CHECK(heap.liveBlocks() == 1U);
    CHECK(HeapAccounting::requestedSize(block) == 1008U);

    void* refused = heap.allocate(1);
    CHECK(refused == nullptr);
    CHECK(heap.refusals() == 1U);
    CHECK(heap.bytesInUse() == 1024U);
    CHECK(heap.liveBlocks() == 1U);

    heap.release(block);
    CHECK(heap.bytesInUse() == 0U);
    CHECK(heap.liveBlocks() == 0U);
    CHECK(heap.peakBytes() == 1024U);

    // Only the header does not fit.
    HeapAccounting tight{1024};
    void* headerless = tight.allocate(1024 - HEAP_HEADER_BYTES + 1);
    CHECK(headerless == nullptr);
    CHECK(tight.refusals() == 1U);
    CHECK(tight.bytesInUse() == 0U);

    CHECK(HeapAccounting::requestedSize(nullptr) == 0U);
    tight.release(nullptr);
    CHECK(tight.bytesInUse() == 0U);
    CHECK(tight.liveBlocks() == 0U);
}

TEST_CASE("script: a refused growth leaves the block intact and charged; an admitted one moves the count (VM37)") {
    using engine::script::internal::HeapAccounting;
    HeapAccounting heap{200};
    void* block = heap.allocate(100);  // 116 in use, 84 left
    REQUIRE(block != nullptr);
    for (std::size_t i = 0; i < 100; ++i) {
        static_cast<unsigned char*>(block)[i] = static_cast<unsigned char>(i);
    }
    const auto intact = [](const void* data) {
        for (std::size_t i = 0; i < 100; ++i) {
            if (static_cast<const unsigned char*>(data)[i] != static_cast<unsigned char>(i)) {
                return false;
            }
        }
        return true;
    };

    // Growth of 90 exceeds the 84 left: refused, and C realloc semantics keep the old block valid and charged.
    void* refused = heap.reallocate(block, 190);
    CHECK(refused == nullptr);
    CHECK(heap.refusals() == 1U);
    CHECK(heap.bytesInUse() == 116U);
    CHECK(HeapAccounting::requestedSize(block) == 100U);
    CHECK(intact(block));

    void* grown = heap.reallocate(block, 150);
    REQUIRE(grown != nullptr);
    CHECK(heap.bytesInUse() == 166U);
    CHECK(heap.peakBytes() == 166U);  // the peak a growth reaches, read before any later allocation can recompute it
    CHECK(HeapAccounting::requestedSize(grown) == 150U);
    CHECK(intact(grown));

    void* shrunk = heap.reallocate(grown, 10);
    REQUIRE(shrunk != nullptr);
    CHECK(heap.bytesInUse() == 26U);

    heap.release(shrunk);
    CHECK(heap.bytesInUse() == 0U);
    CHECK(heap.liveBlocks() == 0U);
}

TEST_CASE("script: allocateZeroed zero-fills and refuses an overflowing count * size (VM38)") {
    using engine::script::internal::HeapAccounting;
    HeapAccounting heap{MIB};
    void* zeroed = heap.allocateZeroed(4, 64);
    REQUIRE(zeroed != nullptr);
    CHECK(HeapAccounting::requestedSize(zeroed) == 256U);
    CHECK(heap.bytesInUse() == 272U);
    bool allZero = true;
    for (std::size_t i = 0; i < 256; ++i) {
        allZero = allZero && static_cast<const unsigned char*>(zeroed)[i] == 0U;
    }
    CHECK(allZero);

    void* overflowing = heap.allocateZeroed(std::numeric_limits<std::size_t>::max() / 2U, 4);
    CHECK(overflowing == nullptr);
    CHECK(heap.refusals() == 1U);
    CHECK(heap.bytesInUse() == 272U);

    // The product above wraps to 2^64 - 4, which the limit refuses anyway. This one wraps to a SMALL number,
    // (2^63 + 1) * 2 = 2^64 + 2 -> 2, which the limit would admit: only the overflow check can refuse it.
    void* wrapsSmall = heap.allocateZeroed((std::numeric_limits<std::size_t>::max() / 2U) + 2U, 2U);
    CHECK(wrapsSmall == nullptr);
    CHECK(heap.refusals() == 2U);
    CHECK(heap.bytesInUse() == 272U);

    heap.release(zeroed);
    CHECK(heap.bytesInUse() == 0U);
    CHECK(heap.liveBlocks() == 0U);
}

TEST_CASE("script: the allocator's grace window admits past the limit, and only what it was given (VM46)") {
    using engine::script::internal::HEAP_HEADER_BYTES;
    using engine::script::internal::HeapAccounting;
    HeapAccounting heap{1024};

    // 1. Fill it to the limit; one more request is refused.
    void* full = heap.allocate(1024 - HEAP_HEADER_BYTES);
    REQUIRE(full != nullptr);
    void* refused = heap.allocate(16);
    CHECK(refused == nullptr);
    CHECK(heap.refusals() == 1U);

    // 2. A window of two decisions and 256 bytes: two admissions past the limit, then the limit again. After two
    // admissions 160 bytes of room remain, so only the spent count can refuse the third request.
    heap.openGraceWindow(2, 256);
    void* first = heap.allocate(32);
    REQUIRE(first != nullptr);
    CHECK(heap.bytesInUse() == 1072U);
    void* second = heap.allocate(32);
    REQUIRE(second != nullptr);
    CHECK(heap.bytesInUse() == 1120U);
    CHECK(heap.bytesInUse() > heap.limitBytes());
    void* third = heap.allocate(32);
    CHECK(third == nullptr);  // the window's two decisions are spent
    CHECK(heap.refusals() == 2U);
    CHECK(heap.bytesInUse() == 1120U);

    // 3. A window admits only what it was given: 32 bytes plus the 16-byte header exceed 40 bytes of grace.
    heap.openGraceWindow(4, 40);
    void* tooBig = heap.allocate(32);
    CHECK(tooBig == nullptr);
    CHECK(heap.refusals() == 3U);
    heap.closeGraceWindow();

    // 4. A growing reallocate is admitted inside a window; the same growth after closeGraceWindow() is refused,
    // and the block stays intact and charged.
    heap.openGraceWindow(8, 256);
    void* block = heap.allocate(32);
    REQUIRE(block != nullptr);
    CHECK(heap.bytesInUse() == 1168U);
    void* grown = heap.reallocate(block, 96);
    REQUIRE(grown != nullptr);
    CHECK(HeapAccounting::requestedSize(grown) == 96U);
    CHECK(heap.bytesInUse() == 1232U);
    heap.closeGraceWindow();
    void* denied = heap.reallocate(grown, 160);
    CHECK(denied == nullptr);
    CHECK(heap.refusals() == 4U);
    CHECK(HeapAccounting::requestedSize(grown) == 96U);
    CHECK(heap.bytesInUse() == 1232U);

    // 5. Everything comes back.
    heap.release(full);
    heap.release(first);
    heap.release(second);
    heap.release(grown);
    CHECK(heap.bytesInUse() == 0U);
    CHECK(heap.liveBlocks() == 0U);
}
