#pragma once
// Aero Engine — what a panel is handed each frame (task 2.2.1, D7). References, never ownership:
// EditorApp owns both objects and rebuilds this aggregate per frame. Engine + editor types only --
// ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9).
//
// It is a plain AGGREGATE of references, mirroring PanelOptions: 2.5.1 appends the current scene path
// and dirty flag, 2.6.1 appends the project, and a panel that ignores a field costs nothing. Being
// reference-typed it is deliberately non-assignable -- it is built fresh each frame, never stored.
//
// FORWARD DECLARATIONS ONLY: this header reaches every panel through panel.hpp, so it must stay free
// of <aero/scene/world.hpp>. A TU that actually TOUCHES the World includes that header itself.
//
// The one exception is command_stack.hpp (task 2.4.2): it pulls no engine header either, only forward
// declarations, so including it here to get CommandContext costs nothing and keeps this rule intact.
#include <aero/editor/command_stack.hpp>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

class Selection;
class RootOrder;

struct PanelContext {
    World& world;
    Selection& selection;
    // Task 2.4.1: the editor's undo/redo history. A REFERENCE, not a pointer: the editor always has
    // exactly one stack, so a null state would mean nothing and would only buy a null check at every
    // present and future call site (2.3.1 D1's rule is that a borrowed reference WITH a null state is
    // spelled as a pointer). Consequence, stated so it is never rediscovered: `{world, selection}` is
    // no longer a valid initialiser -- the five construction sites in the tree (editor_app.cpp:147,
    // shell_test.cpp x3, hierarchy_test.cpp x1) were all updated with this line.
    CommandStack& commands;
    // Task 2.4.2: the editor's ONE display order among root entities, owned by EditorApp and moved out
    // of HierarchyPanel (D10) so a structural command can restore a deleted root to the row it
    // occupied. The Hierarchy still RECONCILES it, in its phase 1, where it must run after this
    // frame's undo and before the tree walk.
    RootOrder& roots;
    // Task 2.3.1: this frame's SPIKE-CLAMPED delta -- FrameClock::deltaSeconds(), capped at 0.25 s
    // (core/time.hpp), NOT io.DeltaTime. Two reasons the clamped engine clock is the right one: the
    // editor throttles to 20 Hz when unfocused (EditorAppConfig::unfocusedFrameCapHz), and a stall
    // during a window drag must not teleport a panel's continuous input.
    // The ONE defaulted member: omitting it still yields 0. `commands` and `roots` are references and
    // have no default (tasks 2.4.1 D7 / 2.4.2 D10).
    float deltaSeconds = 0.0F;
};

// The single place a PanelContext becomes a CommandContext (task 2.4.2). Inline and trivial on
// purpose -- a panel that writes {context.world, context.selection, context.roots} by hand at ten
// call sites is nine chances to pass the wrong RootOrder.
[[nodiscard]] inline CommandContext toCommandContext(PanelContext& context) {
    return CommandContext{context.world, context.selection, context.roots};
}

}  // namespace engine::editor
