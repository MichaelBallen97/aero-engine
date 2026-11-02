// Aero Engine -- the entity side of the script API (task 4.2.1): the World binding (Vm::bindWorld, unbindWorld and
// defineEntityGlobal), engine.Entity, the component objects and their field accessors. script_api.hpp states the rules
// every host function keeps; the ones that shape this file are 4 (a class is found by identity, an instance by its
// opaque), 5 (objects come from JS_NewObjectClass, properties from JS_DefineProperty*) and 9 (every value check
// precedes every World read).
//
// An entity object and a component object hold an Entity and the binding they were made under, never a pointer into
// the World: every access resolves the entity again (resolveEntity / resolveStorage), because a component's address
// is good only until the World next changes (world.hpp).

#include <aero/core/log.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/vm.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

namespace engine::script {
namespace {

constexpr std::size_t NO_COMPONENT = static_cast<std::size_t>(-1);

[[nodiscard]] bool isGlobalName(std::string_view name) noexcept {
    const auto head = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; };
    const auto tail = [&](char c) { return head(c) || (c >= '0' && c <= '9'); };
    if (name.empty() || !head(name.front())) {
        return false;
    }
    for (const char c : name) {
        if (!tail(c)) {
            return false;
        }
    }
    return true;
}

// "<site>: <reason>" as a TypeError, for the roads that fail the same way everywhere.
JSValue throwAt(JSContext* ctx, const Site& site, std::string_view reason) {
    return throwTypeError(ctx, describeSite(site) + ": " + std::string{reason});
}

[[nodiscard]] JSValue newComponentObject(JSContext* ctx, const ScriptApi& api, const EntityRef& ref,
                                         std::size_t component) {
    return newOpaqueObject(ctx, api.componentClasses[component], ref);
}

// Which bound component a class argument names, found by identity against the constructors the API holds: nothing of
// the argument is read, so no getter, Proxy trap or conversion runs. NO_COMPONENT: it names none.
[[nodiscard]] std::size_t findComponentClass(JSContext* ctx, const ScriptApi& api, JSValueConst argument) {
    if (!JS_IsObject(argument)) {
        return NO_COMPONENT;
    }
    for (std::size_t i = 0; i < api.componentConstructors.size(); ++i) {
        if (JS_IsStrictEqual(ctx, argument, api.componentConstructors[i])) {
            return i;
        }
    }
    return NO_COMPONENT;
}

[[nodiscard]] const EntityRef* entityOf(const ScriptApi& api, JSValueConst value) noexcept {
    return static_cast<const EntityRef*>(JS_GetOpaque(value, api.entityClass));
}

JSValue foreignEntity(JSContext* ctx, const Site& site) {
    return throwAt(ctx, site, "the receiver is not an engine.Entity");
}

}  // namespace

std::string_view bindStatusLabel(BindStatus status) noexcept {
    switch (status) {
        case BindStatus::Ok:
            return "Ok";
        case BindStatus::InvalidVm:
            return "InvalidVm";
        case BindStatus::WrongThread:
            return "WrongThread";
        case BindStatus::UnregisteredComponent:
            return "UnregisteredComponent";
        case BindStatus::NoWorld:
            return "NoWorld";
        case BindStatus::DeadEntity:
            return "DeadEntity";
        case BindStatus::InvalidName:
            return "InvalidName";
        case BindStatus::OutOfMemory:
            return "OutOfMemory";
    }
    return "Unknown";
}

std::string describeSite(const Site& site) {
    std::string text = "engine.";
    text += site.type;
    text += '.';
    text += site.member;
    if (!site.axis.empty()) {
        text += '.';
        text += site.axis;
    }
    return text;
}

bool resolveEntity(JSContext* ctx, const ScriptApi& api, const EntityRef& ref, const Site& site) {
    if (api.world == nullptr) {
        static_cast<void>(throwAt(ctx, site, "no World is bound"));
        return false;
    }
    if (ref.binding != api.binding) {
        static_cast<void>(throwAt(ctx, site, "the entity belongs to an earlier World binding"));
        return false;
    }
    if (!api.world->alive(ref.entity)) {
        static_cast<void>(throwAt(ctx, site, "the entity is not alive"));
        return false;
    }
    return true;
}

