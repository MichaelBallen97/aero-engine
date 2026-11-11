#pragma once
// Aero Engine -- the scene a game's play leaves untouched (task 4.7.2, D1 and D3). EditorApp parks the edited World at
// Play and restores it at Stop: park() moves the World into this object and leaves a clone of it in its place, so the
// game runs against the copy, and restore() puts the World from before Play back and destroys the copy. The clone
// keeps every entity handle, so nothing the editor holds is re-handed on either move. Pure: no ImGui, no GPU, no
// EnTT, so it is reachable in every build configuration.
//
// The undo history is deliberately left alone. restore() returns the very World the history was recorded against,
// with every handle identical, and nothing drives the history against the copy: the scene lock keeps every push out
// of play, and undo and redo are refused there.
//
// Every editor member whose type holds an `Entity` says what Stop does with it, in a comment on its line or the line
// above, as `stop:` and a class in square brackets: clear (Stop resets it, in `EditorApp::restoreAfterPlay`, the
// panels' `clearRetainedEntities` or `ParkedScene::restore`), frame (rebuilt before any read within a tick), history
// (a command's value, which the scene lock keeps free of play-time handles) or value (a field of a value type, whose
// holder carries the class). RS7 enforces it.

#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include <optional>
#include <type_traits>

namespace engine::editor {

class ParkedScene {
public:
    // Edit -> Playing. Moves `live` into the park and leaves its clone in its place; copies `roots`.
    // false, changing nothing, when a scene is already parked. A bad_alloc leaves `live`, `roots` and the park as
    // they were: everything that can throw runs before the first move.
    bool park(World& live, const RootOrder& roots);

    // Playing / Paused -> Edit. The selection is pruned against the play World first (an entity the game destroyed
    // while it was selected stays deselected) and against the restored World after (an entity that existed only in
    // play drops out too); prune() never bumps Selection::revision(), so neither does this.
    // false, changing nothing, when nothing is parked.
    bool restore(World& live, RootOrder& roots, Selection& selection);

    [[nodiscard]] bool parked() const noexcept;

private:
    std::optional<World> parkedWorld;  // the World from before Play, while a game plays
    RootOrder parkedRoots;             // [stop:clear] its root order
};

// EditorApp's move is `noexcept = default`; the guarantee has to hold on the member type (the RootOrder precedent).
static_assert(std::is_nothrow_move_constructible_v<ParkedScene>);
static_assert(std::is_nothrow_move_assignable_v<ParkedScene>);

}  // namespace engine::editor
