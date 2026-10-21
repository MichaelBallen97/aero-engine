#pragma once
// Aero Engine -- the script layer's PRIVATE join between the public Vm and the backend (task 4.1.1). It
// includes <quickjs.h>, which is why it lives under src/ -- the side of the boundary that
// .github/scripts/check-script-boundary.sh permits. Nothing outside engine/script may include it.

#include <aero/script/internal/heap_accounting.hpp>
#include <aero/script/vm.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace engine::script {

// A backend value the wrapper holds a reference to. EVERY value the wrapper receives goes into one the
// moment it is received, so a leak needs a release() -- which exists for exactly one reason: a backend
// function that CONSUMES its argument (JS_EvalFunction, quickjs.h:1239; the value rules at :186-204).
class HeldValue {
public:
    HeldValue(JSContext* context, JSValue held) noexcept : context(context), value(held) {}
    HeldValue(const HeldValue&) = delete;
    HeldValue& operator=(const HeldValue&) = delete;
    HeldValue(HeldValue&&) = delete;
    HeldValue& operator=(HeldValue&&) = delete;
    ~HeldValue() { JS_FreeValue(context, value); }

    [[nodiscard]] JSValueConst get() const noexcept { return value; }
    [[nodiscard]] bool isException() const noexcept { return JS_IsException(value); }
    [[nodiscard]] JSValue release() noexcept {
        const JSValue out = value;
        value = JS_UNDEFINED;
        return out;
    }

private:
    JSContext* context;
    JSValue value;
};

// A C string converted from a value (JS_ToCStringLen); freed with JS_FreeCString. ok() is false when the
// conversion threw -- the exception is then PENDING and the caller must discard it.
class HeldCString {
public:
    HeldCString(JSContext* context, JSValueConst value) noexcept
        : context(context), text(JS_ToCStringLen(context, &length, value)) {}
    HeldCString(const HeldCString&) = delete;
    HeldCString& operator=(const HeldCString&) = delete;
    HeldCString(HeldCString&&) = delete;
    HeldCString& operator=(HeldCString&&) = delete;
    ~HeldCString() {
        if (text != nullptr) {
            JS_FreeCString(context, text);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return text != nullptr; }
    [[nodiscard]] std::string_view view() const noexcept {
        return text == nullptr ? std::string_view{} : std::string_view{text, length};
    }

private:
    JSContext* context;
    std::size_t length = 0;  // declared before `text`: the conversion writes it while `text` initialises
    const char* text;
};

enum class Phase : std::uint8_t { Compile, Run };
inline constexpr std::string_view UNPRINTABLE_VALUE = "<unprintable value>";
inline constexpr std::string_view DRAIN_STOP_MESSAGE = "the time budget ran out with promise jobs still queued";

// A promise that rejected with no handler attached yet, and its reason: two held references, freed explicitly
// wherever an entry is consumed (the report, the erase, shutdown).
struct PendingRejection {
    JSValue promise;
    JSValue reason;
    bool afterInterrupt;  // recorded after the call's budget ran out: the budget's doing, never reported
};

struct Vm::Impl {
    explicit Impl(const VmConfig& vmConfig) noexcept;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;  // pinned: the backend holds this address
    Impl& operator=(Impl&&) = delete;
    ~Impl();  // static_cast<void>(shutdown()) unless already finished

    // ---- lifecycle (vm.cpp) ------------------------------------------------------------------------------
    [[nodiscard]] VmTeardownReport shutdown() noexcept;
    [[nodiscard]] bool onVmThread() const noexcept { return std::this_thread::get_id() == vmThread; }

    // ---- one call (vm.cpp) -------------------------------------------------------------------------------
    // The whole of a call's setup and teardown: the deadline, the C stack top, the refusal mark and the file
    // name every record of the call carries; the destructor also gives back the stack limit the interrupt
    // handler takes away when the budget runs out. Outside a call the deadline is already in the past, so if script
    // code ever ran outside one it would be interrupted at its first poll.
    struct CallScope {
        CallScope(Impl& host, const std::string& fileName);
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
        CallScope(CallScope&&) = delete;
        CallScope& operator=(CallScope&&) = delete;
        ~CallScope();  // deadline = time_point::min(); inCall = false
        Impl& impl;
    };
    [[nodiscard]] EvalResult evaluate(std::string_view source, std::string_view fileName);
    void drainJobs(EvalResult& result);
    void noteInterrupt(EvalResult& result);
    void reportUnhandledRejections();
    static void adopt(EvalResult& result, const EvalResult& failure);

    // ---- errors (vm_errors.cpp) --------------------------------------------------------------------------
    [[nodiscard]] EvalResult trap(Phase phase);
    [[nodiscard]] EvalStatus classify(JSValueConst exception, Phase phase);
    [[nodiscard]] bool isOutOfMemoryError(JSValueConst exception);
    [[nodiscard]] ScriptError describe(JSValueConst value, bool isErrorObject);
    [[nodiscard]] std::string stringify(JSValueConst value);
    [[nodiscard]] std::string readStringProperty(JSValueConst object, const char* key, bool nullishIsEmpty);
    void clearPendingException() noexcept;
    void logFailure(EvalStatus status, const ScriptError& error, bool isErrorObject);
    void logDrainStop();
    void logUnhandledRejection(const ScriptError& reason, bool isErrorObject) const;

    // ---- the backend's callbacks (vm.cpp); unread parameters unnamed in the definitions ---------------------
    static int interruptHandler(JSRuntime* runtime, void* opaque) noexcept;
    static void rejectionTracker(JSContext* context, JSValueConst promise, JSValueConst reason, bool isHandled,
                                 void* opaque) noexcept;

    // ---- state: `heap` FIRST, so it is destroyed LAST ------------------------------------------------------
    internal::HeapAccounting heap;
    VmConfig config;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::thread::id vmThread;
    bool finished = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::min();
    bool inCall = false;
    bool interruptFired = false;     // the budget ran out in the current call; see interruptHandler
    bool interruptRecorded = false;  // an "interrupted" record has been logged for the current call
    std::uint64_t refusalsAtCallStart = 0;
    std::string callFileName;
    std::vector<PendingRejection> pendingRejections;
    std::uint64_t unhandledRejections = 0;
    std::uint64_t rejectionsHandledLate = 0;
};

}  // namespace engine::script
