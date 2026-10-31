// Aero Engine -- engine::script::Vm (task 4.3.3): script classes, their instances, and the reload that moves them. A
// script is an ES module whose default export is a class; the VM holds each loaded script's last good class and every
// instance it makes behind a ScriptInstance handle. The contract is vm.hpp's.
//
// The order of loadScriptCall is where the correctness lives:
//   1. the comparison, BEFORE any call: an unchanged file runs nothing, opens no call and costs one read;
//   2. one call: the conditional invalidation, then the entry's evaluation, which captures the text it compiled;
//   3. the state check before the namespace read, then the class check, which runs no script code;
//   4. every live instance is constructed anew, in creation order, into a batch nothing else can see;
//   5. the call's job drain and tail;
//   6. the commit, only now: a job that threw or a budget that fired in the report refuses the load as well.

#include <aero/core/log.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

namespace {

// An atom the wrapper owns; JS_ATOM_NULL frees as a no-op.
class HeldAtom {
public:
    HeldAtom(JSContext* context, JSAtom held) noexcept : context(context), atom(held) {}
    HeldAtom(const HeldAtom&) = delete;
    HeldAtom& operator=(const HeldAtom&) = delete;
    HeldAtom(HeldAtom&&) = delete;
    HeldAtom& operator=(HeldAtom&&) = delete;
    ~HeldAtom() { JS_FreeAtom(context, atom); }

    [[nodiscard]] JSAtom get() const noexcept { return atom; }

private:
    JSContext* context;
    JSAtom atom;
};

// The three values a property descriptor read may hand over; each is freed on every path.
class HeldDescriptor {
public:
    explicit HeldDescriptor(JSContext* context) noexcept : context(context) {}
    HeldDescriptor(const HeldDescriptor&) = delete;
    HeldDescriptor& operator=(const HeldDescriptor&) = delete;
    HeldDescriptor(HeldDescriptor&&) = delete;
    HeldDescriptor& operator=(HeldDescriptor&&) = delete;
    ~HeldDescriptor() {
        JS_FreeValue(context, desc.value);
        JS_FreeValue(context, desc.getter);
        JS_FreeValue(context, desc.setter);
    }

    JSPropertyDescriptor desc{0, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};

private:
    JSContext* context;
};

enum class ClassCheck : std::uint8_t {
    IsClass,
    NotAClass,
    Failed,  // an exception is pending: only a refused allocation can cause one
};

// A class is a constructor whose own `prototype` is a non-writable data property. An ordinary function's `prototype`
// is writable, a bound function has none, and a class defines it read-only (quickjs.c:17203-17210, 17292-17294).
// JS_IsProxy comes first, because a Proxy's descriptor read would run its trap; JS_GetOwnProperty runs no getter.
[[nodiscard]] ClassCheck checkClass(JSContext* ctx, JSValueConst value) {
    if (!JS_IsObject(value) || JS_IsProxy(value) || !JS_IsConstructor(ctx, value)) {
        return ClassCheck::NotAClass;
    }
    const HeldAtom prototype{ctx, JS_NewAtom(ctx, "prototype")};
    if (prototype.get() == JS_ATOM_NULL) {
        return ClassCheck::Failed;  // out of memory; nothing is pending, trap takes the sentinel
    }
    HeldDescriptor own{ctx};
    const int has = JS_GetOwnProperty(ctx, &own.desc, value, prototype.get());
    if (has < 0) {
        return ClassCheck::Failed;
    }
    if (has == 0 || (own.desc.flags & (JS_PROP_GETSET | JS_PROP_WRITABLE)) != 0) {
        return ClassCheck::NotAClass;
    }
    return ClassCheck::IsClass;
}

[[nodiscard]] ScriptStatus scriptStatusOf(EvalStatus status) noexcept {
    switch (status) {
        case EvalStatus::Ok:
            return ScriptStatus::Ok;
        case EvalStatus::CompileError:
            return ScriptStatus::CompileError;
        case EvalStatus::Exception:
            return ScriptStatus::Exception;
        case EvalStatus::OutOfMemory:
            return ScriptStatus::OutOfMemory;
        case EvalStatus::Interrupted:
            return ScriptStatus::Interrupted;
        case EvalStatus::InvalidVm:
            return ScriptStatus::InvalidVm;
        case EvalStatus::WrongThread:
            return ScriptStatus::WrongThread;
        case EvalStatus::LoadError:
            return ScriptStatus::LoadError;
    }
    return ScriptStatus::Exception;
}

[[nodiscard]] ScriptLoadResult loadResultOf(ScriptStatus status) {
    ScriptLoadResult result;
    result.status = status;
    return result;
}

[[nodiscard]] ScriptLoadResult loadResultOf(const EvalResult& evaluation) {
    ScriptLoadResult result;
    result.status = scriptStatusOf(evaluation.status);
    result.error = evaluation.error;
    return result;
}

[[nodiscard]] ScriptInstanceResult instanceResultOf(ScriptStatus status) {
    ScriptInstanceResult result;
    result.status = status;
    return result;
}

[[nodiscard]] ScriptError refusalError(const ScriptRefusal& refusal) { return ScriptError{{}, refusal.text, {}}; }

}  // namespace

