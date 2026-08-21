// editor/src/entity_commands.cpp -- task 2.4.2: the five structural entity commands, wrapping
// entity_ops. Two helpers carry the hard part: create/delete/duplicate share ONE implementation
// (D21's three orientations of one mechanism). Task 3.1.5 PROMOTED both out of this file's anonymous
// namespace onto entity_commands.hpp, so the sixth structural command can call the same two.
#include <aero/core/log.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace engine::editor {

// Capture `roots`' subtrees AND their root display slots, then destroy them. Returns false --
// having changed NOTHING -- when the capture comes out empty, which is exactly "every target is
// already gone" (AC-23/A28). The selection is pruned AFTERWARDS, never before: prune() must not see
// a handle the destroy has not taken yet.
bool captureAndDestroySubtrees(CommandContext& ctx, std::span<const Entity> roots, StructuralUndoState& out) {
    if (!out.subtree.capture(ctx.world, roots)) {
        return false;
    }
    if (out.subtree.empty()) {
        return false;  // nothing destroyed, nothing pruned (A28)
    }
    out.rootSlots.clear();
    for (const Entity r : out.subtree.roots()) {
        out.rootSlots.push_back(ctx.roots.indexOf(r));  // NO_ROOT_SLOT for a non-root (E5)
    }
    // Capture BEFORE this destroy is the single most catastrophic ordering error in this task (S11):
    // destroying first would snapshot nothing.
    destroyEntities(ctx.world, out.subtree.roots());
    ctx.selection.prune(ctx.world);
    return true;
}

// The exact inverse, in the order that makes it invisible: World state first (identities, payloads,
// links, sibling order), then the root display order, then the selection. `selection` is what to
// INSTALL -- the created set on a redo, the pre-command set on an undo (A27).
bool restoreStructuralState(CommandContext& ctx, const StructuralUndoState& state, std::span<const Entity> selection) {
    if (!state.subtree.restore(ctx.world)) {
        return false;
    }
    const std::span<const Entity> restoredRoots = state.subtree.roots();
    // Insert ascending by recorded slot, NOT in restoredRoots/rootSlots order: RootOrder::insert
    // repositions one entity at a time (the same shape as placeAt), so restoring two roots in
    // descending slot order -- exactly what a bottom-up multi-select produces -- would shift an
    // untouched root sitting between them (code-review Gap 1, task 2.4.2). A stable sort by slot
    // alone is enough; this is the ONE flat list, so no per-parent grouping is needed.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < restoredRoots.size() && i < state.rootSlots.size(); ++i) {
        if (state.rootSlots[i] != NO_ROOT_SLOT && !ctx.world.parent(restoredRoots[i]).valid()) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, {}, [&state](std::size_t i) { return state.rootSlots[i]; });
    for (const std::size_t i : order) {
        ctx.roots.insert(restoredRoots[i], state.rootSlots[i]);
    }
    ctx.selection.setAll(selection);
    ctx.selection.prune(ctx.world);  // E14: the restored selection can only contain live handles
    return true;
}

// ---- CreateEntityCommand ------------------------------------------------------------------------

CreateEntityCommand::CreateEntityCommand(Entity parent, std::string_view name, std::span<const Entity> selBefore)
    : parentTarget(parent), createName(name), selectionBefore(selBefore.begin(), selBefore.end()) {}

bool CreateEntityCommand::redo(CommandContext& context) {
    if (!createdEntity.valid()) {
        createdEntity = createEntity(context.world, parentTarget, createName);
        if (!createdEntity.valid()) {
            return false;
        }
        context.selection.set(createdEntity);
        return true;
    }
    // Every LATER redo restores the snapshot ITS OWN undo took (D21) -- calling createEntity twice
    // would silently re-point the identity every cycle (S16).
    return restoreStructuralState(context, state, std::span<const Entity>{&createdEntity, 1});
}

bool CreateEntityCommand::undo(CommandContext& context) {
    if (!createdEntity.valid()) {
        return false;
    }
    if (!captureAndDestroySubtrees(context, std::span<const Entity>{&createdEntity, 1}, state)) {
        return false;
    }
    context.selection.setAll(selectionBefore);
    context.selection.prune(context.world);
    return true;
}

std::string_view CreateEntityCommand::label() const noexcept { return CREATE_ENTITY_COMMAND_LABEL; }
Entity CreateEntityCommand::created() const noexcept { return createdEntity; }

// ---- DeleteEntitiesCommand ----------------------------------------------------------------------

DeleteEntitiesCommand::DeleteEntitiesCommand(std::span<const Entity> targetsIn, std::span<const Entity> selBefore)
    : targets(targetsIn.begin(), targetsIn.end()), selectionBefore(selBefore.begin(), selBefore.end()) {}

bool DeleteEntitiesCommand::redo(CommandContext& context) { return captureAndDestroySubtrees(context, targets, state); }

bool DeleteEntitiesCommand::undo(CommandContext& context) {
    return restoreStructuralState(context, state, selectionBefore);
}

std::string_view DeleteEntitiesCommand::label() const noexcept { return DELETE_ENTITIES_COMMAND_LABEL; }

// ---- DuplicateEntitiesCommand -------------------------------------------------------------------

DuplicateEntitiesCommand::DuplicateEntitiesCommand(std::span<const Entity> sourcesIn, std::span<const Entity> selBefore)
    : sources(sourcesIn.begin(), sourcesIn.end()), selectionBefore(selBefore.begin(), selBefore.end()) {}

