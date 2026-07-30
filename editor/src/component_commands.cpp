// editor/src/component_commands.cpp -- task 2.4.2: SetFieldCommand/AddComponentCommand/
// RemoveComponentCommand, wrapping component_ops.
#include <aero/editor/component_commands.hpp>
#include <aero/scene/world.hpp>

#include <utility>

namespace engine::editor {

namespace {

// The short display name for a label ("Transform" from "engine::Transform"). Duplicated from
// inspector_panel.cpp:27-30 rather than shared: that TU must stay entt-free by file placement (D1),
// and this one is a different layer -- the tree's own house style for tiny helpers (A17; countAtLevel
// is duplicated per test TU "by design", doubleToClamped is duplicated from meta_utils.cpp).
std::string_view shortComponentName(std::string_view fullName) {
    const std::size_t pos = fullName.rfind("::");
    return pos == std::string_view::npos ? fullName : fullName.substr(pos + 2);
}

}  // namespace

// ---- SetFieldCommand --------------------------------------------------------------------------

SetFieldCommand::SetFieldCommand(Entity entity, ComponentTypeId type, std::string_view field,
                                 std::string_view componentName, FieldValue before, FieldValue after)
    : target(entity),
      typeId(type),
      fieldName(field),
      labelText(std::string(shortComponentName(componentName)) + '.' + std::string(field)),
      beforeValue(std::move(before)),
      afterValue(std::move(after)) {}

bool SetFieldCommand::write(World& world, const FieldValue& value) {
    // SILENT, meta-free liveness guard (D15/2.4.1 D16): "you undid past a deleted entity" is not an
    // ERROR-severity event, and the stack already emits exactly one WARN. Also cheap -- no meta lookup
    // on the reject path. Mirrors TransformCommand::write's shape (transform_command.cpp:13-22).
    if (!world.alive(target) || !world.hasRaw(typeId, target)) {
        return false;
    }
    return writeComponentField(world, target, typeId, fieldName, value);
}

bool SetFieldCommand::redo(CommandContext& context) { return write(context.world, afterValue); }
bool SetFieldCommand::undo(CommandContext& context) { return write(context.world, beforeValue); }

std::string_view SetFieldCommand::label() const noexcept { return labelText; }

bool SetFieldCommand::mergeWith(const Command& incoming) {
    const auto* const next = dynamic_cast<const SetFieldCommand*>(&incoming);
    if (next == nullptr || next->target != target || next->typeId != typeId || next->fieldName != fieldName) {
        return false;
    }
    afterValue = next->afterValue;  // `beforeValue` DELIBERATELY UNTOUCHED (S7's discriminator)
    return true;
}

Entity SetFieldCommand::entity() const noexcept { return target; }
ComponentTypeId SetFieldCommand::type() const noexcept { return typeId; }
std::string_view SetFieldCommand::field() const noexcept { return fieldName; }
const FieldValue& SetFieldCommand::before() const noexcept { return beforeValue; }
const FieldValue& SetFieldCommand::after() const noexcept { return afterValue; }

// ---- AddComponentCommand -----------------------------------------------------------------------

AddComponentCommand::AddComponentCommand(Entity entity, ComponentTypeId type, std::string_view componentName)
    : target(entity), typeId(type), labelText(std::string("Add ") + std::string(shortComponentName(componentName))) {}

bool AddComponentCommand::redo(CommandContext& context) { return addComponent(context.world, target, typeId); }
bool AddComponentCommand::undo(CommandContext& context) { return removeComponent(context.world, target, typeId); }

std::string_view AddComponentCommand::label() const noexcept { return labelText; }
Entity AddComponentCommand::entity() const noexcept { return target; }
ComponentTypeId AddComponentCommand::type() const noexcept { return typeId; }

// ---- RemoveComponentCommand ---------------------------------------------------------------------

RemoveComponentCommand::RemoveComponentCommand(Entity entity, ComponentTypeId type, std::string_view componentName)
    : target(entity),
      typeId(type),
      labelText(std::string("Remove ") + std::string(shortComponentName(componentName))) {}

bool RemoveComponentCommand::redo(CommandContext& context) {
    // Captures BEFORE removing, EVERY time (D18/A19): a redo after an undo must capture whatever the
    // entity holds NOW, since an intervening SetFieldCommand undo may have changed it.
    if (!payload.capture(context.world, target, typeId)) {
        return false;  // dead entity, absent component, or an unmirrorable type -- nothing removed
    }
    return removeComponent(context.world, target, typeId);
}

bool RemoveComponentCommand::undo(CommandContext& context) {
    if (!context.world.alive(target)) {
        return false;
    }
    // MUST NOT call addComponent -- that default-constructs and would be silent data loss (D18; S14).
    return payload.restore(context.world, target);
}

std::string_view RemoveComponentCommand::label() const noexcept { return labelText; }
Entity RemoveComponentCommand::entity() const noexcept { return target; }
ComponentTypeId RemoveComponentCommand::type() const noexcept { return typeId; }

}  // namespace engine::editor
