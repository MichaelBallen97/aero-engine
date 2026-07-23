// engine/scene/src/world.cpp — task 1.3.1: the EnTT backend behind engine::World.
// This TU and engine/scene/internal/aero/scene/internal/world_access.hpp are the ONLY places EnTT is
// visible inside engine/ (project rule #3; .github/scripts/check-scene-boundary.sh +
// tests/scene_boundary_probe.cpp). World::Impl never leaves this file.
//
// Everything here is ITERATIVE (misc-no-recursion is live) and allocation-light. No exceptions cross
// the API; nothing asserts on data (0.4.2's C-1 reconciliation: assert is reserved for
// main-thread-ownership checks and internal backend invariants). Every message is ASCII-only.
//
// TWO ENTT FACTS THE WHOLE FILE RESTS ON, both verified at the pinned 3.16.0:
//   * the ENTITY storage is swap_only: its free_list() is the LIVE count and data()[0..free_list())
//     is exactly the live set. Its size() includes destroyed slots kept for recycling, and ITERATING
//     it yields destroyed identifiers. A COMPONENT storage is swap_and_pop: size() is its live count
//     and its free_list() is a sentinel that must never be read.
//   * value(e) is only defined when contains(e) — in a Debug build it is an assert-abort, not silent
//     UB. Every value() call below is guarded by a contains() immediately above it.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/transform.hpp>  // task 1.3.2 — scene::detail::registerBuiltinComponents
#include <aero/scene/world.hpp>

#include <algorithm>  // task 1.3.2 — std::find (ordered child unlink)
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>  // task 1.3.2 — HierarchyNode::children

namespace engine {
namespace {

using scene::internal::ErasedStorage;
using scene::internal::fromEntt;
using scene::internal::Registry;
using scene::internal::SceneEntity;
using scene::internal::toEntt;

// The hierarchy's backing storage (task 1.3.2): a TU-LOCAL component type the registry owns. It is
// created directly on the registry and NEVER bound into the public registration table, so
// registered() / findComponentType() / componentTypeCount() / countRaw() cannot see it, and the type
// itself cannot escape this file. Its per-entity lifetime is therefore the registry's own: destroy()
// and clear() drop nodes along with every other storage, and no manual bookkeeping can drift.
//
// A node exists only for an entity that has a parent OR children. Detaching back to a root leaves an
// empty node behind — bytes, not behaviour: it is invisible through every public API
// (parent() == Entity{}, childCount() == 0) and dies with its entity.
struct HierarchyNode {
    Entity parent{};               // Entity{} == root
    std::vector<Entity> children;  // direct children, in ATTACH ORDER (ordered erase — determinism)
};

}  // namespace

struct World::Impl {
    struct Registration {
        std::string name;
        ErasedStorage* storage = nullptr;  // stable: the registry owns pools through shared_ptr
        ComponentTypeId id{};
        bool emptyType = false;
    };

    Registry registry;
    // The hierarchy storage, created once here and cached for the Impl's whole life (task 1.3.2).
    // A registry pool pointer is stable across every later create/destroy/clear AND across a registry
    // move — the same property Registration::storage below already rests on — so caching it keeps
    // every hierarchy query a plain deref and means no const query path ever has to CREATE a storage.
    ErasedStorage* hierarchy = &registry.storage<HierarchyNode>();
    // deque, NOT vector: componentTypeName() hands out a std::string_view into a record, and a deque
    // never invalidates references to existing elements on push_back.
    std::deque<Registration> registrations;
    // LOOKUP-ONLY, never iterated — no unordered_map ordering can reach any output.
    std::unordered_map<const void*, std::size_t> byId;

    [[nodiscard]] const Registration* find(ComponentTypeId id) const noexcept {
        const auto it = byId.find(id.value);
        return it == byId.end() ? nullptr : &registrations[it->second];
    }

    [[nodiscard]] bool aliveInternal(Entity entity) const noexcept {
        return entity.valid() && registry.valid(toEntt(entity));
    }

