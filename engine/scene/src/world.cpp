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
#include <aero/scene/world.hpp>

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {

using scene::internal::ErasedStorage;
using scene::internal::fromEntt;
using scene::internal::Registry;
using scene::internal::SceneEntity;
using scene::internal::toEntt;

}  // namespace

struct World::Impl {
    struct Registration {
        std::string name;
        ErasedStorage* storage = nullptr;  // stable: the registry owns pools through shared_ptr
        ComponentTypeId id{};
        bool emptyType = false;
    };

    Registry registry;
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
};

World::World() : impl(std::make_unique<Impl>()) {}
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
    // The version_type return is discarded on purpose (N8: not [[nodiscard]], unlike create()).
    impl->registry.destroy(toEntt(entity));
    return true;
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
    // pool and the entity storage but does not drop the pools themselves.
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
