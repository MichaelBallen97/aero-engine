#pragma once
#include <aero/core/math.hpp>  // engine::Vec3, engine::Quat
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <concepts>  // std::same_as (N1 -- the spec's include list omits this)
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace engine::reflect {

void writeJson(JsonWriter& w, bool v);
void writeJson(JsonWriter& w, float v);
void writeJson(JsonWriter& w, double v);
void writeJson(JsonWriter& w, const Vec3& v);  // {"x":..,"y":..,"z":..}
void writeJson(JsonWriter& w, const Quat& v);  // {"x":..,"y":..,"z":..,"w":..}

// Every integral type EXCEPT bool -> a JSON number. Widen by signedness so to_chars never sees a character type
// (portability, F2) and bool never binds here (it has its own exact overload).
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
void writeJson(JsonWriter& w, T v) {
    if constexpr (std::is_signed_v<T>) {
        w.value(static_cast<long long>(v));
    } else {
        w.value(static_cast<unsigned long long>(v));
    }
}

// ---- task 1.2.3: generic DOM re-emission (D9) ------------------------------------------------------
//
// Write any parsed JsonValue back out through a JsonWriter. ITERATIVE (misc-no-recursion is live).
// Numbers CANONICALIZE: integral-form lexemes -> exact i64/u64; "-0" preserved as -0; everything else
// -> shortest-round-trip double; a value that rounds to +/-inf (e.g. 1e999) -> null, mirroring the
// writer's own non-finite policy one layer down. Strings are re-escaped by JsonWriter's rules, so a
// \uXXXX input normalizes to raw UTF-8. The output is a canonicalization FIXPOINT: re-emitting it
// reproduces it byte-for-byte, which is what makes the scene format's write-parse-write idempotence
// guarantee (docs/09) hold. Needed because writeScene must re-emit component payloads it does not
// interpret; useful on its own for future tools and editor copy/paste.
void writeJson(JsonWriter& w, const JsonValue& v);

// ---- task 1.2.2: the read half (spec §3.8, D10) ---------------------------------------------------
//
// DECLARATION ORDER IS LOAD-BEARING (N2, proven): every readJson overload -- the five leaves AND the
// constrained integral template below -- must be declared BEFORE readField. ADL for engine::Vec3
// searches namespace engine, not engine::reflect; fundamental types have no associated namespace at
// all -- so the leaves must be visible at readField's definition point by ORDINARY lookup, not ADL.
// Getting this wrong is a hard compile error ("no matching function for call to 'readJson'"), never a
// silent misbehaviour.

bool readJson(const JsonValue& v, bool& out);
bool readJson(const JsonValue& v, float& out);   // Number -> target-precision parse; Null -> quiet NaN
bool readJson(const JsonValue& v, double& out);  // Number -> target-precision parse; Null -> quiet NaN
bool readJson(const JsonValue& v, Vec3& out);    // Object with x,y,z (each via the float rule)
bool readJson(const JsonValue& v, Quat& out);    // Object with x,y,z,w (each via the float rule)

// Every integral type EXCEPT bool, the exact mirror of the write-side widening template: parse via
// asI64/asU64 by signedness, then range-check against std::numeric_limits<T>.
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
bool readJson(const JsonValue& v, T& out) {
    if constexpr (std::is_signed_v<T>) {
        const std::optional<std::int64_t> parsed = v.asI64();
        if (!parsed || *parsed < std::numeric_limits<T>::min() || *parsed > std::numeric_limits<T>::max()) {
            return false;
        }
        out = static_cast<T>(*parsed);
    } else {
        const std::optional<std::uint64_t> parsed = v.asU64();
        if (!parsed || *parsed > std::numeric_limits<T>::max()) {
            return false;
        }
        out = static_cast<T>(*parsed);
    }
    return true;
}

namespace detail {
// Non-template so the public header stays free of <aero/core/log.hpp> (D10) -- defined in
// serialize.cpp, the only place that logs.
void warnBadField(std::string_view componentName, std::string_view fieldName, const JsonValue& v);
}  // namespace detail

bool expectObject(const JsonValue& v, std::string_view componentName);
void warnUnknownKeys(const JsonValue& v, std::string_view componentName,
                     std::initializer_list<std::string_view> knownKeys);

// Missing key -> true, out left untouched (schema evolution, D9). Present-but-unreadable -> one WARN
// naming "<componentName>.<fieldName>" + the found kind, and false (best-effort: the caller still
// applies every other field).
template <class T>
    requires requires(const JsonValue& v, T& out) {
        { readJson(v, out) } -> std::same_as<bool>;
    }
bool readField(const JsonValue& object, std::string_view componentName, std::string_view fieldName, T& out) {
    const JsonValue* field = object.find(fieldName);
    if (field == nullptr) {
        return true;  // missing key: leave the caller's value untouched (schema evolution, D9)
    }
    if (!readJson(*field, out)) {
        detail::warnBadField(componentName, fieldName, *field);
        return false;
    }
    return true;
}

}  // namespace engine::reflect