    // nodeOf / parentOf / lastChildOf are const AND noexcept and allocate nothing: `hierarchy`
    // already exists. They hand back a MUTABLE node from a const Impl on purpose — the same shape
    // getRaw() uses, and the same shallow-constness property that lets a const World reach a
    // non-const Registry.
    //
    // NEVER cache a HierarchyNode* across a registry.destroy(): the node storage is swap-and-pop, so
    // destroying one entity RELOCATES another node. Every caller below re-resolves by Entity handle.
    [[nodiscard]] HierarchyNode* nodeOf(Entity entity) const noexcept {
        const SceneEntity ee = toEntt(entity);
        // value() is only defined when contains() — an assert-abort in Debug otherwise.
        return hierarchy->contains(ee) ? static_cast<HierarchyNode*>(hierarchy->value(ee)) : nullptr;
    }

    [[nodiscard]] Entity parentOf(Entity entity) const noexcept {
        const HierarchyNode* node = nodeOf(entity);
        return node == nullptr ? Entity{} : node->parent;
    }

    // The LAST direct child of `entity`, or false when there is no node or no children. The "last"
    // choice is what makes destroy()'s descent O(1) per step: unlinking the back of a vector never
    // shifts anything.
    [[nodiscard]] bool lastChildOf(Entity entity, Entity& out) const noexcept {
        const HierarchyNode* node = nodeOf(entity);
        if (node == nullptr || node->children.empty()) {
            return false;
        }
        out = node->children.back();
        return true;
    }

    // Creates the node if absent. push(ee, nullptr) DEFAULT-constructs (verified at the pinned entt).
    // The ERASED primitives are used here rather than the typed storage's emplace/get, so this file
    // keeps relying only on the entt surface task 1.3.1 already pinned. The ONLY allocating helper —
    // and it is deliberately unreachable from destroy() (M1).
    [[nodiscard]] HierarchyNode& ensureNode(Entity entity) {
        const SceneEntity ee = toEntt(entity);
        if (!hierarchy->contains(ee)) {
            hierarchy->push(ee, nullptr);
        }
        return *static_cast<HierarchyNode*>(hierarchy->value(ee));
    }

    // Detaches `child` from whatever parent it currently has: ORDERED erase from that parent's
    // children (std::find + vector::erase, never swap-and-pop — sibling order is part of eachChild's
    // contract) plus clearing the child's own parent link. A no-op when there is no node or no
    // parent. ALLOCATES NOTHING: erasing from a vector of a trivially-copyable element neither
    // reallocates nor throws, which is what lets destroy() stay noexcept (M1).
    void unlinkFromParent(Entity child) noexcept {
        HierarchyNode* node = nodeOf(child);
        if (node == nullptr || !node->parent.valid()) {
            return;
        }
        HierarchyNode* parentNode = nodeOf(node->parent);
        node->parent = Entity{};
        if (parentNode == nullptr) {
            return;
        }
        auto& list = parentNode->children;
        const auto it = std::find(list.begin(), list.end(), child);
        if (it != list.end()) {
            list.erase(it);
        }
    }
};

World::World() : impl(std::make_unique<Impl>()) { scene::detail::registerBuiltinComponents(*this); }
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

// ---- entities -----------------------------------------------------------------------------

Entity World::create() {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "create");
        return Entity{};
    }
    return fromEntt(impl->registry.create());
}

bool World::destroy(Entity entity) noexcept {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "destroy");
        return false;
    }
    if (!impl->aliveInternal(entity)) {
        return false;
    }
    // Post-order subtree destruction with O(1) scratch and NO allocation (this member is
    // noexcept). Descend to the deepest last child, destroy that leaf, step back to its parent,
    // repeat: every iteration destroys exactly one entity, so it terminates in exactly
    // subtree-size iterations, and a child is always destroyed before its parent.
    //
    // NEVER cache a HierarchyNode* across the destroy below: the node storage is swap-and-pop, so
    // destroying one entity RELOCATES another node. Every step re-resolves by Entity handle.
    Entity cur = entity;
    for (;;) {
        Entity child{};
        while (impl->lastChildOf(cur, child)) {  // re-resolves cur's node on every call
            cur = child;
        }
        // Read the parent BEFORE unlinking — unlinkFromParent clears cur's own parent link.
        const Entity up = (cur == entity) ? Entity{} : impl->parentOf(cur);
        impl->unlinkFromParent(cur);          // ordered erase from up's children; no allocation
        impl->registry.destroy(toEntt(cur));  // also erases cur's HierarchyNode
        if (cur == entity) {
            return true;
        }
        cur = up;
    }
}

