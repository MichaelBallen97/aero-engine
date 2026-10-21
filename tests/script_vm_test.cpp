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
std::ostream& operator<<(std::ostream& out, VmCreateStatus status) { return out << vmCreateStatusLabel(status); }
}  // namespace engine::script

namespace {

using engine::LogLevel;
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

    constexpr std::string_view HEAP_MESSAGE = "script: Vm::create refused: heapLimitBytes must be greater than 0";
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

TEST_CASE("script: a heap limit below an empty VM's footprint fails create() with OutOfMemory, cleanly (VM4)") {
    LogCapture log;
    VmConfig config;
    config.heapLimitBytes = 1;
    {
        const VmCreateResult result = Vm::create(config);
        CHECK(result.status == VmCreateStatus::OutOfMemory);
        CHECK_FALSE(result.vm.valid());
        // The runtime's own allocation is the first request. One ERROR is the leak witness too: a block
        // surviving the failed create would add the teardown ERROR.
        expectOnlyError(log, "script: Vm::create failed: out of memory creating the runtime (heap limit 1 bytes)");
    }
    log.clear();
    config.heapLimitBytes = 64 * KIB;
    {
        const VmCreateResult result = Vm::create(config);
        CHECK(result.status == VmCreateStatus::OutOfMemory);
        CHECK_FALSE(result.vm.valid());
        // Which step fails at 64 KiB is a backend size, not asserted.
        const std::vector<CapturedRecord> records = log.atLeast(LogLevel::Warn);
        REQUIRE(records.size() == 1U);
        CHECK(records[0].level == LogLevel::Error);
        CHECK(records[0].message.starts_with("script: Vm::create failed: out of memory creating the "));
        CHECK(records[0].message.ends_with("(heap limit 65536 bytes)"));
    }
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
    expectCleanShutdown(c);
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
    expectCleanShutdown(vm);
}

// ---- VM32: shutdown ---------------------------------------------------------------------------------------

TEST_CASE("script: shutdown() reports zero leaks after a session through every arm, and is idempotent (VM32)") {
    LogCapture log;
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

    heap.release(zeroed);
    CHECK(heap.bytesInUse() == 0U);
    CHECK(heap.liveBlocks() == 0U);
}
