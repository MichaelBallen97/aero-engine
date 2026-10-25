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
    const HeldValue exception{ctx, JS_GetException(ctx)};  // takes the reference, clears the slot
    return trapValue(exception.get(), phase);
}

// trap's body, for a value that is not the pending exception: a module promise's rejection reason (vm_modules.cpp).
EvalResult Vm::Impl::trapValue(JSValueConst value, Phase phase) {
    const EvalStatus status = classify(value, phase);  // BEFORE describe: describe may allocate
    const bool isErrorObject = JS_IsError(value);
    // An out-of-memory the backend reported as another error -- the parser's, after its buffer failed to grow -- also
    // names that error, so the record says what the backend said. Not for the backend's own out-of-memory error, nor
    // for that error once even its message could not be allocated (a message-less InternalError), in either phase.
    const bool namesCause = status == EvalStatus::OutOfMemory && isErrorObject && !isOutOfMemoryError(value) &&
                            !isMessagelessInternalError(value);
    // A load error's text is the engine's own, exact and never cut at the backend's 256-byte message buffer.
    const bool engineText = status == EvalStatus::LoadError && callLoadFailure.has_value();
    // The backend's "no exception" sentinel: a refused atom-array growth makes __JS_NewAtom return JS_ATOM_NULL
    // without throwing (quickjs.c:3136-3150), and the tokenizer in a compile, or a computed key's store in a run,
    // fails with nothing pending, so trap takes JS_UNINITIALIZED. It is no value -- an empty error with no stack,
    // described as nothing -- and the refusal made the status OutOfMemory (in a compile the refused growth, in a
    // run the value arm of classify). An interrupt or a load refusal still names itself, as for every other value.
    ScriptError error;
    if (engineText) {
        error = *callLoadFailure;
    } else if (!JS_IsUninitialized(value)) {
        error = describe(value, isErrorObject);
    }
    if (status == EvalStatus::Interrupted) {
        // Whichever exception carried it -- the interrupt itself, or the RangeError a refused call raised after
        // the budget ran out -- the canonical interrupt is what the caller reads. The stack stays the described one.
        error.name = "InternalError";
        error.message = "interrupted";
    }
    logFailure(status, error, isErrorObject, namesCause);
    if (status == EvalStatus::OutOfMemory) {
        JS_RunGC(rt);  // the failed work is unreachable now; cycles among it need the collector
    }
    return EvalResult{status, std::move(error), {}};
}

EvalStatus Vm::Impl::classify(JSValueConst exception, Phase phase) {
    // Once THIS call's budget has fired, whatever exception ends the call is the budget's: the interrupt, or the
    // RangeError a refused call raised. The handler is the only route to the backend's interrupt and sets the flag
    // first, so the flag alone decides: an uncatchable interrupt object left over from an earlier call (a promise's
    // rejection reason, rethrown by a later reaction job) is an ordinary exception of this one.
    if (interruptFired) {
        return EvalStatus::Interrupted;
    }
    // A load refusal of THIS call's own attempt -- never a dynamic import's, which is the script's to catch -- is why
    // the graph did not compile, whatever error the backend wrapped it in. No engine refusal is ever caused by memory,
    // and it ends the attempt, so it is the call's first failure even when the backend refused an allocation as well.
    if (phase == Phase::Compile && callLoadFailure.has_value()) {
        return EvalStatus::LoadError;
    }
    // Both witnesses are needed while code runs: the refusal alone would mislabel a script that caught an out-of-memory
    // error and later threw something unrelated; the value alone cannot tell the backend's `null` (thrown when not even
    // the error object fits) from a script's own `throw null`.
    if (heap.refusals() > refusalsAtCallStart &&
        (JS_IsNull(exception) || JS_IsUninitialized(exception) || isOutOfMemoryError(exception) ||
         isMessagelessInternalError(exception))) {
        // A message-less InternalError under a refusal is the backend's own out-of-memory error, its message allocation
        // refused as well (JS_MakeError2, quickjs.c:7975-7997): the run phase classifies it too. So does the "no
        // exception" sentinel, which no script can produce: a computed key that interns a new atom under a full
        // heap fails its store with nothing pending (JS_NewAtomStr, quickjs.c:3343-3354).
        return EvalStatus::OutOfMemory;
    }
    // A compile runs no script code, and the parser keeps parsing after its bytecode buffer failed to grow: the next
    // assignment's SyntaxError replaces the out-of-memory error (quickjs.c:23357-23362, :25615-25623, :7594-7600). So
    // in a compile a refused GROWTH decides by itself. A refused zeroed allocation does not: the backend tolerates
    // exactly two refusals, both zeroed -- its atom and shape hash growth (quickjs.c:2972 and :5265, ignored at
    // :3236-3237 and :5341-5342) -- and the compile then fails, or not, for its own reason.
    if (phase == Phase::Compile && heap.growthRefusals() > growthRefusalsAtCallStart) {
        return EvalStatus::OutOfMemory;
    }
    return phase == Phase::Compile ? EvalStatus::CompileError : EvalStatus::Exception;
}

