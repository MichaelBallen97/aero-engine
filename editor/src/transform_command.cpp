// editor/src/transform_command.cpp -- task 2.4.1 (D1): the one concrete command this task ships.
// Deliberately does NOT include <aero/scene/world.hpp>: every World& is FORWARDED to transform_ops,
// which needs no complete type here (transform_ops.cpp is the TU that does, because it instantiates the
// World::get<T> template).
#include <aero/editor/transform_command.hpp>
#include <aero/editor/transform_ops.hpp>

namespace engine::editor {

TransformCommand::TransformCommand(Entity entity, const Transform& before, const Transform& after)
    : target(entity), beforeValue(before), afterValue(after) {}

bool TransformCommand::write(World& world, const Transform& value) {
    // The guard asks the EXACT question writeTransform will ask, so it cannot drift from the thing it
    // guards. It also means this path never reaches writeTransform's own AERO_LOG_ERROR: "you undid
    // past a deleted entity" is not an ERROR-severity event, and two records for one keypress is
    // noise, so the CommandStack's single WARN is the only diagnostic (D16/AC-13).
    if (!readTransform(world, target).has_value()) {
        return false;
    }
    return writeTransform(world, target, value);
}

bool TransformCommand::redo(World& world) { return write(world, afterValue); }
bool TransformCommand::undo(World& world) { return write(world, beforeValue); }

std::string_view TransformCommand::label() const noexcept { return TRANSFORM_COMMAND_LABEL; }

bool TransformCommand::mergeWith(const Command& incoming) {
    const auto* const next = dynamic_cast<const TransformCommand*>(&incoming);
    if (next == nullptr || next->target != target) {
        return false;
    }
    // `beforeValue` is deliberately UNTOUCHED: keeping our own before and taking their after is what
    // makes a 200-frame drag one undo step whose before is the drag's start (AC-14).
    afterValue = next->afterValue;
    // An `incoming.before() == after()` merge guard is DELIBERATELY ABSENT: it could never be false
    // in this task -- the gizmo reads `before` fresh from the World every frame (viewport_panel.cpp),
    // writeTransform stores exactly what it is given, and Transform::operator== is exact -- and an
    // assertion that cannot fail is precisely the defect class this project's log calls out. The first
    // task that can write a Transform from outside the drag loop must add it (E9/H7).
    return true;
}

Entity TransformCommand::entity() const noexcept { return target; }
const Transform& TransformCommand::before() const noexcept { return beforeValue; }
const Transform& TransformCommand::after() const noexcept { return afterValue; }

}  // namespace engine::editor
