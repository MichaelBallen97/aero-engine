#pragma once
// Aero Engine -- the script layer's PRIVATE join between the public Vm and the backend (task 4.1.1). It
// includes <quickjs.h>, which is why it lives under src/ -- the side of the boundary that
// .github/scripts/check-script-boundary.sh permits. Nothing outside engine/script may include it.

#include <aero/core/slot_map.hpp>
#include <aero/script/internal/heap_accounting.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <quickjs.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// A function kept out of line by every compiler in every configuration, so its locals occupy a frame only while it
// runs and never the frame of a caller that stays live across a nested import (R33 in docs/08-risks.md). MSVC does
// not read [[gnu::noinline]].
#if defined(_MSC_VER)
    #define AERO_SCRIPT_NOINLINE __declspec(noinline)
#else
    #define AERO_SCRIPT_NOINLINE [[gnu::noinline]]
#endif

namespace engine::script {

// A backend value the wrapper holds a reference to. EVERY value the wrapper receives goes into one the
// moment it is received, so a leak needs a release() -- which exists for exactly one reason: a backend
// function that CONSUMES its argument -- JS_EvalFunction (quickjs.h:1239), JS_Throw (quickjs.h:817),
// JS_DefinePropertyValueStr (the value rules at :186-204), JS_DefinePropertyGetSet (both accessors) and
// JS_SetClassProto (the prototype).
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

// The new objects of one reload, owned until the commit takes them: whatever is still owned is freed.
class HeldBatch {
public:
    explicit HeldBatch(JSContext* context) noexcept : context(context) {}
    HeldBatch(const HeldBatch&) = delete;
    HeldBatch& operator=(const HeldBatch&) = delete;
    HeldBatch(HeldBatch&&) = delete;
    HeldBatch& operator=(HeldBatch&&) = delete;
    ~HeldBatch() {
        for (const JSValue value : values) {
            JS_FreeValue(context, value);
        }
    }

    void add(JSValue value) { values.push_back(value); }  // takes the reference
    [[nodiscard]] std::size_t size() const noexcept { return values.size(); }
    // Hands the reference at `index` to the caller; the batch owns nothing there afterwards.
    [[nodiscard]] JSValue take(std::size_t index) noexcept {
        const JSValue out = values[index];
        values[index] = JS_UNDEFINED;
        return out;
    }

private:
    JSContext* context;
    std::vector<JSValue> values;
};

// Why a load or a create was refused by the engine itself (not by the backend): the status, and the text after
// "script: <path>: " in the one ERROR it logged.
struct ScriptRefusal {
    ScriptStatus status;
    std::string text;
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
    // The module watches still pending when it was recorded: it may be one of their bodies' own (vm_modules.cpp).
    std::vector<std::uint64_t> watches;
};

// Which way a module evaluation the engine ran was asked for: evaluateModule (Static) or an import() (Dynamic).
enum class ImportPath : std::uint8_t { Static, Dynamic };

// A module evaluation the engine started, watched until its module promise settles. A rejection the tracker records
// while the module promise is still pending -- inside JS_EvalFunction, or in a drain once a top-level await the module
// imports has settled -- is tagged with the watch, and is the module's own if the module promise then rejects with the
// same value. A watch whose outcome is not taken when its call ends is CARRIED into the later calls (endCallWatches).
struct ModuleWatch {
    std::uint64_t id;
    std::string key;
    std::uint32_t generation;  // the instance's: a carried watch updates the record only while it is still current
    ImportPath path;
    JSValue promise;       // the module promise, held; JS_UNDEFINED until JS_EvalFunction returns it
    bool settled = false;  // settleModule has taken its outcome
    bool carried = false;  // its call has ended: its outcome is no call's status
};

// One attempt to load (and then link and evaluate) a module graph. Attempts never nest: an import() only queues a job.
struct LoadAttempt {
    explicit LoadAttempt(bool decides) noexcept : decidesStatus(decides) {}
    bool decidesStatus;                  // evaluateModule's own attempt: its first refusal is the call's LoadError
    bool loading = true;                 // false once the graph compiled: it is linking or evaluating
    std::size_t depth = 0;               // compiles in progress, one inside another
    std::string importer;                // the importing module's key for the next loader call ("" for an entry)
    std::vector<std::string> compiling;  // the keys being compiled, innermost last (the attribute checker's module)
    std::vector<std::string> created;    // keys read or compiled, whose names a failed attempt retires
    bool refused = false;                // an engine refusal (refuse()) ended it -- never caused by memory
    // The cook's: every static import is refused, because a cooked script imports nothing (bytecode_cook.cpp).
    bool refusesImports = false;
    std::uint64_t growthRefusalsAtStart = 0;  // HeapAccounting::growthRefusals() when it began
    // When set, readModuleSource copies the text of the module it reads at depth 0 (the entry) into it.
    std::optional<std::string>* entryText = nullptr;
};

// One live instance: its current object (HELD) and the canonical key of its script.
struct InstanceSlot {
    JSValue object;
    std::string key;
};

// One script path a loadScript call has reached (a record exists from that call's first backend step on).
struct ScriptRecord {
    JSValue scriptClass = JS_UNDEFINED;      // HELD: the last good class; JS_UNDEFINED until a load succeeds
    std::optional<std::string> runningText;  // the bytes scriptClass's generation was compiled from
    std::vector<ScriptInstance> instances;   // the live instances of this script, in creation order
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
    [[nodiscard]] bool isMessagelessInternalError(JSValueConst value);
    [[nodiscard]] ScriptError describe(JSValueConst value, bool isErrorObject);
    [[nodiscard]] std::string stringify(JSValueConst value);
    [[nodiscard]] std::string readStringProperty(JSValueConst object, const char* key, bool nullishIsEmpty);
    void clearPendingException() noexcept;
    // namesCause: an OutOfMemory record also names the error the backend reported (a compile the heap limit starved).
    void logFailure(EvalStatus status, const ScriptError& error, bool isErrorObject, bool namesCause);
    void logDrainStop();
    void logUnhandledRejection(const ScriptError& reason, bool isErrorObject) const;

