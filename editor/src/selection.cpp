#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <utility>

namespace engine::editor {

bool Selection::empty() const noexcept { return items.empty(); }
std::size_t Selection::count() const noexcept { return items.size(); }

bool Selection::contains(Entity entity) const noexcept {
    // A linear scan is the only option: Entity is Handle<Tag>, which has a defaulted operator== but
    // NO std::hash and NO operator< (F24), so no hashed/ordered container compiles. Task 1.4.2 hit
    // the same wall in saveWorld. Selections are a handful of entities; this is not a hot path.
    return std::find(items.begin(), items.end(), entity) != items.end();
}

Entity Selection::primary() const noexcept { return primaryEntity; }
std::span<const Entity> Selection::entities() const noexcept { return std::span<const Entity>{items}; }

void Selection::set(Entity entity) {
    items.clear();
    primaryEntity = {};
    add(entity);  // one gate for the Entity{} rejection (I5)
}

void Selection::add(Entity entity) {
    if (!entity.valid() || contains(entity)) {
        return;
    }
    items.push_back(entity);
    primaryEntity = entity;  // the most recently ADDED entity is the primary (D2)
}

void Selection::remove(Entity entity) {
    const auto it = std::find(items.begin(), items.end(), entity);
    if (it == items.end()) {
        return;
    }
    items.erase(it);  // ORDERED erase: selection order is user-visible
    if (primaryEntity == entity) {
        primaryEntity = items.empty() ? Entity{} : items.back();
    }
}

void Selection::toggle(Entity entity) {
    if (contains(entity)) {
        remove(entity);
    } else {
        add(entity);
    }
}

void Selection::setAll(std::span<const Entity> entities) {
    items.clear();
    primaryEntity = {};
    for (const Entity e : entities) {
        add(e);  // dedupes, rejects Entity{}, and leaves the LAST valid entry primary
    }
}

void Selection::clear() noexcept {
    items.clear();
    primaryEntity = {};
}

std::size_t Selection::prune(const World& world) {
    const std::size_t before = items.size();
    std::erase_if(items, [&world](Entity e) { return !world.alive(e); });
    if (!world.alive(primaryEntity)) {
        primaryEntity = items.empty() ? Entity{} : items.back();
    }
    return before - items.size();
}

}  // namespace engine::editor
