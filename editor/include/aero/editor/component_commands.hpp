#pragma once
// Aero Engine -- the three commands that wrap component_ops (task 2.4.2) -- the Inspector's whole
// write surface. PUBLIC, ImGui-free, entt-free like every editor/include header.

#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_ops.hpp>
#include <aero/editor/scene_snapshot.hpp>
#include <aero/scene/entity.hpp>

#include <string>
#include <string_view>

namespace engine::editor {

// The Edit menu shows "Undo Transform.position". Built ONCE, in the constructor, and never rebuilt:
// that is what makes the label_view lifetime contract (command_stack.hpp:53-65) hold trivially across
// redo/undo/mergeWith (D14).
class SetFieldCommand final : public Command {
public:
    // `before` is the field's value as the widget showed it this frame (the InspectorModel's own
    // snapshot -- F13/D16); `after` is the edited value. `componentName` is the FULL registration name;
    // the label uses its short form.
    SetFieldCommand(Entity entity, ComponentTypeId type, std::string_view field, std::string_view componentName,
                    FieldValue before, FieldValue after);

    bool redo(CommandContext& context) override;  // writes `after`
    bool undo(CommandContext& context) override;  // writes `before`
    [[nodiscard]] std::string_view label() const noexcept override;
    // True iff `incoming` is a SetFieldCommand on the SAME entity, type AND field (D13). Keeps our
    // `before`, takes their `after` -- which is what makes a 200-frame drag one undo step.
    bool mergeWith(const Command& incoming) override;

    [[nodiscard]] Entity entity() const noexcept;
    [[nodiscard]] ComponentTypeId type() const noexcept;
    [[nodiscard]] std::string_view field() const noexcept;
    [[nodiscard]] const FieldValue& before() const noexcept;
    [[nodiscard]] const FieldValue& after() const noexcept;

private:
    bool write(World& world, const FieldValue& value);

    Entity target{};
    ComponentTypeId typeId{};
    std::string fieldName;
    std::string labelText;
    FieldValue beforeValue;
    FieldValue afterValue;
};

// Undoably adds a default-constructed component (task 2.4.2 D1). redo -> component_ops::addComponent,
// which REFUSES a type already present and logs one ERROR of its own when it does (component_ops.cpp:
// 142-146) -- so a redo onto an occupied type produces one ERROR plus the stack's one WARN, and nothing
// is recorded (plan A5 -- the spec's own "the stack WARNs, nothing is recorded" undercounts this by one
// ERROR). undo -> component_ops::removeComponent, which is silent.
class AddComponentCommand final : public Command {
public:
    // `componentName` is the FULL registration name (World::componentTypeName); the label uses its
    // short form and is built ONCE, here (D14).
    AddComponentCommand(Entity entity, ComponentTypeId type, std::string_view componentName);
    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;
    [[nodiscard]] std::string_view label() const noexcept override;  // "Add MeshRenderer"
    [[nodiscard]] Entity entity() const noexcept;
    [[nodiscard]] ComponentTypeId type() const noexcept;

private:
    Entity target{};
    ComponentTypeId typeId{};
    std::string labelText;
};

// Undoably removes a component AND ITS FIELD VALUES (D18). Removing a component is otherwise
// UNRECOVERABLE -- the values are gone the instant the storage erases them -- which is why this command
// exists at all and why its undo restores the CAPTURED value, never a default-constructed shell.
class RemoveComponentCommand final : public Command {
public:
    RemoveComponentCommand(Entity entity, ComponentTypeId type, std::string_view componentName);
    // Captures BEFORE removing, EVERY time (not only the first): a redo after an undo must capture
    // whatever the entity holds NOW, since an intervening SetFieldCommand undo may have changed it.
    // Returns false, removing nothing, when the capture itself fails (a dead entity, an absent
    // component, or -- D18's own refusal -- a type this snapshot mechanism cannot mirror).
    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;
    [[nodiscard]] std::string_view label() const noexcept override;  // "Remove MeshRenderer"
    [[nodiscard]] Entity entity() const noexcept;
    [[nodiscard]] ComponentTypeId type() const noexcept;

private:
    Entity target{};
    ComponentTypeId typeId{};
    std::string labelText;
    ComponentSnapshot payload;  // filled by redo, consumed by undo
};

}  // namespace engine::editor