    [[nodiscard]] EvalResult trapValue(JSValueConst value, Phase phase);

    // ---- the backend's callbacks (vm.cpp); unread parameters unnamed in the definitions ---------------------
    static int interruptHandler(JSRuntime* runtime, void* opaque) noexcept;
    static void rejectionTracker(JSContext* context, JSValueConst promise, JSValueConst reason, bool isHandled,
                                 void* opaque) noexcept;
    void fireInterrupt() noexcept;
    void keepSkipped(JSValue promise);  // takes the reference; evicts the oldest past MAX_SKIPPED_REJECTIONS
    [[nodiscard]] bool isSkipped(JSValueConst promise) const;             // either list below
    [[nodiscard]] bool isModuleWatchPromise(JSValueConst promise) const;  // some live watch's module promise
    void keepModulePromise(JSValue promise);  // takes the reference; one per module promise, never evicted
    void keepIfReferenced(JSValue promise);   // takes the reference; keeps it only if something else still holds it

    // ---- modules (vm_modules.cpp) --------------------------------------------------------------------------
    // Installs an attempt for its scope and removes it on every exit path. Nested, like CallScope: Impl is private
    // to Vm, so only its own members can name it.
    struct AttemptScope {
        AttemptScope(Impl& vm, LoadAttempt& loadAttempt) noexcept;  // asserts no attempt is installed
        AttemptScope(const AttemptScope&) = delete;
        AttemptScope& operator=(const AttemptScope&) = delete;
        AttemptScope(AttemptScope&&) = delete;
        AttemptScope& operator=(AttemptScope&&) = delete;
        ~AttemptScope();  // vm.attempt = nullptr
        Impl& host;
    };
    [[nodiscard]] EvalResult evaluateModuleCall(std::string_view virtualPath);
    void evaluateEntry(std::string_view key, EvalResult& result, std::optional<std::string>* entryText = nullptr);
    // The file's text as a load would read it, without a loading attempt: the same four checks as readModuleSource, in
    // its order, and no refusal, no log and no backend call; nullopt at the first that fails.
    [[nodiscard]] std::optional<std::string> peekModuleSource(std::string_view key) const;
    [[nodiscard]] HeldValue startModule(LoadAttempt& load, std::string_view key, ImportPath path, std::uint64_t& watch);
    [[nodiscard]] JSModuleDef* compileModule(std::string_view key, std::string_view importer);
    // Every check before a module's compile, in order: a source, the depth cap, the deadline, the size, the size cap,
    // the read, the size again. nullopt: refused, or the deadline fired; the failure is pending.
    [[nodiscard]] AERO_SCRIPT_NOINLINE std::optional<std::string> readModuleSource(std::string_view key,
                                                                                   std::string_view importer);
    // Whether a module file is a cooked script: its first bytes are the container's magic, at any path.
    [[nodiscard]] AERO_SCRIPT_NOINLINE static bool isCookedScriptText(const std::string& text) noexcept;
    // The bytecode branch: a container judged, the deadline, the headroom guard, the read, the after-read checks.
    [[nodiscard]] AERO_SCRIPT_NOINLINE JSModuleDef* compileCookedModule(std::string_view key, std::string_view importer,
                                                                        const std::string& text);
    // The budget covers loading. A source read, a compile and a bytecode read never poll the interrupt handler, so the
    // loader checks the deadline before the source read and again before the compile or the bytecode read; a passed
    // deadline fires through the handler's own routine and leaves the InternalError pending.
    [[nodiscard]] bool deadlinePassed();
    // The one module compile (JS_EVAL_TYPE_MODULE): `text` and `name` are C strings to the backend. Compile only, no
    // module is evaluated; the failure is pending on JS_EXCEPTION.
    [[nodiscard]] HeldValue compileModuleText(const std::string& text, const std::string& name);
    // The one place bytecode becomes a module (JS_ReadObject): the payload as the cook wrote it, a module name already
    // in it. The failure is pending on JS_EXCEPTION.
    [[nodiscard]] HeldValue readModuleBytes(std::span<const std::byte> payload);
    // The name the backend holds a module under (JS_GetModuleName, the atom and its C string freed on every path);
    // nullopt when the conversion failed, its out-of-memory error left pending.
    [[nodiscard]] std::optional<std::string> moduleNameOf(JSModuleDef* module);
    [[nodiscard]] char* serveDynamicImport(const char* base, const char* specifier);
    // The outcome of one watched module evaluation: the record (unless a carried watch's path was reloaded since), the
    // attribution of the body's rejections (§C.7 as amended), the flag clearing (D10). A rejected static entry of the
    // current call also returns the call's failure, already logged; a dynamic or a carried watch logs nothing.
    [[nodiscard]] std::optional<EvalResult> settleModule(std::string_view key, JSValueConst promise,
                                                         std::uint64_t watch, ImportPath path);
    void forgetModuleFailure(JSValueConst promise, JSValueConst reason, std::uint64_t watch, ImportPath path,
                             bool carried);
    [[nodiscard]] ModuleWatch* findWatch(std::uint64_t id) noexcept;
    void observeModuleWatches();     // settles every watch evaluateEntry does not own whose promise has settled
    void endCallWatches() noexcept;  // ~CallScope: frees the settled watches, carries the others
    void refuse(std::string_view name, const std::string& message);  // records (an attempt that decides) + throws
    void throwLoadError(std::string_view name, const std::string& message);
    [[nodiscard]] HeldValue moduleValue(JSModuleDef* module) const;
    void replaceWithOutOfMemory();  // the pending error becomes the backend's own out-of-memory error, it as cause
    [[nodiscard]] char* duplicateName(const std::string& name) const;  // js_malloc copy; nullptr: out of memory, thrown
    static char* normalizeModuleName(JSContext* context, const char* base, const char* specifier,
                                     void* opaque) noexcept;
    static JSModuleDef* loadModule(JSContext* context, const char* name, void* opaque,
                                   JSValueConst attributes) noexcept;
    static int checkImportAttributes(JSContext* context, void* opaque, JSValueConst attributes) noexcept;

