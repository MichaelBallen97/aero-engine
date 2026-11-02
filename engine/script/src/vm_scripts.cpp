// Aero Engine -- engine::script::Vm (task 4.3.3): script classes, their instances, and the reload that moves them. A
// script is an ES module whose default export is a class; the VM holds each loaded script's last good class and every
// instance it makes behind a ScriptInstance handle. The contract is vm.hpp's.
//
// The order of loadScriptCall is where the correctness lives:
//   1. the comparison, BEFORE any call: an unchanged file runs nothing, opens no call and costs one read;
//   2. one call: the conditional invalidation, then the entry's evaluation, which captures the text it compiled;
//   3. the state check before the namespace read, then the class check, which runs no script code;
//   4. every live instance is constructed anew, in creation order, and each field of the running instance whose kind
//      did not change is carried into it, all into a batch nothing else can see;
//   5. the call's job drain and tail;
//   6. the commit, only now: a job that threw or a budget that fired in the report refuses the load as well.

#include <aero/core/log.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/internal/script_carry.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <array>
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

// The own property names of one object, freed on every path; JS_GetOwnPropertyNames leaves a null table on failure.
class HeldPropertyEnum {
public:
    explicit HeldPropertyEnum(JSContext* context) noexcept : context(context) {}
    HeldPropertyEnum(const HeldPropertyEnum&) = delete;
    HeldPropertyEnum& operator=(const HeldPropertyEnum&) = delete;
    HeldPropertyEnum(HeldPropertyEnum&&) = delete;
    HeldPropertyEnum& operator=(HeldPropertyEnum&&) = delete;
    ~HeldPropertyEnum() { JS_FreePropertyEnum(context, table, length); }

    // The enumerable string-keyed names of `object`. False: an exception is pending (a refused allocation).
    [[nodiscard]] bool read(JSValueConst object) {
        return JS_GetOwnPropertyNames(context, &table, &length, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0;
    }
    [[nodiscard]] std::uint32_t size() const noexcept { return length; }
    [[nodiscard]] JSAtom atom(std::uint32_t index) const noexcept { return table[index].atom; }

private:
    JSContext* context;
    JSPropertyEnum* table = nullptr;
    std::uint32_t length = 0;
};

// The entity the base class takes during one engine-driven construction, set for the scope and gone on every path.
class PendingEntityScope {
public:
    PendingEntityScope(ScriptApi& api, const std::optional<EntityRef>& pending) : api(api) {
        api.pendingEntity = pending;
    }
    PendingEntityScope(const PendingEntityScope&) = delete;
    PendingEntityScope& operator=(const PendingEntityScope&) = delete;
    PendingEntityScope(PendingEntityScope&&) = delete;
    PendingEntityScope& operator=(PendingEntityScope&&) = delete;
    ~PendingEntityScope() { end(); }

