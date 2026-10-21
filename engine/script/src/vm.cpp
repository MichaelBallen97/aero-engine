// Aero Engine -- engine::script::Vm (task 4.1.1): create, teardown, the thread check and the entry point.

#include <aero/core/log.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    // A promise rejected while the last call's report ran is still listed, so this is required, not defensive.
    for (const PendingRejection& entry : pendingRejections) {
        JS_FreeValue(ctx, entry.promise);
        JS_FreeValue(ctx, entry.reason);
    }
    pendingRejections.clear();
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

// ---- one call -------------------------------------------------------------------------------------------

// The whole of the time budget's mechanism: a clock read, a comparison and, once, the stack-limit line below. No
// allocation, no lock, no log. The backend consults it on the first poll of a context and then every 10 000
// polls (backward jumps and calls), and its regex engine on a counter of its own.
int Vm::Impl::interruptHandler(JSRuntime* runtime, void* opaque) noexcept {
    Impl& self = *static_cast<Impl*>(opaque);
    if (std::chrono::steady_clock::now() < self.deadline) {
        return 0;
    }
    if (!self.interruptFired) {
        self.interruptFired = true;
        // The backend turns the interrupt into an ordinary promise rejection on several C paths (the Promise
        // executor, Promise.try, a thenable job, ...), after which the script would run on. With the stack limit
        // exhausted, every bytecode frame and every native call refuses to start (quickjs.c:17640, :17370,
        // :17494), so no new JavaScript runs once the budget is gone: the frames already running reach their next
        // poll and are interrupted again. ~CallScope restores the limit. 1, never 0: a size of 0 DISABLES the
        // check (quickjs.c:2763-2764).
        JS_SetMaxStackSize(runtime, 1);
    }
    return 1;
}

// A rejection with no handler yet is only LISTED: `Promise.reject(x).catch(f)` written in one script rejects
// before f is attached, so reporting at this moment would be a false alarm. The list is reported at the end of
// the call, after the job drain. A handler arriving for a listed promise removes it silently; one arriving for
// a promise that is no longer listed means the end-of-call report already named it.
void Vm::Impl::rejectionTracker(JSContext* context, JSValueConst promise, JSValueConst reason, bool isHandled,
                                void* opaque) noexcept {
    Impl& self = *static_cast<Impl*>(opaque);
    // The tracker allocates (the list, the log record). A failure there is the engine's standing answer to C++
    // heap exhaustion -- terminate -- spelled out, because nothing may unwind through the backend's C frames.
    try {
        if (!isHandled) {
            self.pendingRejections.push_back(
                PendingRejection{JS_DupValue(context, promise), JS_DupValue(context, reason)});
            return;
        }
        for (auto entry = self.pendingRejections.begin(); entry != self.pendingRejections.end(); ++entry) {
            if (JS_IsStrictEqual(context, entry->promise, promise)) {  // identity for objects: runs no script code
                JS_FreeValue(context, entry->promise);
                JS_FreeValue(context, entry->reason);
                self.pendingRejections.erase(entry);
                return;
            }
        }
        ++self.rejectionsHandledLate;
        AERO_LOG_INFO("script: {}: a promise rejection reported as unhandled was handled later", self.callFileName);
    } catch (...) {
        std::terminate();
    }
}

Vm::Impl::CallScope::CallScope(Impl& host, const std::string& fileName) : impl(host) {
    host.deadline = std::chrono::steady_clock::now() + host.config.callTimeBudget;
    // The backend captured its stack top once, where the runtime was created. A VM created deep inside one
    // call and entered later from a shallow one would otherwise get the difference ADDED to its limit.
    JS_UpdateStackTop(host.rt);
    host.refusalsAtCallStart = host.heap.refusals();
    host.callFileName = fileName;
    host.interruptFired = false;
    host.inCall = true;
}

Vm::Impl::CallScope::~CallScope() {
    JS_SetMaxStackSize(impl.rt, impl.config.stackLimitBytes);  // the handler took it away if the budget ran out
    impl.deadline = std::chrono::steady_clock::time_point::min();
    impl.inCall = false;
}

