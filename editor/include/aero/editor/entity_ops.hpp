#pragma once
// Aero Engine — the pure structural entity operations the Hierarchy panel drives (task 2.2.1, D15).
// ImGui-FREE, engine-typed, and PUBLIC precisely so 2.4.2's CreateEntityCommand / DeleteEntityCommand
// / ReparentCommand / DuplicateCommand can CALL these functions rather than re-derive them -- this is
// the seam the command stack wraps (D15/D16). A public home is also what lets the tier-0 test cover
// them with no src include path.

#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// Not a root, or not tracked. Deliberately not spelled npos (that name belongs to std::string).
inline constexpr std::size_t NO_ROOT_SLOT = static_cast<std::size_t>(-1);

// ---- queries ---------------------------------------------------------------------------------

// True iff `candidate` IS `root` or lies anywhere in root's subtree. Walks candidate's parent chain
// upward -- O(depth), no allocation, ITERATIVE (D13). Entity{} on either side yields false.
//
// NOTE THE DIRECTION: isDescendantOf(world, candidate, root) means "is `candidate` inside `root`'s
// subtree", and it walks CANDIDATE's ancestors looking for `root`. Self counts (candidate == root ->
// true) -- that is what makes topMost() and canReparent() both correct with one function.
[[nodiscard]] bool isDescendantOf(const World& world, Entity candidate, Entity root);

// `entities`, minus every entry that has an ancestor also in `entities`, minus dead/null handles.
// Input order preserved. This is what stops duplicate from copying a child twice and delete from
// chasing handles its own subtree destroy already invalidated (D19).
[[nodiscard]] std::vector<Entity> topMost(const World& world, std::span<const Entity> entities);

// The label a UI shows: the entity's name, or "Entity <index>" when unnamed (D21). Writes into
// `out` (cleared first) so a per-frame draw loop allocates at most once.
void entityLabel(const World& world, Entity entity, std::string& out);

// ---- mutations (2.4.2 wraps these; it does not re-derive them, D15/D16) -----------------------

// A fresh entity with a default Transform, optionally under `parent`, optionally named.
// Entity{} on failure (a moved-from World, or a dead `parent`).
Entity createEntity(World& world, Entity parent = {}, std::string_view name = {});

// Resolves the parent a context-menu *Create Child* should use, per Section O-2's consistency
// extension (review round 2, Gap 5): a right-click on a row OUTSIDE the current `selection` acts on
// that row alone -- the same rule Delete and Duplicate already follow -- so it becomes the parent
// directly; a right-click INSIDE the selection, or CreateChild invoked with no specific row
// (`rightClicked` invalid), falls back to `primary`. This deliberately overrides AC-11's literal
// "creates under the primary" wording in favour of that later, more general rule.
[[nodiscard]] Entity resolveCreateChildParent(std::span<const Entity> selection, Entity primary, Entity rightClicked);

// Destroys `entities` -- each with its whole subtree (World::destroy's own semantic). Filters
// through topMost() first, then skips anything an earlier subtree already took. Returns the number
// of subtree ROOTS destroyed.
std::size_t destroyEntities(World& world, std::span<const Entity> entities);

// Deep-copies each top-most entry: its whole subtree, every REGISTERED component type (walked via
// World::componentTypeAt -- so a component type that does not exist yet is copied too, D4), its
// name, and its internal parent/child structure. Each copy is created as a SIBLING of its source
// (same parent). Returns the new subtree roots, in input order.
[[nodiscard]] std::vector<Entity> duplicateEntities(World& world, std::span<const Entity> entities);

// True iff World::setParent(child, parent) would succeed -- the PRE-CHECK a drag-drop target needs,
// so an illegal drop is never offered rather than being attempted and logged (E14/AC-15).
[[nodiscard]] bool canReparent(const World& world, Entity child, Entity parent);

// setParent, guarded by canReparent. Never logs on a refusal it can predict.
bool reparentEntity(World& world, Entity child, Entity parent);

// The set of entities a drag-drop reparent should actually move, given the CURRENT `selection` and
// the entity the user physically dragged: the whole selection when `dragged` is inside it (E16),
// otherwise `dragged` alone. ALWAYS filtered through topMost() -- so a selected parent dragged
// together with one of its own selected children moves as ONE subtree, never split into its parts
// (D19's consistency extension to the drag-drop path; review round 2, Gap 4). Dead/null handles are
// dropped (topMost's own contract); order preserved.
[[nodiscard]] std::vector<Entity> reparentTargets(const World& world, std::span<const Entity> selection,
                                                  Entity dragged);

// The default new-scene contents (D9). APPENDS to `world` -- clear() it first for a true "new
// scene" (E32). Reused verbatim by 2.5.1's File > New Scene.
//   "Main Camera"       Transform{position {0, 1, 5}} + Camera{}
//   "Directional Light" Transform{rotation: -50 deg about X} + DirectionalLight{}
//   "Cube"              Transform{} + MeshRenderer{primitive = 0}
void seedDefaultScene(World& world);

// ---- root ordering (D10) ----------------------------------------------------------------------

// A stable display order for ROOT entities, which the World deliberately does not model (it orders
// a parent's children by attach order, but has no notion of order AMONG roots) and which
// World::eachEntity cannot supply: that walks the ECS packed array, which swap-and-pops on destroy,
// so a scanned list visibly reshuffles whenever any entity dies.
//
// reconcile() is a RECONCILIATION, not a subscription: it needs no notification and no cooperation
// from whatever changed the World, which is what makes it survive 2.5.1's load and 2.4.2's undo.
class RootOrder {
public:
    // Drops entries that are dead or no longer roots (preserving the order of the rest), then
    // APPENDS every root not already present, in World::eachEntity order. O(N + maxEntityIndex),
    // amortised zero allocation after the first few frames.
    void reconcile(const World& world);

    [[nodiscard]] std::span<const Entity> entities() const noexcept;
    void clear() noexcept;

    // Where `entity` currently sits in the display order, or NO_ROOT_SLOT. What a structural command
    // captures BEFORE destroying a root, so its undo can put the row back where it was (task 2.4.2).
    [[nodiscard]] std::size_t indexOf(Entity entity) const noexcept;

    // Re-inserts `entity` at `index`, CLAMPED to the current size (D25). A no-op for an invalid handle
    // or one already tracked. The counterpart of indexOf: reconcile() preserves the order of entries
    // it already knows (entity_ops.cpp:187-192), so an entry inserted here survives every later frame.
    void insert(Entity entity, std::size_t index);

private:
    std::vector<Entity> order;
    std::vector<std::uint32_t> stamp;  // Entity::index -> generation of the entry in `order`, else 0
    std::vector<Entity> scratch;
};

// EditorApp's move is `noexcept = default` (editor_app.hpp). Under P1286R2 a defaulted move with a
// throwing member is NOT deleted -- it terminates -- so the guarantee has to be asserted on the
// member type itself (2.4.1's equivalent asserts on CommandStack, applied here). These do NOT catch a
// future member whose move happens to be noexcept but semantically wrong -- only the noexcept-ness.
static_assert(std::is_nothrow_move_constructible_v<RootOrder>);
static_assert(std::is_nothrow_move_assignable_v<RootOrder>);

}  // namespace engine::editor
