#pragma once
// Aero Engine — engine::World (task 1.3.1): a move-only, pimpl'd wrapper over an entity-component
// registry. The ECS backing it is an implementation detail and NO third-party type crosses this
// header (project rule #3) — enforced by .github/scripts/check-scene-boundary.sh (the textual half,
// scanning every public engine header) and tests/scene_boundary_probe.cpp (the compile-time half,
// linking aero::scene alone). The word "entt" appears only inside comments in this file, which the
// guard strips before checking.
//
// WHY THE COMPONENT API IS TEMPLATES OVER SIX TYPE-ERASED PRIMITIVES: creating a typed component
// storage is a TEMPLATE in the backing ECS (verified against EnTT 3.16 — there is no runtime-typed
// storage factory), so it cannot be instantiated from an entt-free header. Everything else about a
// component — insert, read, erase, count, iterate — goes through a type-erased base and needs no `T`
// at all. So the public surface below is templates whose bodies do nothing but `static_cast` over
// six non-template primitives (addRaw / getRaw x2 / hasRaw / removeRaw / countRaw); the one operation
// that genuinely needs a template on the entt side (making T a component type of a World) lives
// behind the internal seam, <aero/scene/internal/world_access.hpp>.
//
// REGISTRATION IS PER-WORLD AND MANDATORY: a component type must be registered in THIS World, via
// engine::scene::internal::registerComponent<T>(world, name) (the internal seam), before it can be
// added, read, removed, counted or iterated. Using an unregistered type is a loud runtime rejection
// (nullptr / false / 0, with an ERROR logged from the raw primitive) — never UB. Registration is
// per-World by design: there is no process-global type table (the same global the project already
// refused for JobSystem), so "which types does this World know?" is always answerable from the
// World alone.
//
// NOT THREAD-SAFE: synchronize externally, exactly like <aero/core/slot_map.hpp>'s own contract.
//
// Multiple Worlds may coexist; there is no one-per-process rule, and two Worlds' registrations and
// storages are entirely independent (registering a type in one World does not register it in another,
// even though the *id* — being per-program-image, see component.hpp — is the same in both).
//
// NO EXCEPTIONS CROSS THIS API (docs/04): every rejection path returns an invalid/empty/false answer,
// never throws. Allocation failure still throws std::bad_alloc from the constructor and from any
// growing container inside a component storage, exactly as everywhere else in the engine — that is
// not a boundary this API tries to paper over.
//
// POINTER STABILITY: nothing may cache a T* returned by get<T>()/add<T>() across a later add<T>(),
// remove<T>(), destroy(), or clear() touching the SAME component type on this World — the identical
// rule <aero/core/slot_map.hpp>'s SlotMap::get() already states. A component storage may reallocate
// or reorder on any of those calls.

#include <aero/scene/component.hpp>
#include <aero/scene/entity.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace engine {

namespace scene::internal {
struct WorldAccess;  // the entt seam (engine/scene/internal/...); befriended by World below
}  // namespace scene::internal

namespace scene::detail {

// Expands fn(entity, *static_cast<Ts*>(slots[Is])...) over an index_sequence. The ONE place each()'s
// parameter pack is unpacked, and it contains no third-party name. Iterative by construction: a pack
// expansion is not recursion, so misc-no-recursion has nothing to say about it.
template <typename... Ts, typename Fn, std::size_t... Is>
void applySlots(Fn& fn, Entity entity, void* const* slots, std::index_sequence<Is...> /*seq*/) {
    fn(entity, *static_cast<Ts*>(slots[Is])...);
}

}  // namespace scene::detail

// A move-only ECS world: entities, their components, and a per-World table of registered component
// types. See the file header above for the design rationale.
class World {
public:
    World();
    ~World();
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- entities ---------------------------------------------------------------------------

    // Mints a fresh, live entity. Generation is always >= 1 (0 is the null sentinel, Handle<Tag>
    // convention). Never fails except on a moved-from World, where it returns Entity{} and logs one
    // ERROR — moved-from is the only failure mode create() has.
    [[nodiscard]] Entity create();

    // Destroys `entity`: erases it from every component storage it appears in (running each
    // component's destructor) and invalidates the handle permanently. Returns false — silently — for
    // an already-dead or null entity; returns false plus one ERROR on a moved-from World.
    bool destroy(Entity entity) noexcept;

    // Null check AND liveness: true only for a non-null entity this World actually still holds.
    // Silent in every case, including a moved-from World — polling a possibly-stale handle is a
    // normal, expected pattern (e.g. walking a hierarchy after a subtree was just destroyed), not an
    // error.
    [[nodiscard]] bool alive(Entity entity) const noexcept;

