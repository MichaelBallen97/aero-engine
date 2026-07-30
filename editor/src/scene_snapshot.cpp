// editor/src/scene_snapshot.cpp -- task 2.4.2: the detached-subtree structural-undo payload. This TU
// includes <aero/scene/world.hpp> and nothing third-party -- it is entt-free: the private World is
// the abstraction. Every capture/restore reaches its payload through World::addRaw/getRaw, never
// entt::meta, which is what makes AC-8 ("works with -DAERO_REFLECT_TOOLS=OFF") hold by construction.
#include <aero/core/log.hpp>
#include <aero/editor/scene_snapshot.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {

// ---- SubtreeSnapshot::Impl -----------------------------------------------------------------------

struct SubtreeSnapshot::Impl {
    // One captured entity. Pre-order: a parent always precedes its children, which is what lets
    // restore() create and link in a single pass.
    struct Record {
        Entity handle;                 // the LIVE-world handle, restored exactly
        Entity parent;                 // the LIVE-world parent at capture time; Entity{} == root
        Entity shadow;                 // where this entity's components live inside `store`
        std::size_t siblingIndex = 0;  // position among parent's children (or among roots) at capture
        bool subtreeRoot = false;      // true == its parent is OUTSIDE this snapshot
        std::string name;              // COPIED: World::name()'s view dangles on the next mutation
    };
    World store;                   // the private payload World (D6). Registration-complete (F10).
    std::vector<Entity> rootList;  // records[i].subtreeRoot entries, in INPUT order (post-topMost)
    std::vector<Record> records;   // pre-order
    std::vector<Entity> scratch;   // the capture walk's children buffer AND restore's placeAt buffer
};

SubtreeSnapshot::SubtreeSnapshot() noexcept = default;
SubtreeSnapshot::~SubtreeSnapshot() = default;
SubtreeSnapshot::SubtreeSnapshot(SubtreeSnapshot&&) noexcept = default;
SubtreeSnapshot& SubtreeSnapshot::operator=(SubtreeSnapshot&&) noexcept = default;

namespace {

// One pending node in the capture walk's explicit stack (misc-no-recursion is --warnings-as-errors --
// entity_ops.cpp's duplicateEntities is the tree's own precedent for this shape).
struct PendingCapture {
    Entity entity;
    Entity parent;  // the LIVE-world parent, valid or not
    bool subtreeRoot = false;
};

}  // namespace

bool SubtreeSnapshot::capture(const World& world, std::span<const Entity> roots) {
    clear();
    const std::vector<Entity> filtered = topMost(world, roots);  // dead handles + inner descendants drop
    if (filtered.empty()) {
        return true;  // "nothing to capture" is a legal, empty snapshot
    }
    impl = std::make_unique<Impl>();
    impl->rootList = filtered;

    // A15: the D24 WARN is deduped PER TYPE, per capture -- a subtree with three entities carrying the
    // same unregistered component type must log exactly one WARN, not three.
    std::vector<ComponentTypeId> warned;
    std::vector<PendingCapture> stack;
    std::vector<Entity> children;  // reused per node; pushed back-to-front (A2)

    for (const Entity root : filtered) {
        stack.push_back(PendingCapture{root, world.parent(root), true});
        while (!stack.empty()) {
            const PendingCapture job = stack.back();  // BY VALUE -- the push below would dangle a ref
            stack.pop_back();

            const Entity shadow = impl->store.create();
            Impl::Record record;
            record.handle = job.entity;
            record.parent = job.parent;
            record.shadow = shadow;
            record.subtreeRoot = job.subtreeRoot;
            // A world root's display slot is the COMMAND's business, not the snapshot's (D10's
            // layering): a root with no parent always records siblingIndex 0.
            record.siblingIndex = job.parent.valid() ? childIndexOf(world, job.parent, job.entity) : 0;
            record.name = std::string(world.name(job.entity));  // COPIED -- the view dies on mutation

            for (std::size_t i = 0; i < world.componentTypeCount(); ++i) {
                const ComponentTypeId id = world.componentTypeAt(i);
                if (!world.hasRaw(id, job.entity)) {
                    continue;
                }
                if (!impl->store.registered(id)) {
                    if (std::find(warned.begin(), warned.end(), id) == warned.end()) {
                        warned.push_back(id);
                        AERO_LOG_WARN("editor: snapshot skipped an unregistered component type '{}'",
                                      world.componentTypeName(id));
                    }
                    continue;
                }
                impl->store.addRaw(id, shadow, world.getRaw(id, job.entity));
                // The non-copy-constructible belt (M5's precedent, world.cpp:461-465): a type that
                // is registered but did not actually take gets its own WARN rather than a silent gap.
                if (!impl->store.hasRaw(id, shadow)) {
                    AERO_LOG_WARN("editor: snapshot could not mirror component '{}' -- it did not take",
                                  world.componentTypeName(id));
                }
            }

            impl->records.push_back(std::move(record));

            // READ-ONLY inside eachChild (world.hpp's own contract): only appends to a local vector.
            children.clear();
            world.eachChild(job.entity, [&children](Entity c) { children.push_back(c); });
            // Push back-to-front so the LIFO stack pops them in eachChild ORDER (A2 -- the
            // duplicateEntities precedent, entity_ops.cpp:174-178).
            for (std::size_t i = children.size(); i > 0; --i) {
                stack.push_back(PendingCapture{children[i - 1U], job.entity, false});
            }
        }
    }
    return true;
}

