#pragma once
// Aero Engine -- the script API's PRIVATE state and shared helpers (task 4.2.1). It includes <quickjs.h>, which is
// why it lives under src/: the side of the boundary .github/scripts/check-script-boundary.sh permits.
//
// The API is `globalThis.engine`: one class per bound component (named by its descriptor), Vec3, Quat and Entity.
// Every host function that implements it keeps these nine rules, in this order. Each answers a measurement or a rule
// the tree already had, and a new host function inherits all of them.
//
//  1. Only JS_NewCFunction2 natives. The backend refuses one of them once the call's budget has fired, before its body
//     runs; a function made with JS_NewCFunctionData, JS_NewCClosure or an exotic class hook is NOT refused, and a
//     JS_SetPropertyFunctionList entry is built on its first read, under whatever budget that read has. The class
//     set is built once, at create, with no autoinit property. (script_vm_test.cpp pins the banned names.)
//  2. A host function reads argv[i] only for i below the length it was made with: the backend pads argv to that
//     length with undefined and calls a setter with argv[0], so a setter has length 1, a getter 0.
//  3. A value's type is checked by tag or class before anything converts it: JS_IsNumber before JS_ToFloat64,
//     JS_IsString before a string is read, JS_GetOpaque for an API object. Nothing coerces, so no script code (a
//     valueOf, a toString, a Symbol.toPrimitive) runs inside a binding.
//  4. No property of a script-supplied value is read: a getter or a Proxy trap would run. A class is found by
//     identity, an instance by its opaque.
//  5. Objects are made with JS_NewObjectClass + JS_SetOpaque and properties with JS_DefineProperty*, never
//     JS_SetProperty*, because a setter a script put on Object.prototype would run and the property be lost.
//  6. Every host function is noexcept. The only C++ exception possible is std::bad_alloc (a message), and noexcept
//     turns it into std::terminate rather than an unwind through the backend's C frames.
//  7. A host function never calls back into the VM -- no evaluate, no job drain, no JS_Call -- so nothing re-enters it
//     (the two assert(!inCall) in vm.cpp and vm_modules.cpp rest on this).
//  8. JS_EXCEPTION is returned only with an exception pending: thrown by the binding, or by a failed JS_New* or
//     js_malloc. classify then decides as for script code: a binding's TypeError or RangeError is Exception, a
//     refused allocation OutOfMemory.
//  9. Every value check precedes every World read, and nothing mutates the World between a read and its use. A setter
//     runs receiver, then value, then target, then write.

#include <aero/core/guid.hpp>
#include <aero/scene/entity.hpp>
#include <aero/script/internal/component_binding.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::script {

// engine.Entity's opaque, and every component object's.
struct EntityRef {
    Entity entity;
    std::uint64_t binding = 0;  // ScriptApi::binding when the object was made
};

// engine.Vec3's and engine.Quat's opaque.
struct VectorCell {
    std::array<float, 4> values{};  // a detached vector's storage (x y z w); a view leaves it unused
    EntityRef ref;                  // a view's entity
    std::uint16_t component = 0;    // a view's component and field index into the table
    std::uint16_t field = 0;
    bool view = false;
};

struct ScriptApi {
    const internal::ComponentBindingTable* table = nullptr;  // borrowed
    World* world = nullptr;                                  // borrowed; null when none is bound
    std::uint64_t binding = 0;                               // +1 on every bindWorld
    // Each id starts at 0 and is allocated by THIS runtime (JS_NewClassID): never a static, which every VM on every
    // thread would share.
    JSClassID entityClass = 0;
    JSClassID vec3Class = 0;
    JSClassID quatClass = 0;
    std::vector<JSClassID> componentClasses;     // parallel to table->components()
    std::vector<JSValue> componentConstructors;  // HELD, parallel: entity.get(C)'s identity test
};

// Builds `globalThis.engine` over api.table. False: an allocation failed (the heap limit only) and an exception is
// pending; whatever was built is released by releaseScriptApi and the context.
[[nodiscard]] bool installScriptApi(JSContext* ctx, JSRuntime* rt, ScriptApi& api);
// Frees every reference the API holds; Impl::shutdown calls it before the context is freed.
void releaseScriptApi(JSContext* ctx, ScriptApi& api) noexcept;
// The API a host function belongs to: the context opaque, set at create.
[[nodiscard]] ScriptApi& scriptApiOf(JSContext* ctx) noexcept;
// A new engine.Entity object for `entity`, stamped with the current binding. JS_EXCEPTION, with an exception pending,
// when the heap limit refused it.
[[nodiscard]] JSValue newEntityObject(JSContext* ctx, const ScriptApi& api, Entity entity);

// ---- shared by the API's files --------------------------------------------------------------------------------

// Every API object is allocated first (charged to the VM's heap; on a refusal the backend's out-of-memory error is
// already pending), then the object, then the opaque is set at once: no live object of an API class has a null opaque,
// so JS_GetOpaque(v, id) == nullptr means exactly "not one of ours". JS_EXCEPTION, with an exception pending, on any
// failure.
template <typename Opaque>
[[nodiscard]] JSValue newOpaqueObject(JSContext* ctx, JSClassID classId, const Opaque& init) noexcept {
    static_assert(std::is_trivially_destructible_v<Opaque>,
                  "the shared finalizer frees the block without a destructor");
    void* const memory = js_malloc(ctx, sizeof(Opaque));
    if (memory == nullptr) {
        return JS_EXCEPTION;
    }
    auto* const opaque = ::new (memory) Opaque(init);
    const JSValue object = JS_NewObjectClass(ctx, classId);
    if (JS_IsException(object)) {
        js_free(ctx, opaque);
        return object;
    }
    JS_SetOpaque(object, opaque);
    return object;
}

