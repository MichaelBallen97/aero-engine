#pragma once
// Aero Engine — engine::Transform (task 1.3.2): the engine's FIRST reflected component, and the
// world-matrix computation over the World's entity hierarchy.
//
// The parent/child links themselves live on World (world.hpp — setParent/parent/childCount/
// eachChild), NOT in this component. The FILE format carries `parent` at ENTITY level
// (docs/09-file-formats.md), and a component-less entity may legally be parented there, so the
// runtime model matches the file and a Transform payload never duplicates hierarchy data.
//
// REFLECTION: AERO_COMPONENT (<aero/reflect/annotations.hpp>) is the annotation aero_reflect_gen
// detects. Every field below is inside the reflectable subset (Vec3/Quat/Vec3), so all three
// serialize with ZERO skips and ZERO warnings — pinned in CI by the process-boundary case
// reflect-gen.components_engine_transform and by the generated meta/JSON artifacts compiled into
// aero_reflect_meta_test / aero_reflect_json_test. The ENGINE itself never compiles generated code:
// registration is hand-written, through the internal seam, in engine/scene/src/transform.cpp.

#include <aero/core/math.hpp>
#include <aero/reflect/annotations.hpp>
#include <aero/scene/entity.hpp>

#include <type_traits>

namespace engine {

// Declared, not included: the free functions below take a World by reference and never need its
// definition. Consumers that call World's members include <aero/scene/world.hpp> — or the umbrella
// <aero/scene/scene.hpp>, which provides both. MUST stay `class` to match world.hpp: a struct/class
// mismatch is an MSVC warning.
class World;

// Local TRS relative to the entity's parent (or to the world, for a root). Plain reflected data with
// no behaviour: mutate it freely through World::get<Transform>(), because worldMatrix() recomputes on
// demand and there is no cache to invalidate (see docs/08's open decision on caching).
//
// The DEFAULT value is the identity transform: position {0,0,0}, rotation {0,0,0,1}, scale {1,1,1} —
// so localMatrix(Transform{}) is exactly Mat4::identity(). A scene payload that omits a key leaves
// that field untouched (the generated reader's schema-evolution rule), which is precisely why the
// scale default lives here as a member initializer.
//
// operator== is EXACT by design (Vec3/Quat's own E11 rationale: serialization and change detection
// want exactness). Tolerance is opt-in, field by field, via approxEquals.
struct AERO_COMPONENT Transform {
    Vec3 position;             // parent-space translation
    Quat rotation;             // parent-space rotation; the default IS identity (Quat's own D14)
    Vec3 scale = Vec3::one();  // parent-space scale; non-uniform is allowed — see worldMatrix

    bool operator==(const Transform&) const = default;
};

// The invariants the type-erased component path (addRaw copies raw bytes) and ADR-004 serialization
// both rest on. A padding or layout change must break the BUILD, not corrupt a storage.
static_assert(std::is_trivially_copyable_v<Transform>);
static_assert(std::is_standard_layout_v<Transform>);
static_assert(std::is_aggregate_v<Transform>);
static_assert(sizeof(Transform) == 10 * sizeof(float));  // 12 + 16 + 12, no padding

// compose(Trs{...}) over the component's fields: T * R * S, right-to-left, ADR-005's column-vector
// convention. This is the ONE place `position` maps onto Trs::translation.
[[nodiscard]] Mat4 localMatrix(const Transform& transform);

// The entity's local matrix pre-multiplied by every ancestor's, walking World::parent() up to the
// root: world = M_root * ... * M_parent * M_local. O(depth) Mat4 multiplies, O(1) storage, a plain
// LOOP — no recursion, no collection, no caching (docs/08 records the caching deferral).
//
// An entity or ancestor WITHOUT a Transform contributes identity: it sits at its parent's origin,
// which is exactly the docs/09 component-less-parented-entity case. A dead or null entity, and a
// moved-from World, yield Mat4::identity() SILENTLY — this is a polling path, not an error path.
//
// NON-UNIFORM SCALE composes as plain matrix math: a non-uniformly scaled parent with a rotated
// child yields SHEAR in the child's world matrix. That is inherent to TRS hierarchies (Unity and
// Godot behave identically) and decompose() of such a matrix is out of contract (its own TRS-only
// rule). Values are never validated here: a non-unit rotation silently adds scale (compose's
// documented behaviour) and NaNs propagate.
[[nodiscard]] Mat4 worldMatrix(const World& world, Entity entity);

namespace scene::detail {

// Registers every built-in component type into `world` — currently just Transform, under the name
// "engine::Transform" (the durable identity docs/09's component keys resolve through). Called by
// World's CONSTRUCTOR, so every World knows the engine's own components with zero caller ceremony;
// registration stays per-World, with no process-global table. Idempotent. NOT public API — task
// 1.3.3 appends Camera/Light here.
void registerBuiltinComponents(World& world);

}  // namespace scene::detail

}  // namespace engine
