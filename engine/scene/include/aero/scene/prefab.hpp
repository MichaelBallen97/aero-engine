#pragma once
// engine::Prefab (task 4.4.4): a saved entity tree that a World can copy, over and over.
//
// A Prefab owns ONE private World holding exactly one root and its descendants. Every operation walks that World
// through the public World API (create, setParent, setName, addRaw), so the type needs no reflection and no EnTT:
// it compiles in every configuration, and a public editor header may name it. The component copy walks the
// registration table, never a list, so a component type added after this one copies with no change here.
//
// An instance is a FLAT COPY. It keeps no link to the Prefab it came from or to the asset that Prefab was read
// from, so editing either afterwards changes no instance. Overrides and nesting are not modelled.
//
// The tree is spelled in the same shape a scene file has (docs/09): `world()` is what saveWorld writes, and a
// World that loadScene filled is what `fromWorld` accepts, which is how the `.aeroprefab` file round-trips.
//
// NOT THREAD-SAFE, exactly like World.

#include <aero/scene/entity.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace engine {

class Prefab {
public:
    // A flat copy of `roots`, which the caller has already made disjoint (the editor's topMost). Saved roots keep
    // their local rotation and scale; a position becomes its world position relative to the pivot, the mean of the
    // finite roots' world positions. One root is the pivot, so its position is exactly zero. Several roots go under
    // one new root at the pivot, an identity Transform named `wrapperName`. A root whose world position is NaN or
    // infinite, or whose offset does not fit a float, stores zero. nullopt: `roots` is empty or holds a dead or null
    // handle. A component type the Prefab's World does not register is skipped with one WARN per type.
    [[nodiscard]] static std::optional<Prefab> capture(const World& source, std::span<const Entity> roots,
                                                       std::string_view wrapperName);
    // Takes a World holding exactly one root and refuses anything else (zero, several, moved-from). The World is
    // moved from only on success.
    [[nodiscard]] static std::optional<Prefab> fromWorld(World&& world);

    // A new, independent copy of the whole tree. Its root is placement o stored, linked under `parent` when that
    // is not null; the children's Transforms are untouched. A root the prefab stored without a Transform takes the
    // placement itself. Entity{}: a dead `parent` or a moved-from `destination`, and nothing changed. `placement`
    // is by value: a reference into `destination`'s own storage would be read after the copy grew it.
    [[nodiscard]] Entity instantiate(World& destination, Transform placement, Entity parent = Entity{}) const;

    [[nodiscard]] const World& world() const noexcept { return store; }  // saveWorldText's input
    [[nodiscard]] Entity root() const noexcept { return rootEntity; }
    [[nodiscard]] std::size_t entityCount() const noexcept { return store.entityCount(); }

private:
    Prefab(World&& world, Entity root) noexcept;
    World store;  // registration-complete by construction; never destroys, so saveWorld writes creation order
    Entity rootEntity{};
};

static_assert(std::is_nothrow_move_constructible_v<Prefab>);

}  // namespace engine
