#pragma once
// Aero Engine — engine::editor gizmo model (task 2.3.3): the PURE half of the transform gizmos.
// Every ImGuizmo type stops at editor/src/viewport_panel.cpp; nothing here names one, so a tier-0
// test drives the whole model with no window, no GPU and no ImGui context — the nextGesture /
// pickSelectionAction precedent.
//
// ImGui-FREE, entt-FREE, render-FREE and IMGUIZMO-FREE BY RULE, held by FILE PLACEMENT, not by a
// guard (R12: aero_editor_shell_test links doctest, which puts vcpkg's shared include root on the
// compile line, so a leaked #include <ImGuizmo.h> here would still compile). Do not claim
// enforcement that does not exist. It is render-free specifically because aero::scene_render is
// PRIVATE on aero_editor_core -- naming a render:: type here would break the tier-0 shell test's
// compile outright (the picking.hpp / editor_camera.hpp precedent).

#include <aero/core/math.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/transform.hpp>

#include <cstdint>
#include <optional>

namespace engine {
class World;  // forward-declared: the geometry functions take it by reference and gizmo.cpp includes
              // <aero/scene/world.hpp> itself -- the picking.hpp / scene_bounds.hpp precedent.
}  // namespace engine

namespace engine::editor {

// ---- tool state ---------------------------------------------------------------------------------

enum class GizmoOperation : std::uint8_t { Translate = 0, Rotate, Scale };
enum class GizmoSpace : std::uint8_t { Local = 0, World };

// Which phase of a continuous drag this frame is. Nothing in 2.3.3 consumes Begin/End beyond the
// D12 WARN latch -- it exists so 2.4.1's merge policy for continuous edits has a REAL, TESTED edge
// to hook instead of reverse-engineering one out of ImGuizmo's internals (D22).
enum class GizmoDragEdge : std::uint8_t { None = 0, Begin, Continue, End };

struct GizmoMode {
    GizmoOperation operation = GizmoOperation::Translate;
    GizmoSpace space = GizmoSpace::World;
    bool operator==(const GizmoMode&) const noexcept = default;
};

// One frame of raw key state, as the panel reads it from ImGui. `...Pressed` means the key went
// !down -> down THIS frame (ImGui::IsKeyPressed(key, /*repeat=*/false)). The panel has ALREADY
// applied the hover / WantTextInput / no-camera-gesture gates (D7) -- this function is unconditional.
struct GizmoModeInput {
    bool translatePressed = false;    // W
    bool rotatePressed = false;       // E
    bool scalePressed = false;        // R
    bool spaceTogglePressed = false;  // X
};

// PURE. First match wins among the three OPERATION keys; the space toggle is deliberately NOT in
// that chain, because W and X in the same frame is a legal, unambiguous combination (two fingers)
// and both should apply. An unchanged `current` is returned when nothing is pressed.
[[nodiscard]] GizmoMode nextGizmoMode(GizmoMode current, const GizmoModeInput& in) noexcept;

// ImGuizmo forces LOCAL for SCALE internally: ComputeContext is called with
// `(operation & SCALE) ? LOCAL : mode` ("Scale is always local or matrix will be skewed when
// applying world scale or oriented matrix", ImGuizmo.cpp:2684-2685). Mirroring it HERE is what stops
// the overlay bar from displaying "World" while the library silently did something else (AC-4).
[[nodiscard]] GizmoSpace effectiveSpace(GizmoOperation op, GizmoSpace requested) noexcept;

// TUNING values, judged by the human pass (editor/VALIDATION.md). Each is named so retuning is a
// one-line change; every tier-0 case asserts a RELATIONSHIP, never a magnitude.
inline constexpr float GIZMO_SNAP_TRANSLATE = 0.5F;        // world units
inline constexpr float GIZMO_SNAP_ROTATE_DEGREES = 15.0F;  // DEGREES -- the suffix is load-bearing
inline constexpr float GIZMO_SNAP_SCALE = 0.1F;            // scale factor

// The three snap steps, or nullopt when `snapHeld` is false.
// UNITS ARE NOT UNIFORM and are dictated by ImGuizmo (verified at source, F12):
//   Translate -- all three components used, WORLD UNITS, per axis (ImGuizmo.cpp:1259-1264 loops 0..2).
//   Rotate    -- ONLY .x is read, and it is in DEGREES (ImGuizmo.cpp:2482 multiplies by DEG2RAD).
//   Scale     -- ONLY .x is read; ImGuizmo replicates it to all three axes (ImGuizmo.cpp:2372).
// A Vec3 is returned for all three so the panel has ONE `const float*` to hand over; the y/z
// components of the Rotate/Scale results are filled with the same value rather than left garbage,
// so a future ImGuizmo that starts reading them cannot surprise us.
[[nodiscard]] std::optional<Vec3> gizmoSnapStep(GizmoOperation op, bool snapHeld) noexcept;

// PURE drag-edge derivation (D22). `wasUsing` is the previous frame's latched value.
[[nodiscard]] GizmoDragEdge gizmoDragEdge(bool wasUsing, bool isUsing) noexcept;

// ---- geometry -----------------------------------------------------------------------------------

// The world matrix to hand ImGuizmo, or nullopt when there is nothing to manipulate: a null or dead
// entity, a moved-from World, or an entity with no Transform. Never returns a non-finite matrix: a
// Transform carrying inf/NaN yields nullopt rather than feeding garbage into the library (E15).
[[nodiscard]] std::optional<Mat4> gizmoModelMatrix(const World& world, Entity entity);

// The entity's PARENT's world matrix -- identity for a root, for a null/dead parent, and for a
// moved-from World. This is the matrix gizmoWriteFromWorld inverts.
[[nodiscard]] Mat4 gizmoParentMatrix(const World& world, Entity entity);

// Is the gizmo origin at or behind the near plane? TWO tests, either of which fails closed:
//   1. picking.hpp's projectToViewport, false for w <= CLIP_W_EPSILON or any non-finite result --
//      so pick, highlight and gizmo share ONE definition of "behind the camera" for this half (D9).
//   2. ImGuizmo's OWN behind-camera test, mirrored exactly (ImGuizmo.cpp:2696-2698): RAW clip-space
//      z < 0.001, no perspective divide. Code-review finding (2026-07-29): test 1 alone leaves a
//      reachable band where OUR test says "in front" but ImGuizmo's own test would have refused,
//      so Manipulate gets called and leaks an unmatched PushClipRect (F5). Test 2 closes it.
// FAILS CLOSED throughout: a non-finite model, or a non-finite clip.z, returns true.
// `viewportSizePoints` only scales the (discarded) screen point test 1 computes; any positive size
// gives the same answer, and the panel passes the real one.
[[nodiscard]] bool gizmoOriginBehindCamera(const Mat4& viewProj, const Mat4& model, Vec2 viewportSizePoints) noexcept;

// ---- the write ----------------------------------------------------------------------------------

enum class GizmoWriteStatus : std::uint8_t {
    Applied = 0,      // `transform` is the new value; write it
    NoChange,         // exactly equal to `before`; write NOTHING (D6)
    NotFinite,        // newWorld, the derived local, or the composed result carries inf/NaN
    NotDecomposable,  // degenerate/near-singular scale (decompose() itself refused), OR shear from a
                      // non-uniformly scaled ancestor -- caught by gizmo.cpp's OWN orthogonality
                      // guard, upstream of decompose(), which has no such test (out of contract per
                      // transform.hpp's D9; code-review finding, 2026-07-29)
};

struct GizmoWrite {
    GizmoWriteStatus status = GizmoWriteStatus::NoChange;
    Transform transform{};  // meaningful ONLY when status == Applied; left DEFAULT otherwise, never
                            // partially filled (G11 asserts this)
};

// THE core of this task. `newWorld` is the matrix ImGuizmo just mutated in place; `parentWorld` is
// gizmoParentMatrix's result; `before` is the entity's CURRENT local Transform.
//
//   local = inverse(parentWorld) * newWorld        (identity parent -> the inverse is SKIPPED)
//   reject shear (NotDecomposable)                 (ours: decompose() itself has no such test)
//   decompose(local, trs)                          (ours, never ImGuizmo's -- F11)
//   result = before, with ONLY the channel `op` owns replaced (D5)
//
// Writing one channel is not a micro-optimisation: a full three-field round-trip renormalises the
// quaternion and re-derives scale from column lengths on EVERY frame of a drag, so dragging a
// ROTATION handle would slowly drift a stored scale of {1,1,1}. Channel isolation makes "nothing
// else changed" an assertable property (G7/AC-2) instead of a hope.
[[nodiscard]] GizmoWrite gizmoWriteFromWorld(const Mat4& parentWorld, const Mat4& newWorld, const Transform& before,
                                             GizmoOperation op) noexcept;

}  // namespace engine::editor
