#pragma once
// Aero Engine -- the structural undo payload (task 2.4.2): a detached copy of one or more entity
// SUBTREES, complete enough to put them back exactly as they were. PUBLIC, ImGui-FREE, entt-FREE and
// render-FREE by rule, held by FILE PLACEMENT (R12) like every header under editor/include.
//
// HOW THE PAYLOAD IS STORED, and why it is not what you would guess: component values live in a
// privately owned engine::World, hidden behind the pimpl. A World is the only thing in the tree that
// can hold a TYPED value behind a runtime ComponentTypeId, and World::addRaw/getRaw copy-CONSTRUCT
// across two Worlds (world.cpp:398-442) -- so this needs no reflection, no new engine API, and it is
// correct for a component with a std::string member, not just for PODs. Every World registers the
// built-ins in its CONSTRUCTOR (transform.hpp:76-83), so the private World is registration-complete
// by construction. Contrast the entt::meta route, which would lose every payload under
// -DAERO_REFLECT_TOOLS=OFF -- a configuration in which the editor, and therefore its undo, must work.
//
// IDENTITY IS THE POINT. restore() puts every entity back under its ORIGINAL handle via
// World::recreate (task 2.4.2 D2). Anything less and every older command in the history that names a
// restored entity is silently stranded.

#include <aero/editor/entity_ops.hpp>  // NO_ROOT_SLOT (A6)
#include <aero/scene/component.hpp>    // engine::ComponentTypeId
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// A detached copy of N entity subtrees: handles, names, parent links, child order, sibling positions
// and every registered component's value.
class SubtreeSnapshot {
public:
    SubtreeSnapshot() noexcept;  // empty; allocates nothing until the first capture
    ~SubtreeSnapshot();
    SubtreeSnapshot(SubtreeSnapshot&&) noexcept;
    SubtreeSnapshot& operator=(SubtreeSnapshot&&) noexcept;
    SubtreeSnapshot(const SubtreeSnapshot&) = delete;
    SubtreeSnapshot& operator=(const SubtreeSnapshot&) = delete;

    // Replaces any previous contents. `roots` is topMost()-filtered first, so passing a parent and
    // one of its own children captures ONE subtree, never that child twice. Dead/null handles are
    // dropped. An empty (or wholly dead) input yields an empty snapshot and returns true -- "nothing
    // to capture" is a legal outcome, not a failure.
    bool capture(const World& world, std::span<const Entity> roots);

    // Puts everything back, into the World it came from. Two phases (D8): every handle is recreated
    // first, and if ANY of them fails, every entity this call created is destroyed and it returns
    // false having written nothing else. Phase two -- names, components, links, sibling order -- can
    // only fail on allocation.
    //
    // The snapshot is NOT consumed: restoring twice into a World where the first restore still lives
    // simply fails the phase-one check (the handles are alive), which is what makes a redo/undo cycle
    // safe rather than clever.
    [[nodiscard]] bool restore(World& world) const;

    // NOEXCEPT: drops the private World wholesale (impl.reset()) rather than clearing and reusing it
    // -- constructing a fresh World allocates and can throw, so clear() must not do that (A12).
    // capture() lazily std::make_unique<Impl>()s when there is something to hold.
    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t entityCount() const noexcept;  // total entities, not roots
    // The captured subtree roots, in input order (post-topMost). This is what a command re-selects
    // and what it looks up in RootOrder -- the ONE query the commands need, so it is the ONE query
    // this class offers. Nothing exposes a Record: the internal layout is the pimpl's business.
    [[nodiscard]] std::span<const Entity> roots() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;  // null == empty
};

// A13: the pimpl's out-of-line special members are MANDATORY, not stylistic -- Impl is incomplete in
// this header, so a defaulted destructor/move HERE would fail to compile the moment a caller
// instantiates the type. Both are `= default` in scene_snapshot.cpp instead, where Impl is complete.
// std::unique_ptr's own move is noexcept, so both hold; they fail loudly if a future member breaks it.
static_assert(std::is_nothrow_move_constructible_v<SubtreeSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<SubtreeSnapshot>);

// One component's value, detached from its entity -- what RemoveComponentCommand must hold so that
// undoing a remove gives back the FIELDS, not a default-constructed shell (D18). Same private-World
// mechanism, same reflection-independence.
class ComponentSnapshot {
public:
    ComponentSnapshot() noexcept;
    ~ComponentSnapshot();
    ComponentSnapshot(ComponentSnapshot&&) noexcept;
    ComponentSnapshot& operator=(ComponentSnapshot&&) noexcept;
    ComponentSnapshot(const ComponentSnapshot&) = delete;
    ComponentSnapshot& operator=(const ComponentSnapshot&) = delete;

    // False (and empty) when the entity is dead, the id unregistered (on `world`, or -- same
    // reflection-independence as SubtreeSnapshot's D24 -- unmirrorable into the private snapshot
    // World), or the component absent.
    bool capture(const World& world, Entity entity, ComponentTypeId id);
    // Writes the captured value onto `entity` (which need not be the entity it was captured from),
    // replacing any component of that type already there. False when empty, or when the write did not
    // take. A tag round-trips by presence (F8).
    [[nodiscard]] bool restore(World& world, Entity entity) const;

    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] ComponentTypeId type() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

static_assert(std::is_nothrow_move_constructible_v<ComponentSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<ComponentSnapshot>);

// The position of `child` among `parent`'s children in eachChild order, or NO_ROOT_SLOT when it is
// not one of them. Reading that order is explicitly blessed (world.hpp:157-163); a sibling INDEX is
// not an engine concept, which is exactly why this lives here and not on World (D9/A5).
[[nodiscard]] std::size_t childIndexOf(const World& world, Entity parent, Entity child);

// `child` has just been APPENDED to `parent` (setParent's own semantic, world.hpp:138-140). Move it
// to `index` -- CLAMPED to the current child count (D25) -- by detaching and re-appending every
// sibling that must follow it. `scratch` is caller-owned so a restore of N children allocates once.
//
// TWO CALLERS, WHICH IS WHY IT IS DECLARED HERE RATHER THAN BEING FILE-LOCAL: SubtreeSnapshot::
// restore and ReparentCommand::undo. A third caller under editor/src that is NOT a command (a
// Hierarchy drag-to-reorder) is the trigger to promote this into entity_ops (handoff H6).
void placeAt(World& world, Entity parent, Entity child, std::size_t index, std::vector<Entity>& scratch);

}  // namespace engine::editor
