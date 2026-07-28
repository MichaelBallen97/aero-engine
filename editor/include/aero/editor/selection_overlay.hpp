#pragma once
// Aero Engine — engine::editor::buildSelectionOverlay (task 2.3.2): the selection, turned into a list
// of 2-D screen-space line segments the Viewport panel draws with ImDrawList::AddLine.
//
// PUBLIC, ImGui-free, entt-free AND render-free BY RULE, held by FILE PLACEMENT like every header
// under editor/include (2.1.3 D9) -- see editor_camera.hpp's header comment for the full reasoning
// (R12: no guard can enforce this against aero_editor_shell_test's link line).
//
// THIS HEADER CONTAINS NO DRAWING, ONLY GEOMETRY. That is the whole point of D6: the highlight's
// correctness -- is there a box, is it in the right place, does it have twelve edges, is it clipped
// at the near plane, does the primary read differently -- becomes TIER-0 ASSERTABLE with no window,
// no GPU and no ImGui context. The panel keeps only the two colour constants and the AddLine loop,
// which are the parts no assertion here could check anyway.
//
// Depends on picking.hpp for the shared screen mapping (projectToViewport / clipSegmentToNearPlane /
// ndcToViewportPoints). The dependency runs ONE WAY ONLY, overlay -> picking, so there is no header
// cycle and exactly ONE implementation of world -> screen exists in the tree. That single sourcing is
// what guarantees you can never click one place and see the box somewhere else.

#include <aero/core/math.hpp>
#include <aero/editor/picking.hpp>
#include <aero/scene/entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine {
class World;  // forward-declared, as in picking.hpp; selection_overlay.cpp includes world.hpp itself
}  // namespace engine

namespace engine::editor {

// How the panel should STYLE a segment. Deliberately NOT a colour: an ImU32 in a public editor header
// would break the ImGui-free rule (D1/D6). The panel maps role -> colour + thickness at its single
// call site.
enum class OverlayRole : std::uint8_t { Selected = 0, Primary };

// One line to draw, in VIEWPORT-LOCAL LOGICAL POINTS (D18): {0,0} is the image's TOP-LEFT corner. The
// panel adds imageOrigin to get screen coordinates.
struct OverlaySegment {
    Vec2 a{};
    Vec2 b{};
    OverlayRole role = OverlayRole::Selected;
};

// D15 -- a CONTRACT, never a log line. 12 segments per entity means a 10 000-entity selection would
// build 120 000 segments PER FRAME; a per-frame log line would be unbounded spam straight into
// 2.2.5's Console. Capping in the API and asserting it in a test is the honest way to bound coverage.
inline constexpr std::size_t MAX_HIGHLIGHTED_ENTITIES = 256;
// The D8 diamond's half-diagonal, in POINTS.
inline constexpr float POINT_MARKER_HALF_POINTS = 6.0F;

// The 12 edges of the local box, as index pairs into the 8-corner enumeration this file SHARES with
// editor/src/scene_bounds.cpp (F3b): corner i is {(i&1)?+H:-H, (i&2)?+H:-H, (i&4)?+H:-H}, so bit 0 is
// X, bit 1 is Y and bit 2 is Z. Two corners are adjacent iff they differ in EXACTLY ONE bit, which
// makes the table DERIVED rather than transcribed: the edges are (i, i^1) for the four i with bit 0
// clear, (i, i^2) for the four with bit 1 clear, and (i, i^4) for the four with bit 2 clear.
// PUBLIC so that derivation is testable at all -- the same call D15 makes for the cap above.
struct BoxEdge {
    std::uint8_t a = 0;
    std::uint8_t b = 0;
};
inline constexpr std::array<BoxEdge, 12> BOX_EDGES{{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7},  // X edges: (i, i^1) for i in {0,2,4,6}
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7},  // Y edges: (i, i^2) for i in {0,1,4,5}
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},  // Z edges: (i, i^4) for i in {0,1,2,3}
}};

// The builder. CALLER-OWNED SCRATCH (the walkForest / RenderViewScratch precedent): CLEARED ON ENTRY,
// reused across frames, never allocating once warm.
//
// Per entity, in SELECTION order, capped at MAX_HIGHLIGHTED_ENTITIES:
//   * WITH a MeshRenderer  -> the 12 edges of [-H,H]^3 through worldMatrix(e)          (D7)
//   * WITHOUT one          -> a 4-segment screen-space diamond at its world origin     (D5/D8)
//   * dead / null / behind the eye / non-finite -> NOTHING, silently                   (E2/E4/E7)
//
// D7: the entity's OWN oriented box, never its subtree's AABB. Selecting a parent draws the parent's
// box only. This deliberately differs from F-focus, which frames the whole subtree: the highlight
// says WHAT IS SELECTED, focus says WHAT YOU WANT TO SEE -- and a subtree union would force it back
// to a world AABB (an OBB union is not an OBB), reintroducing exactly the rotated-object mismatch D2
// removes.
//
// Mutates NEITHER the World NOR the Selection (AC-17) -- and specifically does NOT call
// Selection::prune: mutating the selection during a draw walk is forbidden, and pruning is 2.2.1's
// job, done by the Hierarchy at the top of ITS onDraw (F34). Dead handles are skipped here instead.
// Takes a span + a primary handle rather than a const Selection& so a tier-0 test can drive it from a
// plain array, and so it cannot even be TEMPTED to mutate the selection.
void buildSelectionOverlay(const World& world, std::span<const Entity> entities, Entity primary, const Mat4& viewProj,
                           Vec2 viewportSizePoints, std::vector<OverlaySegment>& scratch);

}  // namespace engine::editor
