#include <aero/core/log.hpp>
#include <aero/editor/inspector_model.hpp>
#include <aero/reflect/annotations.hpp>  // engine::reflect::FieldUiMeta

#include "meta_utils.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <string>

namespace engine::editor {

namespace {

// Same shape as component_ops.cpp's readMemberValue, but also reports the FieldKind (the model
// carries both) and whether ANY of the 20 concrete types matched -- the caller logs the O2
// defensive-skip ERROR on a miss, since this is the ONE place that ships the loud diagnostic
// (component_ops.cpp's own read path stays silent -- see its own comment).
struct DispatchedValue {
    bool matched = false;
    FieldKind kind = FieldKind::Bool;
    FieldValue value{false};
};

// A local class cannot carry a member TEMPLATE (operator()<T>()), so this dispatch visitor lives at
// namespace scope, not inside readEntryValue.
struct ArithmeticReader {
    entt::meta_any& value;
    DispatchedValue& out;

    template <typename T>
    void operator()() {
        const T v = value.cast<T>();
        out.matched = true;
        out.kind = kindOfArithmetic<T>();
        if constexpr (std::is_same_v<T, bool>) {
            out.value = FieldValue{v};
        } else if constexpr (std::is_floating_point_v<T>) {
            out.value = FieldValue{static_cast<double>(v)};
        } else if constexpr (std::is_signed_v<T>) {
            out.value = FieldValue{static_cast<std::int64_t>(v)};
        } else {
            out.value = FieldValue{static_cast<std::uint64_t>(v)};
        }
    }
};

DispatchedValue readEntryValue(const entt::meta_data& data, entt::meta_any& handle) {
    const entt::type_info& info = data.type().info();
    entt::meta_any value = data.get(handle);

    if (info == entt::type_id<Vec3>()) {
        return {.matched = true, .kind = FieldKind::Vec3, .value = FieldValue{value.cast<Vec3>()}};
    }
    if (info == entt::type_id<Quat>()) {
        return {.matched = true, .kind = FieldKind::Quat, .value = FieldValue{value.cast<Quat>()}};
    }
    if (info == entt::type_id<std::string>()) {
        return {.matched = true, .kind = FieldKind::String, .value = FieldValue{value.cast<std::string>()}};
    }

    DispatchedValue out;
    ArithmeticReader reader{value, out};
    dispatchArithmetic(info, ArithmeticTypes{}, reader);
    return out;
}

}  // namespace

void buildInspectorModel(const World& world, Entity entity, InspectorModel& out) {
    out.entity = entity;
    // D15 -- caller-owned scratch, reused across frames at BOTH levels (the outer component list
    // and each entry's field list): existing ComponentEntry/FieldEntry SLOTS are overwritten in
    // place rather than destroyed and reallocated, so their vectors' capacity survives a
    // same-shape rebuild. Only the trailing tail (from a previous, LARGER build) is ever dropped,
    // via resize() at the end of each level -- resize()-down never releases capacity.
    std::size_t writeIndex = 0;

    if (world.alive(entity)) {
        const std::size_t count = world.componentTypeCount();
        for (std::size_t i = 0; i < count; ++i) {
            const ComponentTypeId id = world.componentTypeAt(i);
            if (!world.hasRaw(id, entity)) {
                continue;
            }

            if (writeIndex >= out.components.size()) {
                out.components.emplace_back();
            }
            ComponentEntry& entry = out.components[writeIndex];
            entry.typeId = id;
            entry.name = world.componentTypeName(id);  // std::string::operator= reuses its own capacity

            const entt::meta_type metaType = resolveComponentMeta(entry.name);
            entry.hasFields = static_cast<bool>(metaType);

            std::size_t fieldWriteIndex = 0;
            if (metaType) {
                entt::meta_any handle = metaType.from_void(world.getRaw(id, entity));
                for (auto&& [dataId, data] : metaType.data()) {
                    (void)dataId;
                    const DispatchedValue dispatched = readEntryValue(data, handle);
                    if (!dispatched.matched) {
                        // O2: reflect-gen's whitelist and meta_utils' ArithmeticTypes are kept in
                        // lock-step, so this branch is unreachable today. If the two ever drift --
                        // or a future subset gains a type only the generator knows about -- the
                        // field would otherwise vanish from the UI with NO diagnostic anywhere,
                        // which is exactly the silent hole O2 exists to close. One ERROR per
                        // occurrence makes that drift audible the first time anyone opens the
                        // inspector, and AC-12's pin plus sabotage S7 catch it in CI.
                        AERO_LOG_ERROR("inspector: {}.{} has meta type '{}', which no field editor maps -- skipped",
                                       entry.name, data.name(), data.type().info().name());
                        continue;
                    }

                    if (fieldWriteIndex >= entry.fields.size()) {
                        entry.fields.emplace_back();
                    }
                    FieldEntry& field = entry.fields[fieldWriteIndex];
                    field.name = data.name();
                    field.kind = dispatched.kind;
                    field.value = dispatched.value;
                    field.hasRange = false;
                    field.rangeMin = 0.0;
                    field.rangeMax = 0.0;
                    field.color = false;
                    const engine::reflect::FieldUiMeta* uiMeta = data.custom();
                    if (uiMeta != nullptr) {
                        field.hasRange = uiMeta->hasRange;
                        field.rangeMin = uiMeta->rangeMin;
                        field.rangeMax = uiMeta->rangeMax;
                        field.color = uiMeta->color;
                    }
                    ++fieldWriteIndex;
                }
            }
            entry.fields.resize(fieldWriteIndex);  // drop any stale tail; never shrinks capacity

            ++writeIndex;
        }
    }
    out.components.resize(writeIndex);  // drop any stale tail; never shrinks capacity
}

}  // namespace engine::editor
