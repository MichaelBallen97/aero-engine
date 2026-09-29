#pragma once
// Aero Engine -- the five structural entity commands (task 2.4.2): create, delete, duplicate,
// reparent, rename. PUBLIC, ImGui-free, entt-free like every editor/include header. CALLS entity_ops
// -- never re-derives it (D3/F21).

#include <aero/editor/command_stack.hpp>
#include <aero/editor/scene_snapshot.hpp>
#include <aero/scene/camera.hpp>  // task E.5.2: SeedComponent's alternatives, all public and entt-free
#include <aero/scene/entity.hpp>
#include <aero/scene/light.hpp>          // task E.5.2
#include <aero/scene/mesh_renderer.hpp>  // task E.5.2
#include <aero/scene/spot_light.hpp>     // task E.5.2
#include <aero/scene/transform.hpp>      // task E.5.2

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::editor {

// The full undo payload of a structural edit: the World state (subtree) plus the editor-side display
// order (rootSlots, PARALLEL to subtree.roots(), NO_ROOT_SLOT for a non-root). Two halves because they
// live in two places -- the World owns parent links, the editor owns the order among roots
// (entity_ops.hpp:91-98) -- and both must come back for an undo to be invisible.
struct StructuralUndoState {
    SubtreeSnapshot subtree;
    std::vector<std::size_t> rootSlots;
};

// The fixed labels (D14: "everything else uses a string literal"), exposed as constants so tests
// assert against them -- the TRANSFORM_COMMAND_LABEL precedent.
inline constexpr std::string_view CREATE_ENTITY_COMMAND_LABEL = "Create Entity";
inline constexpr std::string_view DELETE_ENTITIES_COMMAND_LABEL = "Delete Entity";  // AC-28's literal
inline constexpr std::string_view DUPLICATE_ENTITIES_COMMAND_LABEL = "Duplicate";
inline constexpr std::string_view REPARENT_COMMAND_LABEL = "Reparent";
inline constexpr std::string_view RENAME_COMMAND_LABEL = "Rename";

// ---- task E.5.2: the SEED a create can carry --------------------------------------------------------
// What a seeded create adds beyond createEntity. monostate == nothing beyond the Transform. Every
// alternative is a trivially copyable built-in, so a command holds its seed BY VALUE -- the "values and
// handles only" rule (command_stack.hpp). A new alternative is a new built-in someone wants a menu entry
// for: add it here and to create_menu.cpp's createSeed, nowhere else.
using SeedComponent = std::variant<std::monostate, MeshRenderer, DirectionalLight, PointLight, SpotLight, Camera>;

struct EntitySeed {
    std::string name;         // "" == unnamed (createEntity's own rule; the Hierarchy shows "Entity <index>")
    std::string label;        // the Edit menu's noun phrase, "Create Cube"; "" == CREATE_ENTITY_COMMAND_LABEL
    Transform transform{};    // written OVER createEntity's default Transform, never beside it
    SeedComponent component;  // added with a TYPED World::add<T> -- no entt::meta, so every configuration
};

// Capture `roots`' subtrees AND their root display slots, then destroy them. Returns false -- having
// changed NOTHING -- when the capture comes out empty. PROMOTED from entity_commands.cpp's anonymous
// namespace at task 3.1.5: InstantiateAssetCommand is the sixth structural command and needs the exact
// same two operations, and a second copy of "capture BEFORE destroy" is the S11 defect waiting to be
// re-introduced.
[[nodiscard]] bool captureAndDestroySubtrees(CommandContext& context, std::span<const Entity> roots,
                                             StructuralUndoState& out);
// The exact inverse, in the order that makes it invisible: World state, then root display order, then
// the selection. `selection` is what to INSTALL -- the created set on a redo, the pre-command set on an
// undo.
[[nodiscard]] bool restoreStructuralState(CommandContext& context, const StructuralUndoState& state,
                                          std::span<const Entity> selection);

class CreateEntityCommand final : public Command {
public:
    // `selectionBefore` is COPIED here, at construction -- which is before push() applies anything,
    // since D5 means the panel has not written yet. It must NOT be a span: redo() mutates the very
    // Selection the caller's span points into (A20).
    CreateEntityCommand(Entity parent, std::string_view name, std::span<const Entity> selectionBefore);
    // task E.5.2: the SEEDED create. The FIRST redo is createEntity plus the seed -- the Transform written
    // over the default one, the one component added TYPED; every LATER redo and every undo is the unseeded
    // form's own code (D21), because the snapshot already carries every component. The label is the seed's,
    // fixed here and never changed (the label contract, command_stack.hpp).
    CreateEntityCommand(Entity parent, EntitySeed seed, std::span<const Entity> selectionBefore);
    bool redo(CommandContext& context) override;  // 1st: createEntity; later: restoreStructuralState (D21)
    bool undo(CommandContext& context) override;  // captureAndDestroySubtrees + restore the old selection
    [[nodiscard]] std::string_view label() const noexcept override;
    [[nodiscard]] Entity created() const noexcept;  // Entity{} until the first redo

private:
    Entity parentTarget{};  // Entity{} == a root
    std::string createName;
    // task E.5.2: the seed. labelText "" == CREATE_ENTITY_COMMAND_LABEL; seedTransform is engaged iff the
    // command was seeded; seedComponent is monostate when unseeded, or seeded with no component.
    std::string labelText;
    std::optional<Transform> seedTransform;
    SeedComponent seedComponent;
    Entity createdEntity{};     // filled by the FIRST redo; the identity every later cycle restores
    StructuralUndoState state;  // filled by undo, consumed by every later redo (D21)
    std::vector<Entity> selectionBefore;
};

class DeleteEntitiesCommand final : public Command {
public:
    DeleteEntitiesCommand(std::span<const Entity> targets, std::span<const Entity> selectionBefore);
    bool redo(CommandContext& context) override;  // captureAndDestroySubtrees(targets)
    bool undo(CommandContext& context) override;  // restoreStructuralState(selectionBefore)
    [[nodiscard]] std::string_view label() const noexcept override;

private:
    std::vector<Entity> targets;
    StructuralUndoState state;
    std::vector<Entity> selectionBefore;
};

class DuplicateEntitiesCommand final : public Command {
public:
    DuplicateEntitiesCommand(std::span<const Entity> sources, std::span<const Entity> selectionBefore);
    // `sources` -> createdEntities (1st redo); later redos restoreStructuralState(createdEntities) (D21)
    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;  // captureAndDestroySubtrees(createdEntities)
    [[nodiscard]] std::string_view label() const noexcept override;

private:
    std::vector<Entity> sources;
    std::vector<Entity> createdEntities;  // filled by the FIRST redo
    StructuralUndoState state;
    std::vector<Entity> selectionBefore;
};

class ReparentCommand final : public Command {
public:
    ReparentCommand(std::span<const Entity> targets, Entity newParent);
    // Captures the OLD parent and slot, per target, INSIDE redo -- EVERY time, not just the first
    // (A19): a redo after an undo must capture whatever the tree looks like NOW.
    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;  // walks `captured` in REVERSE
    [[nodiscard]] std::string_view label() const noexcept override;

    struct Move {
        Entity entity{};
        Entity oldParent{};                  // Entity{} == it was a world root
        std::size_t oldSlot = NO_ROOT_SLOT;  // among oldParent's children, or among the roots
    };

private:
    std::vector<Entity> targets;
    Entity newParentTarget{};
    std::vector<Move> captured;  // filled by redo, consumed by undo
};

class RenameEntityCommand final : public Command {
public:
    // `before`/`after` are OWNED strings, never views: World::name()'s view is invalidated by the
    // very setName() this command is about to perform.
    RenameEntityCommand(Entity entity, std::string before, std::string after);
    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;
    [[nodiscard]] std::string_view label() const noexcept override;

private:
    Entity target{};
    std::string beforeName;
    std::string afterName;
};

}  // namespace engine::editor
