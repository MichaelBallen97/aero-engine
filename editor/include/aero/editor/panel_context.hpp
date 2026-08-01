#pragma once
// Aero Engine — what a panel is handed each frame (task 2.2.1, D7). References, never ownership:
// EditorApp owns both objects and rebuilds this aggregate per frame. Engine + editor types only --
// ImGui-FREE BY RULE, like every header under editor/include (2.1.3 D9).
//
// It is a plain AGGREGATE of references, mirroring PanelOptions. What actually happened, so a future
// reader never re-derives it from the diff: 2.5.1 chose a NARROWER channel (FileMenuContext) instead
// of appending here, 2.6.1 chose a per-frame `setRoot` RECONCILE on the Asset Browser instead of
// appending here, and 2.6.2 is the first task that appends -- the open project, as a `const`
// reference. Being reference-typed it is deliberately non-assignable -- it is built fresh each frame,
// never stored.
//
// WHY CONST (task 2.6.2 D1): the session's setter has exactly one legal call site (INV-P1). A `const`
// reference in the channel that reaches every panel turns a second call site into a COMPILE ERROR
// instead of a review finding -- the strongest enforcement this codebase can give an invariant that
// used to be held by a grep alone.
//
// FORWARD DECLARATIONS ONLY: this header reaches every panel through panel.hpp, so it must stay free
// of <aero/scene/world.hpp> and free of the project header, for the identical reason.
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
class ProjectSession;  // task 2.6.2 (AC-25): a FORWARD DECLARATION, never an include

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
    // Task 2.6.2 -- CONST, and that is the point (see the WHY CONST banner above). Because it is a
    // REFERENCE, a panel sees a project swap performed EARLIER IN THE SAME FRAME:
    // applyFileRequests (where the swap actually happens) runs before drawPanels inside drawShellUi,
    // so a panel's onDraw on the very frame of the swap already reads the NEW session -- there is no
    // cache to invalidate and no one-frame lag, unlike the Asset Browser's own pull-and-compare
    // reconcile (which lags a runtime swap by exactly one tick, measured in imgui_layer_test.cpp's
    // I21). `{world, selection, commands, roots}` stopped being a valid initialiser the moment this
    // member landed -- the five construction sites in the tree were all updated with it.
    const ProjectSession& project;
    // Task 2.3.1: this frame's SPIKE-CLAMPED delta -- FrameClock::deltaSeconds(), capped at 0.25 s
    // (core/time.hpp), NOT io.DeltaTime. Two reasons the clamped engine clock is the right one: the
    // editor throttles to 20 Hz when unfocused (EditorAppConfig::unfocusedFrameCapHz), and a stall
    // during a window drag must not teleport a panel's continuous input.
    // The ONE defaulted member: omitting it still yields 0. Every reference member above has no
    // default (tasks 2.4.1 D7 / 2.4.2 D10 / 2.6.2 D1).
    float deltaSeconds = 0.0F;
};

// The single place a PanelContext becomes a CommandContext (task 2.4.2). Inline and trivial on
// purpose -- a panel that writes {context.world, context.selection, context.roots} by hand at ten
// call sites is nine chances to pass the wrong RootOrder.
[[nodiscard]] inline CommandContext toCommandContext(PanelContext& context) {
    return CommandContext{context.world, context.selection, context.roots};
}

}  // namespace engine::editor
