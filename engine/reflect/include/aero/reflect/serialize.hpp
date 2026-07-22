#pragma once
#include <aero/core/math.hpp>  // engine::Vec3, engine::Quat
#include <aero/reflect/json_writer.hpp>

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

}  // namespace engine::reflect
