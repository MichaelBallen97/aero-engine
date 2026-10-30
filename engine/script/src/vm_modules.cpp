// Aero Engine -- engine::script::Vm (task 4.1.2): ES modules through the VFS. The backend's three module callbacks,
// the one place a source becomes a module (compileModule), the entry point, and the cache's public face. The rules --
// the grammar, the names, the table, the texts -- are module_graph.hpp's; this file is the sequence of backend calls
// around them.

#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

// ---- the attempt ----------------------------------------------------------------------------------------------

Vm::Impl::AttemptScope::AttemptScope(Impl& vm, LoadAttempt& loadAttempt) noexcept : host(vm) {
    assert(host.attempt == nullptr);  // attempts never nest: an import() only queues a job
    host.attempt = &loadAttempt;
}

Vm::Impl::AttemptScope::~AttemptScope() { host.attempt = nullptr; }

// ---- the entry point ------------------------------------------------------------------------------------------

EvalResult Vm::Impl::evaluateModuleCall(std::string_view virtualPath) {
    assert(!inCall);  // a host function never calls back into the VM (script_api.hpp)
    const internal::SpecifierResolution entry = internal::resolveModuleSpecifier(virtualPath, {});
    const bool resolved = entry.refusal == internal::SpecifierRefusal::None;
    const CallScope scope{*this, resolved ? entry.key : std::string{virtualPath}};
    EvalResult result{EvalStatus::Ok, {}, {}};
    if (!resolved || modules == nullptr) {
        // Nothing reaches the backend: the refusal is recorded and logged exactly as a dependency's would be.
        const ScriptError refusal{"TypeError",
                                  resolved ? internal::noModuleSourceMessage(entry.key, {})
                                           : internal::refusedSpecifierMessage(virtualPath, {}, entry.refusal),
                                  {}};
        callLoadFailure = refusal;
        result = EvalResult{EvalStatus::LoadError, refusal, {}};
        logFailure(EvalStatus::LoadError, result.error, /*isErrorObject=*/true, /*namesCause=*/false);
        drainJobs(result);  // jobs an earlier call queued still run, as after evaluateScript's CompileError
    } else {
        evaluateEntry(entry.key, result);
    }
    observeModuleWatches();  // an import() the drain served: BEFORE the report
    noteInterrupt(result);   // the 4.1.1 tail
    reportUnhandledRejections();
    noteInterrupt(result);
    clearPendingException();
    return result;
}

void Vm::Impl::evaluateEntry(std::string_view key, EvalResult& result) {
    LoadAttempt entryAttempt{/*decides=*/true};
    std::uint64_t watch = 0;
    const HeldValue promise = startModule(entryAttempt, key, ImportPath::Static, watch);
    if (promise.isException()) {
        result = trap(Phase::Compile);  // LoadError, CompileError (a link error included), OutOfMemory, Interrupted
        moduleTable.retire(entryAttempt.created);
        if (result.status != EvalStatus::Interrupted && !interruptFired) {
            drainJobs(result);
        }
        return;
    }
    const auto settle = [&] { return settleModule(key, promise.get(), watch, ImportPath::Static); };
    const bool settledAtOnce = JS_PromiseState(ctx, promise.get()) != JS_PROMISE_PENDING;
    if (settledAtOnce) {  // no top-level await: the module's outcome is the call's first failure, before its jobs
        if (const std::optional<EvalResult> failure = settle()) {
            adopt(result, *failure);
        }
    }
    if (result.status != EvalStatus::Interrupted && !interruptFired) {
        drainJobs(result);
    }
    if (!settledAtOnce) {
        if (const std::optional<EvalResult> failure = settle()) {
            adopt(result, *failure);
        } else if (const bool budgetEnded = interruptFired || interruptRecorded;
                   JS_PromiseState(ctx, promise.get()) == JS_PROMISE_PENDING && !budgetEnded) {
            // A call its budget ended is Interrupted, and its record says so: what it left pending is the budget's.
            // Both ways count: the interrupt handler (interruptFired) and the drain's own deadline check, which stops
            // between two jobs and records the interrupt without firing it (interruptRecorded).
            AERO_LOG_WARN(
                "script: {}: module evaluation is still pending at the end of the call (a top-level await has not "
                "settled)",
                callFileName);
        }
    }
}

