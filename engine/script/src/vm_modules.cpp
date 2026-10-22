// Aero Engine -- engine::script::Vm (task 4.1.2): ES modules through the VFS. The backend's three module callbacks,
// the one place a source becomes a module (compileModule), the entry point, and the cache's public face. The rules --
// the grammar, the names, the table, the texts -- are module_graph.hpp's; this file is the sequence of backend calls
// around them.

#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include "vm_impl.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <optional>
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
    assert(!inCall);  // no host function exists, so nothing can re-enter the VM
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
        logFailure(EvalStatus::LoadError, result.error, /*isErrorObject=*/true);
        drainJobs(result);  // jobs an earlier call queued still run, as after evaluateScript's CompileError
    } else {
        evaluateEntry(entry.key, result);
    }
    noteInterrupt(result);  // the 4.1.1 tail, unchanged
    reportUnhandledRejections();
    noteInterrupt(result);
    clearPendingException();
    return result;
}

void Vm::Impl::evaluateEntry(std::string_view key, EvalResult& result) {
    LoadAttempt entryAttempt{/*decides=*/true};
    std::uint64_t opened = rejectionSequence;  // the evaluation window is (opened, closed]
    const HeldValue promise = startModule(entryAttempt, key, opened);
    const std::uint64_t closed = rejectionSequence;
    if (promise.isException()) {
        result = trap(Phase::Compile);  // LoadError, CompileError (a link error included), OutOfMemory, Interrupted
        moduleTable.retire(entryAttempt.created);
        if (result.status != EvalStatus::Interrupted && !interruptFired) {
            drainJobs(result);
        }
        return;
    }
    const auto settle = [&] { return settleModule(key, promise.get(), opened, closed, ImportPath::Static); };
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
        } else if (JS_PromiseState(ctx, promise.get()) == JS_PROMISE_PENDING) {
            AERO_LOG_WARN(
                "script: {}: module evaluation is still pending at the end of the call (a top-level await has not "
                "settled)",
                callFileName);
        }
    }
}

// Steps 4-5 of the entry (and of the dynamic path): inside the attempt's scope, find or compile the graph, then link
// it and start its evaluation. The result is the module promise, or JS_EXCEPTION with the failure pending (a load
// refusal, a compile or link error, an out-of-memory, the budget) -- the caller traps it while it is still pending and
// retires what the attempt created. windowBegin is the rejection stamp before JS_EvalFunction; the caller reads the
// end.
HeldValue Vm::Impl::startModule(LoadAttempt& loadAttempt, std::string_view key, std::uint64_t& windowBegin) {
    const AttemptScope scope{*this, loadAttempt};
    auto* module = static_cast<JSModuleDef*>(moduleTable.instance(key));
    if (module == nullptr) {
        module = compileModule(key, loadAttempt.importer);
        if (module == nullptr) {
            return HeldValue{ctx, JS_EXCEPTION};
        }
    }
    loadAttempt.loading = false;
    windowBegin = rejectionSequence;
    return HeldValue{ctx, JS_EvalFunction(ctx, moduleValue(module).release())};  // consumes the minted reference
}

// ---- import() -------------------------------------------------------------------------------------------------

