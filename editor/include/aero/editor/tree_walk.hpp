#pragma once
// Aero Engine — the Hierarchy panel's pure explicit-stack forest walk (task 2.2.1, review round 2,
// Gap 1). Extracted out of hierarchy_panel.cpp's drawTree so the traversal's own invariants -- each
// node visited exactly once, roots visited front-to-back, and the child arena returning to its
// pre-call size (I4) -- are provable by a tier-0 test with no ImGui context. Nothing in this file
// ever draws or reads back an ImGui state; the caller's `enter`/`unwind` callbacks own presentation.
//
// ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9). ENGINE-typed only.

#include <aero/scene/entity.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// One node of the explicit walk stack (D14): `childBegin/childNext/childEnd` index into the
// caller-owned `childArena` passed to walkForest().
struct TreeWalkEntry {
    Entity entity{};
    std::size_t childBegin = 0;
    std::size_t childNext = 0;
    std::size_t childEnd = 0;
    bool open = false;     // `enter` asked to descend -> `unwind` is owed the matching close (I3)
    bool entered = false;  // `enter` has already run for this node
};

// Walks `roots` and their descendants depth-first, over an EXPLICIT stack -- NEVER recursion
// (misc-no-recursion is --warnings-as-errors on the Linux lane, D13/I7) -- calling, for each node
// exactly once each:
//   enter(entity)          -> whether to descend into its children (ImGui's TreeNodeEx return value
//                              in production; the panel's drawRow)
//   unwind(entity, open)      called once the node (and, if `open`, every one of its children) has
//                              been fully visited, `open` echoing what `enter` returned -- the ONE
//                              place a caller owes ImGui a matching TreePop/PopID (I3)
// Roots are visited front-to-back (`roots[0]` first) and each node's children in
// `World::eachChild` order (attach order). `stack`/`childArena` are caller-owned scratch, reused
// across calls with zero allocation once warm (D14); `childArena` returns to exactly the size it
// had when this call began, once every node it pushed has unwound (I4) -- a caller nested inside a
// larger per-frame arena can rely on that. This function itself never mutates `world` (I1); it only
// calls the read-only `World::eachChild` (F6).
void walkForest(const World& world, std::span<const Entity> roots, std::vector<TreeWalkEntry>& stack,
                std::vector<Entity>& childArena, const std::function<bool(Entity)>& enter,
                const std::function<void(Entity, bool)>& unwind);

}  // namespace engine::editor
