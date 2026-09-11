#pragma once
// Aero Engine -- the reflection-driven inspector's read model (task 2.2.2). PUBLIC and entt-free
// (2.1.3 D9); the entt::meta walk lives in editor/src/inspector_model.cpp. See component_ops.hpp's
// header comment for how "entt-free" is actually verified (a comment-stripped grep, not a bare one --
// review finding 9): this file's own O1 note below cites `entt::` types in prose too.

#include <aero/editor/axis_palette.hpp>   // task E.3.1: the ONE spelling of the axis colours
#include <aero/editor/component_ops.hpp>  // FieldValue, FieldKind
#include <aero/scene/entity.hpp>
#include <aero/scene/world.hpp>  // engine::ComponentTypeId

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// One reflected field, snapshotted for THIS frame's draw.
struct FieldEntry {
    std::string name;
    FieldKind kind = FieldKind::Bool;
    bool hasRange = false;
    double rangeMin = 0.0;
    double rangeMax = 0.0;
    bool color = false;
    FieldValue value;
};

// One present, registered component on the inspected entity.
struct ComponentEntry {
    ComponentTypeId typeId;
    std::string name;  // full registration name (World::componentTypeName)
    bool hasFields = false;
    std::vector<FieldEntry> fields;
};

struct InspectorModel {
    Entity entity{};
    std::vector<ComponentEntry> components;
};

// Rebuilt EVERY FRAME into caller-owned scratch (D15, the 1.4.1 RenderViewScratch pattern): vectors
// keep their capacity, and a component added/removed behind the panel's back can never leave a stale
// pointer, because nothing is cached to go stale (E3). Dead/null entity => empty model.
//
// `const World&` (plan decision O1, 2026-07-26) -- and it genuinely works, end to end:
//     World::getRaw(id, e) const  ->  const void*                       (world.hpp:272)
//     entt::meta_type::from_void(const void*)  ->  a READ-ONLY meta_any ref wrapper
//                                                  (meta.hpp:1425, a DISTINCT overload from
//                                                   from_void(void*, bool) at :1416)
//     entt::meta_data::get(handle)  ->  meta_any BY VALUE -- a COPY, verified: the returned pointer
//                                       is not the component's own address
//     .cast<ConcreteT>()  ->  the snapshot this model stores in FieldValue anyway
// So the model never needed a live reference in the first place, and const makes AC-8's "the build
// performs no mutation" a COMPILER fact rather than a review promise. A caller holding a World& binds
// to this for free -- no cast, no overload, no call-site change (the panel does exactly that).
//
// NOTE THE ASYMMETRY, and do not "tidy" it: component_ops' four functions take World&, because the
// write path (from_void(void*) + meta_data::set) really does refuse a const instance. Task 2.4.2 must
// not assume one constness across all five entry points.
void buildInspectorModel(const World& world, Entity entity, InspectorModel& out);

// ---- task 3.1.5: the Guid row, as a VALUE ---------------------------------------------------------
class AssetDatabase;  // forward-declared: the row takes a POINTER, so this header needs no more of it
                      // and every consumer of the inspector model keeps its old include weight

// Everything a FieldKind::Guid row shows and everything it decides, computed OUTSIDE the draw walk so
// a tier-0 case asserts exactly what the panel renders. Two decisions in one struct on purpose: a
// panel holding its own copy of "is Clear enabled?" is a second answer to a one-line question, which
// is the shape 3.4.1's "a mapping worth having twice is worth having once" rule deletes.
//
// The three states are docs/plans' section 0.23, verbatim:
//   nil guid            -> "None",                            Clear DISABLED
//   record found        -> "<leaf name>  (<kind label>)",     Clear enabled
//   no database/record  -> "<first 8 hex>...  (missing)",     Clear enabled
// The 8-hex prefix comes from formatGuid, never a hand-rolled hex printer.
struct GuidFieldRow {
    std::string text;
    bool clearEnabled = false;  // the panel spells this BeginDisabled(!clearEnabled)
};
[[nodiscard]] GuidFieldRow guidFieldRow(Guid value, const AssetDatabase* database);

