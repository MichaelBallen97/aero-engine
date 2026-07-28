#pragma once
// Aero Engine — engine::editor picking (task 2.3.2): the screen -> world ray, the ray-vs-local-box
// test, the world pick walk, and the pure click decision.
//
// PUBLIC, ImGui-free, entt-free AND render-free BY RULE, held by FILE PLACEMENT like every header
// under editor/include (2.1.3 D9) -- see editor_camera.hpp's header comment for the full reasoning
// (R12: aero_editor_shell_test links doctest, which puts vcpkg's shared include root on the compile
// line, so a leaked `#include <imgui.h>` here would still compile; do not claim enforcement that does
// not exist). It is render-free specifically because aero::scene_render is PRIVATE on
// aero_editor_core: naming a render:: type here would break the tier-0 shell test's compile outright.
//
// THE SCREEN MAPPING BELOW HAS EXACTLY ONE IMPLEMENTATION IN THE TREE. selection_overlay.hpp includes
// THIS header to use it -- the dependency runs ONE WAY ONLY, overlay -> picking, so there is no cycle,
// and the pick and the highlight can never disagree about where a world point lands on screen. If
// they ever did, you would click one place and see a box somewhere else.
//
// Why the EDITOR and not engine/core/math (D19): the same reasoning scene_bounds records for Aabb --
// the only consumers are editor code, and Ray should be promoted WITH Aabb, in one change, when the
// first real ENGINE consumer appears (Jolt raycasts in Phase 3 is the obvious one).

#include <aero/core/math.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/entity.hpp>

#include <cstdint>

namespace engine {
class World;  // forward-declared: pickEntity takes it by reference and picking.cpp includes
              // <aero/scene/world.hpp> itself -- the scene_bounds.hpp / panel_context.hpp precedent.
}  // namespace engine

namespace engine::editor {

// A world-space ray. `direction` is UNIT LENGTH by contract; a ZERO direction means "unbuildable"
// (a non-finite input reached viewportRay) and every consumer treats it as a guaranteed miss.
struct Ray {
    Vec3 origin{};
    Vec3 direction{};
};

// ---- tuning constants (D5/D10/D18) -- LOGICAL POINTS, judged by the human pass -------------------
// "The mouse did not move." OURS, deliberately independent of io.MouseDragThreshold (6.0f, F24):
// that is a DRAG threshold for widgets that drag, nothing in the Viewport reads it, and keeping this
// independent is what lets the gate take a plain float instead of an ImGui global.
inline constexpr float PICK_CLICK_SLOP_POINTS = 4.0F;
// A non-mesh entity's clickable disc: a light has no geometry and would otherwise be unclickable
// forever (D5). Small and precise, in the same units as the mouse (D18).
inline constexpr float POINT_PICK_RADIUS_POINTS = 8.0F;
// "In front of the eye" (D14). This engine is RH / -Z forward with clip Z in [0,1] (ADR-005), so the
// near plane is z_clip = 0 and the in-front test is w > 0 -- NOT z > -w. Do not flip Y for Vulkan.
inline constexpr float CLIP_W_EPSILON = 1.0e-4F;

// ---- the viewport's screen <-> world mapping ----------------------------------------------------

// Mouse position -> NDC, y UP. All three arguments in POINTS (D18). Returns {0,0} -- the CENTRE, and
// therefore the centre ray, which is total and harmless -- for a non-positive or NON-FINITE size, or
// for a non-finite mouse/origin.
[[nodiscard]] Vec2 viewportNdc(Vec2 mousePoints, Vec2 imageOriginPoints, Vec2 imageSizePoints) noexcept;

// NDC (y up) -> viewport-local POINTS (y down), {0,0} at the image's top-left. The exact inverse of
// viewportNdc's mapping with imageOrigin = {0,0}; picking_test.cpp case 3 asserts the round trip.
// Deliberately unguarded: every caller either supplies a checked rect or gates the RESULT on
// finiteness, and a guard here would hide a bad input instead of rejecting it.
[[nodiscard]] Vec2 ndcToViewportPoints(Vec2 ndc, Vec2 viewportSizePoints) noexcept;

// World point -> viewport-local POINTS. False when the point is at or behind the eye
// (w <= CLIP_W_EPSILON) or when any result is non-finite (E4) -- ImDrawList would happily consume a
// NaN and corrupt the whole frame's vertex buffer.
[[nodiscard]] bool projectToViewport(const Mat4& viewProj, Vec3 worldPoint, Vec2 viewportSizePoints,
                                     Vec2& outPoints) noexcept;

// Clips a CLIP-SPACE segment to w > CLIP_W_EPSILON, interpolating IN CLIP SPACE, BEFORE the
// perspective divide (D14) -- lerping the DIVIDED endpoints is the classic bug that bends straight
// lines. Returns false when the whole segment is behind. Mutates its arguments in place; the endpoint
// that was already in front is left untouched.
[[nodiscard]] bool clipSegmentToNearPlane(Vec4& a, Vec4& b) noexcept;

// The D4 basis construction:
//     dir = forward() + right()*(ndc.x*aspect*tanHalf) + up()*(ndc.y*tanHalf)
// No matrix inverse, no near/far dependence, independent of the clip-depth convention, and with known
// closed forms at the centre and the four corners. `aspect` is width/height (UNITLESS -- the ONE
// number that comes from PIXELS, D18). Returns a ZERO direction, never an assert, for non-finite
// inputs: engine::normalize ASSERTS on a zero-length vector, so normalizeOrZero is used instead.
[[nodiscard]] Ray viewportRay(const EditorCamera& camera, Vec2 ndc, float aspect) noexcept;

// Ray vs the axis-aligned box [-halfExtent, halfExtent]^3, IN THE SPACE OF THE ARGUMENTS.
// `direction` is deliberately NOT required to be unit length: pass the LOCAL-space direction
// UNNORMALISED and `outT` comes back in WORLD units (D2). That single omission is what makes hits
// from entities with wildly different scales comparable, so "nearest wins" means something.
// Returns false unless tMin > 0 -- ENTRY hits only, never "the camera is inside the box" (D3).
// `outT` is left UNTOUCHED on a miss.
[[nodiscard]] bool rayLocalBoxHit(Vec3 origin, Vec3 direction, float halfExtent, float& outT) noexcept;

}  // namespace engine::editor
