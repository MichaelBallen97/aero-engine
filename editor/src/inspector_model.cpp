#include <aero/core/log.hpp>
#include <aero/core/math.hpp>              // task E.3.1: degrees/radians/eulerAngles/fromEulerAngles
#include <aero/editor/asset_database.hpp>  // task 3.1.5: the Guid row resolves a reference to a record
#include <aero/editor/asset_view.hpp>      // classifyAssetKind, assetKindLabel
#include <aero/editor/inspector_model.hpp>
#include <aero/editor/project_files.hpp>  // leafOf
#include <aero/reflect/annotations.hpp>   // engine::reflect::FieldUiMeta

#include "meta_utils.hpp"

#include <entt/entt.hpp>

#include <algorithm>
#include <array>
#include <cmath>  // task E.3.1: std::round, the row's display precision
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace engine::editor {

namespace {

// Same shape as component_ops.cpp's readMemberValue, but also reports the FieldKind (the model
// carries both) and whether ANY of the 20 concrete types matched -- the caller logs the O2
// defensive-skip ERROR on a miss. component_ops.cpp's readComponentField logs the identical
// condition too (review finding 8), so a drift between reflect-gen's whitelist and this dispatch
// is audible through either seam, not just this one.
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
    // task 3.1.5, above the arithmetic fall-through for component_ops.cpp's own reason: a Guid is none
    // of the 20, and an unmatched field is a row the inspector never draws.
    if (info == entt::type_id<Guid>()) {
        return {.matched = true, .kind = FieldKind::Guid, .value = FieldValue{value.cast<Guid>()}};
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

GuidFieldRow guidFieldRow(Guid value, const AssetDatabase* database) {
    if (!value.valid()) {
        // A NIL GUID IS "no reference", which is a legal, ordinary value -- not a broken one. Clear is
        // disabled because clearing nothing would push an undo entry that changes no byte.
        return {.text = "None", .clearEnabled = false};
    }
    const AssetRecord* const record = database != nullptr ? database->findByGuid(value) : nullptr;
    if (record == nullptr) {
        // NO DATABASE AND NO RECORD ARE ONE ROW, deliberately: from the user's seat both mean "this
        // project cannot tell you what that is", and inventing a second sentence for a state only a
        // -DAERO_REFLECT_TOOLS=OFF build or a mid-scan frame can reach would be a distinction nobody
        // can act on. The reference is still CLEARABLE -- a dangling reference is exactly the one a
        // user most wants to remove.
        return {.text = formatGuid(value).substr(0, 8) + "...  (missing)", .clearEnabled = true};
    }
    const AssetKind kind = classifyAssetKind(leafOf(record->relativePath), /*isDirectory=*/false);
    return {.text = std::string(leafOf(record->relativePath)) + "  (" + std::string(assetKindLabel(kind)) + ")",
            .clearEnabled = true};
}

// ---- task E.3.1: the axis row's decisions, all of them ---------------------------------------------

namespace {

// ImGui's OWN fourth default colour-channel marker, IM_COL32(140,140,140,255)
// (imgui_widgets.cpp:2257-2260 at the pinned 1.92.8) -- so the out-of-range answer is borrowed rather
// than invented. IT HAS NO CALLER TODAY: AXIS_ROW_COMPONENTS is 3 and every loop in the panel stops
// there. It exists because axisRowColor is TOTAL, and a total function needs an answer for every
// index; VF3 is what keeps that answer from silently becoming X's red.
constexpr std::array<std::uint8_t, 3> AXIS_ROW_NEUTRAL_SRGB{140U, 140U, 140U};

// degrees()/radians() are scalar-only (math/constants.hpp) -- applied componentwise for the euler
// triplet, since there is no Vec3 overload. MOVED here from inspector_panel.cpp at task E.3.1: the
// arithmetic the Quat row draws through now lives once, on the value side, where a tier-0 case can
// reach it.
Vec3 vecDegrees(Vec3 v) { return Vec3{degrees(v.x), degrees(v.y), degrees(v.z)}; }
Vec3 vecRadians(Vec3 v) { return Vec3{radians(v.x), radians(v.y), radians(v.z)}; }

// THE ROW'S OWN DISPLAY PRECISION, AS A NUMBER. ImGuiDataType_Float's PrintFmt is "%.3f"
// (imgui_widgets.cpp:2277) and DragScalar falls back to it when `format` is null (:2743), so three
// decimals is exactly what the drag box shows -- and `{:.3f}`, specified as printf's `%.3f`, is what
// axisResetAction writes into the menu label. Rounding here compares the two numbers a person
// actually reads.
//
// AS FLOATS, NEVER AS FORMATTED STRINGS: "-0.000" and "0.000" are different strings and must compare
// EQUAL. TOTAL, with both tails deliberate -- round(NaN) is NaN and compares unequal to everything,
// so a NaN axis keeps its reset LIVE (VF12's rescue); +/-inf * 1000 is +/-inf and compares equal to
// itself, so an infinite axis sitting on an infinite default is correctly quiet.
[[nodiscard]] float roundedToRowPrecision(float value) noexcept { return std::round(value * 1000.0F) / 1000.0F; }

}  // namespace

bool isAxisRow(FieldKind kind, bool color) noexcept {
    // NO `default:`, so a ninth FieldKind enumerator is a -Wswitch error here rather than a silent
    // "not an axis row" -- axisColorSrgbBytes' own precedent, one file over.
    switch (kind) {
        case FieldKind::Vec3:
            // AERO_COLOR is the ONE discriminator, and it is a flag on the field rather than a name:
            // a colour's three numbers are channels, not axes, and labelling them X/Y/Z would be
            // actively wrong.
            return !color;
        case FieldKind::Quat:
            // Always -- a rotation has no colour flag to carry, and AERO_COLOR on a Quat is not
            // expressible in the annotation set.
            return true;
        case FieldKind::Bool:
        case FieldKind::Int:
        case FieldKind::UInt:
        case FieldKind::Float:
        case FieldKind::String:
        case FieldKind::Guid:
            return false;
    }
    return false;  // unreachable; present so the switch above may stay default-less
}

std::string_view axisRowLabel(std::size_t index) noexcept {
    static constexpr std::array<std::string_view, AXIS_ROW_COMPONENTS> LABELS{"X", "Y", "Z"};
    // The bound test IS the contract: a raw LABELS[index] reads past the array for any index the
    // caller gets wrong, and this is read inside a draw walk.
    return index < LABELS.size() ? LABELS[index] : std::string_view{};
}

std::array<std::uint8_t, 3> axisRowColor(std::size_t index) noexcept {
    // DERIVED from the palette, never restated: a palette edit moves this and the grid's axis lines
    // and the transform gizmo together, which is the whole reason axis_palette.hpp exists.
    switch (index) {
        case 0:
            return axisColorSrgbBytes(Axis::X);
        case 1:
            return axisColorSrgbBytes(Axis::Y);
        case 2:
            return axisColorSrgbBytes(Axis::Z);
        default:
            break;
    }
    return AXIS_ROW_NEUTRAL_SRGB;
}

std::array<float, 3> axisRowValues(const FieldValue& value, FieldKind kind) {
    // std::get_if, never std::get: the pair (kind, variant) comes from the model and always agrees,
    // but a throw would cross a public API boundary, which this project does not do (rule 5).
    if (kind == FieldKind::Vec3) {
        const Vec3* const v = std::get_if<Vec3>(&value);
        return v != nullptr ? std::array<float, 3>{v->x, v->y, v->z} : std::array<float, 3>{};
    }
    if (kind == FieldKind::Quat) {
        const Quat* const q = std::get_if<Quat>(&value);
        if (q == nullptr) {
            return {};
        }
        const Vec3 deg = vecDegrees(eulerAngles(*q));
        return {deg.x, deg.y, deg.z};
    }
    return {};
}

FieldValue axisRowFieldValue(const std::array<float, 3>& shown, FieldKind kind) {
    if (kind == FieldKind::Vec3) {
        return FieldValue{Vec3{shown[0], shown[1], shown[2]}};
    }
    if (kind == FieldKind::Quat) {
        // BYTE FOR BYTE the expression the panel used to write inline, moved rather than restated.
        // normalize() ASSERTS on a zero/non-finite quaternion, which is today's behaviour and stays
        // today's behaviour -- normalizeOrIdentity would be a different function with a different
        // failure mode, and changing it is not this task's work.
        return FieldValue{normalize(fromEulerAngles(vecRadians(Vec3{shown[0], shown[1], shown[2]})))};
    }
    return FieldValue{};  // unreachable: the caller has already asked isAxisRow
}

AxisResetAction axisResetAction(std::optional<std::size_t> axis, FieldKind kind, const FieldValue& current,
                                const std::optional<FieldValue>& defaultValue) {
    // PER-AXIS only when the caller named an axis, the kind really has axes, AND the index is in
    // range; anything else is a whole-field reset. The range test is not decoration: `shown[*axis]`
    // below indexes a std::array and an out-of-range index would be undefined behaviour.
    const bool perAxis = axis.has_value() && *axis < AXIS_ROW_COMPONENTS && isAxisRow(kind, /*color=*/false);

    AxisResetAction action;
    // THE LABEL IS BUILT FIRST AND UNCONDITIONALLY, so a disabled entry still reads sensibly: a
    // greyed-out "Reset X to 1.000" tells the user what it would have done, a greyed-out blank does
    // not. `{:.3f}` is specified as printf's `%.3f` ([charconv.to.chars]), which is exactly
    // ImGuiDataType_Float's PrintFmt (imgui_widgets.cpp:2277) and what DragScalar falls back to when
    // `format` is null (:2743) -- so the number in the menu is the number in the box.
    if (perAxis && defaultValue.has_value()) {
        action.label =
            std::format("Reset {} to {:.3f}", axisRowLabel(*axis), axisRowValues(*defaultValue, kind)[*axis]);
    } else if (perAxis) {
        action.label = std::format("Reset {} to default", axisRowLabel(*axis));
    } else {
        // No number in the whole-field label: a Vec3 or Quat default has no one-number spelling.
        action.label = "Reset to default";
    }

    if (!defaultValue.has_value()) {
        return action;  // `enabled` stays false -- there is nothing to reset TO
    }

    if (perAxis) {
        const std::array<float, 3> currentShown = axisRowValues(current, kind);
        const std::array<float, 3> defaultShown = axisRowValues(*defaultValue, kind);
        std::array<float, 3> shown = currentShown;
        shown[*axis] = defaultShown[*axis];
        action.result = axisRowFieldValue(shown, kind);

        // THE PER-AXIS QUESTION IS "DOES THIS CHANGE THE AXIS IT NAMES, AT THE PRECISION THE ROW
        // DISPLAYS?" -- never "is the recomposed value different". The entry reads "Reset X to
        // 0.000" and the box beside it reads "0.000"; if those are the same number the action
        // changes nothing the user can see or type, so it must not cost an undo entry. That is
        // guidFieldRow's own rule ("clearing nothing would push an undo entry that changes no
        // byte"), applied at the precision this row actually shows.
        //
        // COMPARING THE WHOLE RECOMPOSED VALUE IS WRONG HERE, and measurably so: a Quat's per-axis
        // reset goes out through euler and comes back through fromEulerAngles + normalize, which
        // perturbs the OTHER TWO axes by ~1e-7 every time. From a (0, 20, 40)-degree pose, resetting
        // X leaves X at -2.4e-07, then -3.6e-07, then +5.7e-07 -- it never converges, so a
        // whole-value comparison leaves the entry live FOREVER and every click pushes another undo
        // entry. From a (0, 20, 0) pose X is EXACTLY 0.0 and the entry was still live. Identity was
        // the only exact fixpoint, which is why nothing caught it.
        //
        // ONE BEHAVIOUR CHANGE, DELIBERATE AND STATED: a Vec3 axis differing from its default by
        // less than 0.0005 is DISABLED rather than enabled, because the box shows three decimals and
        // the user is looking at two identical numbers.
        action.enabled = roundedToRowPrecision(currentShown[*axis]) != roundedToRowPrecision(defaultShown[*axis]);
    } else {
        // VERBATIM, never round-tripped through euler -- see the header note: a Quat that came back
        // through eulerAngles is approxEquals to the default but not == to it, which would leave
        // `enabled` true forever and make the entry unclickable-but-live.
        action.result = *defaultValue;
        // `==` on the variant, never approxEquals: a sign-bit-only write (-0.0 against +0.0) compares
        // EQUAL and so costs no undo entry, while a NaN component compares UNEQUAL to everything and so
        // leaves the reset LIVE -- which is exactly the rescue a user with a NaN in a field wants.
        // BITWISE is right HERE and only here: the whole-field write lands the default byte for byte,
        // so the comparison is exact by construction and VF8 pins it.
        action.enabled = action.result != current;
    }
    return action;
}

float inspectorLabelColumnWidth(float widestLabelPx, float cellPaddingPx, float fontSizePx,
                                float availableWidthPx) noexcept {
    const float floorPx = fontSizePx * 5.0F;
    // std::max, not a bare `availableWidthPx * 0.5F`: a zero- or negative-width dock IS reachable (a
    // panel dragged to its minimum, or the frame a dock split settles), and std::clamp with lo > hi
    // is UNDEFINED BEHAVIOUR. VF15(d) is the arm that drives it under UBSan.
    const float ceilPx = std::max(floorPx, availableWidthPx * 0.5F);
    return std::clamp(widestLabelPx + (2.0F * cellPaddingPx), floorPx, ceilPx);
}

}  // namespace engine::editor