// Steps 4-5 of the entry (and of the dynamic path): inside the attempt's scope, find or compile the graph, then link
// it and start its evaluation under a new watch. The result is the module promise, or JS_EXCEPTION with the failure
// pending (a load refusal, a compile or link error, an out-of-memory, the budget): the caller traps it while it is
// still pending and retires what the attempt created. `watch` receives the new watch's id.
HeldValue Vm::Impl::startModule(LoadAttempt& load, std::string_view key, ImportPath path, std::uint64_t& watch) {
    const AttemptScope scope{*this, load};
    auto* module = static_cast<JSModuleDef*>(moduleTable.instance(key));
    if (module == nullptr) {
        module = compileModule(key, load.importer);
        if (module == nullptr) {
            return HeldValue{ctx, JS_EXCEPTION};
        }
    }
    load.loading = false;
    // Watched from here: until the module promise settles, every rejection the tracker records may be the body's own.
    watch = ++nextWatchId;
    moduleWatches.push_back(ModuleWatch{watch, std::string{key}, moduleTable.generation(key), path, JS_UNDEFINED});
    HeldValue promise{ctx, JS_EvalFunction(ctx, moduleValue(module).release())};  // consumes the minted reference
    ModuleWatch* const watched = findWatch(watch);  // nothing inside JS_EvalFunction opens a watch
    if (promise.isException()) {
        watched->settled = true;  // a link error, or no memory for the promise: nothing ran
    } else {
        watched->promise = JS_DupValue(ctx, promise.get());
    }
    return HeldValue{ctx, promise.release()};
}

// ---- import() -------------------------------------------------------------------------------------------------

// A normalizer call that arrives while no engine load is running IS a dynamic import(). It is served like an entry:
// resolved by the same grammar, loaded, linked and evaluated by the engine itself, and only then handed to the backend
// by name -- so a link failure is seen here (the backend never links a module twice), the module body's rejections
// are attributed by the evaluation's watch, and the edge is recorded. A dynamic failure never changes the
// call's status: it rejects the import(), which the script may catch.
char* Vm::Impl::serveDynamicImport(const char* base, const char* specifier) {
    const internal::SpecifierResolution target = internal::resolveModuleSpecifier(specifier, base);
    const std::optional<std::string> importer = internal::importerKey(base);
    const std::string importerText = importer.has_value() ? *importer : std::string{base};
    if (target.refusal != internal::SpecifierRefusal::None) {
        throwLoadError("TypeError", internal::refusedSpecifierMessage(specifier, importerText, target.refusal));
        return nullptr;  // the import() rejects; a dynamic refusal is the script's to catch, never the call's status
    }
    // The edge is the calling generation's: an older instance's import() -- a function it exported, still referenced
    // -- never grows the current record, whose next invalidation would otherwise reload a module it never imported.
    // Global code has no record at all.
    if (const std::optional<internal::ModuleName> caller = internal::decodeModuleName(base);
        caller.has_value() && caller->generation == moduleTable.generation(caller->key)) {
        moduleTable.addImport(caller->key, target.key, internal::ImportKind::Dynamic);
    }
    if (modules == nullptr) {
        throwLoadError("TypeError", internal::noModuleSourceMessage(target.key, importerText));
        return nullptr;
    }
    LoadAttempt dynamicAttempt{/*decides=*/false};
    dynamicAttempt.importer = importerText;
    dynamicAttempt.growthRefusalsAtStart = heap.growthRefusals();
    std::uint64_t watch = 0;
    const HeldValue promise = startModule(dynamicAttempt, target.key, ImportPath::Dynamic, watch);
    if (promise.isException()) {
        moduleTable.retire(dynamicAttempt.created);
        // classify's rule for the static path: a PARSE the heap limit starved is the heap's failure, whatever error the
        // parser was left to report, and an engine refusal is never memory's. The import() carries the same verdict.
        const bool parseStarved = dynamicAttempt.loading && !dynamicAttempt.refused && !interruptFired &&
                                  heap.growthRefusals() > dynamicAttempt.growthRefusalsAtStart;
        if (parseStarved) {
            replaceWithOutOfMemory();
        }
        return nullptr;  // the failure stays pending: the backend rejects the import() with it, and never sees the name
    }
    static_cast<void>(settleModule(target.key, promise.get(), watch, ImportPath::Dynamic));
    return duplicateName(internal::encodeModuleName(target.key, moduleTable.generation(target.key)));
}

