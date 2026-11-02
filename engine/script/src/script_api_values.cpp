// Aero Engine -- the vector classes' host functions (task 4.2.1): engine.Vec3 and engine.Quat. script_api.hpp states
// the rules every host function keeps; the ones that shape this file are 3 (a tag check before any conversion, so a
// valueOf, a getter or a Proxy never runs), 5 (an object is made with JS_NewObjectClass, never by setting
// properties) and 9 (receiver, then values, then the write).
//
// A VectorCell is either a detached vector's own storage, or a view that names one Vec3 or Quat field of one entity's
// component. A view holds no pointer: every axis read or write, `set` and `copy` resolves the entity again, so a view
// of a destroyed entity or a removed component throws and writes nothing. The order is the same everywhere: receiver,
// every value, the source of a copy (read out whole), then the target, then the write.
//
// The second half is the value contract of a component field: one script value in (checked by tag, converting nothing
// a script could observe), one member out, and the other way round, each an exhaustive switch over FieldKind.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>

#include "script_api.hpp"
#include "vm_impl.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace engine::script {
namespace {

constexpr std::array<char, 4> AXIS_LETTERS{'x', 'y', 'z', 'w'};

[[nodiscard]] std::string className(int dimension) { return dimension == 3 ? "engine.Vec3" : "engine.Quat"; }

[[nodiscard]] JSClassID vectorClass(const ScriptApi& api, int dimension) noexcept {
    return dimension == 3 ? api.vec3Class : api.quatClass;
}

[[nodiscard]] std::string axisName(int axis) { return std::string(1, AXIS_LETTERS[static_cast<std::size_t>(axis)]); }

[[nodiscard]] std::string axisWhere(int dimension, int axis) { return className(dimension) + "." + axisName(axis); }

// The receiver as a cell, or nullptr: a prototype, a plain object or the other vector class is not one. The message of
// the refusal is built only when it is thrown, so the common path allocates nothing.
[[nodiscard]] VectorCell* findCell(JSContext* ctx, JSValueConst thisValue, int dimension) noexcept {
    return static_cast<VectorCell*>(JS_GetOpaque(thisValue, vectorClass(scriptApiOf(ctx), dimension)));
}

[[nodiscard]] JSValue foreignReceiver(JSContext* ctx, int dimension, const std::string& where) {
    return throwTypeError(ctx, where + ": the receiver is not an " + className(dimension));
}

enum class NumberRead : std::uint8_t { Ok, NotANumber, Failed };

// A number narrowed to a float: round to nearest, overflow to infinity, as a Float32Array stores it; NaN, the
// infinities and -0 survive. The tag is checked first, so nothing a script wrote can run.
[[nodiscard]] NumberRead readFloat(JSContext* ctx, JSValueConst value, float& out) noexcept {
    static_assert(std::numeric_limits<float>::is_iec559, "the narrowing below relies on IEEE 754 single precision");
    if (!JS_IsNumber(value)) {
        return NumberRead::NotANumber;
    }
    double wide = 0.0;
    if (JS_ToFloat64(ctx, &wide, value) < 0) {
        return NumberRead::Failed;
    }
    out = static_cast<float>(wide);
    return NumberRead::Ok;
}

// "<where>: expected a number, got <actual>"
[[nodiscard]] JSValue notANumber(JSContext* ctx, JSValueConst value, const std::string& where) {
    return throwTypeError(ctx, where + ": expected a number, got " + describeValue(ctx, value, scriptApiOf(ctx)));
}

[[nodiscard]] std::array<float, 4> loadVector(int dimension, const void* address) noexcept {
    if (dimension == 3) {
        const auto& v = *static_cast<const Vec3*>(address);
        return {v.x, v.y, v.z, 0.0F};
    }
    const auto& q = *static_cast<const Quat*>(address);
    return {q.x, q.y, q.z, q.w};
}

void storeVector(int dimension, void* address, const std::array<float, 4>& values) noexcept {
    if (dimension == 3) {
        *static_cast<Vec3*>(address) = Vec3{values[0], values[1], values[2]};
        return;
    }
    *static_cast<Quat*>(address) = Quat{values[0], values[1], values[2], values[3]};
}

// Where a view names its field: "engine.<C>.<f>", plus the axis for an axis access (a view into static storage).
[[nodiscard]] Site viewSite(const ScriptApi& api, const VectorCell& cell, std::string_view axis) noexcept {
    const internal::ComponentBinding& bound = api.table->components()[cell.component];
    return Site{bound.scriptName, bound.fields[cell.field].name, axis};
}

[[nodiscard]] std::string_view axisLetter(int axis) noexcept {
    return std::string_view{&AXIS_LETTERS[static_cast<std::size_t>(axis)], 1};
}

// The field a view names, resolved in the World right now. nullptr: a TypeError is pending. The address is valid until
// the host function returns or the World changes, and is never kept.
[[nodiscard]] void* resolveView(JSContext* ctx, const ScriptApi& api, const VectorCell& cell, std::string_view axis) {
    void* const storage = resolveStorage(ctx, api, cell.ref, cell.component, viewSite(api, cell, axis));
    if (storage == nullptr) {
        return nullptr;
    }
    return api.table->components()[cell.component].fields[cell.field].address(storage);
}

// All of a vector's values: a detached one's own, a view's as the World holds them now. False: a TypeError is pending.
[[nodiscard]] bool readVector(JSContext* ctx, const ScriptApi& api, const VectorCell& cell, int dimension,
                              std::array<float, 4>& out) {
    if (!cell.view) {
        out = cell.values;
        return true;
    }
    const void* const address = resolveView(ctx, api, cell, {});
    if (address == nullptr) {
        return false;
    }
    out = loadVector(dimension, address);
    return true;
}

// Writes all of a vector's values into the cell: its own storage, or the field a view names.
[[nodiscard]] bool writeVector(JSContext* ctx, const ScriptApi& api, VectorCell& cell, int dimension,
                               const std::array<float, 4>& values, std::string_view axis) {
    if (!cell.view) {
        cell.values = values;
        return true;
    }
    void* const address = resolveView(ctx, api, cell, axis);
    if (address == nullptr) {
        return false;
    }
    storeVector(dimension, address, values);
    return true;
}

}  // namespace

