// editor/src/asset_commands.cpp -- task 3.1.5: InstantiateAssetCommand, the sixth structural command
// and the first that creates more than one entity. Built ON TOP of entity_commands.hpp's two promoted
// helpers, never beside a second copy of them.
#include <aero/editor/asset_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/world.hpp>

#include <cassert>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

InstantiateAssetCommand::InstantiateAssetCommand(InstantiatePlan planIn, Entity parent, const Transform& placement,
                                                 std::span<const Entity> selBefore)
    : plan(std::move(planIn)),
      parentTarget(parent),
      rootPlacement(placement),
      selectionBefore(selBefore.begin(), selBefore.end()) {}

bool InstantiateAssetCommand::redo(CommandContext& context) {
    if (rootEntity.valid()) {
        // Every LATER redo restores the snapshot ITS OWN undo took (the D21 shape): re-creating would
        // mint fresh handles every cycle, so a rename or a transform edit pushed after this command
        // would stop naming a live entity after one full undo -> redo (S10).
        return restoreStructuralState(context, state, std::span<const Entity>{&rootEntity, 1});
    }
    if (plan.nodes.empty()) {
        return false;  // a refused plan is never pushed; if one is, nothing is created
    }

    // Slot order IS creation order -- the planner's BFS guarantees parentSlot < i for every i > 0 --
    // so one forward pass suffices and there is no fixup phase.
    std::vector<Entity> created;
    created.reserve(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const InstantiatePlanNode& planned = plan.nodes[i];
        assert((i == 0 || planned.parentSlot < i) && "instantiate plan is not parents-before-children");
        const Entity parent = (i == 0) ? parentTarget : created[planned.parentSlot];
        const Entity entity = createEntity(context.world, parent, planned.name);
        if (!entity.valid()) {
            // PARTIAL CREATION IS FORBIDDEN (Command's own contract: "MUST NOT partially mutate on a
            // false return"). Destroy what this pass made and report. The span form is taken from the
            // vector itself so an empty `created` is a no-op rather than a &created[0] on nothing.
            destroyEntities(context.world, created);
            context.selection.prune(context.world);
            return false;
        }
        created.push_back(entity);

        // createEntity already installed a default Transform, so this SETS rather than adds -- one
        // fewer erase/insert per node, and it never touches an unregistered type.
        Transform placement;
        if (i == 0) {
            placement = rootPlacement;  // the drop decides the root; the plan's synthetic identity is ignored
        } else {
            placement.position = planned.translation;
            placement.rotation = planned.rotation;
            placement.scale = planned.scale;
        }
        if (auto* existing = context.world.get<Transform>(entity); existing != nullptr) {
            *existing = placement;
        } else {
            context.world.add<Transform>(entity, placement);
        }

        if (planned.mesh.valid()) {
            // primitive/color stay at their defaults, so an entity whose reference has not resolved
            // yet draws NOTHING rather than a stray cube -- which is what makes "selectable during
            // load" a correct picture and not a lie.
            context.world.add<MeshRenderer>(entity, MeshRenderer{.mesh = planned.mesh, .meshIndex = planned.meshIndex});
        }
    }

    rootEntity = created.front();
    // Set by the COMMAND and read back by the panel (the AC-22 precedent) -- no panel holds a pointer
    // across a push.
    context.selection.set(rootEntity);
    return true;
}

bool InstantiateAssetCommand::undo(CommandContext& context) {
    if (!rootEntity.valid()) {
        return false;
    }
    if (!captureAndDestroySubtrees(context, std::span<const Entity>{&rootEntity, 1}, state)) {
        return false;
    }
    context.selection.setAll(selectionBefore);
    context.selection.prune(context.world);
    return true;
}

std::string_view InstantiateAssetCommand::label() const noexcept { return INSTANTIATE_ASSET_COMMAND_LABEL; }

Entity InstantiateAssetCommand::createdRoot() const noexcept { return rootEntity; }

}  // namespace engine::editor
