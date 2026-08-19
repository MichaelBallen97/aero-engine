#include <aero/core/log.hpp>
#include <aero/editor/component_ops.hpp>
#include <aero/reflect/annotations.hpp>  // engine::reflect::FieldUiMeta

#include "meta_utils.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace engine::editor {

namespace {

// A local class cannot carry a member TEMPLATE (operator()<T>()), so these two dispatch visitors
// live at namespace scope, not inside the functions that use them.

struct ArithmeticReader {
    entt::meta_any& value;
    std::optional<FieldValue>& result;

    template <typename T>
    void operator()() {
        const T v = value.cast<T>();
        if constexpr (std::is_same_v<T, bool>) {
            result = FieldValue{v};
        } else if constexpr (std::is_floating_point_v<T>) {
            result = FieldValue{static_cast<double>(v)};
        } else if constexpr (std::is_signed_v<T>) {
            result = FieldValue{static_cast<std::int64_t>(v)};
        } else {
            result = FieldValue{static_cast<std::uint64_t>(v)};
        }
    }
};

struct ArithmeticWriter {
    const entt::meta_data& member;
    entt::meta_any& handle;
    const FieldValue& value;
    bool hasRange;
    double rangeMin;
    double rangeMax;
    bool ok = false;

    template <typename T>
    void operator()() {
        if constexpr (std::is_same_v<T, bool>) {
            // Bool is its OWN kind (D7: engine::range never applies to it) -- only an actual
            // bool input is accepted; an int64/uint64/double input is a genuine kind mismatch
            // (C6: EnTT's own set() happily accepts int->bool, which is exactly what the seam
            // must NOT do).
            if (!std::holds_alternative<bool>(value)) {
                return;
            }
            ok = member.set(handle, std::get<bool>(value));
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!std::holds_alternative<double>(value)) {
                return;
            }
            const double clamped = clampRangeDouble(std::get<double>(value), hasRange, rangeMin, rangeMax);
            ok = member.set(handle, narrowFromDouble<T>(clamped));
        } else {
            // Integral T, signed OR unsigned: accept EITHER wide integral alternative (the
            // seam's own four-row widen/clamp table covers int64->signed, int64->unsigned,
            // uint64->signed, uint64->unsigned -- ALL FOUR are defined, not just the
            // same-signedness pairing). A bool/double/Vec3/Quat/string input is the genuine
            // kind mismatch and leaves `ok` false.
            if (std::holds_alternative<std::int64_t>(value)) {
                const std::int64_t clamped =
                    clampRangeInt64(std::get<std::int64_t>(value), hasRange, rangeMin, rangeMax);
                ok = member.set(handle, narrowFromInt64<T>(clamped));
            } else if (std::holds_alternative<std::uint64_t>(value)) {
                const std::uint64_t clamped =
                    clampRangeUint64(std::get<std::uint64_t>(value), hasRange, rangeMin, rangeMax);
                ok = member.set(handle, narrowFromUint64<T>(clamped));
            }
        }
    }
};

// The read half: dispatches on the member's EXACT concrete type (never allow_cast, C6) and widens
// into the transport per O2's rule (Int/UInt/Float widen; Vec3/Quat/std::string carry as-is).
std::optional<FieldValue> readMemberValue(const entt::meta_data& member, entt::meta_any& handle) {
    const entt::type_info& info = member.type().info();
    entt::meta_any value = member.get(handle);

    if (info == entt::type_id<Vec3>()) {
        return FieldValue{value.cast<Vec3>()};
    }
    if (info == entt::type_id<Quat>()) {
        return FieldValue{value.cast<Quat>()};
    }
    if (info == entt::type_id<std::string>()) {
        return FieldValue{value.cast<std::string>()};
    }
    // task 3.1.5. It sits HERE, beside the other three exact-type arms and ABOVE the arithmetic
    // fall-through, because a Guid is none of the 20 arithmetic types: falling through would return
    // nullopt, which the inspector renders as a MISSING ROW rather than as an error.
    if (info == entt::type_id<Guid>()) {
        return FieldValue{value.cast<Guid>()};
    }

    std::optional<FieldValue> result;
    ArithmeticReader reader{value, result};
    dispatchArithmetic(info, ArithmeticTypes{}, reader);
    return result;
}

// The write half: kind-checks `value` against the member's exact concrete type, clamps (D8), and
// writes the EXACT type -- never lets EnTT convert (C6). Returns false, with NO write, on any kind
// mismatch or on a type matching none of the 20.
bool writeMemberValue(const engine::reflect::FieldUiMeta* uiMeta, const entt::meta_data& member, entt::meta_any& handle,
                      const FieldValue& value) {
    const entt::type_info& info = member.type().info();
    const bool hasRange = uiMeta != nullptr && uiMeta->hasRange;
    const double rangeMin = uiMeta != nullptr ? uiMeta->rangeMin : 0.0;
    const double rangeMax = uiMeta != nullptr ? uiMeta->rangeMax : 0.0;

    if (info == entt::type_id<Vec3>()) {
        if (!std::holds_alternative<Vec3>(value)) {
            return false;
        }
        return member.set(handle, std::get<Vec3>(value));
    }
    if (info == entt::type_id<Quat>()) {
        if (!std::holds_alternative<Quat>(value)) {
            return false;
        }
        return member.set(handle, std::get<Quat>(value));
    }
    if (info == entt::type_id<std::string>()) {
        if (!std::holds_alternative<std::string>(value)) {
            return false;
        }
        return member.set(handle, std::get<std::string>(value));
    }
    // task 3.1.5. THE EXACT CONCRETE TYPE, never allow_cast and never the raw variant: `member.set(
    // handle, value)` would let EnTT convert, which is C6's whole rule and is seed S36. No range
    // clamp either -- a Guid has none, and AERO_RANGE cannot be written on one.
    if (info == entt::type_id<Guid>()) {
        if (!std::holds_alternative<Guid>(value)) {
            return false;
        }
        return member.set(handle, std::get<Guid>(value));
    }

    ArithmeticWriter writer{member, handle, value, hasRange, rangeMin, rangeMax};
    const bool dispatched = dispatchArithmetic(info, ArithmeticTypes{}, writer);
    return dispatched && writer.ok;
}

}  // namespace

