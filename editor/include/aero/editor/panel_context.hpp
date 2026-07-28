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

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

class Selection;

struct PanelContext {
    World& world;
    Selection& selection;
    // Task 2.3.1: this frame's SPIKE-CLAMPED delta -- FrameClock::deltaSeconds(), capped at 0.25 s
    // (core/time.hpp), NOT io.DeltaTime. Two reasons the clamped engine clock is the right one: the
    // editor throttles to 20 Hz when unfocused (EditorAppConfig::unfocusedFrameCapHz), and a stall
    // during a window drag must not teleport a panel's continuous input.
    // DEFAULTED so `{world, selection}` stays valid -- this aggregate is designed to grow (2.2.1 D7).
    float deltaSeconds = 0.0F;
};

}  // namespace engine::editor