    void end() noexcept { api.pendingEntity.reset(); }

private:
    ScriptApi& api;
};

// The carry's kind of a value, by tag and then by class id: never by `typeof`, under which an array, a Vec3, a Quat,
// an Entity and an object all read "object". No script code runs here.
[[nodiscard]] internal::ValueKind valueKindOf(JSContext* ctx, JSValueConst value, const ScriptApi& api) {
    using internal::ValueKind;
    if (JS_IsUndefined(value)) {
        return ValueKind::Undefined;
    }
    if (JS_IsNull(value)) {
        return ValueKind::Null;
    }
    if (JS_IsBool(value)) {
        return ValueKind::Boolean;
    }
    if (JS_IsNumber(value)) {
        return ValueKind::Number;
    }
    if (JS_IsBigInt(value)) {
        return ValueKind::BigInt;
    }
    if (JS_IsString(value)) {
        return ValueKind::String;
    }
    if (JS_IsSymbol(value)) {
        return ValueKind::Symbol;
    }
    if (JS_IsFunction(ctx, value)) {
        return ValueKind::Function;
    }
    if (JS_IsArray(value)) {
        return ValueKind::Array;
    }
    const JSClassID id = JS_GetClassID(value);
    if (id == api.vec3Class) {
        return ValueKind::Vec3;
    }
    if (id == api.quatClass) {
        return ValueKind::Quat;
    }
    if (id == api.entityClass) {
        return ValueKind::Entity;
    }
    return ValueKind::Object;
}

// How many fields an object has: its own, enumerable, string-keyed data properties. False: an exception is pending.
[[nodiscard]] bool countFields(JSContext* ctx, JSValueConst object, std::size_t& count) {
    HeldPropertyEnum names{ctx};
    if (!names.read(object)) {
        return false;
    }
    count = 0;
    for (std::uint32_t i = 0; i < names.size(); ++i) {
        HeldDescriptor own{ctx};
        const int has = JS_GetOwnProperty(ctx, &own.desc, object, names.atom(i));
        if (has < 0) {
            return false;
        }
        if (has > 0 && (own.desc.flags & JS_PROP_GETSET) == 0) {
            ++count;
        }
    }
    return true;
}

// One (running, fresh) pair. False: an exception is pending (only a refused allocation can cause one); the caller
// traps it and the load is refused. Writes only to `fresh`, and reads `running` through descriptors only, so no getter
// and no setter runs on either object.
[[nodiscard]] bool carryFields(JSContext* ctx, const ScriptApi& api, JSValueConst running, JSValueConst fresh,
                               FieldCounts& counts) {
    HeldPropertyEnum names{ctx};
    if (!names.read(running)) {
        return false;
    }
    std::size_t matched = 0;
    for (std::uint32_t i = 0; i < names.size(); ++i) {
        const JSAtom atom = names.atom(i);
        HeldDescriptor was{ctx};
        const int hadIt = JS_GetOwnProperty(ctx, &was.desc, running, atom);
        if (hadIt < 0) {
            return false;
        }
        if (hadIt == 0 || (was.desc.flags & JS_PROP_GETSET) != 0) {
            continue;  // not a field of the running instance
        }
        HeldDescriptor now{ctx};
        const int hasIt = JS_GetOwnProperty(ctx, &now.desc, fresh, atom);
        if (hasIt < 0) {
            return false;
        }
        if (hasIt == 0 || (now.desc.flags & JS_PROP_GETSET) != 0 || (now.desc.flags & JS_PROP_ENUMERABLE) == 0) {
            ++counts.dropped;  // removed, turned into an accessor, or no longer enumerable
            continue;
        }
        ++matched;
        if (!internal::carriesValue(valueKindOf(ctx, was.desc.value, api), valueKindOf(ctx, now.desc.value, api))) {
            ++counts.reset;  // a kind change, or a function on either side
            continue;
        }
        // A define with the value alone keeps the new property's attributes and runs no setter; it duplicates the
        // value. Without JS_PROP_THROW an unwritable property answers 0 and stays as constructed.
        const int defined =
            JS_DefineProperty(ctx, fresh, atom, was.desc.value, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_VALUE);
        if (defined < 0) {
            return false;
        }
        if (defined == 0) {
            ++counts.reset;
        } else {
            ++counts.kept;
        }
    }
    std::size_t total = 0;
    if (!countFields(ctx, fresh, total)) {
        return false;
    }
    counts.added += total - matched;  // the fresh instance's fields that matched no running field
    return true;
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
        if (known->second.refusedText == peeked) {
            return loadResultOf(ScriptStatus::AlreadyRefused);  // the same refused bytes: no retry, no second record
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
    FieldCounts counts;
    if (!JS_IsUndefined(newClass.get())) {
        rebuildInstances(record, newClass.get(), evaluation, refusal, fresh, counts);
    }

    // 5. The call's jobs and tail.
    endScriptCall(evaluation);

    // 6. Commit only now.
    const bool succeeded = evaluation.status == EvalStatus::Ok && !refusal.has_value();
    assert(!succeeded || compiled.has_value());  // an Ok load compiled its entry
    if (succeeded && compiled.has_value()) {
        assert(fresh.size() == record.instances.size());
        std::size_t at = 0;
        for (const auto& entry : record.instances) {
            InstanceSlot* const slot = instanceSlots.get(entry.second);
            assert(slot != nullptr);
            if (slot != nullptr) {
                JS_FreeValue(ctx, slot->object);
                slot->object = fresh.take(at);
            }
            ++at;
        }
        JS_FreeValue(ctx, record.scriptClass);
        record.scriptClass = newClass.release();
        record.runningText = std::move(compiled);
        record.refusedText.reset();
        ScriptLoadResult done = loadResultOf(ScriptStatus::Ok);
        done.instancesMoved = record.instances.size();
        done.fieldsKept = counts.kept;
        done.fieldsReset = counts.reset;
        done.fieldsAdded = counts.added;
        done.fieldsDropped = counts.dropped;
        return done;
    }
    // Refused: nothing is committed. The bytes are remembered -- the compiled text when the loader read one -- so the
    // next trigger with the same bytes answers AlreadyRefused without a call.
    record.refusedText = compiled.has_value() ? compiled : peeked;
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
    // A call its budget ended is Interrupted, and the interrupt's record is its one record: an engine refusal of what
    // the budget left unfinished would be a second one, and would win the status. The budget ended when the interrupt
    // handler fired (a promise executor's or a thenable's `then` turns it into a rejection nobody recorded) or when
    // the drain recorded it; endScriptCall's tail then makes the call Interrupted.
    if (interruptFired || interruptRecorded) {
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
    // A call its budget ended is Interrupted, and the interrupt's record is its one record: a value the constructor
    // finished building before the budget ended is not judged, so no engine refusal joins the interrupt.
    if (interruptFired || interruptRecorded) {
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
                                std::optional<ScriptRefusal>& refusal, HeldBatch& fresh, FieldCounts& counts) {
    for (const auto& entry : record.instances) {
        const ScriptInstance instance = entry.second;
        const InstanceSlot* const slot = instanceSlots.get(instance);
        assert(slot != nullptr);
        // The entity this instance was attached with, under the binding it was attached under: never the current one.
        const std::optional<EntityRef> attached = slot != nullptr ? slot->attached : std::nullopt;
        PendingEntityScope pending{scriptApi, attached};
        HeldValue made = constructInstance(scriptClass, evaluation, refusal);
        pending.end();  // nothing else that runs may take it
        if (!JS_IsObject(made.get())) {
            return;  // the failure or the refusal is recorded; the batch frees what it holds
        }
        if (attached.has_value() && !holdEntity(made.get(), *attached, evaluation, refusal)) {
            return;
        }
        if (slot != nullptr && !carryFields(ctx, scriptApi, slot->object, made.get(), counts)) {
            adopt(evaluation, trap(Phase::Run));
            return;
        }
        fresh.add(made.release());
    }
}

bool Vm::Impl::holdEntity(JSValueConst object, const EntityRef& ref, EvalResult& evaluation,
                          std::optional<ScriptRefusal>& refusal) {
    // A call its budget ended is Interrupted, and the interrupt's record is its one record.
    if (interruptFired || interruptRecorded) {
        return false;
    }
    const HeldAtom name{ctx, JS_NewAtom(ctx, "entity")};
    if (name.get() == JS_ATOM_NULL) {
        adopt(evaluation, trap(Phase::Run));  // out of memory; nothing is pending, trap takes the sentinel
        return false;
    }
    {
        // The object is never a Proxy, so the descriptor read runs no script code.
        HeldDescriptor own{ctx};
        const int has = JS_GetOwnProperty(ctx, &own.desc, object, name.get());
        if (has < 0) {
            adopt(evaluation, trap(Phase::Run));
            return false;
        }
        constexpr int SHAPE = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_GETSET;
        if (has > 0 && (own.desc.flags & SHAPE) == JS_PROP_CONFIGURABLE) {
            const auto* const held = static_cast<const EntityRef*>(JS_GetOpaque(own.desc.value, scriptApi.entityClass));
            if (held != nullptr && held->entity == ref.entity && held->binding == ref.binding) {
                return true;  // the base class defined it: the instance keeps that very object
            }
        }
    }
    HeldValue entityObject{ctx, entityObjectFor(ctx, scriptApi, ref)};
    if (entityObject.isException()) {
        adopt(evaluation, trap(Phase::Run));
        return false;
    }
    // The define consumes the value on every path. Without JS_PROP_THROW a refusal answers 0.
    const int defined = JS_DefinePropertyValueStr(ctx, object, "entity", entityObject.release(), JS_PROP_CONFIGURABLE);
    if (defined < 0) {
        adopt(evaluation, trap(Phase::Run));
        return false;
    }
    if (defined == 0) {
        refusal = ScriptRefusal{ScriptStatus::NotAClass,
                                "not a script: its instance cannot hold its entity (frozen, sealed, not extensible, "
                                "or with a non-configurable own 'entity')"};
        logRefusal(*refusal);
        return false;
    }
    return true;
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

ScriptInstanceResult Vm::Impl::createInstanceCall(std::string_view virtualPath, const std::optional<Entity>& entity) {
    assert(!inCall);
    // 1. The script: no class loaded from the path is NotLoaded, before any World check.
    const internal::SpecifierResolution entry = internal::resolveModuleSpecifier(virtualPath, {});
    const bool resolved = entry.refusal == internal::SpecifierRefusal::None;
    const auto found = resolved ? scripts.find(entry.key) : scripts.end();
    if (found == scripts.end() || JS_IsUndefined(found->second.scriptClass)) {
        AERO_LOG_ERROR("script: {}: createInstance refused: no script class is loaded from it",
                       resolved ? std::string_view{entry.key} : virtualPath);
        return instanceResultOf(ScriptStatus::NotLoaded);
    }
    // 2. The World, before a call is opened: nothing runs for a refusal.
    if (entity.has_value()) {
        if (scriptApi.world == nullptr) {
            AERO_LOG_ERROR("script: {}: createInstance refused: no World is bound", entry.key);
            return instanceResultOf(ScriptStatus::NoWorld);
        }
        if (!scriptApi.world->alive(*entity)) {
            AERO_LOG_ERROR("script: {}: createInstance refused: the entity is not alive in the bound World", entry.key);
            return instanceResultOf(ScriptStatus::DeadEntity);
        }
    }
    ScriptRecord& record = found->second;
    std::optional<EntityRef> ref;
    if (entity.has_value()) {
        ref = EntityRef{*entity, scriptApi.binding};
    }
    // 3. One call; the entity is pending only while the constructor runs.
    const CallScope scope{*this, entry.key};
    EvalResult evaluation{EvalStatus::Ok, {}, {}};
    std::optional<ScriptRefusal> refusal;
    PendingEntityScope pending{scriptApi, ref};
    HeldValue made = constructInstance(record.scriptClass, evaluation, refusal);
    pending.end();
    // 4. The instance must hold its entity before anything else runs.
    if (ref.has_value() && evaluation.status == EvalStatus::Ok && !refusal.has_value() && JS_IsObject(made.get())) {
        static_cast<void>(holdEntity(made.get(), *ref, evaluation, refusal));
    }
    endScriptCall(evaluation);
    if (evaluation.status == EvalStatus::Ok && !refusal.has_value() && JS_IsObject(made.get())) {
        ScriptInstanceResult done = instanceResultOf(ScriptStatus::Ok);
        const std::uint64_t order = nextInstanceOrder++;
        done.instance = instanceSlots.insert(InstanceSlot{made.release(), entry.key, ref, order});
        record.instances.emplace(order, done.instance);
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
        home->second.instances.erase(slot->order);
    }
    JS_FreeValue(ctx, slot->object);
    static_cast<void>(instanceSlots.remove(instance));
    return ScriptStatus::Ok;
}

ScriptHookResult Vm::Impl::callHookCall(ScriptInstance instance, ScriptHook hook, double dt) {
    assert(!inCall);
    const InstanceSlot* const slot = instanceSlots.get(instance);
    if (slot == nullptr) {
        AERO_LOG_ERROR("script: callHook refused: the handle names no live instance");
        ScriptHookResult unknown;
        unknown.status = ScriptStatus::UnknownInstance;
        return unknown;
    }
    const std::string key = slot->key;
    const CallScope scope{*this, key};
    const std::uint64_t rejectionsBefore = unhandledRejections;
    EvalResult evaluation{EvalStatus::Ok, {}, {}};
    bool ran = false;
    {
        // Held across the call: nothing a hook does can free the object while it runs.
        const HeldValue object{ctx, JS_DupValue(ctx, slot->object)};
        const HeldAtom name{ctx, JS_NewAtom(ctx, scriptHookName(hook).data())};
        if (name.get() == JS_ATOM_NULL) {
            adopt(evaluation, trap(Phase::Run));  // out of memory; nothing is pending, trap takes the sentinel
        } else {
            const HeldValue hookValue{ctx, JS_GetProperty(ctx, object.get(), name.get())};  // a getter runs here
            if (hookValue.isException()) {
                adopt(evaluation, trap(Phase::Run));
            } else if (JS_IsFunction(ctx, hookValue.get())) {
                ran = true;
                JSValue argument = JS_NewFloat64(ctx, dt);
                const int argc = hook == ScriptHook::Update ? 1 : 0;
                const HeldValue returned{ctx, JS_Call(ctx, hookValue.get(), object.get(), argc, &argument)};
                if (returned.isException()) {
                    adopt(evaluation, trap(Phase::Run));
                }
            }
        }
    }
    endScriptCall(evaluation);
    ScriptHookResult result;
    result.status = scriptStatusOf(evaluation.status);
    result.error = evaluation.error;
    result.ran = ran;
    result.rejected = unhandledRejections > rejectionsBefore;
    return result;
}

void Vm::Impl::releaseScripts() noexcept {
    for (auto& entry : scripts) {
        ScriptRecord& record = entry.second;
        for (const auto& member : record.instances) {
            if (InstanceSlot* const slot = instanceSlots.get(member.second); slot != nullptr) {
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
    return impl->createInstanceCall(virtualPath, std::nullopt);
}

ScriptInstanceResult Vm::createInstance(std::string_view virtualPath, Entity entity) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: createInstance on an invalid Vm (a failed create or a moved-from Vm)");
        return instanceResultOf(ScriptStatus::InvalidVm);
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: createInstance called from a thread that is not the VM's; refused");
        return instanceResultOf(ScriptStatus::WrongThread);
    }
    return impl->createInstanceCall(virtualPath, entity);
}

ScriptHookResult Vm::callHook(ScriptInstance instance, ScriptHook hook, double dt) {
    ScriptHookResult refused;
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: callHook on an invalid Vm (a failed create or a moved-from Vm)");
        refused.status = ScriptStatus::InvalidVm;
        return refused;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: callHook called from a thread that is not the VM's; refused");
        refused.status = ScriptStatus::WrongThread;
        return refused;
    }
    return impl->callHookCall(instance, hook, dt);
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
        case ScriptStatus::AlreadyRefused:
            return "AlreadyRefused";
        case ScriptStatus::NoWorld:
            return "NoWorld";
        case ScriptStatus::DeadEntity:
            return "DeadEntity";
    }
    return "Unknown";
}

std::string_view scriptHookName(ScriptHook hook) noexcept {
    // Terminated literals: the backend's atom is made from the view's data().
    static constexpr std::array<const char*, SCRIPT_HOOK_COUNT> NAMES{"onInit", "onUpdate", "onDestroy"};
    const auto index = static_cast<std::size_t>(hook);
    return std::string_view{index < NAMES.size() ? NAMES[index] : ""};
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
