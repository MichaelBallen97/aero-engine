#pragma once
// Aero Engine -- the hot-reload carry's decision table (task 4.3.3, D4; internal to engine/script). Pure and
// backend-free, so every pair of kinds is tested at tier 0 through aero::script_internal
// (tests/script_reload_test.cpp, HR1-HR2). The backend half, valueKindOf, which maps a value to its kind, is
// src/vm_scripts.cpp's.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine::script::internal {

// What a field's value is, as far as the carry cares. The numbering is the order of the table in HR1.
enum class ValueKind : std::uint8_t {
    Undefined = 0,
    Null,
    Boolean,
    Number,
    BigInt,
    String,
    Symbol,
    Array,
    Vec3,
    Quat,
    Entity,
    Object,
    Function,
};
inline constexpr std::size_t VALUE_KIND_COUNT = 13;

// Whether a field's running value carries into the new instance, given the kind of the new class's default:
//   - a function never carries, on either side (the new instance keeps its own);
//   - an undefined default takes every other kind (the field has no declared shape);
//   - a null default takes null and the reference kinds (array, Vec3, Quat, Entity, object);
//   - any other default takes a value of its own kind only.
[[nodiscard]] constexpr bool carriesValue(ValueKind running, ValueKind newDefault) noexcept {
    if (running == ValueKind::Function || newDefault == ValueKind::Function) {
        return false;
    }
    if (newDefault == ValueKind::Undefined) {
        return true;
    }
    if (newDefault == ValueKind::Null) {
        switch (running) {
            case ValueKind::Null:
            case ValueKind::Array:
            case ValueKind::Vec3:
            case ValueKind::Quat:
            case ValueKind::Entity:
            case ValueKind::Object:
                return true;
            case ValueKind::Undefined:
            case ValueKind::Boolean:
            case ValueKind::Number:
            case ValueKind::BigInt:
            case ValueKind::String:
            case ValueKind::Symbol:
            case ValueKind::Function:
                return false;
        }
        return false;
    }
    return running == newDefault;
}

[[nodiscard]] constexpr std::string_view valueKindLabel(ValueKind kind) noexcept {
    switch (kind) {
        case ValueKind::Undefined:
            return "undefined";
        case ValueKind::Null:
            return "null";
        case ValueKind::Boolean:
            return "boolean";
        case ValueKind::Number:
            return "number";
        case ValueKind::BigInt:
            return "bigint";
        case ValueKind::String:
            return "string";
        case ValueKind::Symbol:
            return "symbol";
        case ValueKind::Array:
            return "array";
        case ValueKind::Vec3:
            return "engine.Vec3";
        case ValueKind::Quat:
            return "engine.Quat";
        case ValueKind::Entity:
            return "engine.Entity";
        case ValueKind::Object:
            return "object";
        case ValueKind::Function:
            return "function";
    }
    return "unknown";
}

}  // namespace engine::script::internal