bool World::alive(Entity entity) const noexcept { return impl != nullptr && impl->aliveInternal(entity); }

std::size_t World::entityCount() const noexcept {
    if (impl == nullptr) {
        return 0;
    }
    // const World still gets a non-const Registry&: unique_ptr's constness applies to the pointer,
    // not the pointee, and storage<T>() needs a non-const registry (F12).
    Registry& reg = impl->registry;
    // free_list(), NEVER size(): the entity storage is swap_only, so free_list() is the live count
    // and size() also counts destroyed slots kept around for recycling.
    return reg.storage<SceneEntity>().free_list();
}

void World::clear() noexcept {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "clear");
        return;
    }
    // Registrations and their cached storage pointers survive (F11): registry.clear() empties every
    // pool and the entity storage but does not drop the pools themselves. That includes the
    // hierarchy storage (task 1.3.2), so every parent/child link vanishes with the entities and the
    // World is immediately reusable — re-parenting after clear() works with no re-registration.
    impl->registry.clear();
}

bool World::nextEntity(std::size_t& cursor, Entity& out) const noexcept {
    if (impl == nullptr) {
        return false;
    }
    Registry& reg = impl->registry;
    auto& es = reg.storage<SceneEntity>();
    // data()[0 .. free_list()), NEVER the entity storage's own iterators: iterating it yields
    // destroyed identifiers too (F10 — "the trap").
    if (cursor >= es.free_list()) {
        return false;
    }
    out = fromEntt(es.data()[cursor]);
    ++cursor;
    return true;
}

// ---- hierarchy (task 1.3.2) -----------------------------------------------------------------

bool World::setParent(Entity child, Entity parent) {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "setParent");
        return false;
    }
    if (!impl->aliveInternal(child)) {
        AERO_LOG_ERROR("scene: setParent on a dead or null child (index {}, generation {})", child.index,
                       child.generation);
        return false;
    }
    if (parent.valid() && !impl->aliveInternal(parent)) {
        AERO_LOG_ERROR("scene: setParent to a dead parent (index {}, generation {})", parent.index, parent.generation);
        return false;
    }
    if (child == parent) {
        AERO_LOG_ERROR("scene: setParent cannot parent an entity to itself (index {})", child.index);
        return false;
    }
    // Walk UP the new parent's ancestor chain: if `child` is on it, `parent` is already a descendant
    // of `child` and the link would close a cycle. O(depth), iterative, and it covers the direct case
    // (A->B then B->A) and every deeper one uniformly.
    for (Entity ancestor = parent; ancestor.valid(); ancestor = impl->parentOf(ancestor)) {
        if (ancestor == child) {
            AERO_LOG_ERROR("scene: setParent would create a cycle (child index {}, parent index {})", child.index,
                           parent.index);
            return false;
        }
    }
    const Entity current = impl->parentOf(child);
    if (current == parent) {
        return true;  // silent no-op; sibling position is deliberately left untouched
    }
    impl->unlinkFromParent(child);  // no-op when child has no node or is already a root
    // Two statements, each using its reference immediately: ensureNode may insert, and an insert may
    // move elements, so no reference is held across the second call.
    impl->ensureNode(child).parent = parent;
    if (parent.valid()) {
        impl->ensureNode(parent).children.push_back(child);
    }
    return true;
}

Entity World::parent(Entity child) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(child)) {
        return Entity{};
    }
    // Live-or-null by the subtree-destroy invariant: nothing can observe a dead parent here.
    return impl->parentOf(child);
}

std::size_t World::childCount(Entity parent) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(parent)) {
        return 0;
    }
    const HierarchyNode* node = impl->nodeOf(parent);
    return node == nullptr ? 0 : node->children.size();
}