void* resolveStorage(JSContext* ctx, const ScriptApi& api, const EntityRef& ref, std::size_t component,
                     const Site& site) {
    if (!resolveEntity(ctx, api, ref, site)) {
        return nullptr;
    }
    const internal::ComponentBinding& bound = api.table->components()[component];
    void* const storage = api.world->getRaw(bound.type, ref.entity);  // re-read on every access: world.hpp
    if (storage == nullptr) {
        static_cast<void>(throwAt(ctx, site, "the entity has no engine." + std::string{bound.scriptName}));
    }
    return storage;
}

// ---- the host functions -----------------------------------------------------------------------------------------

JSValue entityMethod(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* argv, int method) noexcept {
    const ScriptApi& api = scriptApiOf(ctx);
    assert(method >= 0 && static_cast<std::size_t>(method) < ENTITY_METHODS.size());
    const Site site{"Entity", ENTITY_METHODS[static_cast<std::size_t>(method)], {}};
    const EntityRef* const ref = entityOf(api, thisValue);
    if (ref == nullptr) {
        return foreignEntity(ctx, site);
    }
    // The method was made with length 1, so argv[0] exists (undefined when the script passed nothing).
    const std::size_t component = findComponentClass(ctx, api, argv[0]);
    if (component == NO_COMPONENT) {
        return throwTypeError(
            ctx, describeSite(site) + ": expected an engine component class, got " + describeValue(ctx, argv[0], api));
    }
    if (!resolveEntity(ctx, api, *ref, site)) {
        return JS_EXCEPTION;
    }
    World& world = *api.world;
    const ComponentTypeId type = api.table->components()[component].type;
    switch (method) {
        case 0:  // has
            return JS_NewBool(ctx, world.hasRaw(type, ref->entity));
        case 1:  // get
            return world.hasRaw(type, ref->entity) ? newComponentObject(ctx, api, *ref, component) : JS_NULL;
        case 2:  // add: World::add's semantics, so an existing component is replaced by the defaults
            static_cast<void>(world.addRaw(type, ref->entity, nullptr));
            // A tag stores nothing and addRaw answers null for it: presence is the test, not the pointer.
            if (!world.hasRaw(type, ref->entity)) {
                return throwAt(ctx, site, "the component could not be added");
            }
            return newComponentObject(ctx, api, *ref, component);
        default:  // remove
            return JS_NewBool(ctx, world.removeRaw(type, ref->entity));
    }
}

JSValue entityGet(JSContext* ctx, JSValueConst thisValue, int magic) noexcept {
    ScriptApi& api = scriptApiOf(ctx);
    assert(magic >= 0 && static_cast<std::size_t>(magic) < ENTITY_PROPERTIES.size());
    const Site site{"Entity", ENTITY_PROPERTIES[static_cast<std::size_t>(magic)].name, {}};
    const EntityRef* const ref = entityOf(api, thisValue);
    if (ref == nullptr) {
        return foreignEntity(ctx, site);
    }
    switch (magic) {
        case 0:  // alive: never throws for an entity object (no World, an earlier binding and a dead entity read false)
            return JS_NewBool(ctx,
                              api.world != nullptr && ref->binding == api.binding && api.world->alive(ref->entity));
        case 1: {  // name
            if (!resolveEntity(ctx, api, *ref, site)) {
                return JS_EXCEPTION;
            }
            const std::string_view name = api.world->name(ref->entity);  // copied by the call below, at once
            return JS_NewStringLen(ctx, name.empty() ? "" : name.data(), name.size());
        }
        default: {  // parent
            if (!resolveEntity(ctx, api, *ref, site)) {
                return JS_EXCEPTION;
            }
            const Entity parent = api.world->parent(ref->entity);
            return parent.valid() ? entityObjectFor(ctx, api, EntityRef{parent, api.binding}) : JS_NULL;
        }
    }
}

