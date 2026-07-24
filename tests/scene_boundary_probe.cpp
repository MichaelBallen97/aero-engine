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

// ComponentTypeId — value semantics and default-invalid. These are the only properties of it that
// ARE compile-time facts.
//
// Per-type DISTINCTNESS is deliberately not asserted here, and cannot be: an id is the address of a
// vague-linkage anchor, so its value is settled by the LINKER, not the compiler. GCC enforces
// exactly that — it refuses to constant-fold such an address, because identical-COMDAT folding may
// still merge two of them (which is why the anchor is non-const; see component.hpp). Asserting
// distinctness here would therefore be both non-portable and unable to observe the failure it
// claims to guard. It is asserted at RUNTIME instead, by tests/scene_test.cpp's
// "scene: distinct component types have distinct ids".
static_assert(std::is_trivially_copyable_v<engine::ComponentTypeId>);
static_assert(!engine::ComponentTypeId{}.valid());
// Unevaluated contexts: they pin the signature and instantiate the template over a plain struct
// (so the header must compile for a real component shape) without naming an anchor's address.
static_assert(std::is_same_v<decltype(engine::componentTypeId<ProbePosition>()), engine::ComponentTypeId>);
static_assert(std::is_same_v<decltype(engine::componentTypeId<ProbeVelocity>()), engine::ComponentTypeId>);
static_assert(noexcept(engine::componentTypeId<ProbePosition>()));

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

// ---- task 1.3.2 ----------------------------------------------------------------------------------
// The new headers reach this TU through the umbrella (scene.hpp gained transform.hpp), so a leaked
// third-party include in transform.hpp or annotations.hpp fails HERE, on a link line that is still
// exactly `aero::scene`. aero::scene now also propagates aero::reflect PUBLIC — which links no vcpkg
// package and wraps no third-party type — so this guard's strength is unchanged.

// Transform's layout contract (the same four assertions the header carries, re-asserted on a link
// line that proves the header compiles standalone).
static_assert(std::is_trivially_copyable_v<engine::Transform>);
static_assert(std::is_standard_layout_v<engine::Transform>);
static_assert(std::is_aggregate_v<engine::Transform>);
static_assert(sizeof(engine::Transform) == 10 * sizeof(float));

namespace {
// Declared, NEVER defined, and only ever named inside decltype/noexcept — no odr-use, nothing to
// link. This is std::declval by hand, minus the <utility> include — noexcept on each, matching
// std::declval's own noexcept specification, so the noexcept(...) checks below see through to the
// CALLEE's own exception specification instead of tripping on these helpers' (unspecified) one.
const engine::World& constWorld() noexcept;
engine::World& mutableWorld() noexcept;
constexpr engine::Entity NULL_ENTITY{};
const engine::Transform& someTransform() noexcept;
const engine::Camera& someCamera() noexcept;
}  // namespace

// The hierarchy API's shape and its noexcept contract.
static_assert(std::is_same_v<decltype(mutableWorld().setParent(NULL_ENTITY, NULL_ENTITY)), bool>);
static_assert(std::is_same_v<decltype(constWorld().parent(NULL_ENTITY)), engine::Entity>);
static_assert(noexcept(constWorld().parent(NULL_ENTITY)));
static_assert(std::is_same_v<decltype(constWorld().childCount(NULL_ENTITY)), std::size_t>);
static_assert(noexcept(constWorld().childCount(NULL_ENTITY)));

// The matrix functions' shape (free functions, deliberately not World members — World stays
// component-agnostic).
static_assert(std::is_same_v<decltype(engine::localMatrix(someTransform())), engine::Mat4>);
static_assert(std::is_same_v<decltype(engine::worldMatrix(constWorld(), NULL_ENTITY)), engine::Mat4>);

// ---- task 1.3.3 ----
static_assert(std::is_trivially_copyable_v<engine::Camera>);
static_assert(std::is_standard_layout_v<engine::Camera>);
static_assert(std::is_aggregate_v<engine::Camera>);
static_assert(sizeof(engine::Camera) == 3 * sizeof(float));
static_assert(std::is_trivially_copyable_v<engine::DirectionalLight>);
static_assert(std::is_standard_layout_v<engine::DirectionalLight>);
static_assert(std::is_aggregate_v<engine::DirectionalLight>);
static_assert(sizeof(engine::DirectionalLight) == 4 * sizeof(float));
static_assert(std::is_trivially_copyable_v<engine::PointLight>);
static_assert(std::is_standard_layout_v<engine::PointLight>);
static_assert(std::is_aggregate_v<engine::PointLight>);
static_assert(sizeof(engine::PointLight) == 5 * sizeof(float));

static_assert(std::is_same_v<decltype(engine::projectionMatrix(someCamera(), 1.0f)), engine::Mat4>);
static_assert(std::is_same_v<decltype(engine::viewMatrix(constWorld(), NULL_ENTITY)), engine::Mat4>);