JSValue vectorConstructor(JSContext* ctx, JSValueConst /*newTarget*/, int /*argc*/, JSValueConst* argv,
                          int dimension) noexcept {
    // The constructor was made with length == dimension, so argv holds that many values (undefined where the script
    // passed fewer). new.target is ignored: the object always has the engine's prototype.
    std::array<float, 4> values{0.0F, 0.0F, 0.0F, dimension == 4 ? 1.0F : 0.0F};
    for (int axis = 0; axis < dimension; ++axis) {
        const JSValueConst argument = argv[axis];
        if (JS_IsUndefined(argument)) {
            continue;  // the default
        }
        switch (readFloat(ctx, argument, values[static_cast<std::size_t>(axis)])) {
            case NumberRead::Ok:
                break;
            case NumberRead::NotANumber:
                return notANumber(ctx, argument, "new " + className(dimension) + ": " + axisName(axis));
            case NumberRead::Failed:
                return JS_EXCEPTION;
        }
    }
    VectorCell cell;
    cell.values = values;
    return newOpaqueObject(ctx, vectorClass(scriptApiOf(ctx), dimension), cell);
}

JSValue vectorGetAxis(JSContext* ctx, JSValueConst thisValue, int magic) noexcept {
    const int dimension = vectorDimension(magic);
    const int axis = vectorAxis(magic);
    const VectorCell* const cell = findCell(ctx, thisValue, dimension);
    if (cell == nullptr) {
        return foreignReceiver(ctx, dimension, axisWhere(dimension, axis));
    }
    std::array<float, 4> values{};
    if (!cell->view) {
        values = cell->values;
    } else {
        const void* const address = resolveView(ctx, scriptApiOf(ctx), *cell, axisLetter(axis));
        if (address == nullptr) {
            return JS_EXCEPTION;
        }
        values = loadVector(dimension, address);
    }
    return JS_NewFloat64(ctx, static_cast<double>(values[static_cast<std::size_t>(axis)]));
}