// ---- the one place a source becomes a module ------------------------------------------------------------------

JSModuleDef* Vm::Impl::compileModule(std::string_view key, std::string_view importer) {
    assert(attempt != nullptr && attempt->loading);
    if (modules == nullptr) {
        refuse("TypeError", internal::noModuleSourceMessage(key, importer));
        return nullptr;
    }
    const std::size_t maxDepth = internal::maxImportDepth(config.stackLimitBytes);
    if (attempt->depth >= maxDepth) {
        refuse("TypeError", internal::tooDeepMessage(key, importer, maxDepth, config.stackLimitBytes));
        return nullptr;
    }
    // The budget covers loading. A read and a compile never poll the interrupt handler, so the deadline is checked
    // here, before the read and again before the compile, and fires through the handler's own routine.
    const auto deadlinePassed = [this] {
        if (std::chrono::steady_clock::now() < deadline) {
            return false;
        }
        fireInterrupt();
        static_cast<void>(JS_ThrowInternalError(ctx, "%s", "interrupted"));
        return true;
    };
    if (deadlinePassed()) {
        return nullptr;
    }
    // The size first, so a source larger than the heap limit is refused before a byte of it is read.
    const std::optional<std::uint64_t> size = modules->fileSize(key);
    if (!size.has_value()) {
        refuse("TypeError", internal::notFoundMessage(key, importer));
        return nullptr;
    }
    if (!internal::sourceWithinCap(*size, config.heapLimitBytes)) {
        refuse("TypeError", internal::tooLargeMessage(key, importer, *size, config.heapLimitBytes));
        return nullptr;
    }
    const std::optional<std::string> text = modules->readText(key);
    if (!text.has_value()) {
        refuse("TypeError", internal::notFoundMessage(key, importer));
        return nullptr;
    }
    if (!internal::sourceWithinCap(text->size(), config.heapLimitBytes)) {  // the file grew after its size was asked
        refuse("TypeError", internal::tooLargeMessage(key, importer, text->size(), config.heapLimitBytes));
        return nullptr;
    }
    if (deadlinePassed()) {
        return nullptr;
    }
    // The compile resolves the module's imports, so its children are loaded inside it (loadModule), each one level
    // deeper. Its name is the current generation's: a later generation is never found under an older one's name.
    const std::string name = internal::encodeModuleName(key, moduleTable.generation(key));
    moduleTable.beginCompile(key);
    ++attempt->depth;
    attempt->compiling.emplace_back(key);
    const HeldValue compiled = compileModuleText(*text, name);
    attempt->compiling.pop_back();
    --attempt->depth;
    if (compiled.isException()) {
        return nullptr;  // the backend freed the module; the failure is pending
    }
    // ~HeldValue drops the reference the compile handed over; the backend's module list keeps its own.
    auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
    moduleTable.markLoaded(key, module);
    attempt->created.emplace_back(key);
    ++moduleInstances;
    return module;
}

HeldValue Vm::Impl::compileModuleText(const std::string& text, const std::string& name) {
    constexpr int MODULE_COMPILE = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
    return HeldValue{ctx, JS_Eval(ctx, text.c_str(), text.size(), name.c_str(), MODULE_COMPILE)};
}

HeldValue Vm::Impl::readModuleBytes(std::span<const std::byte> payload) {
    return HeldValue{ctx, JS_ReadObject(ctx, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(),
                                        JS_READ_OBJ_BYTECODE)};
}

// ---- a module's outcome ---------------------------------------------------------------------------------------