JSValue entitySet(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept {
    ScriptApi& api = scriptApiOf(ctx);
    assert(magic >= 1 && static_cast<std::size_t>(magic) < ENTITY_PROPERTIES.size());  // alive has no setter
    const Site site{"Entity", ENTITY_PROPERTIES[static_cast<std::size_t>(magic)].name, {}};
    const EntityRef* const ref = entityOf(api, thisValue);
    if (ref == nullptr) {
        return foreignEntity(ctx, site);
    }
    // Receiver, then the value, then the World, then the write (rule 9): a wrong value on a dead entity names the
    // value.
    if (magic == 1) {  // name
        if (!JS_IsString(value)) {
            return throwAt(ctx, site, "expected a string, got " + describeValue(ctx, value, api));
        }
        const HeldCString text{ctx, value};  // the length comes from the backend: an embedded U+0000 is kept
        if (!text.ok()) {
            return JS_EXCEPTION;
        }
        if (!resolveEntity(ctx, api, *ref, site)) {
            return JS_EXCEPTION;
        }
        static_cast<void>(api.world->setName(ref->entity, text.view()));  // "" clears; a live entity cannot refuse
        return JS_UNDEFINED;
    }
    // parent
    const EntityRef* const target = entityOf(api, value);
    if (!JS_IsNull(value) && target == nullptr) {
        return throwAt(ctx, site, "expected an engine.Entity or null, got " + describeValue(ctx, value, api));
    }
    if (!resolveEntity(ctx, api, *ref, site)) {
        return JS_EXCEPTION;
    }
    World& world = *api.world;
    Entity parent{};
    if (target != nullptr) {
        if (target->binding != api.binding) {
            return throwAt(ctx, site, "the new parent belongs to an earlier World binding");
        }
        if (!world.alive(target->entity)) {
            return throwAt(ctx, site, "the new parent is not alive");
        }
        if (target->entity == ref->entity) {
            return throwAt(ctx, site, "the new parent is the entity itself");
        }
        for (Entity ancestor = world.parent(target->entity); ancestor.valid(); ancestor = world.parent(ancestor)) {
            if (ancestor == ref->entity) {
                return throwAt(ctx, site, "the new parent is in the entity's subtree");
            }
        }
        parent = target->entity;
    }
    // Every refusal was decided above, so this cannot log. The local Transform is not touched.
    static_cast<void>(world.setParent(ref->entity, parent));
    return JS_UNDEFINED;
}

JSValue entityChildren(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* /*argv*/,
                       int /*magic*/) noexcept {
    ScriptApi& api = scriptApiOf(ctx);
    const Site site{"Entity", "children", {}};
    const EntityRef* const ref = entityOf(api, thisValue);
    if (ref == nullptr) {
        return foreignEntity(ctx, site);
    }
    if (!resolveEntity(ctx, api, *ref, site)) {
        return JS_EXCEPTION;
    }
    return childrenArray(ctx, api, ref->entity);
}

JSValue fieldGet(JSContext* ctx, JSValueConst thisValue, int magic) noexcept {
    const ScriptApi& api = scriptApiOf(ctx);
    const std::size_t component = fieldMagicComponent(magic);
    const internal::ComponentBinding& bound = api.table->components()[component];
    const internal::FieldBinding& field = bound.fields[fieldMagicField(magic)];
    const Site site{bound.scriptName, field.name, {}};
    const auto* const ref = static_cast<const EntityRef*>(JS_GetOpaque(thisValue, api.componentClasses[component]));
    if (ref == nullptr) {
        return throwAt(ctx, site, "the receiver is not an engine." + std::string{bound.scriptName});
    }
    void* const storage = resolveStorage(ctx, api, *ref, component, site);
    if (storage == nullptr) {
        return JS_EXCEPTION;
    }
    if (isVectorKind(field.kind)) {
        // A view of the field, not a copy of it: nothing is read until an axis is.
        return newVectorView(ctx, api, field.kind, *ref, component, fieldMagicField(magic));
    }
    return loadFieldValue(ctx, field.kind, field.address(storage));
}

JSValue fieldSet(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept {
    const ScriptApi& api = scriptApiOf(ctx);
    const std::size_t component = fieldMagicComponent(magic);
    const internal::ComponentBinding& bound = api.table->components()[component];
    const internal::FieldBinding& field = bound.fields[fieldMagicField(magic)];
    const Site site{bound.scriptName, field.name, {}};
    const auto* const ref = static_cast<const EntityRef*>(JS_GetOpaque(thisValue, api.componentClasses[component]));
    if (ref == nullptr) {
        return throwAt(ctx, site, "the receiver is not an engine." + std::string{bound.scriptName});
    }
    // Receiver, then the value, then the World, then the write (rule 9): a wrong value on a dead entity names the
    // value.
    FieldValue checked;
    if (!readFieldValue(ctx, api, field.kind, site, value, checked)) {
        return JS_EXCEPTION;
    }
    void* const storage = resolveStorage(ctx, api, *ref, component, site);
    if (storage == nullptr) {
        return JS_EXCEPTION;
    }
    storeFieldValue(field.kind, field.address(storage), checked);
    return JS_UNDEFINED;
}

// ---- the World binding ------------------------------------------------------------------------------------------

BindStatus Vm::bindWorld(World& world) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: bindWorld on an invalid Vm (a failed create or a moved-from Vm)");
        return BindStatus::InvalidVm;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: bindWorld called from a thread that is not the VM's; refused");
        return BindStatus::WrongThread;
    }
    assert(!impl->inCall);  // a host function never calls back into the VM, and nothing else runs on this thread
    ScriptApi& api = impl->scriptApi;
    for (const internal::ComponentBinding& bound : api.table->components()) {
        if (!world.registered(bound.type)) {
            AERO_LOG_ERROR("script: bindWorld refused: the World does not register engine.{}", bound.scriptName);
            return BindStatus::UnregisteredComponent;
        }
    }
    ++api.binding;  // every call starts a new binding, the same World included
    api.world = &world;
    releaseEntityObjects(impl->ctx, api);  // every lookup under the new binding makes a new object
    return BindStatus::Ok;
}