JSValue vectorSetAxis(JSContext* ctx, JSValueConst thisValue, JSValueConst value, int magic) noexcept {
    const int dimension = vectorDimension(magic);
    const int axis = vectorAxis(magic);
    VectorCell* const cell = findCell(ctx, thisValue, dimension);
    if (cell == nullptr) {
        return foreignReceiver(ctx, dimension, axisWhere(dimension, axis));
    }
    float narrowed = 0.0F;
    switch (readFloat(ctx, value, narrowed)) {
        case NumberRead::Ok:
            break;
        case NumberRead::NotANumber:
            // A view names its field ("engine.<C>.<f>.<axis>"), a detached vector its class ("engine.Vec3.<axis>").
            return notANumber(ctx, value,
                              cell->view ? describeSite(viewSite(scriptApiOf(ctx), *cell, axisLetter(axis)))
                                         : axisWhere(dimension, axis));
        case NumberRead::Failed:
            return JS_EXCEPTION;
    }
    if (!cell->view) {
        cell->values[static_cast<std::size_t>(axis)] = narrowed;
        return JS_UNDEFINED;
    }
    // The other axes are read back from the World and written unchanged: nothing sits between the read and the write.
    void* const address = resolveView(ctx, scriptApiOf(ctx), *cell, axisLetter(axis));
    if (address == nullptr) {
        return JS_EXCEPTION;
    }
    std::array<float, 4> values = loadVector(dimension, address);
    values[static_cast<std::size_t>(axis)] = narrowed;
    storeVector(dimension, address, values);
    return JS_UNDEFINED;
}

JSValue vectorSet(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* argv, int dimension) noexcept {
    VectorCell* const cell = findCell(ctx, thisValue, dimension);
    if (cell == nullptr) {
        return foreignReceiver(ctx, dimension, className(dimension) + ".set");
    }
    // Every argument is a number, or nothing is written: the values are read into a local first.
    std::array<float, 4> values{};
    for (int axis = 0; axis < dimension; ++axis) {
        switch (readFloat(ctx, argv[axis], values[static_cast<std::size_t>(axis)])) {
            case NumberRead::Ok:
                break;
            case NumberRead::NotANumber:
                return notANumber(ctx, argv[axis], className(dimension) + ".set: " + axisName(axis));
            case NumberRead::Failed:
                return JS_EXCEPTION;
        }
    }
    if (!writeVector(ctx, scriptApiOf(ctx), *cell, dimension, values, {})) {
        return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, thisValue);
}

JSValue vectorCopy(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* argv, int dimension) noexcept {
    VectorCell* const cell = findCell(ctx, thisValue, dimension);
    if (cell == nullptr) {
        return foreignReceiver(ctx, dimension, className(dimension) + ".copy");
    }
    // The argument is an instance of the same class, found by its opaque: no property of a plain object is read.
    const auto* const other =
        static_cast<const VectorCell*>(JS_GetOpaque(argv[0], vectorClass(scriptApiOf(ctx), dimension)));
    if (other == nullptr) {
        return throwTypeError(ctx, className(dimension) + ".copy: expected an " + className(dimension) + ", got " +
                                       describeValue(ctx, argv[0], scriptApiOf(ctx)));
    }
    // The source is read out whole before the target is resolved, so `a.copy(a)` reads before it writes.
    std::array<float, 4> values{};
    if (!readVector(ctx, scriptApiOf(ctx), *other, dimension, values) ||
        !writeVector(ctx, scriptApiOf(ctx), *cell, dimension, values, {})) {
        return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, thisValue);
}

// ---- the in-place helpers (task 4.4.2) --------------------------------------------------------------------------
// One dispatcher for every Vec3 and Quat helper. The order is the one the rules fix: the receiver, then the kind of
// every argument, then every vector argument read out whole, then the receiver read, then the arithmetic, then one
// write for a mutator. So `v.cross(v)` and `t.position.add(t.position)` read before they write, and a stale view
// argument throws before the receiver changes. Every result comes from the engine's own float math, and each core
// function that asserts on an input a script can supply (a zero or NaN quaternion, a non-unit axis) is guarded here.