// ---- the load --------------------------------------------------------------------------------------------------

ScriptLoadResult Vm::Impl::loadScriptCall(std::string_view virtualPath) {
    assert(!inCall);  // a host function never calls back into the VM (script_api.hpp)
    const internal::SpecifierResolution entry = internal::resolveModuleSpecifier(virtualPath, {});
    if (entry.refusal != internal::SpecifierRefusal::None || modules == nullptr) {
        // evaluateModuleCall's own refusal branch: one call, its one LoadError record, its drain. No record is made.
        return loadResultOf(evaluateModuleCall(virtualPath));
    }
    const std::string& key = entry.key;

    // 1. The comparison, before any call. A failed peek compares equal to nothing; the loader reports it.
    const std::optional<std::string> peeked = peekModuleSource(key);
    if (const auto known = scripts.find(key); known != scripts.end() && peeked.has_value()) {
        if (known->second.runningText == peeked) {
            return loadResultOf(ScriptStatus::Unchanged);
        }
    }
    ScriptRecord& record = scripts[key];  // map nodes never move: the reference survives every later step

    // 2. One call. ~CallScope clears loadingScript with the other per-call state.
    const CallScope scope{*this, key};
    loadingScript = true;
    if (moduleTable.instance(key) != nullptr) {
        static_cast<void>(moduleTable.invalidate(key));  // so the entry compiles, and the capture sees its bytes
    }
    EvalResult evaluation{EvalStatus::Ok, {}, {}};
    std::optional<std::string> compiled;  // the bytes the loader compiled (readModuleSource), not the peek's
    evaluateEntry(key, evaluation, &compiled);

    // 3. The class: nothing is read unless the evaluation completed.
    std::optional<ScriptRefusal> refusal;
    HeldValue newClass = takeScriptClass(key, evaluation, refusal);

    // 4. Every live instance, in creation order. Nothing here touches a slot or the record.
    HeldBatch fresh{ctx};
    if (!JS_IsUndefined(newClass.get())) {
        rebuildInstances(record, newClass.get(), evaluation, refusal, fresh);
    }

    // 5. The call's jobs and tail.
    endScriptCall(evaluation);

    // 6. Commit only now.
    const bool succeeded = evaluation.status == EvalStatus::Ok && !refusal.has_value();
    assert(!succeeded || compiled.has_value());  // an Ok load compiled its entry
    if (succeeded && compiled.has_value()) {
        assert(fresh.size() == record.instances.size());
        for (std::size_t i = 0; i < record.instances.size(); ++i) {
            InstanceSlot* const slot = instanceSlots.get(record.instances[i]);
            assert(slot != nullptr);
            if (slot != nullptr) {
                JS_FreeValue(ctx, slot->object);
                slot->object = fresh.take(i);
            }
        }
        JS_FreeValue(ctx, record.scriptClass);
        record.scriptClass = newClass.release();
        record.runningText = std::move(compiled);
        ScriptLoadResult done = loadResultOf(ScriptStatus::Ok);
        done.instancesMoved = record.instances.size();
        return done;
    }
    if (refusal.has_value()) {
        ScriptLoadResult refused = loadResultOf(refusal->status);
        refused.error = refusalError(*refusal);
        return refused;
    }
    return loadResultOf(evaluation);
}