    // ---- script classes and instances (vm_scripts.cpp, task 4.3.3) ------------------------------------------------
    [[nodiscard]] ScriptLoadResult loadScriptCall(std::string_view virtualPath);
    [[nodiscard]] ScriptInstanceResult createInstanceCall(std::string_view virtualPath);
    [[nodiscard]] ScriptStatus destroyInstanceCall(ScriptInstance instance);
    void releaseScripts() noexcept;  // shutdown: every instance object and class, before the context; then clears both
    // The default export of the entry's current generation when it is a class, else JS_UNDEFINED. A failure is adopted
    // into `evaluation`, an engine refusal is set in `refusal` and logged.
    [[nodiscard]] HeldValue takeScriptClass(std::string_view key, EvalResult& evaluation,
                                            std::optional<ScriptRefusal>& refusal);
    // One construction of `scriptClass`: an exception is adopted into `evaluation`, a Proxy is refused.
    [[nodiscard]] HeldValue constructInstance(JSValueConst scriptClass, EvalResult& evaluation,
                                              std::optional<ScriptRefusal>& refusal);
    // Every live instance of `record`, in creation order: constructs from `scriptClass` into `fresh`, and stops at the
    // first failure or refusal. Touches neither a slot nor the record.
    void rebuildInstances(const ScriptRecord& record, JSValueConst scriptClass, EvalResult& evaluation,
                          std::optional<ScriptRefusal>& refusal, HeldBatch& fresh);
    void endScriptCall(EvalResult& evaluation);  // the call's job drain and tail, in evaluateModuleCall's order
    void logRefusal(const ScriptRefusal& refusal) const;