namespace {

// How many arguments a helper takes and of which kind: at most one vector first (its class: 3 for a Vec3, 4 for a
// Quat, 0 for none), then `numbers` numbers.
struct HelperShape {
    int vectorClassDimension = 0;
    int numbers = 0;
};

[[nodiscard]] HelperShape shapeOf(int dimension, VectorOp op) noexcept {
    switch (op) {
        case VectorOp::Add:
        case VectorOp::Sub:
        case VectorOp::Cross:
        case VectorOp::DistanceTo:
            return {3, 0};
        case VectorOp::Scale:
            return {0, 1};
        case VectorOp::AddScaled:
        case VectorOp::Lerp:
        case VectorOp::SetFromAxisAngle:
            return {3, 1};
        case VectorOp::Negate:
        case VectorOp::Normalize:
        case VectorOp::Clone:
        case VectorOp::Length:
        case VectorOp::LengthSquared:
        case VectorOp::Invert:
        case VectorOp::ToEuler:
            return {0, 0};
        case VectorOp::ApplyQuat:
        case VectorOp::Multiply:
        case VectorOp::Premultiply:
            return {4, 0};
        case VectorOp::Slerp:
            return {4, 1};
        case VectorOp::Dot:
            return {dimension, 0};
        case VectorOp::SetFromEuler:
            return {0, 3};
    }
    return {};
}

// "engine.<Class>.<helper>": built only when something is thrown.
[[nodiscard]] std::string helperWhere(int dimension, VectorOp op) {
    const auto named = [&](const auto& helpers) {
        for (const VectorHelper& helper : helpers) {
            if (helper.op == op) {
                return className(dimension) + "." + std::string{helper.name};
            }
        }
        return className(dimension);
    };
    return dimension == 3 ? named(VEC3_HELPERS) : named(QUAT_HELPERS);
}

[[nodiscard]] Vec3 vec3Of(const std::array<float, 4>& values) noexcept { return Vec3{values[0], values[1], values[2]}; }
[[nodiscard]] Quat quatValueOf(const std::array<float, 4>& values) noexcept {
    return Quat{values[0], values[1], values[2], values[3]};
}

struct HelperInput {
    std::array<float, 4> receiver{};
    std::array<float, 4> vector{};  // the vector argument, when the helper takes one
    std::array<float, 3> numbers{};
};

// What a helper produces: new values for the receiver, a number, or a new detached vector (of `dimension`).
struct HelperResult {
    enum class Kind : std::uint8_t { Store, Number, Detached, Unsupported };
    Kind kind = Kind::Unsupported;
    std::array<float, 4> values{};
    float number = 0.0F;
    int dimension = 3;
};

[[nodiscard]] HelperResult storing(Vec3 v) noexcept {
    return {HelperResult::Kind::Store, {v.x, v.y, v.z, 0.0F}, 0.0F, 3};
}
[[nodiscard]] HelperResult storing(Quat q) noexcept {
    return {HelperResult::Kind::Store, {q.x, q.y, q.z, q.w}, 0.0F, 4};
}
[[nodiscard]] HelperResult numeric(float number) noexcept { return {HelperResult::Kind::Number, {}, number, 3}; }
[[nodiscard]] HelperResult detached(Vec3 v) noexcept {
    return {HelperResult::Kind::Detached, {v.x, v.y, v.z, 0.0F}, 0.0F, 3};
}
[[nodiscard]] HelperResult detached(Quat q) noexcept {
    return {HelperResult::Kind::Detached, {q.x, q.y, q.z, q.w}, 0.0F, 4};
}

// invert(): NaN gives NaN in every component (inverse() asserts lenSq > 0, which NaN fails, so the NaN test comes
// first), a length within EPSILON of zero gives the identity, anything else is inverse().
[[nodiscard]] Quat invertTotal(Quat q) noexcept {
    const float lenSq = lengthSquared(q);
    if (std::isnan(lenSq)) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return Quat{nan, nan, nan, nan};
    }
    if (lenSq <= EPSILON * EPSILON) {
        return Quat::identity();
    }
    return inverse(q);
}

// setFromAxisAngle(): fromAxisAngle() asserts a unit axis, which a zero, NaN or infinite axis is not even after
// normalizeOrZero. Zero gives the identity; any other non-unit result (NaN, from a NaN or infinite axis) stores NaN.
[[nodiscard]] Quat axisAngleTotal(Vec3 axis, float radians) noexcept {
    const Vec3 unit = normalizeOrZero(axis);
    const float unitSq = lengthSquared(unit);
    if (unitSq == 0.0F) {
        return Quat::identity();
    }
    if (!approxEquals(unitSq, 1.0F)) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return Quat{nan, nan, nan, nan};
    }
    return fromAxisAngle(unit, radians);
}

