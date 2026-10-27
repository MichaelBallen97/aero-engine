// Aero Engine -- the script API's install (task 4.2.1): `globalThis.engine`, its classes, their prototypes and
// constructors, and the one place a native function is made. script_api.hpp states the rules every host function keeps.
//
// The whole namespace is built inside Vm::createWith, before the call stack limit and the interrupt handler exist and
// before any script has run: no script setter, getter or Proxy can be in the way, no property is built on a first read
// under a budget, and a VM's surface does not depend on call order.

#include "script_api.hpp"

#include <aero/script/internal/component_binding.hpp>

#include "vm_impl.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
namespace {

using MagicGetter = JSValue (*)(JSContext*, JSValueConst, int);
using MagicSetter = JSValue (*)(JSContext*, JSValueConst, JSValueConst, int);
using MagicMethod = JSValue (*)(JSContext*, JSValueConst, int, JSValueConst*, int);

constexpr std::array<std::string_view, 4> AXIS_NAMES{"x", "y", "z", "w"};

// The one place a native function is made (script_vm_test.cpp VM65 counts it). The union member is filled and its
// generic slot passed, exactly as the backend's own JS_NewCFunctionMagic does, so no cast is needed.
[[nodiscard]] JSValue newNative(JSContext* ctx, const std::string& name, const JSCFunctionType& function,
                                JSCFunctionEnum prototype, int length, int magic) {
    return JS_NewCFunction2(ctx, function.generic, name.c_str(), length, prototype, magic);
}

[[nodiscard]] JSValue newGetter(JSContext* ctx, const std::string& property, MagicGetter function, int magic) {
    JSCFunctionType type{};
    type.getter_magic = function;
    return newNative(ctx, "get " + property, type, JS_CFUNC_getter_magic, 0, magic);  // a getter takes no argument
}

[[nodiscard]] JSValue newSetter(JSContext* ctx, const std::string& property, MagicSetter function, int magic) {
    JSCFunctionType type{};
    type.setter_magic = function;
    return newNative(ctx, "set " + property, type, JS_CFUNC_setter_magic, 1, magic);  // a setter takes argv[0]
}

[[nodiscard]] JSValue newMethod(JSContext* ctx, const std::string& name, MagicMethod function, int length, int magic) {
    JSCFunctionType type{};
    type.generic_magic = function;
    return newNative(ctx, name, type, JS_CFUNC_generic_magic, length, magic);
}

// A constructor reached by `new` only (the vectors).
[[nodiscard]] JSValue newConstructor(JSContext* ctx, const std::string& name, MagicMethod function, int length,
                                     int magic) {
    JSCFunctionType type{};
    type.constructor_magic = function;
    return newNative(ctx, name, type, JS_CFUNC_constructor_magic, length, magic);
}

// A constructor that always throws, called or constructed (so a call and a `new` reach the engine's message).
[[nodiscard]] JSValue newThrowingConstructor(JSContext* ctx, const std::string& name, MagicMethod function, int magic) {
    JSCFunctionType type{};
    type.generic_magic = function;
    return newNative(ctx, name, type, JS_CFUNC_constructor_or_func_magic, 0, magic);
}

// The shared finalizer: every API class's opaque is one js_malloc block, trivially destructible. It touches neither the
// World nor the ScriptApi, because it can run inside JS_FreeRuntime.
void finalizeOpaque(JSRuntime* rt, JSValueConst value) { js_free_rt(rt, JS_GetOpaque(value, JS_GetClassID(value))); }

JSValue componentConstructor(JSContext* ctx, JSValueConst /*newTarget*/, int /*argc*/, JSValueConst* /*argv*/,
                             int magic) noexcept {
    const ScriptApi& api = scriptApiOf(ctx);
    const std::string_view name = api.table->components()[static_cast<std::size_t>(magic)].scriptName;
    const int length = static_cast<int>(name.size());
    // The name is the table's, an identifier, and goes in as a "%.*s" argument: no allocation, no format string a
    // script could shape.
    return JS_ThrowTypeError(ctx, "engine.%.*s cannot be constructed; use entity.add(engine.%.*s)", length, name.data(),
                             length, name.data());
}

JSValue entityConstructor(JSContext* ctx, JSValueConst /*newTarget*/, int /*argc*/, JSValueConst* /*argv*/,
                          int /*magic*/) noexcept {
    return throwTypeError(ctx, "engine.Entity cannot be constructed by a script");
}

struct AccessorSpec {
    std::string property;
    int magic = 0;
    bool readOnly = false;  // no setter: a strict-mode assignment throws the backend's own TypeError
};

struct MethodSpec {
    std::string name;
    MagicMethod function = nullptr;
    int length = 0;
    int magic = 0;
};

// One class: its registration, a frozen plain-object prototype carrying the accessors and methods, a frozen
// constructor, and the constructor defined on the namespace.
struct ClassSpec {
    std::string name;
    JSClassID* classId = nullptr;
    MagicMethod constructor = nullptr;
    bool throwing = false;  // the constructor always throws (newThrowingConstructor), else newConstructor
    int constructorLength = 0;
    int constructorMagic = 0;
    MagicGetter getter = nullptr;
    MagicSetter setter = nullptr;
    std::vector<AccessorSpec> accessors;
    std::vector<MethodSpec> methods;
};

// Every value is held the moment it is received. Four calls CONSUME their value argument, so a held value enters them
// through release() or a fresh JS_DupValue: JS_DefinePropertyGetSet (both accessors), JS_DefinePropertyValueStr (freed
// on failure too) and JS_SetClassProto. `keep`, when given, receives one extra reference to the constructor (its
// capacity was reserved, so the push cannot throw).
[[nodiscard]] bool installClass(JSContext* ctx, JSRuntime* rt, JSValueConst ns, const ClassSpec& spec,
                                std::vector<JSValue>* keep) {
    JSClassDef def{};
    def.class_name = spec.name.c_str();  // copied into an atom by JS_NewClass
    def.finalizer = &finalizeOpaque;
    if (JS_NewClassID(rt, spec.classId) == JS_INVALID_CLASS_ID || JS_NewClass(rt, *spec.classId, &def) != 0) {
        return false;
    }
    const HeldValue proto{ctx, JS_NewObject(ctx)};
    if (proto.isException()) {
        return false;
    }
    for (const AccessorSpec& accessor : spec.accessors) {
        HeldValue getter{ctx, newGetter(ctx, accessor.property, spec.getter, accessor.magic)};
        HeldValue setter{
            ctx, accessor.readOnly ? JS_UNDEFINED : newSetter(ctx, accessor.property, spec.setter, accessor.magic)};
        if (getter.isException() || setter.isException()) {
            return false;
        }
        const JSAtom atom = JS_NewAtom(ctx, accessor.property.c_str());
        if (atom == JS_ATOM_NULL) {
            return false;
        }
        const int defined = JS_DefinePropertyGetSet(ctx, proto.get(), atom, getter.release(), setter.release(),
                                                    JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
        if (defined < 0) {
            return false;
        }
    }
    for (const MethodSpec& method : spec.methods) {
        HeldValue function{ctx, newMethod(ctx, method.name, method.function, method.length, method.magic)};
        if (function.isException() ||
            JS_DefinePropertyValueStr(ctx, proto.get(), method.name.c_str(), function.release(),
                                      JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) {
            return false;
        }
    }
    const HeldValue constructor{
        ctx, spec.throwing
                 ? newThrowingConstructor(ctx, spec.name, spec.constructor, spec.constructorMagic)
                 : newConstructor(ctx, spec.name, spec.constructor, spec.constructorLength, spec.constructorMagic)};
    if (constructor.isException() || JS_SetConstructor(ctx, constructor.get(), proto.get()) < 0 ||
        JS_FreezeObject(ctx, proto.get()) < 0 || JS_FreezeObject(ctx, constructor.get()) < 0) {
        return false;
    }
    JS_SetClassProto(ctx, *spec.classId, JS_DupValue(ctx, proto.get()));  // the context owns the prototype from here
    if (JS_DefinePropertyValueStr(ctx, ns, spec.name.c_str(), JS_DupValue(ctx, constructor.get()), JS_PROP_ENUMERABLE) <
        0) {
        return false;
    }
    if (keep != nullptr) {
        keep->push_back(JS_DupValue(ctx, constructor.get()));
    }
    return true;
}

[[nodiscard]] ClassSpec vectorSpec(ScriptApi& api, int dimension) {
    ClassSpec spec;
    spec.name = dimension == 3 ? "Vec3" : "Quat";
    spec.classId = dimension == 3 ? &api.vec3Class : &api.quatClass;
    spec.constructor = &vectorConstructor;
    spec.constructorLength = dimension;
    spec.constructorMagic = dimension;
    spec.getter = &vectorGetAxis;
    spec.setter = &vectorSetAxis;
    for (int axis = 0; axis < dimension; ++axis) {
        spec.accessors.push_back(
            AccessorSpec{std::string{AXIS_NAMES[static_cast<std::size_t>(axis)]}, vectorMagic(dimension, axis)});
    }
    spec.methods.push_back(MethodSpec{"set", &vectorSet, dimension, dimension});
    spec.methods.push_back(MethodSpec{"copy", &vectorCopy, 1, dimension});
    return spec;
}

}  // namespace

bool installScriptApi(JSContext* ctx, JSRuntime* rt, ScriptApi& api) {
    const HeldValue global{ctx, JS_GetGlobalObject(ctx)};
    const HeldValue ns{ctx, JS_NewObject(ctx)};
    if (ns.isException()) {
        return false;
    }
    const std::span<const internal::ComponentBinding> components = api.table->components();
    api.componentClasses.assign(components.size(), JS_INVALID_CLASS_ID);
    api.componentConstructors.reserve(components.size());

    // One class per bound component, in the table's order, with one accessor per field the value contract binds.
    for (std::size_t i = 0; i < components.size(); ++i) {
        ClassSpec spec;
        spec.name = std::string{components[i].scriptName};
        spec.classId = &api.componentClasses[i];
        spec.constructor = &componentConstructor;
        spec.throwing = true;
        spec.constructorMagic = static_cast<int>(i);
        spec.getter = &fieldGet;
        spec.setter = &fieldSet;
        for (std::size_t f = 0; f < components[i].fields.size(); ++f) {
            const internal::FieldBinding& field = components[i].fields[f];
            spec.accessors.push_back(AccessorSpec{std::string{field.name}, fieldMagic(i, f), false});
        }
        if (!installClass(ctx, rt, ns.get(), spec, &api.componentConstructors)) {
            return false;
        }
    }
    if (!installClass(ctx, rt, ns.get(), vectorSpec(api, 3), nullptr) ||
        !installClass(ctx, rt, ns.get(), vectorSpec(api, 4), nullptr)) {
        return false;
    }
    ClassSpec entity;
    entity.name = "Entity";
    entity.classId = &api.entityClass;
    entity.constructor = &entityConstructor;
    entity.throwing = true;
    entity.getter = &entityAlive;
    entity.accessors.push_back(AccessorSpec{"alive", 0, true});
    for (std::size_t method = 0; method < ENTITY_METHODS.size(); ++method) {
        entity.methods.push_back(
            MethodSpec{std::string{ENTITY_METHODS[method]}, &entityMethod, 1, static_cast<int>(method)});
    }
    if (!installClass(ctx, rt, ns.get(), entity, nullptr)) {
        return false;
    }
    // Frozen after the last class, and defined on the global object with flags 0: not writable, not enumerable, not
    // configurable.
    return JS_FreezeObject(ctx, ns.get()) >= 0 &&
           JS_DefinePropertyValueStr(ctx, global.get(), "engine", JS_DupValue(ctx, ns.get()), 0) >= 0;
}

void releaseScriptApi(JSContext* ctx, ScriptApi& api) noexcept {
    for (const JSValue constructor : api.componentConstructors) {
        JS_FreeValue(ctx, constructor);
    }
    api.componentConstructors.clear();
    api.componentClasses.clear();
    api.world = nullptr;
}

ScriptApi& scriptApiOf(JSContext* ctx) noexcept { return *static_cast<ScriptApi*>(JS_GetContextOpaque(ctx)); }

JSValue throwTypeError(JSContext* ctx, const std::string& text) noexcept {
    return JS_ThrowTypeError(ctx, "%s", text.c_str());
}

JSValue throwRangeError(JSContext* ctx, const std::string& text) noexcept {
    return JS_ThrowRangeError(ctx, "%s", text.c_str());
}

std::string describeValue(JSContext* ctx, JSValueConst value, const ScriptApi& api) {
    if (JS_IsUndefined(value)) {
        return "undefined";
    }
    if (JS_IsNull(value)) {
        return "null";
    }
    if (JS_IsBool(value)) {
        return "a boolean";
    }
    if (JS_IsNumber(value)) {
        return "a number";
    }
    if (JS_IsString(value)) {
        return "a string";
    }
    if (JS_IsSymbol(value)) {
        return "a symbol";
    }
    if (JS_IsBigInt(value)) {
        return "a bigint";
    }
    if (JS_IsObject(value)) {
        const JSClassID id = JS_GetClassID(value);
        if (id == api.vec3Class) {
            return "an engine.Vec3";
        }
        if (id == api.quatClass) {
            return "an engine.Quat";
        }
        if (id == api.entityClass) {
            return "an engine.Entity";
        }
        const std::span<const internal::ComponentBinding> components = api.table->components();
        for (std::size_t i = 0; i < api.componentClasses.size() && i < components.size(); ++i) {
            if (id == api.componentClasses[i]) {
                return "an engine." + std::string{components[i].scriptName};
            }
        }
        if (JS_IsFunction(ctx, value)) {
            return "a function";
        }
    }
    return "an object";
}

}  // namespace engine::script