std::optional<EvalResult> Vm::Impl::settleModule(std::string_view key, JSValueConst promise, std::uint64_t watch,
                                                 ImportPath path) {
    const JSPromiseStateEnum state = JS_PromiseState(ctx, promise);
    ModuleWatch* const watched = findWatch(watch);
    const bool carried = watched != nullptr && watched->carried;
    // A carried watch's module may have been reloaded since: the record then belongs to the new generation.
    const bool current = !carried || moduleTable.generation(key) == watched->generation;
    if (state != JS_PROMISE_PENDING && watched != nullptr) {
        watched->settled = true;
    }
    switch (state) {
        case JS_PROMISE_FULFILLED:
            if (current) {
                moduleTable.markEvaluatedGraph(key);
            }
            return std::nullopt;
        case JS_PROMISE_REJECTED: {
            if (current) {
                moduleTable.markFailed(key);
            }
            const HeldValue reason{ctx, JS_PromiseResult(ctx, promise)};
            std::optional<EvalResult> failure;
            if (path == ImportPath::Static && !carried) {
                failure = trapValue(reason.get(), Phase::Run);  // classified, described, logged once
            }
            forgetModuleFailure(promise, reason.get(), watch, path, carried);
            JS_ClearUncatchableError(ctx, reason.get());  // D10: every later import rethrows this very object
            return failure;
        }
        case JS_PROMISE_PENDING:
            if (current) {
                moduleTable.markEvaluating(key);
            }
            return std::nullopt;
        case JS_PROMISE_NOT_A_PROMISE:
            break;
    }
    assert(false && "JS_EvalFunction on a module returns a promise (quickjs.c:31540-31590)");
    return std::nullopt;
}

// A promise some module watch is responsible for: its own watch's settle reports it. A parent module's promise is
// rejected before the child's own (quickjs.c:31297-31309) and carries the child's reason, so without this the child's
// same-value arm claims it and the failure is reported zero times.
bool Vm::Impl::isModuleWatchPromise(JSValueConst promise) const {
    for (const ModuleWatch& w : moduleWatches) {
        if (!JS_IsUndefined(w.promise) && JS_IsStrictEqual(ctx, w.promise, promise)) {
            return true;
        }
    }
    return false;
}

// A module body that throws rejects TWO promises with one reason -- the backend's internal promise for the body and
// the module promise. The body's is rejected while the module promise is still pending (inside JS_EvalFunction, or in
// a drain once a top-level await the module imports has settled), so the tracker tagged it with this watch; the
// module promise is matched by identity. Same value, not strict equality: a module that throws NaN is one failure too.
// The module promise goes to its own list -- its module holds it until teardown anyway -- and any other erased promise
// is kept only while something else holds it, so a handler attached later is silent, never "handled late".
void Vm::Impl::forgetModuleFailure(JSValueConst promise, JSValueConst reason, std::uint64_t watch, ImportPath path,
                                   bool carried) {
    bool kept = false;
    for (auto entry = pendingRejections.begin(); entry != pendingRejections.end();) {
        const bool ownModulePromise = JS_IsStrictEqual(ctx, entry->promise, promise);
        // A carried watch's module promise is no call's status: it stays listed, and is this call's one record.
        const bool isModulePromise = !carried && ownModulePromise;
        const bool tagged = std::find(entry->watches.begin(), entry->watches.end(), watch) != entry->watches.end();
        // The same-value arm is for a module body's OWN internal promise, which is no watch's module promise; a module
        // promise, this watch's or another's, is settled by its own watch.
        const bool bodyPromise =
            tagged && JS_IsSameValue(ctx, entry->reason, reason) && !isModuleWatchPromise(entry->promise);
        if (isModulePromise || bodyPromise) {
            kept = kept || isModulePromise;
            JS_FreeValue(ctx, entry->reason);
            if (isModulePromise) {
                // takes the listed reference; a later import's handler is silent
                keepModulePromise(entry->promise);
            } else {
                keepIfReferenced(entry->promise);  // the body's own promise goes; a script's is kept
            }
            entry = pendingRejections.erase(entry);
        } else if (carried && ownModulePromise) {
            // A carried module promise rejected in a later call stays listed: it is that call's one unhandled record.
            // It is kept as well, so a later import()'s handler is recognised, never "handled later".
            keepModulePromise(JS_DupValue(ctx, entry->promise));
            ++entry;
        } else {
            ++entry;
        }
    }
    if (path == ImportPath::Static && !carried && !kept && !isSkipped(promise)) {
        keepModulePromise(JS_DupValue(ctx, promise));  // a cached failure re-observed, not kept yet
    }
}

ModuleWatch* Vm::Impl::findWatch(std::uint64_t id) noexcept {
    for (ModuleWatch& watch : moduleWatches) {
        if (watch.id == id) {
            return &watch;
        }
    }
    return nullptr;
}