// An op the class does not install is unreachable (the magic names the class), and answers Unsupported.
[[nodiscard]] HelperResult vec3Helper(VectorOp op, const HelperInput& in) noexcept {
    const Vec3 a = vec3Of(in.receiver);
    const Vec3 v = vec3Of(in.vector);
    const float s = in.numbers[0];
    switch (op) {
        case VectorOp::Add:
            return storing(a + v);
        case VectorOp::Sub:
            return storing(a - v);
        case VectorOp::Scale:
            return storing(a * s);
        case VectorOp::AddScaled:
            return storing(a + v * s);
        case VectorOp::Negate:
            return storing(-a);
        case VectorOp::Normalize:
            return storing(normalizeOrZero(a));
        case VectorOp::Lerp:
            return storing(lerp(a, v, s));
        case VectorOp::Cross:
            return storing(cross(a, v));
        case VectorOp::ApplyQuat:
            return storing(normalizeOrIdentity(quatValueOf(in.vector)) * a);
        case VectorOp::Clone:
            return detached(a);
        case VectorOp::Dot:
            return numeric(dot(a, v));
        case VectorOp::Length:
            return numeric(length(a));
        case VectorOp::LengthSquared:
            return numeric(lengthSquared(a));
        case VectorOp::DistanceTo:
            return numeric(distance(a, v));
        case VectorOp::Multiply:
        case VectorOp::Premultiply:
        case VectorOp::Invert:
        case VectorOp::Slerp:
        case VectorOp::SetFromAxisAngle:
        case VectorOp::SetFromEuler:
        case VectorOp::ToEuler:
            return {};
    }
    return {};
}

[[nodiscard]] HelperResult quatHelper(VectorOp op, const HelperInput& in) noexcept {
    const Quat a = quatValueOf(in.receiver);
    const Quat q = quatValueOf(in.vector);
    switch (op) {
        case VectorOp::Multiply:
            return storing(a * q);
        case VectorOp::Premultiply:
            return storing(q * a);
        case VectorOp::Invert:
            return storing(invertTotal(a));
        case VectorOp::Normalize:
            return storing(normalizeOrIdentity(a));
        case VectorOp::Slerp:
            return storing(slerp(a, q, in.numbers[0]));
        case VectorOp::SetFromAxisAngle:
            return storing(axisAngleTotal(vec3Of(in.vector), in.numbers[0]));
        case VectorOp::SetFromEuler:
            return storing(normalizeOrIdentity(fromEulerAngles(Vec3{in.numbers[0], in.numbers[1], in.numbers[2]})));
        case VectorOp::Clone:
            return detached(a);
        case VectorOp::ToEuler:
            return detached(eulerAngles(a));
        case VectorOp::Dot:
            return numeric(dot(a, q));
        case VectorOp::Add:
        case VectorOp::Sub:
        case VectorOp::Scale:
        case VectorOp::AddScaled:
        case VectorOp::Negate:
        case VectorOp::Lerp:
        case VectorOp::Cross:
        case VectorOp::ApplyQuat:
        case VectorOp::Length:
        case VectorOp::LengthSquared:
        case VectorOp::DistanceTo:
            return {};
    }
    return {};
}

// "<where>: expected an engine.Vec3, got <actual>"
[[nodiscard]] JSValue notAVector(JSContext* ctx, JSValueConst value, int dimension, const std::string& where) {
    return throwTypeError(
        ctx, where + ": expected an " + className(dimension) + ", got " + describeValue(ctx, value, scriptApiOf(ctx)));
}

}  // namespace

