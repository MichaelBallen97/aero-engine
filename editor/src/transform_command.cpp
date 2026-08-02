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

bool TransformCommand::redo(CommandContext& context) { return write(context.world, afterValue); }
bool TransformCommand::undo(CommandContext& context) { return write(context.world, beforeValue); }

std::string_view TransformCommand::label() const noexcept { return TRANSFORM_COMMAND_LABEL; }

bool TransformCommand::mergeWith(const Command& incoming) {
    const auto* const next = dynamic_cast<const TransformCommand*>(&incoming);
    if (next == nullptr || next->target != target) {
        return false;
    }
    // The merge guard task 2.4.1 deliberately left out, added now that its own stated trigger has
    // fired (E9/H7): "the first task that can write a Transform from outside the drag loop must add
    // it." That task was the very next one -- 2.4.2's SetFieldCommand writes Transform.position /
    // rotation / scale from the Inspector, Transform being one reflected component among five --  and
    // the handoff went unread until the Phase 2 audit. It is no longer an assertion that cannot fail.
    //
    // Merging keeps OUR before and takes THEIR after, which is sound only while the two are
    // CONTIGUOUS. If anything wrote the Transform in between, `beforeValue` skips that write and undo
    // would restore a value the entity never held. Refusing costs a second history entry -- always
    // correct, merely less tidy -- while merging a stale pair is silent corruption of the undo record.
    if (!(next->beforeValue == afterValue)) {
        return false;
    }
    // `beforeValue` is deliberately UNTOUCHED: keeping our own before and taking their after is what
    // makes a 200-frame drag one undo step whose before is the drag's start (AC-14).
    afterValue = next->afterValue;
    return true;
}

Entity TransformCommand::entity() const noexcept { return target; }
const Transform& TransformCommand::before() const noexcept { return beforeValue; }
const Transform& TransformCommand::after() const noexcept { return afterValue; }

}  // namespace engine::editor