    // The number of LIVE entities — not the recycling capacity (which can be larger: destroyed slots
    // are kept around to be recycled by a later create()). 0 on a moved-from World, silently.
    [[nodiscard]] std::size_t entityCount() const noexcept;

    // Destroys every entity and empties every component storage. Component and entity-type
    // REGISTRATIONS survive — repopulating the World afterwards needs no re-registration. Every
    // Entity handle issued before this call becomes stale. One ERROR on a moved-from World; otherwise
    // silent.
    void clear() noexcept;

    // Visits every live entity exactly once, as fn(Entity). Never yields a destroyed entity. The
    // callback must not mutate this World (create/destroy an entity, add/remove a component, or
    // register a type) — the same contract each<Ts...>() states below, for the same reason: doing so
    // invalidates the cursor mid-walk. Zero live entities means zero invocations, silently — including
    // on a moved-from World.
    template <typename Fn>
    void eachEntity(Fn&& fn) const;

    // ---- components (entt-free templates over the raw primitives) -----------------------------

    // Ensures `entity` holds a T equal to `value`. If T is already present on `entity`, the OLD
    // element is erased and the new one inserted in its place — the returned address may differ from
    // a previous add<T>()'s, and the storage's packed order may change; nothing may depend on either.
    // Returns nullptr, WITHOUT storing anything, for an empty (tag) component T — has<T>() is true
    // afterwards regardless. Returns nullptr plus one ERROR for a dead/null entity or an unregistered
    // T (register it first via engine::scene::internal::registerComponent<T>).
    template <typename T>
    T* add(Entity entity, T value = T{});

    // The live T* on `entity`, or nullptr — silently — if the entity is dead, T is unregistered, or T
    // is simply absent from this entity. A compile error for an empty (tag) component: it stores no
    // value, so use has<T>() instead.
    template <typename T>
    [[nodiscard]] T* get(Entity entity) noexcept;
    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const noexcept;

    // True iff `entity` is alive and currently holds a T. Works for empty (tag) components. Silent in
    // every rejection case.
    template <typename T>
    [[nodiscard]] bool has(Entity entity) const noexcept;

    // Erases T from `entity` if present. Idempotent: a second call (or a call on an entity that never
    // had T) returns false, silently — never logs, never treats a miss as an error.
    template <typename T>
    bool remove(Entity entity) noexcept;

    // The exact number of live entities currently holding a T. 0, silently, for an unregistered T.
    template <typename T>
    [[nodiscard]] std::size_t componentCount() const noexcept;

    // Visits fn(Entity, Ts&...) for exactly the entities holding EVERY Ts, 1..MAX_QUERY_TYPES of them
    // (a compile error outside that range, and for any empty Ts — a tag cannot be bound by reference).
    // Driven by the smallest of the Ts storages; iteration order is that storage's packed order,
    // descending — deterministic for one fixed sequence of prior operations, but NOT stable across
    // edits, and nothing may depend on it. An unregistered or currently-empty Ts storage yields zero
    // invocations (an unregistered Ts additionally logs one ERROR from beginQuery; an empty storage
    // does not — see the ERROR table in world.cpp). The callback must not create/destroy entities,
    // add/remove components, or register a type on this World — collect into a std::vector<Entity>
    // and act after the loop if you need to. each<Ts...>() nested inside another each<Us...>() on the
    // SAME World is fine (each query's cursor lives entirely on the caller's stack).
    template <typename... Ts, typename Fn>
    void each(Fn&& fn);

    // ---- the component-type registry ----------------------------------------------------------

    [[nodiscard]] bool registered(ComponentTypeId id) const noexcept;

    // A linear scan of the registration table IN REGISTRATION ORDER, returning the first type
    // registered under `name`. A miss returns ComponentTypeId{}, silently. Order is preserved
    // deliberately, so a UI "Add Component" menu built by walking the table is deterministic.
    [[nodiscard]] ComponentTypeId findComponentType(std::string_view name) const noexcept;

    // The name a type was registered under. The returned view is into World-owned storage and stays
    // valid until ~World. An unregistered id (including ComponentTypeId{}) returns an empty view,
    // silently.
    [[nodiscard]] std::string_view componentTypeName(ComponentTypeId id) const noexcept;

    [[nodiscard]] std::size_t componentTypeCount() const noexcept;

    // ---- the type-erased primitives (public: the loader/inspector face) ------------------------
    // Same contracts as their typed faces above, minus the compile-time guards T's static_asserts
    // give you — this is the name-driven face the scene loader (1.4.2) and the editor inspector use,
    // where the caller only has a ComponentTypeId and a void*, never a concrete T.

