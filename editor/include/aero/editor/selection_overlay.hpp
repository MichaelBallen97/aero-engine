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

// The builder. CALLER-OWNED SCRATCH (the walkForest / RenderViewScratch precedent): CLEARED ON ENTRY,
// reused across frames, never allocating once warm.
//
// task E.1.4: `entities` IS THE MARKER LIST -- the selected entities that produced NO instance at
// all, as decided by scene_render::buildSelectionMaskSet. THIS BUILDER NO LONGER DECIDES WHAT HAS
// GEOMETRY; it draws a diamond for everything it is given, and everything else in the selection gets
// a GPU silhouette from the mask pass instead.
//
// WHY THAT IS ONE DECISION RATHER THAN TWO. The mask pass answers "has geometry" from the
// AssetBindingTable; this builder used to answer it from a MeshBoundsLookup. Any frame in which the
// two disagreed produced an entity with NO OUTLINE AND NO MARKER -- visually unselected, with every
// automated observable green. There is now exactly one answer, made once per tick and consumed
// twice.
//
// Per entity, in SELECTION order, capped at MAX_HIGHLIGHTED_ENTITIES:
//   * a 4-segment screen-space diamond at its world origin                             (D5/D8)
//   * dead / null / behind the eye / non-finite -> NOTHING, silently                   (E2/E4/E7)
//
// Mutates NEITHER the World NOR the Selection (AC-17) -- and specifically does NOT call
// Selection::prune: mutating the selection during a draw walk is forbidden, and pruning is 2.2.1's
// job, done by the Hierarchy at the top of ITS onDraw (F34). Dead handles are skipped here instead.
// Takes a span + a primary handle rather than a const Selection& so a tier-0 test can drive it from a
// plain array, and so it cannot even be TEMPTED to mutate the selection.
void buildSelectionOverlay(const World& world, std::span<const Entity> entities, Entity primary, const Mat4& viewProj,
                           Vec2 viewportSizePoints, std::vector<OverlaySegment>& scratch);

}  // namespace engine::editor
