// Aero Engine -- engine::script::Vm (task 4.1.1): create, teardown, and the thread check.

#include <aero/core/log.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace engine::script {
namespace {

// The five allocator trampolines (§C.9). Each is one line, and each is noexcept: the backend is C, and an
// exception unwinding through its frames is undefined. The opaque pointer is the VM's HeapAccounting, which
// is heap-pinned inside Vm::Impl for the runtime's whole life.
[[nodiscard]] internal::HeapAccounting& heapOf(void* opaque) noexcept {
    return *static_cast<internal::HeapAccounting*>(opaque);
}

void* heapCalloc(void* opaque, std::size_t count, std::size_t size) noexcept {
    return heapOf(opaque).allocateZeroed(count, size);
}

void* heapMalloc(void* opaque, std::size_t size) noexcept { return heapOf(opaque).allocate(size); }

void heapFree(void* opaque, void* block) noexcept { heapOf(opaque).release(block); }

void* heapRealloc(void* opaque, void* block, std::size_t size) noexcept {
    return heapOf(opaque).reallocate(block, size);
}

// The backend's usable-size hook takes no context argument, which is why every block carries its own size.
std::size_t heapUsableSize(const void* block) noexcept { return internal::HeapAccounting::requestedSize(block); }

constexpr JSMallocFunctions HEAP_FUNCTIONS{&heapCalloc, &heapMalloc, &heapFree, &heapRealloc, &heapUsableSize};

// The first failing field decides (heap, then stack, then budget) and is named in the one ERROR record.
[[nodiscard]] VmCreateStatus validate(const VmConfig& config) {
    if (config.heapLimitBytes == 0) {
        AERO_LOG_ERROR("script: Vm::create refused: heapLimitBytes must be greater than 0");
        return VmCreateStatus::InvalidHeapLimit;
    }
    if (config.stackLimitBytes < MIN_STACK_LIMIT_BYTES || config.stackLimitBytes > MAX_STACK_LIMIT_BYTES) {
        AERO_LOG_ERROR("script: Vm::create refused: stackLimitBytes {} is outside [{}, {}]", config.stackLimitBytes,
                       MIN_STACK_LIMIT_BYTES, MAX_STACK_LIMIT_BYTES);
        return VmCreateStatus::InvalidStackLimit;
    }
    if (config.callTimeBudget < MIN_CALL_TIME_BUDGET || config.callTimeBudget > MAX_CALL_TIME_BUDGET) {
        AERO_LOG_ERROR("script: Vm::create refused: callTimeBudget {} ms is outside [{}, {}] ms",
                       config.callTimeBudget.count(), MIN_CALL_TIME_BUDGET.count(), MAX_CALL_TIME_BUDGET.count());
        return VmCreateStatus::InvalidTimeBudget;
    }
    return VmCreateStatus::Ok;
}

}  // namespace

// ---- Impl -----------------------------------------------------------------------------------------------

Vm::Impl::Impl(const VmConfig& vmConfig) noexcept
    : heap(vmConfig.heapLimitBytes), config(vmConfig), vmThread(std::this_thread::get_id()) {}

Vm::Impl::~Impl() {
    static_cast<void>(shutdown());  // a no-op when the Vm already shut it down
}

VmTeardownReport Vm::Impl::shutdown() noexcept {
    if (finished) {
        return {};
    }
    finished = true;
    if (ctx != nullptr) {
        JS_FreeContext(ctx);  // a reference drop: functions still referenced by queued jobs hold their realm alive
        ctx = nullptr;
        // Queued jobs legitimately keep objects alive until the runtime frees them, so the check below is
        // skipped (rather than made to lie) while any are queued. Otherwise it gives an assert-enabled
        // build's abort in JS_FreeRuntime a readable cause one statement earlier.
        if (!JS_IsJobPending(rt)) {
            JS_RunGC(rt);
            JSMemoryUsage usage{};
            JS_ComputeMemoryUsage(rt, &usage);
            if (usage.obj_count != 0) {
                AERO_LOG_ERROR(
                    "script: VM teardown: {} JavaScript objects are still referenced after the context was freed "
                    "-- a held reference leaked; an assert-enabled build aborts in JS_FreeRuntime next",
                    usage.obj_count);
            }
        }
    }
    if (rt != nullptr) {
        JS_FreeRuntime(rt);  // frees queued jobs, collects, asserts (Debug), then frees the runtime itself
        rt = nullptr;
    }
    // The runtime's last act was returning its own block, so on a leak-free run both counts are zero.
    const VmTeardownReport report{heap.liveBlocks(), heap.bytesInUse()};
    if (report.leakedAllocations != 0) {
        AERO_LOG_ERROR("script: VM teardown: {} allocations ({} bytes) were never returned to the heap",
                       report.leakedAllocations, report.leakedBytes);
    }
    return report;
}