bool SubtreeSnapshot::restore(World& world) const {
    if (impl == nullptr) {
        return true;  // N8/A12: nothing to restore is a legal outcome
    }
    // PHASE A -- identity (D8). The only phase that can fail. If any handle cannot be recreated, every
    // entity THIS CALL created is destroyed, in reverse, and nothing else has been written yet.
    std::size_t created = 0;
    for (const Impl::Record& record : impl->records) {
        if (world.recreate(record.handle) != record.handle) {
            for (std::size_t i = created; i > 0; --i) {
                world.destroy(impl->records[i - 1U].handle);
            }
            return false;
        }
        ++created;
    }

    // PHASE B -- payload, names, links, sibling order (cannot fail except on allocation). THREE
    // PASSES, in THIS order (A3, extended by the code-review round's Gap 1): names + components for
    // every record; THEN links for every record (pre-order guarantees a parent already exists before
    // its children are linked); THEN position-restore for the subtree roots that need one, ORDERED
    // ascending by recorded slot rather than records order.
    //
    // Why a third pass, and why sorted: placeAt repositions ONE entity at a time by nudging every
    // sibling that must follow it, which is only equivalent to "put the whole list back" if entries
    // are placed in ASCENDING recorded-slot order per parent. `impl->records` order is capture order
    // (the caller's target/selection order for a subtreeRoot), which a descending selection (e.g.
    // Ctrl-clicking a lower sibling before a higher one) makes descending too -- and placing a lower
    // slot AFTER a higher one shifts an untouched sibling that sits between them. A stable sort by
    // siblingIndex alone (no grouping by parent -- Entity has no operator<, G5) is sufficient: any
    // one parent's own entries are a subsequence of the sorted whole, and a subsequence of a sorted
    // sequence is itself sorted.
    for (const Impl::Record& record : impl->records) {
        if (!record.name.empty()) {
            world.setName(record.handle, record.name);
        }
        for (std::size_t i = 0; i < impl->store.componentTypeCount(); ++i) {
            const ComponentTypeId id = impl->store.componentTypeAt(i);
            if (impl->store.hasRaw(id, record.shadow)) {
                world.addRaw(id, record.handle, impl->store.getRaw(id, record.shadow));
            }
        }
    }
    std::vector<const Impl::Record*> toPlace;
    for (const Impl::Record& record : impl->records) {
        if (!record.subtreeRoot) {
            // Its parent is INSIDE this snapshot, already recreated above (pre-order): setParent
            // APPENDS, and every sibling of one parent is linked in the same relative order it was
            // captured in (A2), so the captured order reproduces with no extra reorder needed.
            world.setParent(record.handle, record.parent);
        } else if (record.parent.valid()) {
            if (!world.alive(record.parent)) {
                // D23's fail-soft path: leave it a root rather than fail the whole restore.
                AERO_LOG_WARN("editor: snapshot restored an entity as a root -- its parent is gone (index {})",
                              record.handle.index);
            } else {
                world.setParent(record.handle, record.parent);  // appends, temporarily last
                toPlace.push_back(&record);                     // positioned in the sorted pass below
            }
        }
        // else: it was already a world root (record.parent invalid) -- nothing to link.
    }
    std::ranges::stable_sort(toPlace, {}, [](const Impl::Record* r) { return r->siblingIndex; });
    for (const Impl::Record* record : toPlace) {
        placeAt(world, record->parent, record->handle, record->siblingIndex, impl->scratch);
    }
    return true;
}

void SubtreeSnapshot::clear() noexcept { impl.reset(); }

bool SubtreeSnapshot::empty() const noexcept { return impl == nullptr || impl->records.empty(); }

std::size_t SubtreeSnapshot::entityCount() const noexcept { return impl == nullptr ? 0 : impl->records.size(); }

