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
#include <cstdint>
#include <deque>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>  // task 1.3.2 — HierarchyNode::children

// Why the patch digit too: RuntimeStorage overrides get_at and swap_or_move, private virtuals of basic_sparse_set, and
// override catches a rename, never a changed contract. A bump re-reads sparse_set.hpp and storage.hpp, then moves this
// line.
static_assert(ENTT_VERSION_MAJOR == 3 && ENTT_VERSION_MINOR == 16 && ENTT_VERSION_PATCH == 0,
              "RuntimeStorage was written against EnTT 3.16.0: re-read its overrides before moving this pin");

namespace engine {
namespace {

using scene::internal::ErasedStorage;
using scene::internal::fromEntt;
using scene::internal::Registry;
using scene::internal::SceneEntity;
using scene::internal::toEntt;
using scene::internal::Traits;  // task 2.4.2 — World::recreate's occupancy pre-check

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

// The name storage (task 2.2.1): the SAME TU-local, never-registered pattern HierarchyNode uses
// above, for the same reason -- docs/09 models `name` at ENTITY level, so a name is entity-level
// data, not a component. It never enters Impl::registrations, so registered() / findComponentType()
// / componentTypeAt() / componentTypeCount() / countRaw() / each<Ts...>() cannot see it and no count
// assertion moves (D1/AC-6). Its per-entity lifetime is the registry's own: destroy() and clear()
// drop records with every other storage, so AC-2 holds structurally with no teardown code here.
//
// A record exists only for an entity with a NON-EMPTY name; setName(e, "") erases it.
struct EntityName {
    std::string value;
};

// The marker every runtime-registered component is stored under; each game type has its own pool under its own id.
struct RuntimeComponent {};

// A component storage sized at run time, the registry's to own (entt::storage_type below). A push only adds pages, so
// it never relocates an element: copyComponent rests on that. A tag (size 0) stores nothing, like a typed tag storage.
//
// Why a page and not one growing array: a typed storage is paged for the same reason, and World::copyComponent reads
// its source AFTER the only call that could move it.
class RuntimeStorage final : public ErasedStorage {
    using Base = ErasedStorage;
    using Iterator = Base::basic_iterator;
    static constexpr std::size_t PAGE_ELEMENTS = 1024;

public:
    using allocator_type = Base::allocator_type;  // NOLINT(readability-identifier-naming): EnTT's name

    explicit RuntimeStorage(const allocator_type& allocator)
        : Base{entt::type_id<RuntimeComponent>(), entt::deletion_policy::swap_and_pop, allocator} {}
    RuntimeStorage(const RuntimeStorage&) = delete;
    RuntimeStorage& operator=(const RuntimeStorage&) = delete;
    RuntimeStorage(RuntimeStorage&&) = delete;
    RuntimeStorage& operator=(RuntimeStorage&&) = delete;
    ~RuntimeStorage() override {
        RuntimeStorage::pop_all();
        for (std::byte* page : pages) {
            ::operator delete(page);
        }
    }

    // Runs before the first push.
    void configure(const scene::detail::ComponentOps& componentOps) { ops = componentOps; }

private:
    [[nodiscard]] std::byte* at(std::size_t pos) const noexcept {
        return pages[pos / PAGE_ELEMENTS] + ((pos % PAGE_ELEMENTS) * ops.size);
    }

    // nullptr for a tag, which stores no value.
    [[nodiscard]] const void* get_at(std::size_t pos) const override { return ops.size == 0 ? nullptr : at(pos); }

    // Three relocates through one temporary element. No caller in production sorts a pool; basic_sparse_set's public
    // swap_elements reaches this, and a case drives it.
    void swap_or_move(std::size_t from, std::size_t to) override {
        if (ops.size == 0 || from == to) {
            return;
        }
        void* held = ::operator new(ops.size);
        ops.relocate(held, at(from));
        ops.relocate(at(from), at(to));
        ops.relocate(at(to), held);
        ::operator delete(held);
    }

protected:
    // Swap-and-pop, as a typed storage does it: the last element moves into the erased slot.
    void pop(Iterator first, Iterator last) override {
        for (; first != last; ++first) {
            if (ops.size != 0) {
                std::byte* erased = at(index(*first));  // before swap_and_pop rewrites the sparse entry
                std::byte* back = at(size() - 1U);
                ops.destroy(erased);
                if (erased != back) {
                    ops.relocate(erased, back);
                }
            }
            Base::swap_and_pop(first);
        }
    }