void Vm::unbindWorld() noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: unbindWorld on an invalid Vm (a failed create or a moved-from Vm)");
        return;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: unbindWorld called from a thread that is not the VM's; refused");
        return;
    }
    assert(!impl->inCall);
    impl->scriptApi.world = nullptr;
    releaseEntityObjects(impl->ctx, impl->scriptApi);
}

BindStatus Vm::defineEntityGlobal(std::string_view name, Entity entity) {
    if (impl == nullptr) {
        AERO_LOG_ERROR("script: defineEntityGlobal on an invalid Vm (a failed create or a moved-from Vm)");
        return BindStatus::InvalidVm;
    }
    if (!impl->onVmThread()) {
        AERO_LOG_ERROR("script: defineEntityGlobal called from a thread that is not the VM's; refused");
        return BindStatus::WrongThread;
    }
    assert(!impl->inCall);
    if (!isGlobalName(name) || name == "engine") {
        AERO_LOG_ERROR("script: defineEntityGlobal refused: a name of {} bytes is not an identifier, or is 'engine'",
                       name.size());
        return BindStatus::InvalidName;
    }
    ScriptApi& api = impl->scriptApi;
    if (api.world == nullptr) {
        AERO_LOG_ERROR("script: defineEntityGlobal refused: no World is bound");
        return BindStatus::NoWorld;
    }
    if (!api.world->alive(entity)) {
        AERO_LOG_ERROR("script: defineEntityGlobal refused: the entity for '{}' is not alive in the bound World", name);
        return BindStatus::DeadEntity;
    }
    JSContext* const ctx = impl->ctx;
    HeldValue object{ctx, entityObjectFor(ctx, api, EntityRef{entity, api.binding})};
    if (object.isException()) {
        impl->clearPendingException();
        AERO_LOG_ERROR("script: defineEntityGlobal '{}' failed: out of memory (heap limit {} bytes)", name,
                       impl->config.heapLimitBytes);
        return BindStatus::OutOfMemory;
    }
    // A define, not a set: no setter a script put on the global object runs. It consumes the value either way.
    const HeldValue global{ctx, JS_GetGlobalObject(ctx)};
    const std::string cName{name};
    const int defined = JS_DefinePropertyValueStr(ctx, global.get(), cName.c_str(), object.release(), JS_PROP_C_W_E);
    if (defined < 0) {
        impl->clearPendingException();
        AERO_LOG_ERROR("script: defineEntityGlobal '{}' failed: out of memory (heap limit {} bytes)", name,
                       impl->config.heapLimitBytes);
        return BindStatus::OutOfMemory;
    }
    if (defined == 0) {
        AERO_LOG_ERROR(
            "script: defineEntityGlobal refused: the global '{}' cannot be defined (a non-configurable "
            "property of that name, or a non-extensible global object)",
            name);
        return BindStatus::InvalidName;
    }
    return BindStatus::Ok;
}

}  // namespace engine::script