bool World::nextChild(Entity parent, std::size_t& cursor, Entity& out) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(parent)) {
        return false;
    }
    // The node is resolved on every call rather than cached across the walk — the same shape
    // nextEntity uses, and the reason eachChild's callback must not mutate the World.
    const HierarchyNode* node = impl->nodeOf(parent);
    if (node == nullptr || cursor >= node->children.size()) {
        return false;
    }
    out = node->children[cursor];
    ++cursor;
    return true;
}

// ---- components (the type-erased primitives) -----------------------------------------------

void* World::addRaw(ComponentTypeId id, Entity entity, const void* source) {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "addRaw");
        return nullptr;
    }
    if (!impl->aliveInternal(entity)) {
        AERO_LOG_ERROR("scene: add on a dead or null entity (index {}, generation {})", entity.index,
                       entity.generation);
        return nullptr;
    }
    const Impl::Registration* reg = impl->find(id);
    if (reg == nullptr) {
        AERO_LOG_ERROR("scene: component type is not registered in this world (id {})", id.value);
        return nullptr;
    }
    ErasedStorage& s = *reg->storage;
    const SceneEntity ee = toEntt(entity);
    // Erase-then-insert replace (D11): a second push() on a CONTAINED entity is an assert-abort in
    // Debug (F5's correction), so an existing element must be removed first.
    if (s.contains(ee)) {
        s.remove(ee);
    }
    s.push(ee, source);
    // M5: belt-and-braces behind add<T>()'s static_assert — converts F9's silent no-op (a
    // non-copy-constructible T reached through addRaw with no compile-time guard) into a nullptr.
    if (reg->emptyType || !s.contains(ee)) {
        return nullptr;
    }
    return s.value(ee);
}

const void* World::getRaw(ComponentTypeId id, Entity entity) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(entity)) {
        return nullptr;
    }
    const Impl::Registration* reg = impl->find(id);
    if (reg == nullptr) {
        return nullptr;
    }
    const ErasedStorage& s = *reg->storage;
    const SceneEntity ee = toEntt(entity);
    // value() is only defined when contains() (F5's correction: an assert-abort in Debug otherwise).
    return s.contains(ee) ? s.value(ee) : nullptr;
}

void* World::getRaw(ComponentTypeId id, Entity entity) noexcept {
    return const_cast<void*>(std::as_const(*this).getRaw(id, entity));
}

bool World::hasRaw(ComponentTypeId id, Entity entity) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(entity)) {
        return false;
    }
    const Impl::Registration* reg = impl->find(id);
    return reg != nullptr && reg->storage->contains(toEntt(entity));
}

bool World::removeRaw(ComponentTypeId id, Entity entity) noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "removeRaw");
        return false;
    }
    if (!impl->aliveInternal(entity)) {
        return false;
    }
    const Impl::Registration* reg = impl->find(id);
    if (reg == nullptr) {
        return false;
    }
    // remove() is idempotent (F5: contains(entt) && (erase(entt), true)) — a miss returns false.
    return reg->storage->remove(toEntt(entity));
}

std::size_t World::countRaw(ComponentTypeId id) const noexcept {
    if (impl == nullptr) {
        return 0;
    }
    const Impl::Registration* reg = impl->find(id);
    // size(), NEVER free_list(): a COMPONENT storage is swap_and_pop, so size() is its live count and
    // free_list() is a sentinel (0xFFFFFFFF) that must never be read (F10).
    return reg == nullptr ? 0 : reg->storage->size();
}

bool World::beginQuery(const ComponentTypeId* ids, std::size_t count, QueryCursor& cursor) const noexcept {
    AERO_PROFILE_ZONE;
    if (impl == nullptr || ids == nullptr || count == 0 || count > MAX_QUERY_TYPES) {
        return false;
    }
    std::size_t lead = 0;
    std::size_t leadSize = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Impl::Registration* reg = impl->find(ids[i]);
        if (reg == nullptr) {
            AERO_LOG_ERROR("scene: each() over a component type that is not registered in this world");
            return false;
        }
        // Belt-and-braces behind each<Ts...>()'s static_assert (M5's sibling case): the raw path has
        // no compile-time guard against an empty (tag) component being bound by reference.
        if (reg->emptyType) {
            AERO_LOG_ERROR("scene: each() cannot bind an empty (tag) component by reference");
            return false;
        }
        cursor.pools[i] = reg->storage;
        const std::size_t size = reg->storage->size();
        // An empty storage yields zero invocations SILENTLY (N12) — a registered-but-unpopulated type
        // is normal, not an error.
        if (size == 0) {
            return false;
        }
        if (i == 0 || size < leadSize) {
            lead = i;
            leadSize = size;
        }
    }
    cursor.count = count;
    cursor.leadIndex = lead;
    cursor.remaining = leadSize;
    return true;
}