std::span<const Entity> SubtreeSnapshot::roots() const noexcept {
    return impl == nullptr ? std::span<const Entity>{} : std::span<const Entity>{impl->rootList};
}

// ---- ComponentSnapshot::Impl ---------------------------------------------------------------------

struct ComponentSnapshot::Impl {
    World store;  // the degenerate case: ONE store entity holds the whole payload
    Entity shadow{};
    ComponentTypeId typeId{};
};

ComponentSnapshot::ComponentSnapshot() noexcept = default;
ComponentSnapshot::~ComponentSnapshot() = default;
ComponentSnapshot::ComponentSnapshot(ComponentSnapshot&&) noexcept = default;
ComponentSnapshot& ComponentSnapshot::operator=(ComponentSnapshot&&) noexcept = default;

bool ComponentSnapshot::capture(const World& world, Entity entity, ComponentTypeId id) {
    clear();
    if (!world.hasRaw(id, entity)) {
        return false;  // dead entity, unregistered id, or the component genuinely absent -- one refusal
    }
    auto candidate = std::make_unique<Impl>();
    if (!candidate->store.registered(id)) {
        // The single-component sibling of D24: a type the private store does not know cannot be
        // mirrored (F10). Silent -- the caller (RemoveComponentCommand) reads this as "refuse rather
        // than silently lose data it cannot restore" (D18), never as an error of its own.
        return false;
    }
    candidate->typeId = id;
    candidate->shadow = candidate->store.create();
    candidate->store.addRaw(id, candidate->shadow, world.getRaw(id, entity));
    if (!candidate->store.hasRaw(id, candidate->shadow)) {
        return false;  // the non-copy-constructible belt (M5's precedent)
    }
    impl = std::move(candidate);
    return true;
}

bool ComponentSnapshot::restore(World& world, Entity entity) const {
    if (impl == nullptr || !world.alive(entity)) {
        return false;
    }
    world.addRaw(impl->typeId, entity, impl->store.getRaw(impl->typeId, impl->shadow));
    return world.hasRaw(impl->typeId, entity);  // AFTERWARDS: a tag's addRaw returns nullptr on success too
}

void ComponentSnapshot::clear() noexcept { impl.reset(); }

bool ComponentSnapshot::empty() const noexcept { return impl == nullptr; }

ComponentTypeId ComponentSnapshot::type() const noexcept { return impl == nullptr ? ComponentTypeId{} : impl->typeId; }

// ---- placeAt / childIndexOf (A6: public -- entity_commands.cpp is a second caller) ----------------

std::size_t childIndexOf(const World& world, Entity parent, Entity child) {
    std::size_t index = 0;
    std::size_t result = NO_ROOT_SLOT;
    // eachChild visits every child unconditionally (no early exit in its contract); the flag just
    // ignores anything after the first match, which is fine at these single-to-double-digit sizes (G5).
    world.eachChild(parent, [&](Entity c) {
        if (c == child && result == NO_ROOT_SLOT) {
            result = index;
        }
        ++index;
    });
    return result;
}

// The caller's contract is that `child` was just APPENDED, so it is expected to be `scratch.back()`
// -- but a failed append (the setParent that should have put it there refused, e.g. a live cycle
// check) leaves `scratch` NOT containing `child` at all once the parent already has >=2 real
// children, and the loop below would then detach and re-append genuine siblings for no reason:
// spurious reordering on a pure failure path. Verifying `scratch.back() == child` first is what
// makes that unreachable rather than merely unlikely -- unreachable TODAY only because LIFO undo
// means the sole refusal (a cycle) cannot be live at undo time, per this function's own comment.
void placeAt(World& world, Entity parent, Entity child, std::size_t index, std::vector<Entity>& scratch) {
    scratch.clear();
    world.eachChild(parent, [&scratch](Entity c) { scratch.push_back(c); });  // child is currently last
    if (scratch.empty() || scratch.back() != child) {
        return;  // the append never took -- nothing to reposition, and nothing else may be touched
    }
    if (scratch.size() < 2U) {
        return;  // exactly 1 child (already placed)
    }
    const std::size_t target = std::min(index, scratch.size() - 1U);  // D25: clamp, never assert
    if (target + 1U >= scratch.size()) {
        return;  // it belongs last -- already there
    }
    for (std::size_t i = target; i + 1U < scratch.size(); ++i) {  // every sibling that must follow it
        const Entity sibling = scratch[i];
        world.setParent(sibling, Entity{});  // detach to root...
        world.setParent(sibling, parent);    // ...and re-append, now after `child`
    }
}

}  // namespace engine::editor