JSValue vectorHelper(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* argv, int magic) noexcept {
    const int dimension = magic / 32;
    const auto op = static_cast<VectorOp>(magic % 32);
    const ScriptApi& api = scriptApiOf(ctx);
    VectorCell* const cell = findCell(ctx, thisValue, dimension);
    if (cell == nullptr) {
        return foreignReceiver(ctx, dimension, helperWhere(dimension, op));
    }
    // The kind of every argument first, by opaque or by tag: nothing a script could observe runs. The helper was made
    // with its length, so argv holds that many values (undefined where the script passed fewer).
    const HelperShape shape = shapeOf(dimension, op);
    HelperInput input;
    const VectorCell* argument = nullptr;
    int next = 0;
    if (shape.vectorClassDimension != 0) {
        argument = static_cast<const VectorCell*>(JS_GetOpaque(argv[0], vectorClass(api, shape.vectorClassDimension)));
        if (argument == nullptr) {
            return notAVector(ctx, argv[0], shape.vectorClassDimension, helperWhere(dimension, op));
        }
        next = 1;
    }
    for (int i = 0; i < shape.numbers; ++i) {
        const JSValueConst number = argv[next + i];
        switch (readFloat(ctx, number, input.numbers[static_cast<std::size_t>(i)])) {
            case NumberRead::Ok:
                break;
            case NumberRead::NotANumber:
                return notANumber(ctx, number, helperWhere(dimension, op));
            case NumberRead::Failed:
                return JS_EXCEPTION;
        }
    }
    // Then the vector argument out whole, then the receiver: both before anything is written.
    if ((argument != nullptr && !readVector(ctx, api, *argument, shape.vectorClassDimension, input.vector)) ||
        !readVector(ctx, api, *cell, dimension, input.receiver)) {
        return JS_EXCEPTION;
    }
    const HelperResult result = dimension == 3 ? vec3Helper(op, input) : quatHelper(op, input);
    switch (result.kind) {
        case HelperResult::Kind::Store:
            if (!writeVector(ctx, api, *cell, dimension, result.values, {})) {
                return JS_EXCEPTION;
            }
            return JS_DupValue(ctx, thisValue);
        case HelperResult::Kind::Number:
            return JS_NewFloat64(ctx, static_cast<double>(result.number));
        case HelperResult::Kind::Detached:
            return newDetachedVector(ctx, api, result.dimension, result.values);
        case HelperResult::Kind::Unsupported:
            break;
    }
    return foreignReceiver(ctx, dimension, helperWhere(dimension, op));
}

bool readVectorArgument(JSContext* ctx, const ScriptApi& api, JSValueConst value, int dimension,
                        const std::string& where, std::array<float, 4>& out) {
    const auto* const cell = static_cast<const VectorCell*>(JS_GetOpaque(value, vectorClass(api, dimension)));
    if (cell == nullptr) {
        static_cast<void>(notAVector(ctx, value, dimension, where));
        return false;
    }
    return readVector(ctx, api, *cell, dimension, out);
}

JSValue newDetachedVector(JSContext* ctx, const ScriptApi& api, int dimension,
                          const std::array<float, 4>& values) noexcept {
    VectorCell cell;
    cell.values = values;
    return newOpaqueObject(ctx, vectorClass(api, dimension), cell);
}

}  // namespace engine::script

// ---- a component field's value ----------------------------------------------------------------------------------