    // ---- state: `heap` FIRST, so it is destroyed LAST ------------------------------------------------------
    internal::HeapAccounting heap;
    ScriptApi scriptApi;  // the `engine` namespace's state (script_api.hpp); the context opaque points at it
    VmConfig config;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::thread::id vmThread;
    bool finished = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::min();
    bool inCall = false;
    bool quiet = false;              // the cook's VMs: no failure is logged (the result carries it; bytecode_cook.cpp)
    bool interruptFired = false;     // the budget ran out in the current call; see interruptHandler
    bool interruptRecorded = false;  // an "interrupted" record has been logged for the current call
    std::uint64_t refusalsAtCallStart = 0;
    std::uint64_t growthRefusalsAtCallStart = 0;
    std::string callFileName;
    std::vector<PendingRejection> pendingRejections;
    // Promises whose rejection the end-of-call report skipped because the budget caused it. The reference is kept
    // so a handler attached in a later call is recognised as handling something never reported.
    std::vector<JSValue> skippedRejections;
    // The module promises of failed module evaluations: each is held by its module until teardown anyway, so keeping
    // them costs no memory and needs no cap -- there is at most one per module instance. Never in skippedRejections.
    std::vector<JSValue> failedModulePromises;
    std::uint64_t unhandledRejections = 0;
    std::uint64_t rejectionsHandledLate = 0;
    // The modules (task 4.1.2). moduleTable holds strings and opaque addresses, never a backend value, so it may
    // outlive the runtime.
    const VirtualFileSystem* modules = nullptr;  // borrowed; null = no module source
    internal::ModuleTable moduleTable;
    LoadAttempt* attempt = nullptr;              // the attempt being served, or null between attempts
    std::optional<ScriptError> callLoadFailure;  // the call's own attempt's first refusal; reset by CallScope
    std::vector<ModuleWatch> moduleWatches;      // the current call's, then the carried ones (vm_modules.cpp)
    std::uint64_t nextWatchId = 0;               // never reused, so a stale tag matches no later watch
    std::uint64_t moduleInstances = 0;
    // The script classes (task 4.3.3). scripts holds each path's last good class and its live instances; the slots hold
    // the instances' current objects. Both change on the VM's thread, outside the backend's callbacks.
    std::map<std::string, ScriptRecord, std::less<>> scripts;  // by canonical key
    SlotMap<InstanceSlot, ScriptInstanceTag> instanceSlots;
    bool loadingScript = false;  // the current call is a loadScript: the pending-await WARN is the load's to refuse
};

}  // namespace engine::script