bool DuplicateEntitiesCommand::redo(CommandContext& context) {
    if (createdEntities.empty()) {
        createdEntities = duplicateEntities(context.world, sources);
        if (createdEntities.empty()) {
            return false;
        }
        context.selection.setAll(createdEntities);
        return true;
    }
    // Every LATER redo restores the ORIGINAL copies' handles (D21/E12) -- calling duplicateEntities
    // twice would mint different handles every cycle.
    return restoreStructuralState(context, state, createdEntities);
}

bool DuplicateEntitiesCommand::undo(CommandContext& context) {
    if (createdEntities.empty()) {
        return false;
    }
    if (!captureAndDestroySubtrees(context, createdEntities, state)) {
        return false;
    }
    context.selection.setAll(selectionBefore);
    context.selection.prune(context.world);
    return true;
}

std::string_view DuplicateEntitiesCommand::label() const noexcept { return DUPLICATE_ENTITIES_COMMAND_LABEL; }

// ---- ReparentCommand -----------------------------------------------------------------------------

ReparentCommand::ReparentCommand(std::span<const Entity> targetsIn, Entity newParent)
    : targets(targetsIn.begin(), targetsIn.end()), newParentTarget(newParent) {}

bool ReparentCommand::redo(CommandContext& context) {
    // Captures the OLD parent and slot INSIDE redo, EVERY time (A19): a redo after an undo must
    // capture whatever the tree looks like NOW, not what it looked like at construction.
    //
    // TWO PASSES, deliberately not interleaved with the actual reparent (found while writing
    // code-review Gap 1's mandated shared-parent test, task 2.4.2): querying childIndexOf for a LATER
    // target after an EARLIER target sharing the same parent has already been detached reads a
    // list that has already lost one entry, silently shifting the later target's captured slot down
    // by one. Every old-parent/old-slot pair is captured against the UNCHANGED tree first; only then
    // does anything actually move.
    struct Pending {
        Entity entity;
        Entity oldParent;
        std::size_t oldSlot;
    };
    std::vector<Pending> pending;
    pending.reserve(targets.size());
    for (const Entity e : targets) {
        if (!context.world.alive(e)) {
            continue;
        }
        const Entity oldParent = context.world.parent(e);
        const std::size_t slot =
            oldParent.valid() ? childIndexOf(context.world, oldParent, e) : context.roots.indexOf(e);
        pending.push_back(Pending{e, oldParent, slot});
    }
    captured.clear();
    for (const Pending& p : pending) {
        if (!reparentEntity(context.world, p.entity, newParentTarget)) {
            continue;  // canReparent already refused; predicted, silent
        }
        captured.push_back(Move{p.entity, p.oldParent, p.oldSlot});
    }
    return !captured.empty();
}

bool ReparentCommand::undo(CommandContext& context) {
    // LOCAL, not a member: undo is human-paced, and a member would be per-command memory sitting idle
    // in a 128-entry history.
    std::vector<Entity> scratch;
    // Ascending by the recaptured OLD slot, NOT the reverse of capture order (code-review Gap 1, task
    // 2.4.2): placeAt/RootOrder::insert reposition one entity at a time, so replaying them out of
    // ascending-per-parent order corrupts an untouched sibling captured between two of them -- exactly
    // what an ordinary in-row-order multi-select reparent produces once reversed. The reverse walk
    // this replaced was only ever needed to undo NESTING (a parent before its own child), and
    // `targets` can never contain both: topMost()/reparentTargets() (D19) always collapse a selected
    // parent and its own selected child into one target before this command ever sees them.
    std::vector<std::size_t> order(captured.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, {}, [this](std::size_t i) { return captured[i].oldSlot; });

    bool any = false;
    for (const std::size_t idx : order) {
        const Move& move = captured[idx];
        if (!context.world.alive(move.entity)) {
            continue;
        }
        if (move.oldParent.valid() && !context.world.alive(move.oldParent)) {
            // D23's fail-soft path: leave it a root rather than fail the whole undo.
            AERO_LOG_WARN("editor: reparent undo left an entity at the root -- its previous parent is gone (index {})",
                          move.entity.index);
            context.world.setParent(move.entity, Entity{});
            any = true;
            continue;
        }
        context.world.setParent(move.entity, move.oldParent);
        if (move.oldParent.valid()) {
            placeAt(context.world, move.oldParent, move.entity, move.oldSlot, scratch);
        } else if (move.oldSlot != NO_ROOT_SLOT) {
            context.roots.insert(move.entity, move.oldSlot);
        }
        any = true;
    }
    return any;
}

std::string_view ReparentCommand::label() const noexcept { return REPARENT_COMMAND_LABEL; }

// ---- RenameEntityCommand -------------------------------------------------------------------------

RenameEntityCommand::RenameEntityCommand(Entity entity, std::string before, std::string after)
    : target(entity), beforeName(std::move(before)), afterName(std::move(after)) {}

bool RenameEntityCommand::redo(CommandContext& context) {
    if (!context.world.alive(target)) {
        return false;
    }
    return context.world.setName(target, afterName);
}

bool RenameEntityCommand::undo(CommandContext& context) {
    if (!context.world.alive(target)) {
        return false;
    }
    return context.world.setName(target, beforeName);
}

std::string_view RenameEntityCommand::label() const noexcept { return RENAME_COMMAND_LABEL; }

}  // namespace engine::editor