    void pop_all() override {
        if (ops.size != 0) {
            for (std::size_t pos = size(); pos-- > 0U;) {
                ops.destroy(at(pos));
            }
        }
        Base::pop_all();
    }

    Iterator try_emplace(const SceneEntity entity, const bool forceBack, const void* value) override {
        const Iterator it = Base::try_emplace(entity, forceBack);
        if (ops.size == 0) {
            return it;
        }
        const auto pos = static_cast<std::size_t>(it.index());
        try {
            while (pos / PAGE_ELEMENTS >= pages.size()) {
                pages.push_back(static_cast<std::byte*>(::operator new(ops.size * PAGE_ELEMENTS)));
            }
            ops.construct(at(pos), value);
        } catch (...) {
            Base::pop(it, it + 1);  // the base's pop: no element was constructed
            throw;
        }
        return it;
    }

private:
    scene::detail::ComponentOps ops{};
    std::vector<std::byte*> pages;
};

}  // namespace
}  // namespace engine

// The pinned EnTT's own customisation point: the registry allocates this storage for the marker type.
template <typename Allocator>
struct entt::storage_type<engine::RuntimeComponent, engine::scene::internal::SceneEntity, Allocator> {
    using type = engine::RuntimeStorage;  // NOLINT(readability-identifier-naming): EnTT's name
};

namespace engine {

struct World::Impl {
    struct Registration {
        std::string name;
        ErasedStorage* storage = nullptr;  // stable: the registry owns pools through shared_ptr
        ComponentTypeId id{};
        bool emptyType = false;
        // The recipe that made `storage`, so World::clone (task 4.7.2) can make the same pool in another registry.
        // Invariant: makeStorage != nullptr (a typed pool, made by it), or makeStorage == nullptr and `storage` is a
        // RuntimeStorage configured with `ops`.
        scene::internal::StorageFactory makeStorage = nullptr;
        scene::detail::ComponentOps ops{};
    };

    Registry registry;
    // The hierarchy storage, created once here and cached for the Impl's whole life (task 1.3.2).
    // A registry pool pointer is stable across every later create/destroy/clear AND across a registry
    // move — the same property Registration::storage below already rests on — so caching it keeps
    // every hierarchy query a plain deref and means no const query path ever has to CREATE a storage.
    ErasedStorage* hierarchy = &registry.storage<HierarchyNode>();
    // The name storage, cached exactly like `hierarchy` above and for the same reason: a registry
    // pool pointer is stable for the Impl's whole life, so every name query is a plain deref and no
    // const path ever has to CREATE a storage (task 2.2.1).
    ErasedStorage* names = &registry.storage<EntityName>();
    // deque, NOT vector: componentTypeName() hands out a std::string_view into a record, and a deque
    // never invalidates references to existing elements on push_back.
    std::deque<Registration> registrations;
    // LOOKUP-ONLY, never iterated — no unordered_map ordering can reach any output.
    std::unordered_map<const void*, std::size_t> byId;

    [[nodiscard]] const Registration* find(ComponentTypeId id) const noexcept {
        const auto it = byId.find(id.value);
        return it == byId.end() ? nullptr : &registrations[it->second];
    }

    // A run-time pool for `ops`, under a pool id no pool holds. Built-ins sit under their type hashes, so the walk from
    // a fixed seed ends at the first free value and depends on registration order alone. Shared by registerComponentRaw
    // and clone().
    RuntimeStorage& makeRuntimePool(const scene::detail::ComponentOps& poolOps) {
        auto poolId = static_cast<entt::id_type>(0x41455230U + registrations.size());
        while (registry.storage(poolId) != nullptr) {
            ++poolId;
        }
        RuntimeStorage& storage = registry.storage<RuntimeComponent>(poolId);
        storage.configure(poolOps);
        return storage;
    }

