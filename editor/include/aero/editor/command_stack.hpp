#pragma once
// Aero Engine -- the editor's undo/redo backbone (task 2.4.1). PUBLIC, ImGui-FREE, entt-FREE,
// ImGuizmo-FREE and render-FREE by rule, held by FILE PLACEMENT (R12) exactly like every other header
// under editor/include -- which is what lets aero_editor_shell_test drive the whole model tier-0,
// with no window, no GPU and no ImGui context.
//
// THE CONTRACT IN ONE LINE: push() APPLIES the command (D5). A caller must NOT have already written
// the edit -- there is exactly one write path, and it runs inside redo().
//
// A SUCCESSFUL push() IS SILENT -- only undo()/redo() log, and only at AERO_LOG_DEBUG on success
// (AERO_LOG_WARN on failure). A gizmo drag pushes on every frame it reports `changed`; logging that
// would be the 60-lines-a-second flood 2.3.3's D12 latch exists to prevent (A10). Do not "fix" the
// missing success record on push -- it is intentional.

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine {
class World;  // forward-declared: every entry point takes it by reference; the .cpp includes the
              // definition. The picking.hpp / gizmo.hpp precedent.
}  // namespace engine

namespace engine::editor {

class Selection;  // forward declarations only: this header stays free of world.hpp/selection.hpp/
class RootOrder;  //   entity_ops.hpp weight, exactly as it already is for World.

// What a command is handed at apply time (task 2.4.2). References, never ownership -- the PanelContext
// shape, and for the same reason: a command may outlive the frame that created it by an arbitrary
// number of frames, so it must hold VALUES and HANDLES only and receive everything else as an
// argument (the rule this header already states below).
//
// PASSED PER CALL, never stored on the stack: a reference member on CommandStack would delete
// EditorApp's defaulted move assignment (2.4.1 F15) and would let one stack straddle a scene swap,
// which INV-6 exists to make impossible.
//
// Three fields, each earned:
//   world      -- what a command mutates.
//   selection  -- create/delete/duplicate change it, so their undo must put it back (2.4.2 D12;
//                selection.hpp:11 has said so since 2.2.1).
//   roots      -- the editor's display order among ROOT entities, which the World deliberately does
//                not model (entity_ops.hpp:91-98). An undone delete of a root must return that root
//                to the row it occupied, not to the bottom of the list.
struct CommandContext {
    World& world;
    Selection& selection;
    RootOrder& roots;
};

// One undoable editor mutation. Concrete commands live next to the seam they wrap
// (transform_command.hpp here; 2.4.2 adds the reflected property-set and the structural four).
//
// LIFETIME: a command is owned by the CommandStack and may outlive the frame that created it by an
// arbitrary number of frames. It must therefore hold VALUES and HANDLES only -- never a World&, never
// a Selection&, never a pointer into a panel (the Selection's own "holds handles, never pointers"
// rule, selection.hpp:6-7). Everything it needs at apply time arrives as an argument.
class Command {
public:
    Command() = default;
    virtual ~Command() = default;
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    // Apply, or re-apply. CALLED BY push() (D5) -- the first application is a redo. Returns false
    // when the edit could not be applied (its target is gone, typically); the stack turns that into
    // exactly one WARN. MUST NOT partially mutate on a false return.
    virtual bool redo(CommandContext& context) = 0;

    // Restore the state as it was before redo(). Same false semantics.
    virtual bool undo(CommandContext& context) = 0;

    // A short, NON-EMPTY, human-facing noun phrase for the Edit menu: "Undo <label>". Backed by a
    // string literal or by a std::string the command itself owns; valid for the command's lifetime.
    //
    // CONTRACT (code-review round, Gap 6): the returned view must stay VALID AND UNCHANGED across this
    // command's OWN redo(), undo() and mergeWith() calls -- a command must not mutate whatever backs
    // its label from inside any of those three. This is what makes CommandStack::undo()/redo()'s
    // "capture the label, then call the command, then log it" idiom sound: holding a string_view
    // across a call that could reallocate the buffer behind it would dangle. Callers must additionally
    // not hold a label view ACROSS a push()/undo()/redo()/clear() call on the owning CommandStack (a
    // merge or a capacity trim can destroy the command that view points into). If a future command
    // genuinely needs a label that changes on redo()/undo() (D18's std::string-backed case), the
    // caller must copy it into a std::string BEFORE the call that could change it, not hold this
    // string_view across it -- a handoff for 2.4.2.
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;