// The backend builds an error object first and its message after (JS_MakeError2, quickjs.c:7975-7997); when the
// message cannot be allocated either, the error keeps none, and reads as Error.prototype's empty message.
bool Vm::Impl::isMessagelessInternalError(JSValueConst value) {
    if (!JS_IsError(value)) {
        return false;
    }
    return readStringProperty(value, "name", false) == "InternalError" &&
           readStringProperty(value, "message", false).empty();
}

bool Vm::Impl::isOutOfMemoryError(JSValueConst exception) {
    if (!JS_IsError(exception)) {
        return false;
    }
    const HeldValue message{ctx, JS_GetPropertyStr(ctx, exception, "message")};
    if (message.isException()) {
        // The backend's own out-of-memory error carries `message` as an own data property, read through an atom that
        // already exists, so reading it never allocates: a read that fails belongs to an Error a script made.
        clearPendingException();
        return false;
    }
    // The backend's own out-of-memory error always carries a string message, and an ASCII string converts without
    // allocating, so a message that is not a string was written by a script.
    if (!JS_IsString(message.get())) {
        return false;
    }
    const HeldCString text{ctx, message.get()};
    if (!text.ok()) {
        clearPendingException();
        return true;  // a string that needed memory the full heap no longer has, under a witnessed refusal
    }
    // "out of memory" and "out of memory in regexp execution" both start so.
    return text.view().starts_with("out of memory");
}

ScriptError Vm::Impl::describe(JSValueConst value, bool isErrorObject) {
    if (!isErrorObject) {
        return ScriptError{{}, stringify(value), {}};
    }
    std::string stack = readStringProperty(value, "stack", /*nullishIsEmpty=*/true);
    if (!stack.empty() && stack.back() == '\n') {
        stack.pop_back();  // the backend ends every frame line with '\n'
    }
    return ScriptError{readStringProperty(value, "name", false), readStringProperty(value, "message", false),
                       std::move(stack)};
}

std::string Vm::Impl::stringify(JSValueConst value) {
    // Nothing is legitimately pending here: trap has already taken the call's own exception. Some builtins (parseFloat,
    // parseInt, JSON.parse, normalize) convert an Error to a string and return success with its toString's exception
    // still set, so a stale one is cleared first and cannot be blamed on this conversion.
    clearPendingException();
    const HeldCString text{ctx, value};
    // A conversion that leaves an exception pending has failed even when it produced text: the backend falls
    // back to an Error's `message` when its toString throws, and reports success with the exception still set.
    if (!text.ok() || JS_HasException(ctx)) {
        clearPendingException();
        return std::string{UNPRINTABLE_VALUE};
    }
    return std::string{text.view()};
}

std::string Vm::Impl::readStringProperty(JSValueConst object, const char* key, bool nullishIsEmpty) {
    const HeldValue property{ctx, JS_GetPropertyStr(ctx, object, key)};
    if (property.isException()) {
        clearPendingException();
        return std::string{UNPRINTABLE_VALUE};
    }
    // null counts as absent as well: once the budget has run out a script's Error.prepareStackTrace is refused, and
    // the backend then builds every error with a null stack. That is no stack, not the text "null".
    if (nullishIsEmpty && (JS_IsUndefined(property.get()) || JS_IsNull(property.get()))) {
        return {};
    }
    return stringify(property.get());
}

void Vm::Impl::clearPendingException() noexcept {
    // A no-op when nothing is pending: the slot then holds a non-reference value.
    JS_FreeValue(ctx, JS_GetException(ctx));
}

void Vm::Impl::logFailure(EvalStatus status, const ScriptError& error, bool isErrorObject, bool namesCause) {
    std::string text;
    switch (status) {
        case EvalStatus::CompileError:
            text = std::format("script: {}: compile error: {}: {}", callFileName, error.name, error.message);
            break;
        case EvalStatus::LoadError:  // a load error has no stack: nothing ran
            text = std::format("script: {}: load error: {}: {}", callFileName, error.name, error.message);
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
            if (namesCause) {  // Error.prototype.toString's shape: the name alone when the message is empty
                text += error.message.empty() ? std::format(": {}", error.name)
                                              : std::format(": {}: {}", error.name, error.message);
            }
            break;
        case EvalStatus::Interrupted:
            if (interruptRecorded) {
                return;  // one interrupt record per call, whichever failure carries it next
            }
            interruptRecorded = true;
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

void Vm::Impl::logDrainStop() {
    interruptRecorded = true;
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
        case EvalStatus::LoadError:
            return "LoadError";
    }
    return "Unknown";
}

}  // namespace engine::script