// A TypeError / RangeError with exactly `text`: it goes through "%s", so a '%' a script controls never reaches a
// format string. Both return JS_EXCEPTION.
JSValue throwTypeError(JSContext* ctx, const std::string& text) noexcept;
JSValue throwRangeError(JSContext* ctx, const std::string& text) noexcept;

// What a value is, for an error message, decided by tag and class and running nothing: "undefined", "null",
// "a boolean", "a number", "a string", "a symbol", "a bigint", "an engine.<Class>" for the API's own objects,
// "a function", else "an object".
[[nodiscard]] std::string describeValue(JSContext* ctx, JSValueConst value, const ScriptApi& api);

// The magic of a vector accessor carries the class as well as the axis, so a Quat's getter lifted onto a Vec3
// refuses it: dimension (3 for Vec3, 4 for Quat) in the high bits, the axis in the low three.
[[nodiscard]] constexpr int vectorMagic(int dimension, int axis) noexcept { return dimension * 8 + axis; }
[[nodiscard]] constexpr int vectorDimension(int magic) noexcept { return magic / 8; }
[[nodiscard]] constexpr int vectorAxis(int magic) noexcept { return magic % 8; }

// Where an error is raised, spelled "engine.<type>.<member>" or "engine.<type>.<member>.<axis>". Three views into
// static storage (the table's names, literals), so a host function that does not fail builds no text.
struct Site {
    std::string_view type;    // "Entity", a component's script name
    std::string_view member;  // a method, a property or a field
    std::string_view axis;    // a vector view's axis, else empty
};
[[nodiscard]] std::string describeSite(const Site& site);

// ---- the road to storage (script_api_entity.cpp) --------------------------------------------------------------
// The one way a host function reaches the World. False / nullptr leaves a TypeError pending, naming `site`:
// "no World is bound", "the entity belongs to an earlier World binding", "the entity is not alive" and, for the
// storage, "the entity has no engine.<C>". The pointer is valid until the host function returns or the World changes:
// it is never kept.
[[nodiscard]] bool resolveEntity(JSContext* ctx, const ScriptApi& api, const EntityRef& ref, const Site& site);
[[nodiscard]] void* resolveStorage(JSContext* ctx, const ScriptApi& api, const EntityRef& ref, std::size_t component,
                                   const Site& site);

// The host functions the class install attaches (magic: see the comment on each).
JSValue entityMethod(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int method) noexcept;
JSValue entityAlive(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
// A component field's accessor pair; magic = component << 8 | field.
JSValue fieldGet(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
JSValue fieldSet(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept;

// The methods of engine.Entity, in magic order.
inline constexpr std::array<std::string_view, 4> ENTITY_METHODS{"has", "get", "add", "remove"};

[[nodiscard]] constexpr int fieldMagic(std::size_t component, std::size_t field) noexcept {
    return static_cast<int>((component << 8U) | field);
}
[[nodiscard]] constexpr std::size_t fieldMagicComponent(int magic) noexcept {
    return static_cast<std::size_t>(magic) >> 8U;
}
[[nodiscard]] constexpr std::size_t fieldMagicField(int magic) noexcept {
    return static_cast<std::size_t>(magic) & 0xFFU;
}

// ---- a component field's value (script_api_values.cpp) ----------------------------------------------------------
// One value of any field kind, between the script and a member. Each of the three functions is one exhaustive switch
// over FieldKind.
struct FieldValue {
    float real = 0.0F;              // Float
    bool flag = false;              // Bool
    std::uint32_t whole = 0;        // UInt32
    Guid guid{};                    // Guid
    std::string text;               // String
    std::array<float, 4> vector{};  // Vec3 and Quat
};

// True for the kinds whose accessors the install attaches.
[[nodiscard]] bool fieldIsScriptBound(internal::FieldKind kind) noexcept;
// Checks `value` against the field's kind and reads it, converting nothing a script could observe. False: a TypeError
// or RangeError is pending, and nothing has been read from or written to the World.
[[nodiscard]] bool readFieldValue(JSContext* ctx, const ScriptApi& api, internal::FieldKind kind, const Site& site,
                                  JSValueConst value, FieldValue& out);
void storeFieldValue(internal::FieldKind kind, void* member, const FieldValue& value);
// The member as a script value. JS_EXCEPTION with an exception pending when the heap limit refused it.
[[nodiscard]] JSValue loadFieldValue(JSContext* ctx, internal::FieldKind kind, const void* member) noexcept;

// ---- the vector class (script_api_values.cpp) -------------------------------------------------------------------
// A constructor and `set` / `copy` carry the dimension (3 or 4) as their magic; an axis accessor carries
// vectorMagic(dimension, axis).

JSValue vectorConstructor(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int dimension) noexcept;
JSValue vectorGetAxis(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
JSValue vectorSetAxis(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept;
JSValue vectorSet(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int dimension) noexcept;
JSValue vectorCopy(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int dimension) noexcept;

}  // namespace engine::script
