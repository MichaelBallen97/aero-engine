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

// "In front of the eye" in ORTHOGRAPHIC (task E.1.3). An ortho proj's bottom row is (0,0,0,1) and the
// view matrix is affine, so clip.w does not depend on the world point at all and the w test above is
// VACUOUS. Under ADR-005's [0,1] clip volume the near plane IS z_clip == 0, so this is the correct
// gate: glm::orthoRH_ZO gives clip.z = -(z_view + zNear) / (zFar - zNear).
//
// THE TWO GATES ARE NOT EQUIVALENT, AND THE DIFFERENCE IS ACCEPTED. Perspective's `w > 0` means
// "in front of the EYE" and admits a point closer than nearPlane; ortho's `z > 0` means "beyond the
// NEAR PLANE" and rejects it. So an entity 0.05 units in front of the eye draws its selection box in
// perspective and does not in ortho. Making the perspective gate z-based too is strictly more correct
// and is 2.3.2's contract to change, not this task's -- it is recorded as an unowned handoff, and
// PK14 asserts BOTH arms so the asymmetry is recorded rather than discovered.
//
// THE MAGNITUDE IS A NORMALISED-DEPTH EPSILON, NOT A WORLD ONE, and is not derived from anything.
// Its world scale follows the depth range: 1e-6 * (farPlane - nearPlane), so about 1 mm at the
// shipped defaults and about 10 cm at farPlane = 100000. That is acceptable because this is a
// STRICTNESS KNOB on a "behind the eye" refusal rather than a correctness threshold -- PK14 asserts
// the two ARMS and never the value, so a later retune reddens nothing.
inline constexpr float CLIP_Z_EPSILON = 1.0e-6F;

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
// (perspective: w <= CLIP_W_EPSILON; orthographic: z <= CLIP_Z_EPSILON) or when any result is
// non-finite (E4) -- ImDrawList would happily consume a NaN and corrupt the whole frame's vertex
// buffer.
//
// `mode` is NON-DEFAULTED on purpose (task E.1.3 D12): a default would let a future call site silently
// take the perspective arm under an orthographic camera, which is a wrong picture with no error, and
// there is no answer that is right in both modes. Every existing call site was converted explicitly.
[[nodiscard]] bool projectToViewport(const Mat4& viewProj, ProjectionMode mode, Vec3 worldPoint,
                                     Vec2 viewportSizePoints, Vec2& outPoints) noexcept;

// Clips a CLIP-SPACE segment to the mode's own in-front test, interpolating IN CLIP SPACE, BEFORE the
// perspective divide (D14) -- lerping the DIVIDED endpoints is the classic bug that bends straight
// lines. Returns false when the whole segment is behind. Mutates its arguments in place; the endpoint
// that was already in front is left untouched. `mode` is non-defaulted for the reason above, and
// trails the two mutated arguments because those read best adjacent.
[[nodiscard]] bool clipSegmentToNearPlane(Vec4& a, Vec4& b, ProjectionMode mode) noexcept;

// The D4 basis construction:
//     dir = forward() + right()*(ndc.x*aspect*tanHalf) + up()*(ndc.y*tanHalf)
// No matrix inverse, no near/far dependence, independent of the clip-depth convention, and with known
// closed forms at the centre and the four corners. `aspect` is width/height (UNITLESS -- the ONE
// number that comes from PIXELS, D18). Returns a ZERO direction, never an assert, for non-finite
// inputs: engine::normalize ASSERTS on a zero-length vector, so normalizeOrZero is used instead.
//
// Task E.1.3: it reads `camera.projectionMode()` itself rather than taking the mode as a parameter,
// because it already takes the whole camera -- so a PARALLEL arm is the ortho answer, with the ORIGIN
// varying across the image plane and the DIRECTION constant:
//     origin = position() + right()*(ndc.x*halfW) + up()*(ndc.y*halfH)   dir = forward()
// The origin therefore sits on the eye PLANE rather than at the eye POINT, which is exactly why
// rayLocalBoxHit's entry-hits-only rule (tMin > 0) makes ortho picking correct with no change: a box
// behind that plane yields t < 0 and is correctly missed.
[[nodiscard]] Ray viewportRay(const EditorCamera& camera, Vec2 ndc, float aspect) noexcept;

// Where a dropped asset lands, given the drop ray (task 3.1.5). A TOTAL function: every input yields a
// FINITE point. It lives here rather than beside the drag payload because it is the fourth member of
// the screen<->world family this header already owns, and putting it anywhere else would give the tree
// a second place where a ray meets a plane.
inline constexpr float DROP_FALLBACK_DISTANCE = 5.0F;  // world units along the ray
inline constexpr float DROP_PLANE_EPSILON = 1.0e-6F;   // |dir.y| below this is "parallel to y = 0"
[[nodiscard]] Vec3 dropPlacementPoint(const Ray& ray) noexcept;

