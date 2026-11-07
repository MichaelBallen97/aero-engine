#pragma once
// Aero Engine -- the script API's PRIVATE state and shared helpers (task 4.2.1). It includes <quickjs.h>, which is
// why it lives under src/: the side of the boundary .github/scripts/check-script-boundary.sh permits.
//
// The API is `globalThis.engine`: one class per bound component (named by its descriptor), Vec3, Quat, Entity and
// Script, the namespace functions, and the frozen object `input` (task 4.4.3).
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
#include <aero/core/math.hpp>
#include <aero/core/slot_map.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/prefab.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/vm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <quickjs.h>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::platform {
class InputState;  // ScriptApi::input borrows one; script_api_input.cpp includes <aero/platform/input.hpp>
}  // namespace engine::platform

namespace engine::script {

struct InstanceSlot;  // vm_impl.hpp: a host function reaches the slots through ScriptApi::instanceSlots only

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

// The one key every per-entity map of the script layer uses (the 3.7.2 rule: one comparator, one place).
[[nodiscard]] constexpr std::uint64_t entityKeyOf(Entity entity) noexcept {
    return (std::uint64_t{entity.index} << 32U) | entity.generation;
}

// The key of an attached instance in ScriptApi::attachedInstances. Sorted by binding, then entity, then creation order,
// so the earliest live attach of one entity under one binding is the lower_bound of {binding, entity, 0}.
struct AttachKey {
    std::uint64_t binding = 0;
    std::uint64_t entity = 0;  // entityKeyOf
    std::uint64_t order = 0;   // InstanceSlot::order
    auto operator<=>(const AttachKey&) const = default;
};

// "No component": componentClassIndex's answer for a value that names none, and ScriptApi::transformComponent's
// value while the table binds no Transform.
inline constexpr std::size_t NO_COMPONENT = static_cast<std::size_t>(-1);

struct ScriptApi {
    const internal::ComponentBindingTable* table = nullptr;  // borrowed
    World* world = nullptr;                                  // borrowed; null when none is bound
    std::uint64_t binding = 0;                               // +1 on every bindWorld
    const platform::InputState* input = nullptr;             // borrowed; null when none is bound (task 4.4.3)
    // Each id starts at 0 and is allocated by THIS runtime (JS_NewClassID): never a static, which every VM on every
    // thread would share.
    JSClassID entityClass = 0;
    JSClassID vec3Class = 0;
    JSClassID quatClass = 0;
    std::vector<JSClassID> componentClasses;     // parallel to table->components()
    std::vector<JSValue> componentConstructors;  // HELD, parallel: entity.get(C)'s identity test
    JSValue scriptBase = JS_UNDEFINED;           // HELD: engine.Script, for the direct-new refusal; released by
                                                 // releaseScriptApi before the context
    std::optional<EntityRef> pendingEntity;      // set by the VM around ONE engine-driven construction; taken by the
                                                 // first base construction inside it (C4)
    // Task 4.4.2: the one engine.Entity object of each entity of the current binding, HELD. Emptied by every bindWorld
    // and unbindWorld and by releaseScriptApi, and an entry is evicted by Vm::destroyQueued.
    std::unordered_map<std::uint64_t, JSValue> entityObjects;
    // engine.destroy's queue under the current binding, in queue order, and the mark of every entity queued and not yet
    // destroyed through Vm::destroyQueued (a taken queue keeps its marks). Emptied with the table.
    std::vector<Entity> destroyQueue;
    std::unordered_set<std::uint64_t> destroyMarks;
    // Every live attached instance: inserted by createInstance, erased by destroyInstance, cleared by releaseScripts.
    // A reload keeps the handle, so it changes nothing here. The slot map is Vm::Impl's, borrowed (set once in create;
    // Impl is heap-pinned).
    std::map<AttachKey, ScriptInstance> attachedInstances;
    const SlotMap<InstanceSlot, ScriptInstanceTag>* instanceSlots = nullptr;
    // The table index of the Transform binding, or NO_COMPONENT (the table is empty with the reflect tools OFF). The
    // transform helpers are installed on that class only.
    std::size_t transformComponent = NO_COMPONENT;
    // spawn(asset, ...) reads a prefab file through these (task 4.4.4): the VM's module source, borrowed, null when the
    // VM was made with none, and the heap limit that caps a file's size as it caps a module's.
    const VirtualFileSystem* modules = nullptr;
    std::size_t heapLimitBytes = 0;
    // "res://..." (normalized) -> the prefab read from that file under the current binding. Emptied with the entity
    // table, so a new binding reads the file again; a failed read inserts nothing. It holds no JSValue.
    std::unordered_map<std::string, Prefab> prefabs;
};

enum class ClassCheck : std::uint8_t {
    IsClass,
    NotAClass,
    Failed,  // an exception is pending: only a refused allocation can cause one
};

// The class rule, once: a non-Proxy constructor whose own `prototype` is a non-writable data property. An ordinary
// function's `prototype` is writable, a bound function has none, and a class defines it read-only. A Proxy is refused
// before any read, because a descriptor read would run its trap; JS_GetOwnProperty runs no getter. Runs no script
// code. `prototype`, when given, receives the prototype value HELD by the caller (IsClass only).
[[nodiscard]] ClassCheck checkClass(JSContext* ctx, JSValueConst value, JSValue* prototype = nullptr);

// Builds `globalThis.engine` over api.table. False: an allocation failed (the heap limit only) and an exception is
// pending; whatever was built is released by releaseScriptApi and the context.
[[nodiscard]] bool installScriptApi(JSContext* ctx, JSRuntime* rt, ScriptApi& api);
// Frees every reference the API holds; Impl::shutdown calls it before the context is freed.
void releaseScriptApi(JSContext* ctx, ScriptApi& api) noexcept;
// The API a host function belongs to: the context opaque, set at create.
[[nodiscard]] ScriptApi& scriptApiOf(JSContext* ctx) noexcept;

// ---- the entity table (script_api_world.cpp) --------------------------------------------------------------------
// The one engine.Entity object of `ref` while `ref` is of the current binding and a World is bound: made once,
// non-extensible, held until the binding ends, and returned with a new reference on every call. Any other ref (an
// earlier binding, or no World) gets a new non-extensible object the table never holds. JS_EXCEPTION, with an exception
// pending, when the heap limit refused it.
[[nodiscard]] JSValue entityObjectFor(JSContext* ctx, ScriptApi& api, const EntityRef& ref);
// Frees every held object and empties the table, the destroy queue, the marks and the prefab cache. noexcept:
// JS_FreeValue runs only the shared finalizer (finalizeOpaque), never script code.
void releaseEntityObjects(JSContext* ctx, ScriptApi& api) noexcept;
// A new array holding the entity object of each of `entities`, in order, all of the current binding (the array rule: no
// World access happens while objects are made). JS_EXCEPTION, with an exception pending, when the heap limit refused
// one; whatever was made is freed then.
[[nodiscard]] JSValue entityArray(JSContext* ctx, ScriptApi& api, const std::vector<Entity>& entities);
// entityArray over the direct children of `parent`, in attach order. Its own function, in this file, so a noexcept host
// function calls it without a vector's throwing paths being visible to clang-tidy's bugprone-exception-escape.
[[nodiscard]] JSValue childrenArray(JSContext* ctx, ScriptApi& api, Entity parent);
// engine.findAll's and engine.query's results (the World is bound; the checks were made by the caller). Their own
// functions, in this file, for the reason childrenArray gives. Ascending entity index.
[[nodiscard]] JSValue findAllArray(JSContext* ctx, ScriptApi& api, std::string_view name);
[[nodiscard]] JSValue queryArray(JSContext* ctx, ScriptApi& api, std::span<const std::size_t> components);

// ---- the namespace functions (script_api_world.cpp) -------------------------------------------------------------
struct WorldFunction {
    std::string_view name;
    int length;
};
// In magic order. query's length is World::MAX_QUERY_TYPES (8), so argv always holds 8 values.
inline constexpr std::array<WorldFunction, 5> WORLD_FUNCTIONS{
    {{"spawn", 4}, {"destroy", 1}, {"find", 1}, {"findAll", 1}, {"query", 8}}};
inline constexpr std::size_t MAX_QUERY_CLASSES = 8;
// spawn's length is its most arguments: spawn(asset, position, rotation, scale) (rule 2: argv[0..3] only).
inline constexpr int SPAWN_MAX_ARGUMENTS = 4;
static_assert(WORLD_FUNCTIONS[0].length == SPAWN_MAX_ARGUMENTS);
// One handler per entry of WORLD_FUNCTIONS, each with its checks in the contract's order. They build strings and
// vectors, so they are not noexcept: the host function that calls them (script_api.cpp) is, and keeps their throwing
// paths out of its own translation unit, which clang-tidy's bugprone-exception-escape would otherwise report (the
// reason childrenArray gives).
// spawnEntity serves spawn(): with no argument, a new entity; with any, spawnPrefab.
[[nodiscard]] JSValue spawnEntity(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv);
[[nodiscard]] JSValue queueDestroy(JSContext* ctx, ScriptApi& api, JSValueConst value);
[[nodiscard]] JSValue findEntity(JSContext* ctx, ScriptApi& api, JSValueConst value);
[[nodiscard]] JSValue findAllEntities(JSContext* ctx, ScriptApi& api, JSValueConst value);
[[nodiscard]] JSValue queryEntities(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv);

// ---- spawn(asset, ...) (script_api_prefab.cpp, script_api_transform.cpp) -----------------------------------------
// A placement as spawn reads it. Not the component: script_api_transform.cpp is the one source that may name it.
struct SpawnPlacement {
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3::one();
};
// The whole of spawn(asset, ...), the checks in the contract's order. Not noexcept: it builds strings; the host
// function that calls it (script_api.cpp) is.
[[nodiscard]] JSValue spawnPrefab(JSContext* ctx, ScriptApi& api, int argc, JSValueConst* argv);
// The kinds of argv[1..3], every kind before any read, then each read out whole. False: a TypeError is pending and
// nothing has changed. Reads argv[1] to argv[3] only: spawn's length is 4 (rule 2).
[[nodiscard]] bool readSpawnPlacement(JSContext* ctx, const ScriptApi& api, JSValueConst* argv, SpawnPlacement& out);
// The one call of Prefab::instantiate in the script layer: it needs a Transform, which only script_api_transform.cpp
// may name. A null Entity: the prefab or the World refused.
[[nodiscard]] Entity instantiateAtPlacement(const Prefab& prefab, World& world, const SpawnPlacement& placement);

// ---- engine.input (script_api_input.cpp) --------------------------------------------------------------------------
// The functions of the frozen object `engine.input`. One host function (script_api.cpp) serves them all; its magic is
// the op.
enum class InputOp : std::uint8_t {
    KeyDown,
    KeyPressed,
    KeyReleased,
    MouseButtonDown,
    MouseButtonPressed,
    MouseButtonReleased,
    MousePosition,
    MouseDelta,
    MouseWheel,
    GamepadConnected,
    GamepadButtonDown,
    GamepadButtonPressed,
    GamepadButtonReleased,
    GamepadAxis,
    GamepadStick,
};
struct InputFunction {
    std::string_view name;
    InputOp op;
    int length;  // counts an optional slot argument (rule 2)
};
// In op order, which is the definition order on engine.input.
inline constexpr std::array<InputFunction, 15> INPUT_FUNCTIONS{
    {{"keyDown", InputOp::KeyDown, 1},
     {"keyPressed", InputOp::KeyPressed, 1},
     {"keyReleased", InputOp::KeyReleased, 1},
     {"mouseButtonDown", InputOp::MouseButtonDown, 1},
     {"mouseButtonPressed", InputOp::MouseButtonPressed, 1},
     {"mouseButtonReleased", InputOp::MouseButtonReleased, 1},
     {"mousePosition", InputOp::MousePosition, 0},
     {"mouseDelta", InputOp::MouseDelta, 0},
     {"mouseWheel", InputOp::MouseWheel, 0},
     {"gamepadConnected", InputOp::GamepadConnected, 1},
     {"gamepadButtonDown", InputOp::GamepadButtonDown, 2},
     {"gamepadButtonPressed", InputOp::GamepadButtonPressed, 2},
     {"gamepadButtonReleased", InputOp::GamepadButtonReleased, 2},
     {"gamepadAxis", InputOp::GamepadAxis, 2},
     {"gamepadStick", InputOp::GamepadStick, 2}}};
// The handlers, behind one exhaustive switch over `op`. Not noexcept (they build strings on their throw paths); the one
// noexcept host function that calls them is in script_api.cpp, the worldFunction shape (bugprone-exception-escape).
// `argv` holds at least the function's length of values.
[[nodiscard]] JSValue callInputFunction(JSContext* ctx, ScriptApi& api, InputOp op, JSValueConst* argv);

// ---- the one source that names the Transform component (script_api_transform.cpp) -------------------------------
[[nodiscard]] ComponentTypeId transformTypeId() noexcept;
// world.add<Transform>(entity, {}) != nullptr: the Transform spawn gives a new entity.
[[nodiscard]] bool addIdentityTransform(World& world, Entity entity);

// The helpers engine.Transform carries beyond its bound fields (task 4.4.2). One dispatcher; its magic is the op. The
// receiver is checked against the Transform class by its opaque, every argument's kind before any is read, every
// argument read before the World is touched, and the entity's Transform last, so a refusal writes nothing.
enum class TransformOp : std::uint8_t { WorldPosition, Forward, Right, Up, Translate, Rotate, LookAt };
struct TransformHelper {
    std::string_view name;
    TransformOp op;
    int length;
};
// In definition order on the prototype, after the fields.
inline constexpr std::array<TransformHelper, 7> TRANSFORM_HELPERS{{{"worldPosition", TransformOp::WorldPosition, 0},
                                                                   {"forward", TransformOp::Forward, 0},
                                                                   {"right", TransformOp::Right, 0},
                                                                   {"up", TransformOp::Up, 0},
                                                                   {"translate", TransformOp::Translate, 1},
                                                                   {"rotate", TransformOp::Rotate, 1},
                                                                   {"lookAt", TransformOp::LookAt, 2}}};
JSValue transformHelper(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int magic) noexcept;

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

// Which bound component a class argument names, found by identity against the constructors the API holds: nothing of
// the argument is read, so no getter, Proxy trap or conversion runs. NO_COMPONENT: it names none.
[[nodiscard]] std::size_t componentClassIndex(JSContext* ctx, const ScriptApi& api, JSValueConst argument);

// The host functions the class install attaches (magic: see the comment on each).
JSValue entityMethod(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int method) noexcept;
// engine.Entity's accessors (magic = index into ENTITY_PROPERTIES) and children().
JSValue entityGet(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
JSValue entitySet(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept;
JSValue entityChildren(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int magic) noexcept;
// A component field's accessor pair; magic = component << 8 | field.
JSValue fieldGet(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
JSValue fieldSet(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept;

// The methods of engine.Entity, in magic order.
inline constexpr std::array<std::string_view, 4> ENTITY_METHODS{"has", "get", "add", "remove"};

// The accessors of engine.Entity, in magic order.
struct EntityProperty {
    std::string_view name;
    bool readOnly;
};
inline constexpr std::array<EntityProperty, 4> ENTITY_PROPERTIES{
    {{"alive", true}, {"name", false}, {"parent", false}, {"script", true}}};

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

// A Vec3 or Quat field reads as a view, never as a value; the vector's dimension is 3 or 4.
[[nodiscard]] constexpr bool isVectorKind(internal::FieldKind kind) noexcept {
    return kind == internal::FieldKind::Vec3 || kind == internal::FieldKind::Quat;
}
[[nodiscard]] constexpr int vectorDimensionOf(internal::FieldKind kind) noexcept {
    return kind == internal::FieldKind::Vec3 ? 3 : 4;
}
// Checks `value` against the field's kind and reads it, converting nothing a script could observe. False: a TypeError
// or RangeError is pending, and nothing has been read from or written to the World.
[[nodiscard]] bool readFieldValue(JSContext* ctx, const ScriptApi& api, internal::FieldKind kind, const Site& site,
                                  JSValueConst value, FieldValue& out);
void storeFieldValue(internal::FieldKind kind, void* member, const FieldValue& value);
// The member as a script value (not for a vector kind). JS_EXCEPTION with an exception pending when the heap limit
// refused it.
[[nodiscard]] JSValue loadFieldValue(JSContext* ctx, internal::FieldKind kind, const void* member) noexcept;
// A Vec3 or Quat field as a new view of (ref, component, field); nothing is read yet. JS_EXCEPTION, with an exception
// pending, when the heap limit refused it.
[[nodiscard]] JSValue newVectorView(JSContext* ctx, const ScriptApi& api, internal::FieldKind kind,
                                    const EntityRef& ref, std::size_t component, std::size_t field) noexcept;

// ---- the vector class (script_api_values.cpp) -------------------------------------------------------------------
// A constructor and `set` / `copy` carry the dimension (3 or 4) as their magic; an axis accessor carries
// vectorMagic(dimension, axis).

JSValue vectorConstructor(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv, int dimension) noexcept;
JSValue vectorGetAxis(JSContext* ctx, JSValueConst thisValue, int magic) noexcept;
JSValue vectorSetAxis(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept;
JSValue vectorSet(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int dimension) noexcept;
JSValue vectorCopy(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int dimension) noexcept;

// The in-place helpers (task 4.4.2). One dispatcher; its magic carries the class as well as the operation, so a Quat
// helper lifted onto a Vec3 refuses it as the axis accessors do.
enum class VectorOp : std::uint8_t {
    Add,
    Sub,
    Scale,
    AddScaled,
    Negate,
    Normalize,
    Lerp,
    Cross,
    ApplyQuat,
    Clone,
    Dot,
    Length,
    LengthSquared,
    DistanceTo,
    Multiply,
    Premultiply,
    Invert,
    Slerp,
    SetFromAxisAngle,
    SetFromEuler,
    ToEuler,
};
struct VectorHelper {
    std::string_view name;
    VectorOp op;
    int length;
};
[[nodiscard]] constexpr int vectorHelperMagic(int dimension, VectorOp op) noexcept {
    return dimension * 32 + static_cast<int>(op);  // 21 ops < 32
}
// In definition order on the prototype, after set and copy.
inline constexpr std::array<VectorHelper, 14> VEC3_HELPERS{{{"add", VectorOp::Add, 1},
                                                            {"sub", VectorOp::Sub, 1},
                                                            {"scale", VectorOp::Scale, 1},
                                                            {"addScaled", VectorOp::AddScaled, 2},
                                                            {"negate", VectorOp::Negate, 0},
                                                            {"normalize", VectorOp::Normalize, 0},
                                                            {"lerp", VectorOp::Lerp, 2},
                                                            {"cross", VectorOp::Cross, 1},
                                                            {"applyQuat", VectorOp::ApplyQuat, 1},
                                                            {"clone", VectorOp::Clone, 0},
                                                            {"dot", VectorOp::Dot, 1},
                                                            {"length", VectorOp::Length, 0},
                                                            {"lengthSquared", VectorOp::LengthSquared, 0},
                                                            {"distanceTo", VectorOp::DistanceTo, 1}}};
inline constexpr std::array<VectorHelper, 10> QUAT_HELPERS{{{"multiply", VectorOp::Multiply, 1},
                                                            {"premultiply", VectorOp::Premultiply, 1},
                                                            {"invert", VectorOp::Invert, 0},
                                                            {"normalize", VectorOp::Normalize, 0},
                                                            {"slerp", VectorOp::Slerp, 2},
                                                            {"setFromAxisAngle", VectorOp::SetFromAxisAngle, 2},
                                                            {"setFromEuler", VectorOp::SetFromEuler, 3},
                                                            {"clone", VectorOp::Clone, 0},
                                                            {"toEuler", VectorOp::ToEuler, 0},
                                                            {"dot", VectorOp::Dot, 1}}};
JSValue vectorHelper(JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv, int magic) noexcept;

// Shared with the transform helpers. `where` is the site text ("engine.Transform.translate"). An instance of the
// dimension's class read out whole (a view resolves its entity now); false: a TypeError is pending
// ("<where>: expected an engine.Vec3, got ...") or the view's own resolve error.
[[nodiscard]] bool readVectorArgument(JSContext* ctx, const ScriptApi& api, JSValueConst value, int dimension,
                                      const std::string& where, std::array<float, 4>& out);
// A new detached engine.Vec3 / engine.Quat holding `values`. JS_EXCEPTION with an exception pending on a refusal.
[[nodiscard]] JSValue newDetachedVector(JSContext* ctx, const ScriptApi& api, int dimension,
                                        const std::array<float, 4>& values) noexcept;

}  // namespace engine::script
