#pragma once
// Aero Engine -- the reflection-driven inspector's read model (task 2.2.2). PUBLIC and entt-free
// (2.1.3 D9); the entt::meta walk lives in editor/src/inspector_model.cpp. See component_ops.hpp's
// header comment for how "entt-free" is actually verified (a comment-stripped grep, not a bare one --
// review finding 9): this file's own O1 note below cites `entt::` types in prose too.

#include <aero/editor/component_ops.hpp>  // FieldValue, FieldKind
#include <aero/scene/entity.hpp>
#include <aero/scene/world.hpp>  // engine::ComponentTypeId

#include <string>
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

}  // namespace engine::editor