bool World::advanceQuery(QueryCursor& cursor, void** slots) const noexcept {
    auto* lead = static_cast<ErasedStorage*>(cursor.pools[cursor.leadIndex]);
    // Iterative by construction (misc-no-recursion is live): a `continue` skips a lead entity that
    // one of the other storages does not contain, without ever recursing.
    while (cursor.remaining > 0) {
        --cursor.remaining;
        const SceneEntity ee = lead->data()[cursor.remaining];
        bool ok = true;
        for (std::size_t i = 0; i < cursor.count && ok; ++i) {
            if (i != cursor.leadIndex && !static_cast<ErasedStorage*>(cursor.pools[i])->contains(ee)) {
                ok = false;
            }
        }
        if (!ok) {
            continue;
        }
        for (std::size_t i = 0; i < cursor.count; ++i) {
            slots[i] = static_cast<ErasedStorage*>(cursor.pools[i])->value(ee);
        }
        cursor.entity = fromEntt(ee);
        return true;
    }
    return false;
}

// ---- the component-type registry -------------------------------------------------------------

bool World::registered(ComponentTypeId id) const noexcept { return impl != nullptr && impl->find(id) != nullptr; }

ComponentTypeId World::findComponentType(std::string_view name) const noexcept {
    if (impl == nullptr) {
        return {};
    }
    // Linear scan IN REGISTRATION ORDER — the first match wins, and order is preserved deliberately
    // (a UI "Add Component" menu built by walking the table stays deterministic).
    for (const Impl::Registration& reg : impl->registrations) {
        if (reg.name == name) {
            return reg.id;
        }
    }
    return {};
}

std::string_view World::componentTypeName(ComponentTypeId id) const noexcept {
    if (impl == nullptr) {
        return {};
    }
    const Impl::Registration* reg = impl->find(id);
    return reg == nullptr ? std::string_view{} : std::string_view{reg->name};
}

std::size_t World::componentTypeCount() const noexcept { return impl == nullptr ? 0 : impl->registrations.size(); }

// ---- the internal registration seam ------------------------------------------------------------

namespace scene::internal {

Registry* WorldAccess::registry(World& world) noexcept {
    return world.impl == nullptr ? nullptr : &world.impl->registry;
}

ComponentTypeId WorldAccess::bind(World& world, ComponentTypeId id, std::string_view name, ErasedStorage* storage,
                                  bool emptyType) {
    if (world.impl == nullptr || !id.valid() || storage == nullptr) {
        return {};
    }
    World::Impl& impl = *world.impl;
    // Idempotent: an existing id returns unchanged and never touches storage. A DIFFERING name WARNs
    // and keeps the first — the id is the identity, the name is a label.
    if (const auto it = impl.byId.find(id.value); it != impl.byId.end()) {
        const World::Impl::Registration& existing = impl.registrations[it->second];
        if (existing.name != name) {
            AERO_LOG_WARN("scene: component type re-registered under a different name; keeping '{}'", existing.name);
        }
        return existing.id;
    }
    // A duplicate NAME on a NEW id WARNs; findComponentType keeps resolving to the first registrant.
    for (const World::Impl::Registration& reg : impl.registrations) {
        if (reg.name == name) {
            AERO_LOG_WARN(
                "scene: component type name '{}' is already registered to a different type; "
                "findComponentType will resolve to the first",
                name);
            break;
        }
    }
    impl.registrations.push_back(World::Impl::Registration{std::string{name}, storage, id, emptyType});
    impl.byId.emplace(id.value, impl.registrations.size() - 1U);
    return id;
}

}  // namespace scene::internal
}  // namespace engine
