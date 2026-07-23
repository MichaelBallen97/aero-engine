// Aero Engine — the scene EnTT-boundary COMPILE-TIME guard (task 1.3.1; project rule #3).
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is
// that it COMPILES. Its target (aero_scene_boundary_probe, tests/CMakeLists.txt) links ONLY
// aero::scene, which links EnTT::EnTT PRIVATE — and whose only PUBLIC dep, aero::core, propagates no
// vcpkg header — so vcpkg's shared per-triplet include/ root never reaches this compile line and
// <entt/...> is genuinely unresolvable here. If any public scene header ever starts including entt,
// THIS TU fails to compile the moment the leak is written.
//
// aero_tests CANNOT do this: it links doctest, SDL3 AND aero::scene_internal directly and inherits
// the whole shared vcpkg include root, so <entt/entt.hpp> resolves there regardless of what
// aero_scene links (risk R12, docs/08-risks.md). The grep guard
// (.github/scripts/check-scene-boundary.sh) is the enforcement that reaches every public header;
// THIS probe holds the compile-time half.
//
// KEEP THIS TARGET DEPENDENCY-FREE — see tests/CMakeLists.txt. Adding doctest, aero::profiling
// (Tracy, a vcpkg package, in Release), EnTT::EnTT, or — above all — aero::scene_internal (which
// carries EnTT::EnTT INTERFACE by design) restores the shared include root and silently reduces this
// guard to a no-op WHILE CI STAYS GREEN. That is the only way it can rot.

#include <aero/scene/scene.hpp>  // the umbrella: component + entity + world

#include <cstddef>
#include <type_traits>

namespace {
struct ProbePosition {
    float x, y, z;
};
struct ProbeVelocity {
    float x, y, z;
};
}  // namespace

// Entity — the Handle<Tag> contract (AC-2).
static_assert(sizeof(engine::Entity) == 8);
static_assert(std::is_trivially_copyable_v<engine::Entity>);
static_assert(!engine::Entity{}.valid());
static_assert(engine::Entity{1U, 1U}.valid());
static_assert(engine::Entity{1U, 1U} == engine::Entity{1U, 1U});
static_assert(!(engine::Entity{1U, 1U} == engine::Entity{1U, 2U}));  // generation is part of identity
static_assert(!(engine::Entity{1U, 1U} == engine::Entity{2U, 1U}));  // so is the index

// ComponentTypeId — value semantics, default-invalid, per-type distinct (M1).
static_assert(std::is_trivially_copyable_v<engine::ComponentTypeId>);
static_assert(!engine::ComponentTypeId{}.valid());
static_assert(engine::componentTypeId<ProbePosition>().valid());
static_assert(engine::componentTypeId<ProbePosition>() == engine::componentTypeId<ProbePosition>());
static_assert(!(engine::componentTypeId<ProbePosition>() == engine::componentTypeId<ProbeVelocity>()));

// World — force the class COMPLETE via traits that also assert its move-only contract. (Its members
// are odr-usable at LINK time only; naming a trait needs the complete type, which is exactly what
// pulls the header's full definition — and any leaked entt include — into this TU.)
static_assert(std::is_default_constructible_v<engine::World>);
static_assert(!std::is_copy_constructible_v<engine::World>);
static_assert(!std::is_copy_assignable_v<engine::World>);
static_assert(std::is_move_constructible_v<engine::World>);
static_assert(std::is_move_assignable_v<engine::World>);
static_assert(std::is_nothrow_move_constructible_v<engine::World>);
static_assert(engine::World::MAX_QUERY_TYPES == 8U);