// The current call's entry watch is settled by evaluateEntry, which owns the call's status; every other watch -- an
// import()'s, and a carried one whose top-level await settled in this call -- is settled here once its promise has, so
// a body rejected in a drain is attributed before the report, whichever path evaluated it and whichever call it is.
void Vm::Impl::observeModuleWatches() {
    // settleModule never adds or removes a watch, so the references stay valid through the loop.
    for (const ModuleWatch& watch : moduleWatches) {
        const bool owned = watch.path == ImportPath::Static && !watch.carried;
        if (watch.settled || owned || JS_IsUndefined(watch.promise) ||
            JS_PromiseState(ctx, watch.promise) == JS_PROMISE_PENDING) {
            continue;
        }
        const std::string key = watch.key;
        static_cast<void>(settleModule(key, watch.promise, watch.id, watch.path));
    }
}

// A watch whose outcome is not taken when its call ends -- its top-level await still pending, or settled after the
// call's last look -- is carried: a later call attributes the body's rejections and updates the record the same way.
// At most one watch per module instance (an instance evaluated again while it waits adds none; the members of a cycle
// share one promise and keep one watch each), so the list is bounded by the module instances. Compacted in place: a
// destructor's path allocates nothing.
void Vm::Impl::endCallWatches() noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < moduleWatches.size(); ++i) {
        ModuleWatch& watch = moduleWatches[i];
        bool carry = !watch.settled && !JS_IsUndefined(watch.promise);
        // One watch per module INSTANCE: every member of a cycle evaluates to the cycle root's promise
        // (quickjs.c:31548-31553), and each member's record must follow it.
        for (std::size_t k = 0; carry && k < kept; ++k) {
            const ModuleWatch& other = moduleWatches[k];
            carry = !(JS_IsStrictEqual(ctx, other.promise, watch.promise) && other.key == watch.key &&
                      other.generation == watch.generation);
        }
        if (!carry) {
            JS_FreeValue(ctx, watch.promise);
            continue;
        }
        watch.carried = true;
        if (kept != i) {
            moduleWatches[kept] = std::move(watch);
        }
        ++kept;
    }
    moduleWatches.erase(moduleWatches.begin() + static_cast<std::ptrdiff_t>(kept), moduleWatches.end());
    // Each watch is a distinct instance's, so the carried list can never outgrow the instances.
    assert(moduleWatches.size() <= moduleInstances);
}

// ---- refusals -------------------------------------------------------------------------------------------------

void Vm::Impl::refuse(std::string_view name, const std::string& message) {
    assert(attempt != nullptr);
    attempt->refused = true;
    // The FIRST refusal of the call's own attempt decides its LoadError; a dynamic attempt's is the script's to catch.
    if (attempt->decidesStatus && !callLoadFailure.has_value()) {
        callLoadFailure = ScriptError{std::string{name}, message, {}};
    }
    throwLoadError(name, message);
}