// Ray vs the axis-aligned box [-halfExtent, halfExtent]^3, IN THE SPACE OF THE ARGUMENTS.
// `direction` is deliberately NOT required to be unit length: pass the LOCAL-space direction
// UNNORMALISED and `outT` comes back in WORLD units (D2). That single omission is what makes hits
// from entities with wildly different scales comparable, so "nearest wins" means something.
// Returns false unless tMin > 0 -- ENTRY hits only, never "the camera is inside the box" (D3).
// `outT` is left UNTOUCHED on a miss.
[[nodiscard]] bool rayLocalBoxHit(Vec3 origin, Vec3 direction, float halfExtent, float& outT) noexcept;

// Ray vs the axis-aligned box `box`, IN THE SPACE OF THE ARGUMENTS -- the min/max generalisation of the
// half-extent overload above, and the one every real mesh uses. A ZERO-THICKNESS axis is LEGAL and is
// the flat Plane primitive's own shape: the precondition is min <= max on all three axes, never
// min < max -- i.e. exactly Aabb::valid(), which already exists and already means this. Entry hits only
// (tMin > 0), outT untouched on a miss -- both semantics identical to the overload above.
//
// This one is the PRIMARY: the half-extent overload delegates to it, so there is ONE slab ladder in the
// tree rather than two that can drift apart.
[[nodiscard]] bool rayLocalBoxHit(Vec3 origin, Vec3 direction, const Aabb& box, float& outT) noexcept;

// One click, in the units the pure functions want. Built by ViewportPanel::updatePick from ImGui
// values; built by hand in a tier-0 test.
struct PickRequest {
    Vec2 ndc{};                 // the click, NDC, y UP
    float aspect = 1.0F;        // width/height, derived from drawExtent in PIXELS -- UNITLESS (D18)
    Vec2 viewportSizePoints{};  // the image rect's size, POINTS -- for the screen-space radius (D5)
    float pointRadiusPoints = POINT_PICK_RADIUS_POINTS;
    // task 3.1.5: the resolved local boxes of referenced meshes. A DEFAULTED MEMBER on an aggregate, so
    // every existing PickRequest{...} designated initializer still compiles. Null means "primitives
    // only", which is exactly what this walk saw before references existed.
    const MeshBoundsLookup* meshBounds = nullptr;
};

struct PickResult {
    Entity entity{};        // an INVALID handle is the miss -- never a null pointer, never a throw
    float distance = 0.0F;  // WORLD units from the eye. Kept even on a point hit, because
                            // per-triangle picking (Handoffs) wants this box pass as its broadphase.
    bool isPoint = false;   // true when a NON-MESH entity won its screen-space disc (D5)
    [[nodiscard]] bool hit() const noexcept { return entity.valid(); }
};

// The world walk. O(N) with one Mat4 inverse per mesh entity, run ONCE PER CLICK, not per frame -- a
// BVH is the right answer at a scale this repository does not have (A8).
//
// const World& is LOAD-BEARING, not stylistic: it is what structurally forbids World::each<T>, which
// logs one AERO_LOG_ERROR PER CALL for an unregistered type -- straight into 2.2.5's Console. This
// walk uses eachEntity + has/get exclusively, all of them const and all of them silent (F15/F17).
// It emits NO log record on ANY path (AC-11/INV-5). Do not "improve" it into each<MeshRenderer>.
[[nodiscard]] PickResult pickEntity(const World& world, const EditorCamera& camera, const PickRequest& request);

// What a click should DO. Mirrors the Hierarchy's modifier vocabulary (F5) and drops Range, which has
// no meaning in a viewport (there is no linear order over candidates to extend along).
enum class PickAction : std::uint8_t { None = 0, Select, Toggle, Add, Clear };

// The D9 table:
//   hit, no modifier   -> Select   replace the selection with exactly this entity
//   hit, Ctrl/Cmd      -> Toggle   add if absent, remove if present
//   hit, Shift         -> Add      add if absent; a NO-OP if present -- never removes
//   miss, no modifier  -> Clear    the universal "click empty space to deselect"
//   miss, any modifier -> None     a user extending a selection who misses by two pixels must not
//                                  lose what they have been building
// Ctrl/Cmd is tested BEFORE Shift, so the matrix is total and order-independent.
//
// PURE, and takes no Selection, so a tier-0 test drives all sixteen rows with no World and no panel.
// `alreadySelected` is DELIBERATELY not consulted: Toggle defers the add/remove choice to
// Selection::toggle and Add defers the no-op to Selection::add. It stays in the signature because
// that INDEPENDENCE is the contract, and picking_test.cpp case 12 asserts it by running every row
// with the flag both false and true. The definition leaves the name out (misc-unused-parameters is
// --warnings-as-errors on the Linux lane).
[[nodiscard]] PickAction pickSelectionAction(bool hit, bool alreadySelected, bool ctrlOrCmd, bool shift) noexcept;

// The ONE place a PickAction becomes a Selection mutation. Separated from the decision so the decision
// stays pure and this stays a five-arm switch with nothing to get wrong. Both writers of the shared
// Selection -- the Hierarchy and now the Viewport -- funnel through the same Selection mutators, so
// whatever 2.4.2 decides to wrap for undo, it wraps both call sites at once (D22).
void applyPickAction(Selection& selection, PickAction action, Entity entity);

}  // namespace engine::editor
