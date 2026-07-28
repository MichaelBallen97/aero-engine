#pragma once
// Aero Engine — engine::editor::EditorCamera (task 2.3.1): the editor's OWN camera, a tool eye, never
// a document one. This is what the Viewport renders through instead of the scene's `Camera`
// component — orbit, pan, dolly and fly the scene without ever touching the World.
//
// PUBLIC and ImGui-free, entt-free and render-free BY RULE — held by FILE PLACEMENT, not by a guard
// (R12: aero_editor_shell_test links doctest, which puts vcpkg's shared include root on the compile
// line, so a leaked `#include <imgui.h>` here would still compile; do not claim enforcement that does
// not exist). It is render-free specifically because `aero::scene_render` is PRIVATE on
// aero_editor_core (editor/CMakeLists.txt) — naming a `render::` type here would break the tier-0
// shell test's compile outright (D2). `viewport_panel.cpp`, which is src-private and does see
// `aero::render` transitively, assembles the one `render::CameraView` at its single call site.
//
// This file lands the pure gesture half first (task 2.3.1 step 2); the EditorCamera model itself
// arrives in step 4 of the same task.

#include <cstdint>

namespace engine::editor {

// Which continuous navigation gesture owns the camera right now (D5). Explicit uint8_t:
// performance-enum-size, like every enum in this tree.
enum class CameraGesture : std::uint8_t { None = 0, Orbit, Pan, Dolly, Fly };

// Which physical button is holding that gesture open. `None` iff gesture == None.
enum class CameraButton : std::uint8_t { None = 0, Left, Right, Middle };

// LATCHED ACROSS FRAMES by the panel: rule 1 below needs the previous frame's value.
struct CameraGestureState {
    CameraGesture gesture = CameraGesture::None;
    CameraButton button = CameraButton::None;
    bool operator==(const CameraGestureState&) const noexcept = default;
};

// One frame of raw button/modifier state, as the panel reads it from ImGui. `...Pressed` means the
// button went !down -> down THIS frame (ImGui::IsMouseClicked); `...Down` means it is held
// (ImGui::IsMouseDown). `ctrlOrCmd` is io.KeyCtrl ALONE -- ImGui has already remapped Cmd on macOS
// (imgui.h:2683), so `io.KeyCtrl || io.KeySuper` would ALSO fire on physical Ctrl on macOS and on
// the Super key elsewhere (F22b).
struct CameraGestureInput {
    bool hovered = false;
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    bool leftPressed = false;
    bool rightPressed = false;
    bool middlePressed = false;
    bool alt = false;
    bool ctrlOrCmd = false;
};

// PURE (D5) -- no ImGui type reaches this function, which is what lets a tier-0 test drive the whole
// matrix with no window, no GPU and no ImGui context (the clickSelectionAction precedent).
// Three rules, IN ORDER:
//   1. CONTINUE -- current.gesture != None and the button named by current.button is still down
//      -> return `current` UNCHANGED. Modifier changes mid-gesture NEVER re-classify: letting go of
//      Alt halfway through an orbit keeps orbiting, which is what every 3D application does.
//      Deliberately does NOT consult `hovered`, so a drag that leaves the panel keeps going (E3).
//   2. END -- otherwise the gesture is over; if !in.hovered -> {None, None}.
//   3. START -- only on a FRESH PRESS while hovered, first match wins:
//        rightPressed && alt              -> {Dolly,  Right}
//        rightPressed                     -> {Fly,    Right}
//        middlePressed                    -> {Pan,    Middle}
//        leftPressed && alt && ctrlOrCmd  -> {Pan,    Left}     (the Mac-trackpad path)
//        leftPressed && alt               -> {Orbit,  Left}
//        otherwise                        -> {None,   None}
// The fresh-press requirement is what makes rule 3 independent of ImGui's hover subtleties (F23) and
// what guarantees a PLAIN LMB press can never retroactively become an orbit -- plain LMB is
// deliberately unbound and belongs to 2.3.2's click-picking (AC-13).
[[nodiscard]] CameraGestureState nextGesture(CameraGestureState current, const CameraGestureInput& in) noexcept;

}  // namespace engine::editor