// The error is created with an EMPTY message and its `message` set from the full text: the backend formats its own
// messages into a 256-byte buffer, and a '%' in a path must never be read as a format directive. Created with no
// bytecode frame on the stack, it has no stack text. Every failure on the way is an out-of-memory, left pending.
void Vm::Impl::throwLoadError(std::string_view name, const std::string& message) {
    HeldValue error{ctx, name == "SyntaxError" ? JS_NewSyntaxError(ctx, "%s", "") : JS_NewTypeError(ctx, "%s", "")};
    if (error.isException()) {
        return;
    }
    HeldValue text{ctx, JS_NewStringLen(ctx, message.data(), message.size())};
    if (text.isException()) {
        return;  // tested first: JS_DefinePropertyValueStr would store JS_EXCEPTION as the message
    }
    if (JS_DefinePropertyValueStr(ctx, error.get(), "message", text.release(),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        return;
    }
    static_cast<void>(JS_Throw(ctx, error.release()));  // consumes the error
}

// The pending error -- the parser's, left behind by a compile the heap limit starved -- becomes the backend's own
// out-of-memory error, with the parser's error as its `cause` when that fits (a failed definition costs only the
// cause). An error that already is the backend's out-of-memory error stays.
void Vm::Impl::replaceWithOutOfMemory() {
    HeldValue cause{ctx, JS_GetException(ctx)};
    // The backend's own out-of-memory error -- the one left without its message included -- stays as it is, never the
    // cause of a fresh one.
    if (isOutOfMemoryError(cause.get()) || isMessagelessInternalError(cause.get())) {
        static_cast<void>(JS_Throw(ctx, cause.release()));
        return;
    }
    static_cast<void>(JS_ThrowOutOfMemory(ctx));
    HeldValue error{ctx, JS_GetException(ctx)};
    if (JS_IsObject(error.get()) && JS_IsError(cause.get())) {
        // consumes the cause; a failure leaves an out-of-memory error pending, which the throw below replaces
        static_cast<void>(JS_DefinePropertyValueStr(ctx, error.get(), "cause", cause.release(),
                                                    JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE));
    }
    static_cast<void>(JS_Throw(ctx, error.release()));
}

// A module's long-lived handle is its address; a value is minted only to be consumed (JS_EvalFunction). The backend's
// own JS_NewModuleValue is exactly this (quickjs.c:30955-30958), and the module list's reference keeps the count above
// zero (quickjs.c:6778-6779 aborts at zero).
HeldValue Vm::Impl::moduleValue(JSModuleDef* module) const {
    return HeldValue{ctx, JS_DupValue(ctx, JS_MKPTR(JS_TAG_MODULE, module))};
}

char* Vm::Impl::duplicateName(const std::string& name) const {
    auto* copy = static_cast<char*>(js_malloc(ctx, name.size() + 1));  // the backend js_frees it
    if (copy == nullptr) {
        return nullptr;  // out of memory, already thrown
    }
    std::memcpy(copy, name.c_str(), name.size() + 1);
    return copy;
}

// ---- the backend's three module callbacks ---------------------------------------------------------------------
// Each is noexcept and catches everything: the bodies allocate, and nothing may unwind through the backend's C frames.

char* Vm::Impl::normalizeModuleName(JSContext* /*context*/, const char* base, const char* specifier,
                                    void* opaque) noexcept {
    try {
        Impl& self = *static_cast<Impl*>(opaque);
        if (self.attempt == nullptr) {
            // No engine load is running: a dynamic import(), arriving from a job.
            return self.serveDynamicImport(base, specifier);
        }
        if (!self.attempt->loading) {
            // Unreachable: no module request is made while a graph links or evaluates.
            self.throwLoadError("TypeError",
                                std::format("cannot resolve '{}' imported by '{}': a module was requested while a "
                                            "module graph links or evaluates",
                                            specifier, base));
            return nullptr;
        }
        if (self.attempt->refusesImports) {  // the cook: a cooked script imports nothing
            self.refuse("TypeError", internal::cookImportMessage(self.callFileName, specifier));
            return nullptr;
        }
        // A static import, resolved while one of the engine's compiles runs.
        const internal::SpecifierResolution resolved = internal::resolveModuleSpecifier(specifier, base);
        const std::optional<std::string> importer = internal::importerKey(base);
        const std::string importerText = importer.has_value() ? *importer : std::string{base};
        if (resolved.refusal != internal::SpecifierRefusal::None) {
            self.refuse("TypeError", internal::refusedSpecifierMessage(specifier, importerText, resolved.refusal));
            return nullptr;
        }
        if (importer.has_value()) {
            self.moduleTable.addImport(*importer, resolved.key, internal::ImportKind::Static);
        }
        self.attempt->importer = importerText;
        // When that name is already loaded the backend uses it; otherwise it asks loadModule for it.
        const std::uint32_t generation = self.moduleTable.generation(resolved.key);
        return self.duplicateName(internal::encodeModuleName(resolved.key, generation));
    } catch (...) {
        std::terminate();
    }
}

JSModuleDef* Vm::Impl::loadModule(JSContext* /*context*/, const char* name, void* opaque,
                                  JSValueConst /*attributes*/) noexcept {
    try {
        Impl& self = *static_cast<Impl*>(opaque);
        if (self.attempt == nullptr || !self.attempt->loading) {
            // Unreachable: the backend asks for a module only after normalizeModuleName produced its name.
            self.throwLoadError("TypeError", "a module was requested outside a module load");
            return nullptr;
        }
        const std::optional<internal::ModuleName> decoded = internal::decodeModuleName(name);
        if (!decoded.has_value()) {
            assert(false && "normalizeModuleName produces only names that decode");
            self.throwLoadError("TypeError", "a module was requested outside a module load");
            return nullptr;
        }
        // Defence in depth: the backend asks for a name only when its list holds no module of that name, so a current
        // instance the table already has means the name did not find it -- refuse rather than compile a key twice.
        if (self.moduleTable.instance(decoded->key) != nullptr) {
            self.refuse("TypeError", internal::alreadyLoadedMessage(decoded->key, self.attempt->importer));
            return nullptr;
        }
        // A copy: a nested import's normalizer rewrites attempt->importer while this compile runs.
        const std::string importer = self.attempt->importer;
        return self.compileModule(decoded->key, importer);
    } catch (...) {
        std::terminate();
    }
}

// Keys are checked, never values ("only the keys should be tested", quickjs-libc.c:799-800). The static check runs
// in the parser, inside a loading attempt, and only for a clause with a key; the dynamic one runs at the import() call
// itself, for an empty `with: {}` too -- so an empty set is accepted and any key is refused.
int Vm::Impl::checkImportAttributes(JSContext* context, void* opaque, JSValueConst attributes) noexcept {
    try {
        Impl& self = *static_cast<Impl*>(opaque);
        JSPropertyEnum* table = nullptr;
        std::uint32_t length = 0;
        constexpr int OWN_STRING_KEYS = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
        if (JS_GetOwnPropertyNames(context, &table, &length, attributes, OWN_STRING_KEYS) < 0) {
            return -1;
        }
        if (length == 0) {
            JS_FreePropertyEnum(context, table, length);
            return 0;
        }
        std::string key;
        bool keyRead = false;
        {
            std::size_t keyLength = 0;
            const char* keyText = JS_AtomToCStringLen(context, &keyLength, table[0].atom);
            if (keyText != nullptr) {
                key.assign(keyText, keyLength);
                keyRead = true;
                JS_FreeCString(context, keyText);
            }
        }
        JS_FreePropertyEnum(context, table, length);
        if (!keyRead) {
            return -1;  // out of memory, already thrown
        }
        if (self.attempt != nullptr && self.attempt->loading) {
            // The static path: the parser of the module being compiled is running.
            const std::vector<std::string>& compiling = self.attempt->compiling;
            const std::string module = compiling.empty() ? std::string{} : compiling.back();
            self.refuse("SyntaxError", internal::unsupportedAttributeMessage(key, module));
        } else {
            self.throwLoadError("TypeError", internal::unsupportedAttributeMessage(key, {}));
        }
        return -1;
    } catch (...) {
        std::terminate();
    }
}

// ---- Vm's public face -----------------------------------------------------------------------------------------

EvalResult Vm::evaluateModule(std::string_view virtualPath) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: evaluateModule on an invalid Vm (a failed create or a moved-from Vm)");
        return EvalResult{EvalStatus::InvalidVm, {}, {}};
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: evaluateModule called from a thread that is not the VM's; refused");
        return EvalResult{EvalStatus::WrongThread, {}, {}};
    }
    return impl->evaluateModuleCall(virtualPath);
}

std::vector<ModuleRecord> Vm::moduleRecords() const {
    if (impl == nullptr) {
        return {};  // the stats() precedent: an invalid Vm has nothing to report, silently
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: moduleRecords called from a thread that is not the VM's; refused");
        return {};
    }
    return impl->moduleTable.records();
}

// The reload hook. It changes the table only -- no read, no compile, no backend call: the next load of each changed
// path reads the VFS again under its next generation name. Nothing is logged on success.
std::vector<std::string> Vm::invalidateModule(std::string_view virtualPath) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: invalidateModule on an invalid Vm (a failed create or a moved-from Vm)");
        return {};
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: invalidateModule called from a thread that is not the VM's; refused");
        return {};
    }
    const internal::SpecifierResolution path = internal::resolveModuleSpecifier(virtualPath, {});
    if (path.refusal != internal::SpecifierRefusal::None) {
        AERO_LOG_ERROR("script: invalidateModule: '{}' is not an absolute res:// module path", virtualPath);
        return {};
    }
    return impl->moduleTable.invalidate(path.key);
}

}  // namespace engine::script