// A normalizer call that arrives while no engine load is running IS a dynamic import(). It is served like an entry:
// resolved by the same grammar, loaded, linked and evaluated by the engine itself, and only then handed to the backend
// by name -- so a link failure is seen here (the backend never links a module twice), the module body's internal
// rejection is attributed inside the engine's window, and the edge is recorded. A dynamic failure never changes the
// call's status: it rejects the import(), which the script may catch.
char* Vm::Impl::serveDynamicImport(const char* base, const char* specifier) {
    const internal::SpecifierResolution target = internal::resolveModuleSpecifier(specifier, base);
    const std::optional<std::string> importer = internal::importerKey(base);
    const std::string importerText = importer.has_value() ? *importer : std::string{base};
    if (target.refusal != internal::SpecifierRefusal::None) {
        throwLoadError("TypeError", internal::refusedSpecifierMessage(specifier, importerText, target.refusal));
        return nullptr;  // the import() rejects; a dynamic refusal is the script's to catch, never the call's status
    }
    if (importer.has_value()) {
        moduleTable.addImport(*importer, target.key, internal::ImportKind::Dynamic);  // global code has no record
    }
    if (modules == nullptr) {
        throwLoadError("TypeError", internal::noModuleSourceMessage(target.key, importerText));
        return nullptr;
    }
    LoadAttempt dynamicAttempt{/*decides=*/false};
    dynamicAttempt.importer = importerText;
    std::uint64_t opened = rejectionSequence;  // the evaluation window is (opened, closed]
    const HeldValue promise = startModule(dynamicAttempt, target.key, opened);
    const std::uint64_t closed = rejectionSequence;
    if (promise.isException()) {
        moduleTable.retire(dynamicAttempt.created);
        return nullptr;  // the failure stays pending: the backend rejects the import() with it, and never sees the name
    }
    static_cast<void>(settleModule(target.key, promise.get(), opened, closed, ImportPath::Dynamic));
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
    constexpr int MODULE_COMPILE = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
    const HeldValue compiled{ctx, JS_Eval(ctx, text->c_str(), text->size(), name.c_str(), MODULE_COMPILE)};
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

// ---- a module's outcome ---------------------------------------------------------------------------------------

std::optional<EvalResult> Vm::Impl::settleModule(std::string_view key, JSValueConst promise, std::uint64_t windowBegin,
                                                 std::uint64_t windowEnd, ImportPath path) {
    switch (JS_PromiseState(ctx, promise)) {
        case JS_PROMISE_FULFILLED:
            moduleTable.markEvaluatedGraph(key);
            return std::nullopt;
        case JS_PROMISE_REJECTED: {
            moduleTable.markFailed(key);
            const HeldValue reason{ctx, JS_PromiseResult(ctx, promise)};
            std::optional<EvalResult> failure;
            if (path == ImportPath::Static) {
                failure = trapValue(reason.get(), Phase::Run);  // classified, described, logged once
            }
            forgetModuleFailure(promise, reason.get(), windowBegin, windowEnd, path);
            JS_ClearUncatchableError(ctx, reason.get());  // D10: every later import rethrows this very object
            return failure;
        }
        case JS_PROMISE_PENDING:
            moduleTable.markEvaluating(key);
            return std::nullopt;
        case JS_PROMISE_NOT_A_PROMISE:
            break;
    }
    assert(false && "JS_EvalFunction on a module returns a promise (quickjs.c:31540-31590)");
    return std::nullopt;
}

// A module body that throws rejects TWO promises with one reason -- the backend's internal promise for the body and
// the module promise -- and both reach the tracker unhandled. The rejections recorded inside the engine's evaluation
// window with the module's reason are that one failure; the stamps are pre-incremented, so "inside" is (begin, end].
void Vm::Impl::forgetModuleFailure(JSValueConst promise, JSValueConst reason, std::uint64_t windowBegin,
                                   std::uint64_t windowEnd, ImportPath path) {
    bool kept = false;
    for (auto entry = pendingRejections.begin(); entry != pendingRejections.end();) {
        const bool isModulePromise = JS_IsStrictEqual(ctx, entry->promise, promise);
        const bool inWindow = entry->sequence > windowBegin && entry->sequence <= windowEnd;
        if (isModulePromise && path == ImportPath::Static) {
            JS_FreeValue(ctx, entry->reason);
            keepSkipped(entry->promise);  // takes the listed reference: a later import's handler is silent
            kept = true;
            entry = pendingRejections.erase(entry);
        } else if (!isModulePromise && inWindow && JS_IsStrictEqual(ctx, entry->reason, reason)) {
            JS_FreeValue(ctx, entry->promise);  // the backend's internal promise for the module body
            JS_FreeValue(ctx, entry->reason);
            entry = pendingRejections.erase(entry);
        } else {
            ++entry;  // a Dynamic module promise stays listed: the backend attaches its own handler right after
        }
    }
    if (path == ImportPath::Static && !kept && !isSkipped(promise)) {
        keepSkipped(JS_DupValue(ctx, promise));  // a cached failure re-observed, not kept yet
    }
}

// ---- refusals -------------------------------------------------------------------------------------------------

void Vm::Impl::refuse(std::string_view name, const std::string& message) {
    assert(attempt != nullptr);
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

}  // namespace engine::script
