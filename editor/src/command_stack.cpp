// editor/src/command_stack.cpp -- task 2.4.1: the editor's undo/redo backbone.
// Deliberately does NOT include <aero/scene/world.hpp>: every World& here is FORWARDED, never
// dereferenced, so the incomplete type declared in the header is enough (transform_command.cpp is the
// same case; transform_ops.cpp is NOT, because it instantiates the World::get<T> template).
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>

#include <cstddef>
#include <utility>

namespace engine::editor {

CommandStack::CommandStack(std::size_t capacity)
    // Clamped to >= 1: a zero-capacity history would silently swallow every command it was handed
    // (D8/AC-8, S12's discriminator).
    : capacityLimit(capacity < 1 ? std::size_t{1} : capacity) {}

bool CommandStack::push(World& world, std::unique_ptr<Command> command) {
    if (command == nullptr) {
        return false;  // AC-11/E8: silent -- there is nothing to name in a WARN
    }
    // Step 2 BEFORE step 3 is load-bearing: a failed command must not destroy a redo branch that is
    // still valid (D21 -- S3's discriminator).
    if (!command->redo(world)) {
        AERO_LOG_WARN("editor: command '{}' could not be applied and was not recorded", command->label());
        return false;  // the redo branch is UNTOUCHED
    }
    // Step 3 BEFORE step 4 is defensive, not load-bearing: a non-empty redo branch implies an earlier
    // undo()/redo(), both of which break the chain (D10), so `mergeOpen && applied < count()` is
    // unreachable today. Written in this order, it stays correct even if that invariant is weakened.
    history.erase(history.begin() + static_cast<std::ptrdiff_t>(applied), history.end());  // AC-3

    if (mergeOpen && applied > 0 && history[applied - 1]->mergeWith(*command)) {
        return true;  // AC-6: `command` is destroyed HERE, at scope exit
    }

    history.push_back(std::move(command));  // the std::move keeps performance-unnecessary-value-param quiet
    applied = history.size();
    mergeOpen = true;  // AC-7: a recording push OPENS the chain
    trimToCapacity();  // AC-8
    // A successful push is SILENT -- see command_stack.hpp's banner (A10).
    return true;
}

bool CommandStack::undo(World& world) {
    if (applied == 0) {
        return false;  // E1: the guard PRECEDES the log, so held key repeat stays silent
    }
    Command& top = *history[applied - 1];
    const std::string_view label = top.label();  // BEFORE the call: a command may mutate itself
    const bool ok = top.undo(world);
    --applied;          // ALWAYS, even on failure (D20/AC-5)
    mergeOpen = false;  // D10/AC-7
    if (ok) {
        AERO_LOG_DEBUG("editor: undo '{}' ({} left)", label, applied);
    } else {
        AERO_LOG_WARN("editor: undo '{}' could not be applied -- its target is gone", label);
    }
    return true;  // D20: "did the history move", never "did the command work"
}

bool CommandStack::redo(World& world) {
    if (applied == history.size()) {
        return false;
    }
    Command& next = *history[applied];
    const std::string_view label = next.label();  // BEFORE the call: a command may mutate itself
    const bool ok = next.redo(world);
    ++applied;
    mergeOpen = false;  // D10/AC-7
    if (ok) {
        AERO_LOG_DEBUG("editor: redo '{}' ({} applied)", label, applied);
    } else {
        AERO_LOG_WARN("editor: redo '{}' could not be applied -- its target is gone", label);
    }
    return true;  // D20: "did the history move", never "did the command work"
}

void CommandStack::trimToCapacity() {
    while (history.size() > capacityLimit) {
        history.erase(history.begin());
        // UNCONDITIONAL and correct: trimming only ever runs from push step 5, where step 3 has
        // already emptied the redo branch, so applied == history.size() here -- the evicted front
        // entry is always an applied one.
        --applied;
        if (cleanPosition.has_value()) {
            // Position 0 meant "before history[0]". Once history[0] is evicted that state can never
            // be returned to -- you cannot undo past a command that no longer exists -- so the clean
            // state becomes UNREACHABLE, not "position 0" (AC-9/E17; S8's discriminator). Every other
            // position shifts down by one and continues to denote the same document state.
            cleanPosition = (*cleanPosition == 0) ? std::nullopt : std::optional<std::size_t>{*cleanPosition - 1};
        }
    }
}

void CommandStack::breakMergeChain() noexcept { mergeOpen = false; }

void CommandStack::clear() noexcept {
    history.clear();
    applied = 0;
    mergeOpen = false;
    cleanPosition = 0;  // AC-9: a cleared history is CLEAN
}

bool CommandStack::canUndo() const noexcept { return applied > 0; }
bool CommandStack::canRedo() const noexcept { return applied < history.size(); }

std::string_view CommandStack::undoLabel() const noexcept {
    return canUndo() ? history[applied - 1]->label() : std::string_view{};
}

std::string_view CommandStack::redoLabel() const noexcept {
    return canRedo() ? history[applied]->label() : std::string_view{};
}

std::size_t CommandStack::count() const noexcept { return history.size(); }
std::size_t CommandStack::appliedCount() const noexcept { return applied; }
std::size_t CommandStack::capacity() const noexcept { return capacityLimit; }

void CommandStack::setClean() noexcept {
    cleanPosition = applied;
    // Without this, a post-save edit could merge into a pre-save command, leaving
    // applied == *cleanPosition while the document has genuinely changed -- isClean() would then lie
    // and return true after a real edit (S7).
    mergeOpen = false;
}

bool CommandStack::isClean() const noexcept { return cleanPosition.has_value() && *cleanPosition == applied; }

}  // namespace engine::editor