bool addComponent(World& world, Entity entity, ComponentTypeId id) {
    if (world.hasRaw(id, entity)) {
        AERO_LOG_ERROR("component_ops: addComponent refused -- entity already has this component type (D10)");
        return false;
    }
    world.addRaw(id, entity, nullptr);
    // hasRaw AFTERWARDS is the success signal: a tag component's addRaw returns nullptr ON SUCCESS
    // too (E13), so the return value alone cannot distinguish success from every rejection path.
    return world.hasRaw(id, entity);
}

bool removeComponent(World& world, Entity entity, ComponentTypeId id) { return world.removeRaw(id, entity); }

bool componentFieldsAreReflected(const World& world, ComponentTypeId id) {
    // Deliberately silent: this is the question a caller asks INSTEAD of tripping readComponentField's
    // ERROR, so answering it must not log the very line it exists to avoid.
    const std::string_view typeName = world.componentTypeName(id);
    if (typeName.empty()) {
        return false;
    }
    return static_cast<bool>(resolveComponentMeta(typeName));
}

std::optional<FieldValue> readComponentField(World& world, Entity entity, ComponentTypeId id, std::string_view field) {
    const std::string_view typeName = world.componentTypeName(id);
    if (typeName.empty()) {
        AERO_LOG_ERROR("component_ops: readComponentField -- unregistered component id");
        return std::nullopt;
    }
    const entt::meta_type metaType = resolveComponentMeta(typeName);
    if (!metaType) {
        AERO_LOG_ERROR("component_ops: readComponentField -- no entt::meta registered for '{}'", typeName);
        return std::nullopt;
    }
    void* raw = world.getRaw(id, entity);
    if (raw == nullptr) {
        AERO_LOG_ERROR("component_ops: readComponentField -- '{}' absent or entity dead", typeName);
        return std::nullopt;
    }
    entt::meta_any handle = metaType.from_void(raw);
    const entt::meta_data member = metaType.data(entt::hashed_string::value(field.data(), field.size()));
    if (!member) {
        AERO_LOG_ERROR("component_ops: readComponentField -- unknown field '{}.{}'", typeName, field);
        return std::nullopt;
    }
    std::optional<FieldValue> result = readMemberValue(member, handle);
    if (!result.has_value()) {
        // O2: reflect-gen's whitelist and meta_utils' ArithmeticTypes are kept in lock-step, so this
        // is unreachable today -- but component_ops.hpp promises every rejection logs (review
        // finding 8: this branch used to return nullopt silently, the one seam gap that promise
        // didn't hold), and inspector_model.cpp's readEntryValue already ships the identical
        // diagnostic for the identical condition, so both consumers of the O2 dispatch table are
        // audible on drift, not just the model.
        AERO_LOG_ERROR("component_ops: readComponentField -- '{}.{}' has meta type '{}', which no field editor maps",
                       typeName, field, member.type().info().name());
    }
    return result;
}

bool writeComponentField(World& world, Entity entity, ComponentTypeId id, std::string_view field,
                         const FieldValue& value) {
    const std::string_view typeName = world.componentTypeName(id);
    if (typeName.empty()) {
        AERO_LOG_ERROR("component_ops: writeComponentField -- unregistered component id");
        return false;
    }
    const entt::meta_type metaType = resolveComponentMeta(typeName);
    if (!metaType) {
        AERO_LOG_ERROR("component_ops: writeComponentField -- no entt::meta registered for '{}'", typeName);
        return false;
    }
    void* raw = world.getRaw(id, entity);
    if (raw == nullptr) {
        AERO_LOG_ERROR("component_ops: writeComponentField -- '{}' absent or entity dead", typeName);
        return false;
    }
    entt::meta_any handle = metaType.from_void(raw);
    const entt::meta_data member = metaType.data(entt::hashed_string::value(field.data(), field.size()));
    if (!member) {
        AERO_LOG_ERROR("component_ops: writeComponentField -- unknown field '{}.{}'", typeName, field);
        return false;
    }
    const engine::reflect::FieldUiMeta* uiMeta = member.custom();
    const bool ok = writeMemberValue(uiMeta, member, handle, value);
    if (!ok) {
        AERO_LOG_ERROR("component_ops: writeComponentField -- kind mismatch on '{}.{}'", typeName, field);
    }
    return ok;
}

}  // namespace engine::editor