HeldValue Vm::Impl::takeScriptClass(std::string_view key, EvalResult& evaluation,
                                    std::optional<ScriptRefusal>& refusal) {
    // The loader already logged its one record.
    if (evaluation.status != EvalStatus::Ok) {
        return HeldValue{ctx, JS_UNDEFINED};
    }
    // EvalStatus::Ok also covers a pending top-level await, and a pending module may already expose a constructible
    // class, so the state is checked before the namespace is read.
    if (moduleTable.state(key) != ModuleState::Evaluated) {
        refusal = ScriptRefusal{ScriptStatus::StillEvaluating,
                                "not a script: its top-level await had not settled when the load ended"};
        logRefusal(*refusal);
        return HeldValue{ctx, JS_UNDEFINED};
    }
    const auto notAClass = [&](JSValueConst value) {
        refusal = ScriptRefusal{
            ScriptStatus::NotAClass,
            std::format("not a script: its default export is {}, not a class", describeValue(ctx, value, scriptApi))};
        logRefusal(*refusal);
    };
    auto* const module = static_cast<JSModuleDef*>(moduleTable.instance(key));
    if (module == nullptr) {
        notAClass(JS_UNDEFINED);  // an Evaluated key always has its instance; kept for safety
        return HeldValue{ctx, JS_UNDEFINED};
    }
    // The current generation's namespace is live: it reads each binding's value now, and runs no script code.
    const HeldValue names{ctx, JS_GetModuleNamespace(ctx, module)};
    if (names.isException()) {
        adopt(evaluation, trap(Phase::Run));
        return HeldValue{ctx, JS_UNDEFINED};
    }
    HeldValue value{ctx, JS_GetPropertyStr(ctx, names.get(), "default")};
    if (value.isException()) {
        adopt(evaluation, trap(Phase::Run));  // a read in the temporal dead zone: unreachable once Evaluated
        return HeldValue{ctx, JS_UNDEFINED};
    }
    switch (checkClass(ctx, value.get())) {
        case ClassCheck::IsClass:
            return HeldValue{ctx, value.release()};
        case ClassCheck::NotAClass:
            notAClass(value.get());
            return HeldValue{ctx, JS_UNDEFINED};
        case ClassCheck::Failed:
            adopt(evaluation, trap(Phase::Run));
            return HeldValue{ctx, JS_UNDEFINED};
    }
    return HeldValue{ctx, JS_UNDEFINED};
}

HeldValue Vm::Impl::constructInstance(JSValueConst scriptClass, EvalResult& evaluation,
                                      std::optional<ScriptRefusal>& refusal) {
    HeldValue made{ctx, JS_CallConstructor(ctx, scriptClass, 0, nullptr)};
    if (made.isException()) {
        adopt(evaluation, trap(Phase::Run));
        return HeldValue{ctx, JS_UNDEFINED};
    }
    // A Proxy would run script code at every later key read, so no live instance is one.
    if (JS_IsProxy(made.get())) {
        refusal = ScriptRefusal{ScriptStatus::NotAClass, "not a script: its constructor returned a Proxy"};
        logRefusal(*refusal);
        return HeldValue{ctx, JS_UNDEFINED};
    }
    return HeldValue{ctx, made.release()};
}

void Vm::Impl::rebuildInstances(const ScriptRecord& record, JSValueConst scriptClass, EvalResult& evaluation,
                                std::optional<ScriptRefusal>& refusal, HeldBatch& fresh) {
    for (std::size_t i = 0; i < record.instances.size(); ++i) {
        HeldValue made = constructInstance(scriptClass, evaluation, refusal);
        if (!JS_IsObject(made.get())) {
            return;  // the failure or the refusal is recorded; the batch frees what it holds
        }
        fresh.add(made.release());
    }
}

void Vm::Impl::endScriptCall(EvalResult& evaluation) {
    if (evaluation.status != EvalStatus::Interrupted && !interruptFired) {
        drainJobs(evaluation);
    }
    observeModuleWatches();
    noteInterrupt(evaluation);
    reportUnhandledRejections();
    noteInterrupt(evaluation);
    clearPendingException();
}

void Vm::Impl::logRefusal(const ScriptRefusal& refusal) const {
    AERO_LOG_ERROR("script: {}: {}", callFileName, refusal.text);
}

// ---- create and destroy ----------------------------------------------------------------------------------------

ScriptInstanceResult Vm::Impl::createInstanceCall(std::string_view virtualPath) {
    assert(!inCall);
    const internal::SpecifierResolution entry = internal::resolveModuleSpecifier(virtualPath, {});
    const bool resolved = entry.refusal == internal::SpecifierRefusal::None;
    const auto found = resolved ? scripts.find(entry.key) : scripts.end();
    if (found == scripts.end() || JS_IsUndefined(found->second.scriptClass)) {
        AERO_LOG_ERROR("script: {}: createInstance refused: no script class is loaded from it",
                       resolved ? std::string_view{entry.key} : virtualPath);
        return instanceResultOf(ScriptStatus::NotLoaded);
    }
    ScriptRecord& record = found->second;
    const CallScope scope{*this, entry.key};
    EvalResult evaluation{EvalStatus::Ok, {}, {}};
    std::optional<ScriptRefusal> refusal;
    HeldValue made = constructInstance(record.scriptClass, evaluation, refusal);
    endScriptCall(evaluation);
    if (evaluation.status == EvalStatus::Ok && !refusal.has_value() && JS_IsObject(made.get())) {
        ScriptInstanceResult done = instanceResultOf(ScriptStatus::Ok);
        done.instance = instanceSlots.insert(InstanceSlot{made.release(), entry.key});
        record.instances.push_back(done.instance);
        return done;
    }
    if (refusal.has_value()) {
        ScriptInstanceResult refused = instanceResultOf(refusal->status);
        refused.error = refusalError(*refusal);
        return refused;
    }
    ScriptInstanceResult failed = instanceResultOf(scriptStatusOf(evaluation.status));
    failed.error = evaluation.error;
    return failed;
}