// ---- Vm -------------------------------------------------------------------------------------------------

VmCreateResult Vm::create(const VmConfig& config) {
    if (const VmCreateStatus refusal = validate(config); refusal != VmCreateStatus::Ok) {
        return VmCreateResult{refusal, Vm{}};  // validate() logged the field and its value
    }
    auto impl = std::make_unique<Impl>(config);
    impl->rt = JS_NewRuntime2(&HEAP_FUNCTIONS, &impl->heap);
    if (impl->rt == nullptr) {
        AERO_LOG_ERROR("script: Vm::create failed: out of memory creating the runtime (heap limit {} bytes)",
                       config.heapLimitBytes);
        return VmCreateResult{VmCreateStatus::OutOfMemory, Vm{}};  // ~Impl: shutdown() checks the heap is empty
    }
    JS_SetMaxStackSize(impl->rt, config.stackLimitBytes);
    impl->ctx = JS_NewContext(impl->rt);
    if (impl->ctx == nullptr) {
        AERO_LOG_ERROR("script: Vm::create failed: out of memory creating the context (heap limit {} bytes)",
                       config.heapLimitBytes);
        return VmCreateResult{VmCreateStatus::OutOfMemory, Vm{}};  // ~Impl: shutdown() frees the runtime
    }
    AERO_LOG_DEBUG(
        "script: VM created: quickjs-ng {}, heap limit {} bytes, stack limit {} bytes, call budget {} ms, "
        "{} bytes in use",
        JS_GetVersion(), config.heapLimitBytes, config.stackLimitBytes, config.callTimeBudget.count(),
        impl->heap.bytesInUse());
    return VmCreateResult{VmCreateStatus::Ok, Vm{std::move(impl)}};
}

Vm::Vm() noexcept = default;
Vm::Vm(Vm&& other) noexcept = default;
Vm& Vm::operator=(Vm&& other) noexcept = default;
Vm::~Vm() = default;

Vm::Vm(std::unique_ptr<Impl> owned) noexcept : impl(std::move(owned)) {}

bool Vm::valid() const noexcept { return impl != nullptr; }

void Vm::collectGarbage() noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: collectGarbage on an invalid Vm (a failed create or a moved-from Vm)");
        return;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: collectGarbage called from a thread that is not the VM's; refused");
        return;
    }
    JS_RunGC(impl->rt);
}

VmStats Vm::stats() const noexcept {
    if (impl == nullptr) {
        return VmStats{};
    }
    assert(impl->onVmThread());
    const internal::HeapAccounting& heap = impl->heap;
    VmStats out;
    out.heapLimitBytes = heap.limitBytes();
    out.heapBytesInUse = heap.bytesInUse();
    out.heapBytesPeak = heap.peakBytes();
    out.liveAllocations = heap.liveBlocks();
    out.refusedAllocations = heap.refusals();
    return out;  // the two rejection counters read 0 until promise jobs are drained and reported
}

VmTeardownReport Vm::shutdown() noexcept {
    if (impl == nullptr) {
        return {};
    }
    const VmTeardownReport report = impl->shutdown();
    impl.reset();
    return report;
}

// ---- labels and the version -----------------------------------------------------------------------------

std::string_view vmCreateStatusLabel(VmCreateStatus status) noexcept {
    switch (status) {
        case VmCreateStatus::Ok:
            return "Ok";
        case VmCreateStatus::InvalidHeapLimit:
            return "InvalidHeapLimit";
        case VmCreateStatus::InvalidStackLimit:
            return "InvalidStackLimit";
        case VmCreateStatus::InvalidTimeBudget:
            return "InvalidTimeBudget";
        case VmCreateStatus::OutOfMemory:
            return "OutOfMemory";
    }
    return "Unknown";
}

std::string_view scriptBackendVersion() noexcept { return JS_GetVersion(); }

}  // namespace engine::script