    // Fold `incoming` into this command, so a continuous gesture is ONE history entry. Called only
    // on the top of the stack, only while the merge chain is open, and only AFTER incoming->redo()
    // has already run. Return true to absorb it (the caller then destroys `incoming`); false to let
    // it be pushed as its own entry.
    //
    // The parameter is UNNAMED in this default body on purpose: misc-unused-parameters is
    // --warnings-as-errors on the Linux lane (F13, the pickSelectionAction precedent).
    virtual bool mergeWith(const Command& /*incoming*/) { return false; }
};

// How many commands the history keeps. Oldest are EVICTED past this (the console_model.hpp ring
// precedent, F12). Deliberately NOT spelled DEFAULT_* -- see F14, the <wingdi.h> DEFAULT_PITCH trap.
inline constexpr std::size_t COMMAND_HISTORY_CAPACITY = 128;

class CommandStack {
public:
    // `capacity` is CLAMPED to >= 1: a zero-capacity history would silently swallow every command.
    explicit CommandStack(std::size_t capacity = COMMAND_HISTORY_CAPACITY);
    ~CommandStack() = default;
    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;
    // noexcept is EXPLICIT, not inherited: EditorApp's own move is `noexcept = default` (F15), so a
    // future member that is not noexcept-movable must fail HERE, loudly, not delete a defaulted move
    // two headers away.
    //
    // HAND-WRITTEN, not defaulted (code-review round, Gap 5/INV-1): a defaulted move copies the
    // scalars (`applied`, `cleanPosition`, `mergeOpen`) while std::vector's own move leaves `history`
    // empty behind -- a moved-from stack would then read `applied > 0` over an empty `history`,
    // reporting `canUndo() == true` and having `undoLabel()`/`undo()` index off the end of an empty
    // vector. Defined in the .cpp to reset the SOURCE to the same empty/clean/chain-closed state
    // clear() produces.
    CommandStack(CommandStack&& other) noexcept;
    CommandStack& operator=(CommandStack&& other) noexcept;

    // Apply `command` and record it. Returns true iff it was applied (and therefore recorded, or
    // merged into the top). A null command returns false silently.
    bool push(CommandContext& context, std::unique_ptr<Command> command);

    // Returns false iff there was nothing to undo/redo. A command that FAILS still consumes its step
    // and returns true -- a frozen Ctrl+Z is worse than a step that reverted nothing (D20).
    bool undo(CommandContext& context);
    bool redo(CommandContext& context);

    // The next push starts a NEW entry instead of merging into the top. Idempotent.
    void breakMergeChain() noexcept;

    // Drops every command, resets the position, breaks the chain and marks the history CLEAN --
    // "this document has no history and no unsaved edits", which is exactly the state 2.5.1's New /
    // Open Scene needs (INV-6).
    void clear() noexcept;

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] std::string_view undoLabel() const noexcept;  // "" when !canUndo()
    [[nodiscard]] std::string_view redoLabel() const noexcept;  // "" when !canRedo()
    [[nodiscard]] std::size_t count() const noexcept;           // entries held: undo branch + redo branch
    [[nodiscard]] std::size_t appliedCount() const noexcept;    // how many of them are currently applied
    [[nodiscard]] std::size_t capacity() const noexcept;

    // The 2.5.1 seam (D15). setClean() records "the document on disk matches the state right now";
    // isClean() answers "is it still the same state". Both are pure history arithmetic -- this class
    // knows nothing about files.
    //
    // INV-6 (a contract, not a type-enforced property): a stack must only ever be driven against the
    // World its commands were recorded against. 2.5.1's New/Open Scene must clear() this stack in the
    // SAME operation that replaces that World.
    void setClean() noexcept;
    [[nodiscard]] bool isClean() const noexcept;

private:
    void trimToCapacity();

    std::vector<std::unique_ptr<Command>> history;
    std::size_t applied = 0;  // history[0, applied) are applied; history[applied, size) are redoable
    // nullopt == the clean state is UNREACHABLE (its command was evicted by the capacity trim).
    std::optional<std::size_t> cleanPosition = 0;
    bool mergeOpen = false;
    // Distinct from capacity() by the tree's accessor-collision rule (RenderTarget::depthFormatValue
    // <-> depthFormat(), 2.3.1's eight collisions).
    std::size_t capacityLimit = COMMAND_HISTORY_CAPACITY;
};

// F15: EditorApp holds a CommandStack BY VALUE and its own move is `noexcept = default`
// (editor_app.hpp:70-71), so this type's move must stay noexcept or EditorApp's silently degrades.
//
// WHAT THESE ASSERTS DO AND DO NOT CATCH -- corrected in the code-review round, because the comment
// that stood here claimed a guarantee they never provided. The move above is HAND-WRITTEN and declared
// `noexcept`, so the first two hold BY DECLARATION: a member whose own move can throw does NOT make
// this ill-formed, it makes the move call std::terminate instead (declaring a function `noexcept` whose
// body may throw is well-formed; P1286R2 removed the "defined as deleted" rule even for the defaulted
// spelling, and Clang applies it as a DR under -std=c++17). The first two therefore only catch a member
// that is not movable AT ALL -- worth keeping, but far weaker than advertised.
//
// The PER-MEMBER asserts below are what actually enforce the intent: a future member whose move can
// throw fails HERE, loudly, instead of turning a moved-from EditorApp into a terminate two headers
// away. The scalar members need no assert; only the three non-trivial ones can regress.
static_assert(std::is_nothrow_move_constructible_v<CommandStack>);
static_assert(std::is_nothrow_move_assignable_v<CommandStack>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<std::unique_ptr<Command>>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<std::unique_ptr<Command>>>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<std::size_t>>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<std::size_t>>);

}  // namespace engine::editor