ScriptStatus Vm::Impl::destroyInstanceCall(ScriptInstance instance) {
    assert(!inCall);
    InstanceSlot* const slot = instanceSlots.get(instance);
    if (slot == nullptr) {
        AERO_LOG_ERROR("script: destroyInstance refused: the handle names no live instance");
        return ScriptStatus::UnknownInstance;
    }
    if (const auto home = scripts.find(slot->key); home != scripts.end()) {
        std::erase(home->second.instances, instance);
    }
    JS_FreeValue(ctx, slot->object);
    static_cast<void>(instanceSlots.remove(instance));
    return ScriptStatus::Ok;
}

void Vm::Impl::releaseScripts() noexcept {
    for (auto& entry : scripts) {
        ScriptRecord& record = entry.second;
        for (const ScriptInstance instance : record.instances) {
            if (InstanceSlot* const slot = instanceSlots.get(instance); slot != nullptr) {
                JS_FreeValue(ctx, slot->object);
                slot->object = JS_UNDEFINED;
            }
        }
        JS_FreeValue(ctx, record.scriptClass);
        record.scriptClass = JS_UNDEFINED;
    }
    scripts.clear();
    instanceSlots.clear();
}

// ---- Vm's public face ------------------------------------------------------------------------------------------

ScriptLoadResult Vm::loadScript(std::string_view virtualPath) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: loadScript on an invalid Vm (a failed create or a moved-from Vm)");
        return loadResultOf(ScriptStatus::InvalidVm);
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: loadScript called from a thread that is not the VM's; refused");
        return loadResultOf(ScriptStatus::WrongThread);
    }
    ScriptLoadResult result = impl->loadScriptCall(virtualPath);
    // Read here, after the call's holders released what they held: the figures equal stats() right after.
    result.heapBytesInUse = impl->heap.bytesInUse();
    result.heapLimitBytes = impl->heap.limitBytes();
    return result;
}

ScriptInstanceResult Vm::createInstance(std::string_view virtualPath) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: createInstance on an invalid Vm (a failed create or a moved-from Vm)");
        return instanceResultOf(ScriptStatus::InvalidVm);
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: createInstance called from a thread that is not the VM's; refused");
        return instanceResultOf(ScriptStatus::WrongThread);
    }
    return impl->createInstanceCall(virtualPath);
}

ScriptStatus Vm::destroyInstance(ScriptInstance instance) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: destroyInstance on an invalid Vm (a failed create or a moved-from Vm)");
        return ScriptStatus::InvalidVm;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: destroyInstance called from a thread that is not the VM's; refused");
        return ScriptStatus::WrongThread;
    }
    return impl->destroyInstanceCall(instance);
}

std::string_view scriptStatusLabel(ScriptStatus status) noexcept {
    switch (status) {
        case ScriptStatus::Ok:
            return "Ok";
        case ScriptStatus::Unchanged:
            return "Unchanged";
        case ScriptStatus::InvalidVm:
            return "InvalidVm";
        case ScriptStatus::WrongThread:
            return "WrongThread";
        case ScriptStatus::LoadError:
            return "LoadError";
        case ScriptStatus::CompileError:
            return "CompileError";
        case ScriptStatus::Exception:
            return "Exception";
        case ScriptStatus::OutOfMemory:
            return "OutOfMemory";
        case ScriptStatus::Interrupted:
            return "Interrupted";
        case ScriptStatus::StillEvaluating:
            return "StillEvaluating";
        case ScriptStatus::NotAClass:
            return "NotAClass";
        case ScriptStatus::NotLoaded:
            return "NotLoaded";
        case ScriptStatus::UnknownInstance:
            return "UnknownInstance";
    }
    return "Unknown";
}

// ---- the test seam ---------------------------------------------------------------------------------------------

bool internal::VmAccess::defineInstanceGlobal(Vm& vm, ScriptInstance instance, std::string_view name) {
    if (vm.impl == nullptr || !vm.impl->onVmThread()) {
        return false;
    }
    Vm::Impl& host = *vm.impl;
    const InstanceSlot* const slot = host.instanceSlots.get(instance);
    if (slot == nullptr) {
        return false;
    }
    // A define, not a set: no setter a script put on the global object runs. It consumes the value either way.
    const HeldValue global{host.ctx, JS_GetGlobalObject(host.ctx)};
    const std::string cName{name};
    const int defined = JS_DefinePropertyValueStr(host.ctx, global.get(), cName.c_str(),
                                                  JS_DupValue(host.ctx, slot->object), JS_PROP_C_W_E);
    if (defined < 0) {
        host.clearPendingException();
        return false;
    }
    return defined == 1;
}

}  // namespace engine::script
