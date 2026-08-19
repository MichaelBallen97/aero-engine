#pragma once
// Aero Engine -- the undoable asset-drop command (task 3.1.5). Pair 19. PUBLIC, ImGui-free and
// entt-free like every header under editor/include. It is the tree's SIXTH structural command and the
// FIRST that creates more than one entity, which is why task 3.1.5 promoted entity_commands.cpp's
// two helpers onto entity_commands.hpp rather than growing a second copy of "capture BEFORE destroy".
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_commands.hpp>  // StructuralUndoState + the two promoted helpers
#include <aero/editor/instantiate_plan.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/transform.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr std::string_view INSTANTIATE_ASSET_COMMAND_LABEL = "Instantiate Asset";

class InstantiateAssetCommand final : public Command {
public:
    // `plan` is MOVED IN and OWNED: a command may outlive the frame that built it by any number of
    // frames (command_stack.hpp), so it holds VALUES and HANDLES only. `parent` is Entity{} for a
    // scene-root drop. `rootPlacement` is the placement the DROP resolved -- the plan never carries
    // it, because the same plan is valid for a viewport drop (a world point) and a hierarchy-row drop
    // (local identity), and baking placement into the plan would make it surface-specific.
    //
    // `selectionBefore` is COPIED at construction, which is before push() applies anything (D5): it
    // must NOT be a span, because redo() mutates the very Selection a caller's span would point into.
    InstantiateAssetCommand(InstantiatePlan plan, Entity parent, const Transform& rootPlacement,
                            std::span<const Entity> selectionBefore);

    bool redo(CommandContext& context) override;  // 1st: create the subtree; later: restoreStructuralState
    bool undo(CommandContext& context) override;  // captureAndDestroySubtrees + restore the old selection
    [[nodiscard]] std::string_view label() const noexcept override;

    // Entity{} until the FIRST redo. The member behind it is named `rootEntity`, not `createdRoot_`:
    // members are plain camelBack with no trailing underscore, and on a collision with an accessor the
    // MEMBER takes the distinct name (AssetDatabase::records()/recordList,
    // SceneRenderer::bindings()/bindingTable).
    [[nodiscard]] Entity createdRoot() const noexcept;

private:
    InstantiatePlan plan;
    Entity parentTarget{};  // Entity{} == a scene-root drop
    Transform rootPlacement{};
    Entity rootEntity{};        // filled by the FIRST redo; the identity every later cycle restores
    StructuralUndoState state;  // filled by undo, consumed by every later redo (the D21 shape)
    std::vector<Entity> selectionBefore;
};

}  // namespace engine::editor