// ---- task E.3.1: the axis row, as VALUES ----------------------------------------------------------
// Everything the Vec3/Quat rows decide, computed OUTSIDE the draw walk so a tier-0 case asserts what
// the panel renders -- guidFieldRow's own shape, one kind over. NOTHING here knows about a component
// or a field NAME: the row is chosen by KIND and by the AERO_COLOR flag, which is the standing rule
// that keeps the inspector generic (a new component gets the treatment for free, ADR-004).
//
// IMGUI-FREE AND ENTT-FREE BY PLACEMENT, like every header under editor/include. The colours come
// back as sRGB BYTES; the one call site that needs an ImU32 builds it there with IM_COL32, exactly as
// axis_palette.hpp specifies.

// X, Y, Z. Not a "vector width": nothing in the reflectable subset has four components
// (reflect-gen's FieldCategory is {Primitive, Vec3, Quat, String, Guid, Unsupported}), so there is
// deliberately no W anywhere in this surface.
inline constexpr std::size_t AXIS_ROW_COMPONENTS = 3;

// Does this field draw three axis-labelled sub-widgets? A non-colour Vec3 and any Quat do; an
// AERO_COLOR Vec3 keeps its ColorEdit3, because a colour's three numbers are not axes.
[[nodiscard]] bool isAxisRow(FieldKind kind, bool color) noexcept;

// TOTAL over any index: past the last axis, an EMPTY label and a neutral grey. Total rather than
// asserting, because both are read inside a draw walk where an abort is the worst possible failure.
[[nodiscard]] std::string_view axisRowLabel(std::size_t index) noexcept;
[[nodiscard]] std::array<std::uint8_t, 3> axisRowColor(std::size_t index) noexcept;

// The two conversions the row draws THROUGH, in both directions. A Vec3 passes verbatim; a Quat goes
// out as euler DEGREES and comes back normalized, which is the expression the panel used to spell
// inline. `kind` is the FieldEntry's kind, never the variant's index -- a mismatched pair yields
// zeros rather than throwing, because no exception may cross this API (project rule 5).
[[nodiscard]] std::array<float, 3> axisRowValues(const FieldValue& value, FieldKind kind);
[[nodiscard]] FieldValue axisRowFieldValue(const std::array<float, 3>& shown, FieldKind kind);

// One entry of the right-click reset menu: its exact text, whether it is live, and what it writes.
// `enabled` is false when there is no default to reset TO, and equally when the reset would change
// nothing -- a menu entry that writes an identical value would cost a real undo entry for no edit.
struct AxisResetAction {
    std::string label;  // "Reset X to 1.000" per axis, "Reset to default" for the whole field
    bool enabled = false;
    FieldValue result{};  // meaningful only when `enabled`
};

// `axis` is nullopt for the whole field and 0/1/2 for one component of an axis row. ONE function
// rather than two, because the whole-field case must write the default VERBATIM: routing a Quat's
// whole-field reset through euler lands a value that is approxEquals to the default but not == to
// it, which would make `enabled` compute true forever.
[[nodiscard]] AxisResetAction axisResetAction(std::optional<std::size_t> axis,  // nullopt == the whole field
                                              FieldKind kind, const FieldValue& current,
                                              const std::optional<FieldValue>& defaultValue);

// The label column's width, as ARITHMETIC. Pure so the clamp is tier-0 testable: the panel measures
// the four inputs with ImGui and this decides. floorPx = fontSizePx * 5; ceilPx = max(floorPx,
// availableWidthPx * 0.5) -- the max is what keeps std::clamp's range from crossing, which is UB,
// and a zero- or negative-width dock IS reachable (a panel dragged to its minimum, or the frame a
// dock split settles). The two multipliers are TUNING VALUES, judged on hardware; changing one is a
// one-line edit here plus a VF15 expectation.
[[nodiscard]] float inspectorLabelColumnWidth(float widestLabelPx, float cellPaddingPx, float fontSizePx,
                                              float availableWidthPx) noexcept;

}  // namespace engine::editor
