#pragma once
// Aero Engine — INTERNAL scene seam (task 1.3.1). NOT a public header: this directory is not on
// aero_scene's PUBLIC include path; it ships through the aero::scene_internal INTERFACE target and is
// the ONE place, besides engine/scene/src/world.cpp, where EnTT is visible inside engine/.
//
// WHY IT EXISTS: creating a typed component storage (entt::basic_registry::storage<T>()) is a
// TEMPLATE and therefore cannot be instantiated from an entt-free header — verified against EnTT
// 3.16: there is no runtime-typed storage factory. Everything else about a component (insert, read,
// erase, count, iterate) goes through the type-erased base and needs no `T`, which is why the public
// World API (world.hpp) is entt-free while this ONE operation is not.
//
// Deliberately OUTSIDE the engine/*/include/* glob, so check-scene-boundary.sh's public-header scan
// does not flag the entt include below. That same script uses THIS FILE as its canary: if it is
// renamed, or stops including entt, the guard refuses to pass rather than passing vacuously.
//
// WHO INCLUDES THIS: engine/scene's own TUs; engine TUs that author built-in components (1.3.2+);
// generated registration code (Phase 4/5). Ordinary consumers never need it — they only *use*
// components, which the public templates in world.hpp already allow.
//
// Adding any other accessor here requires a spec-level decision — the seam stays two functions wide.

#include <aero/scene/world.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace engine::scene::internal {

// 64-bit ON PURPOSE (spec D2). entt_traits<uint64_t> is 32 bits of index + 32 bits of version — a
// lossless bijection with engine::Handle. The DEFAULT entt::entity would give 12-bit versions: a
// stale handle would silently alias a live one after 4096 recycles of one slot, which is the precise
// use-after-free class ADR-001 exists to eliminate, and 8000x weaker than the SlotMap guarantee
// already shipped alongside it. Its 20-bit index (1 048 575 entities) is also a real ceiling.
enum class SceneEntity : std::uint64_t {};

using Registry = entt::basic_registry<SceneEntity>;
using ErasedStorage = Registry::common_type;  // == entt::basic_sparse_set<SceneEntity>
using Traits = entt::entt_traits<SceneEntity>;

// Entity -> the ECS identifier. Entity{} (generation 0) maps to the null identifier. A live entt
// version is never version_mask (basic_entt_traits::next() skips it), so generation is always >= 1
// for a live entity and the -1 below is guarded by valid().
[[nodiscard]] inline SceneEntity toEntt(Entity entity) noexcept {
    if (!entity.valid()) {
        return static_cast<SceneEntity>(entt::null);
    }
    return Traits::construct(static_cast<Traits::entity_type>(entity.index),
                             static_cast<Traits::version_type>(entity.generation - 1U));
}

// The inverse. A fresh entity (version 0) becomes generation 1, matching SlotMap's convention, with
// 0 left free as the null sentinel. +1 cannot overflow a u32 (see above).
[[nodiscard]] inline Entity fromEntt(SceneEntity entity) noexcept {
    if (entity == entt::null) {
        return Entity{};
    }
    return Entity{static_cast<std::uint32_t>(Traits::to_entity(entity)),
                  static_cast<std::uint32_t>(Traits::to_version(entity)) + 1U};
}

// Befriended by World. Both members are DEFINED in engine/scene/src/world.cpp, the only TU where
// World::Impl and EnTT are simultaneously visible — so World::Impl never leaves that file.
struct WorldAccess {
    [[nodiscard]] static Registry* registry(World& world) noexcept;  // nullptr on a moved-from World
    static ComponentTypeId bind(World& world, ComponentTypeId id, std::string_view name, ErasedStorage* storage,
                                bool emptyType);
};

// Make T a component type of THIS World: create its typed storage and record it under `name`.
// Idempotent — a second call returns the same id and does not touch the storage. Registration is
// PER-WORLD by design: there is no process-global type table (the same global the project refused
// for JobSystem), so "which types does this World know?" is always answerable.
//
// On a moved-from World this registers nothing and returns an invalid ComponentTypeId, silently —
// consistent with every other operation on a moved-from World.
template <typename T>
ComponentTypeId registerComponent(World& world, std::string_view name) {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "component type must be a plain value type");
    static_assert(std::is_default_constructible_v<T>, "a component must be default-constructible");
    static_assert(std::is_copy_constructible_v<T>, "a component must be copy-constructible");
    Registry* reg = WorldAccess::registry(world);
    if (reg == nullptr) {
        return {};
    }
    return WorldAccess::bind(world, componentTypeId<T>(), name, &reg->template storage<T>(), std::is_empty_v<T>);
}

}  // namespace engine::scene::internal
