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
    return readMemberValue(member, handle);
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
