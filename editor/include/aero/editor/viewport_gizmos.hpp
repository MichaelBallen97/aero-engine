#pragma once
// Aero Engine — the viewport's icon + gizmo walk (task E.2.3). The ONE call site is
// ViewportPanel::renderScene, behind the Gizmos toggle: editor chrome, never scene content, never
// serialized, never exported (emitDebugGrid's rule; VG15 and I131 are what keep it true).
//
// PUBLIC and ImGui-free. It DOES name render:: types -- DebugDrawBatch, LightGizmoStyle -- which is
// legal because aero::render is PUBLIC on aero_editor_core (editor/CMakeLists.txt,
// material_edit.hpp's precedent). It must NEVER name a scene_render:: type: that library is PRIVATE
// there, which on a STATIC library propagates as $<LINK_ONLY:...>, so engine/scene_render/include
// does not reach aero_editor_shell_test and such a header would not compile there at all. The one
// scene_render call this feature makes -- activeDirectionalLight -- is made by the PANEL, and its
// answer arrives here as a plain Entity.
//
// eachEntity + has/get ONLY, never World::each<T>, which logs one AERO_LOG_ERROR PER CALL for an
// unregistered type straight into 2.2.5's Console. Emits NO log record on any path (VG14), and
// allocates nothing once its caller-owned scratch is warm.

#include <aero/core/math.hpp>
#include <aero/editor/selection_overlay.hpp>  // MAX_HIGHLIGHTED_ENTITIES
#include <aero/editor/viewport_icons.hpp>
#include <aero/render/debug_draw.hpp>
#include <aero/render/debug_grid.hpp>   // DEBUG_GRID_MAX_LINES, for the budget static_assert
#include <aero/render/light_gizmo.hpp>  // LightGizmoStyle, LIGHT_GIZMO_MAX_LINES_PER_ENTITY
#include <aero/scene/entity.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine {
class World;  // forward-declared, as in picking.hpp; viewport_gizmos.cpp includes world.hpp itself
}  // namespace engine