    // source == nullptr default-constructs. WARNING: source must NOT point into THIS World's own
    // storage for the SAME component type — the erase-then-insert replace (see add<T>() above)
    // destroys the old element before reading `source`, so aliasing it silently reads freed memory.
    // add<T>() cannot hit this (its `value` parameter is always a fresh by-value copy); a caller of
    // addRaw() directly must copy first. This is a caller contract, not defended at runtime.
    void* addRaw(ComponentTypeId id, Entity entity, const void* source);
    [[nodiscard]] void* getRaw(ComponentTypeId id, Entity entity) noexcept;
    [[nodiscard]] const void* getRaw(ComponentTypeId id, Entity entity) const noexcept;
    [[nodiscard]] bool hasRaw(ComponentTypeId id, Entity entity) const noexcept;
    bool removeRaw(ComponentTypeId id, Entity entity) noexcept;
    [[nodiscard]] std::size_t countRaw(ComponentTypeId id) const noexcept;

    // Upper bound on how many component types a single each<Ts...>() call may query at once. Raising
    // it is a one-constant change with no API break.
    static constexpr std::size_t MAX_QUERY_TYPES = 8;

private:
    friend struct scene::internal::WorldAccess;

    struct QueryCursor {
        Entity entity{};
        std::array<void*, MAX_QUERY_TYPES> pools{};
        std::size_t count = 0;
        std::size_t leadIndex = 0;
        std::size_t remaining = 0;
    };

    [[nodiscard]] bool beginQuery(const ComponentTypeId* ids, std::size_t count, QueryCursor& cursor) const noexcept;
    [[nodiscard]] bool advanceQuery(QueryCursor& cursor, void** slots) const noexcept;
    [[nodiscard]] bool nextEntity(std::size_t& cursor, Entity& out) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

template <typename Fn>
void World::eachEntity(Fn&& fn) const {
    std::size_t cursor = 0;
    Entity entity{};
    while (nextEntity(cursor, entity)) {
        fn(entity);
    }
}

template <typename T>
T* World::add(Entity entity, T value) {
    static_assert(std::is_default_constructible_v<T>, "a component must be default-constructible");
    // VERIFIED (EnTT 3.16, spec F9): the type-erased insert COPY-constructs from the source pointer
    // and SILENTLY inserts nothing for a non-copy-constructible element — no diagnostic, no crash,
    // no component. Never let that compile.
    static_assert(std::is_copy_constructible_v<T>, "a component must be copy-constructible");
    // `value` is a BY-VALUE parameter, so &value can never alias this World's own storage — which
    // addRaw's erase-then-insert replace would otherwise read after destroying it.
    return static_cast<T*>(addRaw(componentTypeId<T>(), entity, &value));
}

template <typename T>
T* World::get(Entity entity) noexcept {
    static_assert(!std::is_empty_v<T>, "an empty (tag) component stores no value - use has<T>()");
    return static_cast<T*>(getRaw(componentTypeId<T>(), entity));
}

template <typename T>
const T* World::get(Entity entity) const noexcept {
    static_assert(!std::is_empty_v<T>, "an empty (tag) component stores no value - use has<T>()");
    return static_cast<const T*>(getRaw(componentTypeId<T>(), entity));
}

template <typename T>
bool World::has(Entity entity) const noexcept {
    return hasRaw(componentTypeId<T>(), entity);
}

template <typename T>
bool World::remove(Entity entity) noexcept {
    return removeRaw(componentTypeId<T>(), entity);
}

template <typename T>
std::size_t World::componentCount() const noexcept {
    return countRaw(componentTypeId<T>());
}

// A leading parameter pack followed by a further template parameter (Fn, deduced from the function
// argument) is standard-legal ([temp.param]/14: a pack may be followed by another template parameter
// if it is deducible from the parameter-type-list) — this is what lets callers write
// world.each<Position, Velocity>(fn), the same shape entt's own view API uses.
template <typename... Ts, typename Fn>
void World::each(Fn&& fn) {
    static_assert(sizeof...(Ts) >= 1 && sizeof...(Ts) <= MAX_QUERY_TYPES, "each() takes 1..8 component types");
    static_assert(((!std::is_empty_v<Ts>) && ...), "an empty (tag) component cannot be bound by reference");
    const std::array<ComponentTypeId, sizeof...(Ts)> ids{componentTypeId<Ts>()...};
    std::array<void*, sizeof...(Ts)> slots{};
    QueryCursor cursor{};
    if (!beginQuery(ids.data(), ids.size(), cursor)) {
        return;
    }
    while (advanceQuery(cursor, slots.data())) {
        scene::detail::applySlots<Ts...>(fn, cursor.entity, slots.data(), std::index_sequence_for<Ts...>{});
    }
}

}  // namespace engine