    // Records a registration: idempotent (an existing id returns unchanged and never touches storage; a DIFFERING name
    // WARNs and keeps the first -- the id is the identity, the name is a label), a duplicate NAME on a NEW id WARNs
    // (findComponentType keeps resolving to the first registrant), then the append. Shared by both registration paths.
    ComponentTypeId bindRegistration(ComponentTypeId id, std::string_view name, ErasedStorage* storage, bool emptyType,
                                     scene::internal::StorageFactory makeStorage,
                                     const scene::detail::ComponentOps& poolOps) {
        if (const auto it = byId.find(id.value); it != byId.end()) {
            const Registration& existing = registrations[it->second];
            if (existing.name != name) {
                AERO_LOG_WARN("scene: component type re-registered under a different name; keeping '{}'",
                              existing.name);
            }
            return existing.id;
        }
        for (const Registration& reg : registrations) {
            if (reg.name == name) {
                AERO_LOG_WARN(
                    "scene: component type name '{}' is already registered to a different type; "
                    "findComponentType will resolve to the first",
                    name);
                break;
            }
        }
        registrations.push_back(Registration{std::string{name}, storage, id, emptyType, makeStorage, poolOps});
        byId.emplace(id.value, registrations.size() - 1U);
        return id;
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

    // The name record for `entity`, or nullptr. Const AND noexcept for the same shallow-constness
    // reason nodeOf() is. NEVER cache the result across a setName(other, "") / destroy() / clear():
    // the name storage is swap-and-pop, so erasing one record RELOCATES another.
    [[nodiscard]] EntityName* nameOf(Entity entity) const noexcept {
        const SceneEntity ee = toEntt(entity);
        // value() is only defined when contains() -- an assert-abort in Debug otherwise.
        return names->contains(ee) ? static_cast<EntityName*>(names->value(ee)) : nullptr;
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
        // destroy()'s descent always unlinks the element lastChildOf() just handed back — i.e. the
        // BACK of the list — so check that first and pop it: O(1), no search. Without this fast
        // path the search below makes destroying a node with N direct children O(N^2), which a flat
        // scene (the shape docs/09 encodes, and what an editor "select root, Delete" produces)
        // reaches immediately. setParent's general case — and `entity` itself in destroy(), which
        // may sit mid-list in its own parent's vector — falls through to the ordered find+erase
        // that keeps sibling order intact.
        if (!list.empty() && list.back() == child) {
            list.pop_back();
            return;
        }
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

Entity World::recreate(Entity entity) {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "recreate");
        return Entity{};
    }
    if (!entity.valid() || impl->aliveInternal(entity)) {
        return Entity{};  // silent: nothing to restore
    }
    Registry& reg = impl->registry;
    const SceneEntity ee = toEntt(entity);

    // F6: the entity storage's size() counts every slot this World has EVER issued. An index beyond
    // it was never minted here, and honouring it would punch a hole in the packed array.
    if (static_cast<std::size_t>(entity.index) >= reg.storage<SceneEntity>().size()) {
        AERO_LOG_ERROR("scene: recreate of an index this world never issued (index {})", entity.index);
        return Entity{};
    }
    // F5: side-effect-free occupancy test. current(ee) is the version that index holds NOW; if the
    // identifier built from it is alive, the slot belongs to a different generation and entt's own
    // create(hint) would silently hand back a substitute — which this contract forbids.
    const SceneEntity occupant = Traits::construct(Traits::to_entity(ee), reg.current(ee));
    if (reg.valid(occupant)) {
        AERO_LOG_ERROR("scene: recreate of an occupied index (index {}, generation {})", entity.index,
                       entity.generation);
        return Entity{};
    }
    const Entity made = fromEntt(reg.create(ee));
    // Belt-and-braces behind F3's contract, in the addRaw M5 spirit: if a future entt ever falls back
    // despite the checks above, undo the surprise rather than hand it to the caller.
    if (made != entity) {
        reg.destroy(toEntt(made));
        AERO_LOG_ERROR("scene: recreate could not honour the requested identity (index {})", entity.index);
        return Entity{};
    }
    return made;
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

// ---- names (task 2.2.1) ----------------------------------------------------------------------

bool World::setName(Entity entity, std::string_view name) {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "setName");
        return false;
    }
    if (!impl->aliveInternal(entity)) {
        AERO_LOG_ERROR("scene: setName on a dead or null entity (index {}, generation {})", entity.index,
                       entity.generation);
        return false;
    }
    const SceneEntity ee = toEntt(entity);
    if (name.empty()) {
        impl->names->remove(ee);  // idempotent; "" == absent (docs/09)
        return true;
    }
    // ASSIGN IN PLACE when a record already exists -- never erase-then-push. remove() swap-and-pops,
    // which relocates ANOTHER entity's record and would dangle a name() view just handed out. The
    // in-place path is the only reason setName on one entity cannot invalidate another's view.
    if (EntityName* existing = impl->nameOf(entity); existing != nullptr) {
        existing->value.assign(name);
        return true;
    }
    // push(ee, nullptr) DEFAULT-constructs (the ensureNode precedent, verified at the pinned entt):
    // growing the paged payload allocates a NEW page and never relocates an existing one.
    impl->names->push(ee, nullptr);
    static_cast<EntityName*>(impl->names->value(ee))->value.assign(name);
    return true;
}

std::string_view World::name(Entity entity) const noexcept {
    if (impl == nullptr || !impl->aliveInternal(entity)) {
        return {};
    }
    const EntityName* record = impl->nameOf(entity);
    return record == nullptr ? std::string_view{} : std::string_view{record->value};
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

bool World::copyComponent(ComponentTypeId id, Entity from, Entity to) {
    AERO_PROFILE_ZONE;
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "copyComponent");
        return false;
    }
    if (!impl->aliveInternal(from) || !impl->aliveInternal(to)) {
        AERO_LOG_ERROR("scene: copyComponent with a dead or null entity (from index {}, to index {})", from.index,
                       to.index);
        return false;
    }
    const Impl::Registration* reg = impl->find(id);
    if (reg == nullptr) {
        AERO_LOG_ERROR("scene: component type is not registered in this world (id {})", id.value);
        return false;
    }
    ErasedStorage& s = *reg->storage;
    const SceneEntity src = toEntt(from);
    const SceneEntity dst = toEntt(to);
    if (!s.contains(src)) {
        return false;  // SILENT: a miss is the normal answer in a copy-everything loop
    }
    if (src == dst) {
        return true;  // self-copy: nothing to do -- and the remove below would destroy the very
    }  // element the push is about to read (E5)
    if (reg->emptyType) {
        // A tag storage has no payload at all (page_size == 0 at the pinned entt), so value() must
        // never be called on it. Presence is the whole state.
        if (!s.contains(dst)) {
            s.push(dst, nullptr);
        }
        return true;  // E6
    }
    // REMOVE FIRST -- this is D17, and it is the whole reason this function is a World member.
    // remove() swap-and-pops: it MOVES the storage's last element into the erased slot and destroys
    // the old last, so it can RELOCATE the element at `src`. push() only grows the paged payload,
    // which allocates a new page and never relocates an existing one. Reading value(src) before the
    // remove would hand push() a pointer to a moved-from, destroyed element whenever the removal
    // relocated the source (F10). Do not "simplify" these three statements into two.
    if (s.contains(dst)) {
        s.remove(dst);
    }
    s.push(dst, s.value(src));  // value(src) is read AFTER the only relocating call
    // Belt-and-braces behind add<T>()'s static_assert, exactly like addRaw's M5: the type-erased
    // insert silently stores nothing for a non-copy-constructible element.
    return s.contains(dst);
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

// ---- clone (task 4.7.2) -------------------------------------------------------------------------

namespace {

// Every element of `from`, in packed order, copy-constructed into the empty pool `to` (pushing in packed order
// reproduces the packed array, so each/view order matches). A tag pool stores no value, so value() is never called on
// one -- copyComponent's rule.
void copyPool(const ErasedStorage& from, ErasedStorage& to, bool emptyType) {
    for (std::size_t i = 0; i < from.size(); ++i) {
        const SceneEntity entity = from.data()[i];
        to.push(entity, emptyType ? nullptr : from.value(entity));
    }
}

}  // namespace

World World::clone() const {
    AERO_PROFILE_ZONE;
    World copy;  // a live World, whose Impl is replaced below so the source's table is replayed in its own order
    if (impl == nullptr) {
        AERO_LOG_ERROR("scene: {} on a moved-from World", "clone");
        copy.impl.reset();  // a moved-from World clones to a moved-from World
        return copy;
    }
    copy.impl = std::make_unique<Impl>();
    Impl& dst = *copy.impl;

    // 1. The entity pool. Every packed entry -- live identifiers and each free slot's stored version alike -- in packed
    //    order, then the live count. A fresh pool keeps everything live while it is replayed, so each entry lands at
    //    the back and the packed order is the source's. No start_from: the first new index is found by the pool's own
    //    walk, which needs no assumption about holes.
    auto& sourceEntities = impl->registry.storage<SceneEntity>();
    auto& copyEntities = dst.registry.storage<SceneEntity>();
    copyEntities.reserve(sourceEntities.size());
    for (std::size_t i = 0; i < sourceEntities.size(); ++i) {
        copyEntities.generate(sourceEntities.data()[i]);
    }
    copyEntities.free_list(sourceEntities.free_list());

    // 2. The registration table, in order: each pool made by the recipe that made the source's, filled in packed
    //    order. Appended directly, never through bindRegistration, so a table with a shared name replays silently.
    for (const Impl::Registration& reg : impl->registrations) {
        ErasedStorage* const pool =
            reg.makeStorage != nullptr ? &reg.makeStorage(dst.registry) : &dst.makeRuntimePool(reg.ops);
        dst.registrations.push_back(
            Impl::Registration{reg.name, pool, reg.id, reg.emptyType, reg.makeStorage, reg.ops});
        dst.byId.emplace(reg.id.value, dst.registrations.size() - 1U);
        copyPool(*reg.storage, *pool, reg.emptyType);
    }

    // 3. The hierarchy and the names: never registered, already made by Impl's own initialisers.
    copyPool(*impl->hierarchy, *dst.hierarchy, false);
    copyPool(*impl->names, *dst.names, false);
    return copy;
}

// ---- the component-type registry -------------------------------------------------------------

ComponentTypeId World::registerComponentRaw(ComponentTypeId id, std::string_view name,
                                            const scene::detail::ComponentOps& ops) {
    if (impl == nullptr || !id.valid()) {
        return {};
    }
    // Registered already: bindRegistration keeps its idempotence and its WARN, and no second pool is made.
    if (const Impl::Registration* existing = impl->find(id); existing != nullptr) {
        return impl->bindRegistration(id, name, existing->storage, existing->emptyType, existing->makeStorage,
                                      existing->ops);
    }
    RuntimeStorage& storage = impl->makeRuntimePool(ops);
    return impl->bindRegistration(id, name, &storage, ops.size == 0, nullptr, ops);
}

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

ComponentTypeId World::componentTypeAt(std::size_t index) const noexcept {
    if (impl == nullptr || index >= impl->registrations.size()) {
        return {};
    }
    // deque::operator[] is O(1); registrations is never reordered, only appended (see bind()).
    return impl->registrations[index].id;
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
                                  bool emptyType, StorageFactory makeStorage) {
    if (world.impl == nullptr || !id.valid() || storage == nullptr || makeStorage == nullptr) {
        return {};
    }
    return world.impl->bindRegistration(id, name, storage, emptyType, makeStorage, scene::detail::ComponentOps{});
}

}  // namespace scene::internal
}  // namespace engine
