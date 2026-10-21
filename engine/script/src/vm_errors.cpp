// Aero Engine -- engine::script::Vm (task 4.1.1): the trapping half. A pending backend exception is taken,
// classified, described and logged here, once, in one fixed shape per arm.

#include <aero/core/log.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <cassert>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace engine::script {

EvalResult Vm::Impl::trap(Phase phase) {
    const HeldValue exception{ctx, JS_GetException(ctx)};        // takes the reference, clears the slot
    const EvalStatus status = classify(exception.get(), phase);  // BEFORE describe: describe may allocate
    const bool isErrorObject = JS_IsError(exception.get());
    ScriptError error = describe(exception.get(), isErrorObject);
    if (status == EvalStatus::Interrupted) {
        // Whichever exception carried it -- the interrupt itself, or the RangeError a refused call raised after
        // the budget ran out -- the canonical interrupt is what the caller reads. The stack stays the described one.
        error.name = "InternalError";
        error.message = "interrupted";
    }
    logFailure(status, error, isErrorObject);
    if (status == EvalStatus::OutOfMemory) {
        JS_RunGC(rt);  // the failed work is unreachable now; cycles among it need the collector
    }
    return EvalResult{status, std::move(error), {}};
}

EvalStatus Vm::Impl::classify(JSValueConst exception, Phase phase) {
    // The backend's interrupt is the only thing that sets the uncatchable flag. Once the budget has fired, whatever
    // exception ends the call is the budget's: the interrupt, or the RangeError a refused call raised.
    if (JS_IsUncatchableError(exception) || interruptFired) {
        return EvalStatus::Interrupted;
    }
    // Both witnesses are needed. The refusal alone would mislabel a script that caught an out-of-memory error
    // and later threw something unrelated; the value alone cannot tell the backend's `null` (thrown when not
    // even the error object fits) from a script's own `throw null`.
    if (heap.refusals() > refusalsAtCallStart && (JS_IsNull(exception) || isOutOfMemoryError(exception))) {
        return EvalStatus::OutOfMemory;
    }
    return phase == Phase::Compile ? EvalStatus::CompileError : EvalStatus::Exception;
}

bool Vm::Impl::isOutOfMemoryError(JSValueConst exception) {
    if (!JS_IsError(exception)) {
        return false;
    }
    const HeldValue message{ctx, JS_GetPropertyStr(ctx, exception, "message")};
    if (message.isException()) {
        clearPendingException();
        return true;  // unreadable while a refusal is already witnessed: count it as the OOM it most likely is
    }
    const HeldCString text{ctx, message.get()};
    if (!text.ok()) {
        clearPendingException();
        return true;  // the same, for a conversion that needed memory the full heap no longer has
    }
    // "out of memory" and "out of memory in regexp execution" both start so.
    return text.view().starts_with("out of memory");
}

ScriptError Vm::Impl::describe(JSValueConst value, bool isErrorObject) {
    if (!isErrorObject) {
        return ScriptError{{}, stringify(value), {}};
    }
    std::string stack = readStringProperty(value, "stack", /*undefinedIsEmpty=*/true);
    if (!stack.empty() && stack.back() == '\n') {
        stack.pop_back();  // the backend ends every frame line with '\n'
    }
    return ScriptError{readStringProperty(value, "name", false), readStringProperty(value, "message", false),
                       std::move(stack)};
}

std::string Vm::Impl::stringify(JSValueConst value) {
    const HeldCString text{ctx, value};
    // A conversion that leaves an exception pending has failed even when it produced text: the backend falls
    // back to an Error's `message` when its toString throws, and reports success with the exception still set.
    if (!text.ok() || JS_HasException(ctx)) {
        clearPendingException();
        return std::string{UNPRINTABLE_VALUE};
    }
    return std::string{text.view()};
}

std::string Vm::Impl::readStringProperty(JSValueConst object, const char* key, bool undefinedIsEmpty) {
    const HeldValue property{ctx, JS_GetPropertyStr(ctx, object, key)};
    if (property.isException()) {
        clearPendingException();
        return std::string{UNPRINTABLE_VALUE};
    }
    if (undefinedIsEmpty && JS_IsUndefined(property.get())) {
        return {};
    }
    return stringify(property.get());
}

void Vm::Impl::clearPendingException() noexcept {
    // A no-op when nothing is pending: the slot then holds a non-reference value.
    JS_FreeValue(ctx, JS_GetException(ctx));
}

void Vm::Impl::logFailure(EvalStatus status, const ScriptError& error, bool isErrorObject) const {
    std::string text;
    switch (status) {
        case EvalStatus::CompileError:
            text = std::format("script: {}: compile error: {}: {}", callFileName, error.name, error.message);
            break;
        case EvalStatus::Exception:
            if (isErrorObject) {
                text = std::format("script: {}: uncaught {}: {}", callFileName, error.name, error.message);
            } else {
                text = std::format("script: {}: uncaught exception: {}", callFileName, error.message);
            }
            break;
        case EvalStatus::OutOfMemory:
            text = std::format("script: {}: out of memory (heap limit {} bytes, {} bytes in use)", callFileName,
                               config.heapLimitBytes, heap.bytesInUse());
            break;
        case EvalStatus::Interrupted:
            text = std::format("script: {}: interrupted after exceeding the {} ms time budget", callFileName,
                               config.callTimeBudget.count());
            break;
        case EvalStatus::Ok:
        case EvalStatus::InvalidVm:
        case EvalStatus::WrongThread:
            assert(false);  // trap() never produces these
            return;
    }
    if (!error.stack.empty()) {
        text += '\n';
        text += error.stack;
    }
    AERO_LOG_ERROR("{}", text);
}

void Vm::Impl::logDrainStop() const {
    AERO_LOG_ERROR("script: {}: interrupted after exceeding the {} ms time budget with promise jobs still queued",
                   callFileName, config.callTimeBudget.count());
}

void Vm::Impl::logUnhandledRejection(const ScriptError& reason, bool isErrorObject) const {
    std::string text;
    if (isErrorObject) {
        const std::string head = std::format("script: {}: unhandled promise rejection:", callFileName);
        text = std::format("{} {}: {}", head, reason.name, reason.message);
        if (!reason.stack.empty()) {
            text += '\n';
            text += reason.stack;
        }
    } else {
        text = std::format("script: {}: unhandled promise rejection: {}", callFileName, reason.message);
    }
    AERO_LOG_ERROR("{}", text);
}

std::string_view evalStatusLabel(EvalStatus status) noexcept {
    switch (status) {
        case EvalStatus::Ok:
            return "Ok";
        case EvalStatus::CompileError:
            return "CompileError";
        case EvalStatus::Exception:
            return "Exception";
        case EvalStatus::OutOfMemory:
            return "OutOfMemory";
        case EvalStatus::Interrupted:
            return "Interrupted";
        case EvalStatus::InvalidVm:
            return "InvalidVm";
        case EvalStatus::WrongThread:
            return "WrongThread";
    }
    return "Unknown";
}

}  // namespace engine::script
