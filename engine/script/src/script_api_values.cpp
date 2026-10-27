// Aero Engine -- the vector classes' host functions (task 4.2.1): engine.Vec3 and engine.Quat. script_api.hpp states
// the rules every host function keeps; the ones that shape this file are 3 (a tag check before any conversion, so a
// valueOf, a getter or a Proxy never runs), 5 (an object is made with JS_NewObjectClass, never by setting
// properties) and 9 (receiver, then values, then the write).
//
// A VectorCell is a detached vector's storage. Every function below handles that case; the place where a vector that
// names a component field would be resolved is marked by assert(!cell.view).
//
// The second half is the value contract of a component field: one script value in (checked by tag, converting nothing
// a script could observe), one member out, and the other way round, each an exhaustive switch over FieldKind.

#include <aero/core/guid.hpp>

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

bool fieldIsScriptBound(internal::FieldKind kind) noexcept {
    switch (kind) {
        case internal::FieldKind::Float:
        case internal::FieldKind::Bool:
        case internal::FieldKind::UInt32:
        case internal::FieldKind::Guid:
        case internal::FieldKind::String:
            return true;
        case internal::FieldKind::Vec3:
        case internal::FieldKind::Quat:
            return false;  // the vector fields arrive with the view runtime
    }
    return false;
}

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
        case internal::FieldKind::Quat:
            assert(false && "a vector field has no accessor yet");
            return wrongType(ctx, api, site, value,
                             kind == internal::FieldKind::Vec3 ? "an engine.Vec3" : "an engine.Quat");
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
            assert(false && "a vector field has no accessor yet");
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
            assert(false && "a vector field has no accessor yet");
            return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

}  // namespace engine::script
