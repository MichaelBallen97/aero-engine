#pragma once
// Aero Engine — engine::editor::Selection (task 2.2.1, D2). The editor's shared entity selection:
// ordered by SELECTION order, not by entity index or tree position. The PRIMARY entity is the most
// recently ADDED one -- the single entity 2.2.2's inspector edits and 2.3.3's gizmo attaches to.
//
// Holds handles, never pointers, and never keeps a World reference: an entity destroyed behind this
// object's back simply becomes a stale handle, which prune() drops.
//
// CONSUMERS: 2.2.2's inspector reads primary() only; 2.2.3's viewport reads the whole set; 2.3.2's
// picking WRITES into this same object (click-to-select in the viewport and click-to-select in the
// hierarchy must remain ONE selection, never two); 2.4.2's commands capture and restore it.
//
// ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9).

#include <aero/scene/entity.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

class Selection {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;
    [[nodiscard]] bool contains(Entity entity) const noexcept;  // false for Entity{}
    [[nodiscard]] Entity primary() const noexcept;              // Entity{} when empty
    [[nodiscard]] std::span<const Entity> entities() const noexcept;

    void set(Entity entity);                        // replace with exactly one; Entity{} clears
    void add(Entity entity);                         // no-op if present; otherwise appended and becomes primary
    void remove(Entity entity);                      // no-op if absent
    void toggle(Entity entity);                       // add or remove
    void setAll(std::span<const Entity> entities);   // replace wholesale (Shift+click ranges)
    void clear() noexcept;

    // Drops every dead or null handle. Returns how many were dropped. The primary follows: if it
    // was dropped, the new primary is the last surviving entry (or Entity{}).
    std::size_t prune(const World& world);

private:
    std::vector<Entity> items;
    Entity primaryEntity{};
};

}  // namespace engine::editor
