// Aero Engine -- the vector classes' host functions (task 4.2.1): engine.Vec3 and engine.Quat. script_api.hpp states
// the rules every host function keeps; the ones that shape this file are 3 (a tag check before any conversion, so a
// valueOf, a getter or a Proxy never runs), 5 (an object is made with JS_NewObjectClass, never by setting
// properties) and 9 (receiver, then values, then the write).
//
// A VectorCell is a detached vector's storage. Every function below handles that case; the place where a vector that
// names a component field would be resolved is marked by assert(!cell.view).

#include "script_api.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    assert(!cell->view);  // a vector that names a component field resolves its entity here
    return JS_NewFloat64(ctx, static_cast<double>(cell->values[static_cast<std::size_t>(axis)]));
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
            return notANumber(ctx, value, axisWhere(dimension, axis));
        case NumberRead::Failed:
            return JS_EXCEPTION;
    }
    assert(!cell->view);
    cell->values[static_cast<std::size_t>(axis)] = narrowed;
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
    assert(!cell->view);
    cell->values = values;
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
    assert(!cell->view && !other->view);
    cell->values = other->values;  // read whole, then written
    return JS_DupValue(ctx, thisValue);
}

}  // namespace engine::script