EvalResult Vm::Impl::evaluate(std::string_view source, std::string_view fileName) {
    assert(!inCall);                     // no host function exists, so nothing can re-enter the VM
    const std::string text{source};      // the backend reads text[text.size()] (quickjs.h:1012)
    const std::string name{fileName};    // and the file name is a C string
    const CallScope scope{*this, name};  // deadline, stack top, refusal mark, inCall
    EvalResult result;
    HeldValue compiled{ctx, JS_Eval(ctx, text.c_str(), text.size(), name.c_str(),
                                    JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_COMPILE_ONLY)};
    if (compiled.isException()) {
        result = trap(Phase::Compile);
    } else {
        // JS_EvalFunction CONSUMES its argument: the one release() in the layer.
        const HeldValue completion{ctx, JS_EvalFunction(ctx, compiled.release())};
        if (completion.isException()) {
            result = trap(Phase::Run);
        } else {
            result.status = EvalStatus::Ok;
            result.completion = stringify(completion.get());  // a failed rendering never changes the status
        }
    }
    if (result.status != EvalStatus::Interrupted && !interruptFired) {
        drainJobs(result);  // the microtask checkpoint, under the SAME deadline
    }
    noteInterrupt(result);  // BEFORE the report: a converted interrupt is the call's, not a rejection's
    reportUnhandledRejections();
    return result;  // ~CallScope: deadline = time_point::min(); inCall = false
}

// The budget ran out, but the backend converted the interrupt into a rejection (or swallowed it) and the call
// came back looking fine: it is Interrupted all the same. The converted interrupt has no stack of its own.
void Vm::Impl::noteInterrupt(EvalResult& result) {
    if (!interruptFired || result.status != EvalStatus::Ok) {
        return;
    }
    const ScriptError error{"InternalError", "interrupted", {}};
    logFailure(EvalStatus::Interrupted, error, /*isErrorObject=*/true);
    adopt(result, EvalResult{EvalStatus::Interrupted, error, {}});
}

// The first failure of a call decides its status (the script before its jobs), and the completion value is
// never cleared: a script that completed and whose microtask then threw is an Exception that still has it.
void Vm::Impl::adopt(EvalResult& result, const EvalResult& failure) {
    if (result.status == EvalStatus::Ok) {
        result.status = failure.status;
        result.error = failure.error;
    }
}

// The pre-job deadline check stops an endless job chain cleanly between two jobs. It races with the interrupt
// handler for the moment the deadline passes (every job polls the handler at each call entry), so a chain may
// instead be stopped inside a job; both end the call Interrupted.
void Vm::Impl::drainJobs(EvalResult& result) {
    while (JS_IsJobPending(rt)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            logDrainStop();
            adopt(result,
                  EvalResult{EvalStatus::Interrupted, ScriptError{{}, std::string{DRAIN_STOP_MESSAGE}, {}}, {}});
            return;  // the jobs stay queued for the next call
        }
        JSContext* jobContext = nullptr;
        if (JS_ExecutePendingJob(rt, &jobContext) < 0) {
            const EvalResult failure = trap(Phase::Run);  // a throw from queueMicrotask or a finalization callback
            adopt(result, failure);
            if (failure.status == EvalStatus::Interrupted) {
                return;
            }
        }
    }
}

// The list is swapped out BEFORE anything is described: describe may run script code (a getter) that rejects
// another promise, and the tracker would then append to the vector being iterated. Anything rejected while
// reporting is reported at the end of the next call.
void Vm::Impl::reportUnhandledRejections() {
    const std::vector<PendingRejection> batch = std::exchange(pendingRejections, {});
    for (const PendingRejection& entry : batch) {
        // The interrupt itself, rejected into a promise by the backend: the call reports it as Interrupted.
        if (!JS_IsUncatchableError(entry.reason)) {
            const bool isErrorObject = JS_IsError(entry.reason);
            const ScriptError reason = describe(entry.reason, isErrorObject);
            logUnhandledRejection(reason, isErrorObject);
            ++unhandledRejections;
        }
        JS_FreeValue(ctx, entry.promise);
        JS_FreeValue(ctx, entry.reason);
    }
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
    // Installed only now, so creation never depends on the deadline, whatever the backend does while it builds
    // the intrinsics.
    JS_SetInterruptHandler(impl->rt, &Impl::interruptHandler, impl.get());
    JS_SetHostPromiseRejectionTracker(impl->rt, &Impl::rejectionTracker, impl.get());
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

EvalResult Vm::evaluateScript(std::string_view source, std::string_view fileName) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: evaluateScript on an invalid Vm (a failed create or a moved-from Vm)");
        return EvalResult{EvalStatus::InvalidVm, {}, {}};
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: evaluateScript called from a thread that is not the VM's; refused");
        return EvalResult{EvalStatus::WrongThread, {}, {}};
    }
    return impl->evaluate(source, fileName);
}

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
    out.unhandledRejections = impl->unhandledRejections;
    out.rejectionsHandledLate = impl->rejectionsHandledLate;
    return out;
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
