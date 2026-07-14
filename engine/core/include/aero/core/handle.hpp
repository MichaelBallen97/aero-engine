#pragma once
// Aero Engine — Handle<Tag>: the project-wide resource-reference primitive (task 0.2.1).
// ADR-001 mitigation #1, "handles, not pointers": every resource (textures, meshes, entities, ...)
// is referenced by a {index, generation} pair — never a raw owning pointer. A Handle is a
// trivially-copyable 64-bit value: cheap to pass and trivially serializable (docs/03). Generation 0
// is the reserved null sentinel, so a default-constructed Handle is invalid; a stale handle (its slot
// freed, its generation bumped) no longer matches its slot. The generational SlotMap that mints and
// validates handles lives in <aero/core/slot_map.hpp>. Distinct Tag types yield distinct,
// non-interchangeable Handle types: a Handle<MeshTag> cannot be passed where a Handle<TextureTag> is
// wanted — the type system enforces resource-kind safety at zero runtime cost.

#include <cstdint>

namespace engine {

template <typename Tag>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 == null; a live handle's generation is always >= 1

    // Null check only: "is this a non-null handle?". It does NOT prove the handle is still live —
    // liveness (was the slot freed?) is answered by SlotMap::get()/contains(), which compare this
    // generation against the slot's current one.
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    // Identity: equal index AND equal generation. A handle to an older generation of the same slot
    // is not equal to a newer one (the older is stale). Defaulted == also synthesizes !=.
    constexpr bool operator==(const Handle&) const noexcept = default;
};

}  // namespace engine