namespace engine::script {
namespace {

// "<site>: expected <expected>, got <actual>". False, so a caller returns it.
[[nodiscard]] bool wrongType(JSContext* ctx, const ScriptApi& api, const Site& site, JSValueConst value,
                             std::string_view expected) {
    static_cast<void>(throwTypeError(
        ctx, describeSite(site) + ": expected " + std::string{expected} + ", got " + describeValue(ctx, value, api)));
    return false;
}

// std::format's spelling, except that NaN and the infinities read as a script writes them.
[[nodiscard]] std::string numberText(double number) {
    if (std::isnan(number)) {
        return "NaN";
    }
    if (std::isinf(number)) {
        return number < 0.0 ? "-Infinity" : "Infinity";
    }
    return std::format("{}", number);
}

constexpr double MAX_UINT32 = 4294967295.0;

}  // namespace

bool readFieldValue(JSContext* ctx, const ScriptApi& api, internal::FieldKind kind, const Site& site,
                    JSValueConst value, FieldValue& out) {
    switch (kind) {
        case internal::FieldKind::Float: {
            // Narrowed as a Float32Array stores it: NaN, the infinities and -0 survive, an overflow is infinite.
            if (!JS_IsNumber(value)) {
                return wrongType(ctx, api, site, value, "a number");
            }
            double wide = 0.0;
            if (JS_ToFloat64(ctx, &wide, value) < 0) {
                return false;
            }
            out.real = static_cast<float>(wide);
            return true;
        }
        case internal::FieldKind::Bool:
            if (!JS_IsBool(value)) {
                return wrongType(ctx, api, site, value, "a boolean");
            }
            out.flag = JS_ToBool(ctx, value) != 0;
            return true;
        case internal::FieldKind::UInt32: {
            if (!JS_IsNumber(value)) {
                return wrongType(ctx, api, site, value, "a number");
            }
            double wide = 0.0;
            if (JS_ToFloat64(ctx, &wide, value) < 0) {
                return false;
            }
            // Checked before the conversion, so it is always in range (and -0 stores 0).
            if (!std::isfinite(wide) || wide != std::floor(wide) || wide < 0.0 || wide > MAX_UINT32) {
                static_cast<void>(throwRangeError(
                    ctx, describeSite(site) + ": expected an integer in [0, 4294967295], got " + numberText(wide)));
                return false;
            }
            out.whole = static_cast<std::uint32_t>(wide);
            return true;
        }
        case internal::FieldKind::Guid: {
            if (!JS_IsString(value)) {
                return wrongType(ctx, api, site, value, "a string");
            }
            const HeldCString text{ctx, value};
            if (!text.ok()) {
                return false;
            }
            const std::optional<Guid> parsed = parseGuid(text.view());
            if (!parsed.has_value()) {
                static_cast<void>(throwRangeError(ctx, describeSite(site) + ": expected 32 hex digits"));
                return false;
            }
            out.guid = *parsed;
            return true;
        }
        case internal::FieldKind::String: {
            if (!JS_IsString(value)) {
                return wrongType(ctx, api, site, value, "a string");
            }
            const HeldCString text{ctx, value};
            if (!text.ok()) {
                return false;
            }
            out.text.assign(text.view());  // an embedded U+0000 is kept: the length comes from the backend
            return true;
        }
        case internal::FieldKind::Vec3:
        case internal::FieldKind::Quat: {
            // An instance of the field's class, found by its opaque: no property of a plain object is read. A view
            // source is resolved and copied out here, before the caller resolves the target.
            const int dimension = vectorDimensionOf(kind);
            const JSClassID classId = vectorClass(api, dimension);
            const auto* const cell = static_cast<const VectorCell*>(JS_GetOpaque(value, classId));
            if (cell == nullptr) {
                return wrongType(ctx, api, site, value, dimension == 3 ? "an engine.Vec3" : "an engine.Quat");
            }
            return readVector(ctx, api, *cell, dimension, out.vector);
        }
    }
    return false;
}

void storeFieldValue(internal::FieldKind kind, void* member, const FieldValue& value) {
    switch (kind) {
        case internal::FieldKind::Float:
            *static_cast<float*>(member) = value.real;
            return;
        case internal::FieldKind::Bool:
            *static_cast<bool*>(member) = value.flag;
            return;
        case internal::FieldKind::UInt32:
            *static_cast<std::uint32_t*>(member) = value.whole;
            return;
        case internal::FieldKind::Guid:
            *static_cast<Guid*>(member) = value.guid;
            return;
        case internal::FieldKind::String:
            static_cast<std::string*>(member)->assign(value.text);
            return;
        case internal::FieldKind::Vec3:
        case internal::FieldKind::Quat:
            storeVector(vectorDimensionOf(kind), member, value.vector);
            return;
    }
}

JSValue loadFieldValue(JSContext* ctx, internal::FieldKind kind, const void* member) noexcept {
    switch (kind) {
        case internal::FieldKind::Float:
            return JS_NewFloat64(ctx, static_cast<double>(*static_cast<const float*>(member)));
        case internal::FieldKind::Bool:
            return JS_NewBool(ctx, *static_cast<const bool*>(member));
        case internal::FieldKind::UInt32:
            return JS_NewUint32(ctx, *static_cast<const std::uint32_t*>(member));
        case internal::FieldKind::Guid: {
            const std::string text = formatGuid(*static_cast<const Guid*>(member));
            return JS_NewStringLen(ctx, text.data(), text.size());
        }
        case internal::FieldKind::String: {
            const auto& text = *static_cast<const std::string*>(member);
            return JS_NewStringLen(ctx, text.data(), text.size());
        }
        case internal::FieldKind::Vec3:
        case internal::FieldKind::Quat:
            assert(false && "a vector field is read as a view (newVectorView), never as a value");
            return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

JSValue newVectorView(JSContext* ctx, const ScriptApi& api, internal::FieldKind kind, const EntityRef& ref,
                      std::size_t component, std::size_t field) noexcept {
    VectorCell cell;
    cell.ref = ref;
    cell.component = static_cast<std::uint16_t>(component);
    cell.field = static_cast<std::uint16_t>(field);
    cell.view = true;
    return newOpaqueObject(ctx, vectorClass(api, vectorDimensionOf(kind)), cell);
}

}  // namespace engine::script