namespace engine::editor {

// LINEAR RGBA, every one of them -- the batch's convention. The two SELECTED tints are E.1.4's
// OUTLINE colours decoded from sRGB, so selection reads identically for a mesh and for a light; the
// sRGB bytes are in the comments and VG13 pins each channel by pushing it back through
// render::linearToSrgbEncode and asserting the byte comes back, which is AX1's form and a real pin
// rather than a restatement. ALPHA IS NOT GAMMA-ENCODED, so those two channels round-trip without one.
//
// AND THE BYTES ON SCREEN ARE NOT THESE BYTES. A billboard goes THROUGH 3.6.3's tonemap; the outline
// composites AFTER it (selection_outline.hpp: "amber must stay amber regardless of exposure"). They
// agree on screen only under TonemapOperator::None at exposure 1. Stated, measured on the validation
// page, and not fixable inside this task -- moving the icons after the tonemap is D2's rejected
// ImDrawList design.
struct ViewportGizmoTints {
    // The NOLINT is not a waiver: 176/255 decoded through the sRGB EOTF is 0.43415, which happens to
    // sit 9.4e-05 from std::numbers::log10e and trips modernize-use-std-numbers. Every accurate
    // spelling of this colour does -- it is a coincidence of the constant, not a missed abstraction.
    // NOLINTNEXTLINE(modernize-use-std-numbers)
    Vec4 primarySelected{1.0F, 0.4342F, 0.0513F, 1.0F};       // sRGB 255,176,64,255 -- ..._PRIMARY_DEFAULT
    Vec4 secondarySelected{1.0F, 0.2961F, 0.0144F, 0.7451F};  // sRGB 255,148,32,190 -- ..._SECONDARY_DEFAULT
    Vec4 unselected{0.62F, 0.64F, 0.68F, 0.90F};              // chrome, brighter than the grid's line colour
    Vec4 mutedDirectional{0.30F, 0.31F, 0.33F, 0.35F};        // "the bridge ignored this one"
    bool operator==(const ViewportGizmoTints&) const = default;
};

struct ViewportGizmoParams {
    std::span<const Entity> selected{};  // SELECTION ORDER, the buildSelectionMaskSet convention
    Entity primary{};
    // The winner of scene_render::activeDirectionalLight, resolved ONCE by the caller. An INVALID
    // handle means "the scene has no directional light", under which NOTHING is muted -- never
    // "everything is muted", which is what a defaulted-invalid mistake would produce (VG7).
    Entity activeDirectional{};
    float iconSizePixels = VIEWPORT_ICON_SIZE_POINTS;  // points * framebufferScale, resolved by the caller
    std::uint32_t entityCap = static_cast<std::uint32_t>(MAX_HIGHLIGHTED_ENTITIES);
    ViewportGizmoTints tints{};
    // `color` is OVERWRITTEN per entity from `tints`, so the style's own colour is never read. Its
    // `segments` IS read, and the budget static_assert below holds only at the DEFAULT value -- see
    // LIGHT_GIZMO_MAX_LINES_PER_ENTITY's own comment for the general bound.
    render::LightGizmoStyle gizmoStyle{};
};

struct ViewportGizmoCounts {
    std::uint32_t icons = 0;  // billboards the batch ACCEPTED
    // THE BUDGET, AND NOTHING ELSE. A non-finite world origin never reaches the batch at all -- the
    // walk's own finiteness gate skips it -- so it is counted NOWHERE, neither here nor in `icons`,
    // exactly as an emitter that returns 0 lines is counted nowhere (AC-12). The only other route
    // into billboard()'s rejection branch from here is a CALLER handing over a non-finite or
    // non-positive iconSizePixels, which is a broken caller rather than a degenerate world.
    std::uint32_t iconsDropped = 0;
    std::uint32_t gizmoLines = 0;      // lines the batch ACCEPTED
    std::uint32_t gizmoEntities = 0;   // selected entities that emitted a gizmo
    std::uint32_t skippedOverCap = 0;  // selected entities past entityCap
    bool operator==(const ViewportGizmoCounts&) const = default;
};

// CALLER-OWNED SCRATCH, cleared on entry and reused across frames -- RenderViewScratch's contract,
// verbatim. It exists so the selection membership test is a binary search over a sorted vector of
// entity INDICES rather than a linear scan per entity: at 256 selected and a few thousand entities
// the naive form is over a million comparisons EVERY FRAME.
struct ViewportGizmoScratch {
    std::vector<std::uint32_t> selectedIndices;
};

// PURE apart from the batch it mutates. Pushes gizmo LINES first and icons (billboards) second, which
// matches flush()'s own draw order and costs nothing either way -- the two live in separate budgets
// and cannot compete.
[[nodiscard]] ViewportGizmoCounts emitViewportGizmos(const World& world, const ViewportGizmoParams& params,
                                                     ViewportGizmoScratch& scratch, render::DebugDrawBatch& batch);

// DERIVED, and it is what cashes in debug_grid.hpp's "with room left for E.2.3".
// 2304 + 256*96 = 26 880 <= 32 768, with 5 888 spare.
//
// IT HOLDS AT THE DEFAULT LightGizmoStyle, which is the only one anything in this tree constructs.
// A caller that overrides `segments` scales every emitter with it and must re-derive its own bound;
// LIGHT_GIZMO_MAX_LINES_PER_ENTITY's own comment states the general form.
static_assert(render::DEBUG_GRID_MAX_LINES + (static_cast<std::uint32_t>(MAX_HIGHLIGHTED_ENTITIES) *
                                              render::LIGHT_GIZMO_MAX_LINES_PER_ENTITY) <=
                  render::DebugDrawBudget{}.maxLines,
              "the grid and a fully selected scene's gizmos must both fit E.1.1's default line budget");

}  // namespace engine::editor
