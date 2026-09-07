#pragma once
// Aero Engine — render::emitLightGizmo* (task E.2.3): the three light gizmos, as world-space lines.
//
// PURE ARITHMETIC, the debug_grid.hpp posture: no GPU, no rhi type, no logging, no allocation, no
// static state, and no scene type -- a light reaches this file as a position, a direction, a radius
// and two angles, never as an Entity. That is what keeps engine/render scene-free (the golden rule)
// and what lets aero_tests drive every shape with no device and no editor.
//
// IT DOES NOT INHERIT debug_grid.hpp'S BIT-DETERMINISM CLAIM, AND THAT IS DELIBERATE. The cone needs
// cos(outer) and sin(outer), and wireCircle needs cos/sin per segment; neither is required by
// IEEE-754 to be correctly rounded, so two libm implementations may differ in the last ulp. The libm
// list is sqrt, sin, cos and isfinite. Every GZ assertion over an emitted vertex is therefore
// written with an EPSILON -- except the rim-ray endpoints, which are exact because they are built
// from debugCircleBasis and from the SAME TWO_PI * i / n expression wireCircle itself evaluates, so
// both sides of that comparison come from ONE computation rather than two that agree by rounding.
// Each emitter hands wireCircle the caller's RAW direction rather than its own normalized copy, for
// exactly that reason: debugCircleBasis is deterministic in its argument, so passing the same
// argument twice is what makes the two bases the same bits. Normalizing first would make the
// emitter's basis and wireCircle's differ by an ulp whenever dot(axis, axis) is not exactly 1.
//
// EVERYTHING GOES INTO DebugDepth::Overlay BY DEFAULT (the style's own field): a light gizmo is meant
// to be seen through the wall the light is behind, which is the editor convention every 3D tool
// follows and what debug_draw.hpp's Overlay comment named this task for.
//
// TOTAL, LIKE emitDebugGrid. A non-finite position, direction, radius or angle emits NOTHING, returns
// 0, and adds NO rejection to the batch: a badly-authored light is not a bad push. A legal-but-
// degenerate shape (outer == 0, outer == HALF_PI) has a STATED PICTURE, documented per emitter, and
// never reaches wireCircle with a non-positive radius.

#include <aero/core/math.hpp>
#include <aero/render/debug_draw.hpp>

#include <cstdint>

namespace engine::render {

// ---- tuning constants (the DebugGridStyle posture: named, so a retune is a one-line change, and
//      every tier-0 case asserts a RELATIONSHIP rather than a magnitude) --------------------------
inline constexpr std::uint32_t LIGHT_GIZMO_CIRCLE_SEGMENTS = 32;
inline constexpr std::uint32_t LIGHT_GIZMO_SPOT_RIM_RAYS = 4;     // apex -> rim, ON the outer circle
inline constexpr std::uint32_t LIGHT_GIZMO_DIRECTIONAL_RAYS = 8;  // disc -> forward, along the axis
inline constexpr float LIGHT_GIZMO_DIRECTIONAL_RADIUS = 0.5F;     // world units; a directional light
inline constexpr float LIGHT_GIZMO_DIRECTIONAL_LENGTH = 2.0F;     // has no position-dependent extent

// A ray must land ON an emitted circle vertex, not merely on the circle. Both of these make
// `segments / rays` an exact integer stride, which is what lets the emitters reuse wireCircle's own
// TWO_PI * i / n expression verbatim.
static_assert(LIGHT_GIZMO_CIRCLE_SEGMENTS % LIGHT_GIZMO_SPOT_RIM_RAYS == 0);
static_assert(LIGHT_GIZMO_CIRCLE_SEGMENTS % LIGHT_GIZMO_DIRECTIONAL_RAYS == 0);
// Two statements rather than one conjunction, so neither line approaches the column limit the two
// clang-format-18 builds disagree about.
static_assert(LIGHT_GIZMO_CIRCLE_SEGMENTS >= MIN_CIRCLE_SEGMENTS,
              "the default segment count must survive wireCircle's own clamp unchanged");
static_assert(LIGHT_GIZMO_CIRCLE_SEGMENTS <= MAX_CIRCLE_SEGMENTS,
              "the default segment count must survive wireCircle's own clamp unchanged");

// DERIVED, NEVER A LITERAL, and it is the POINT light that sets it: wireSphere is exactly three
// wireCircle calls. The spot emits 2*segments + rays (68 at the defaults) and the directional
// segments + rays (40), both strictly below.
//
// IT IS A BOUND AT THE DEFAULT STYLE, which is the only style anything in this tree constructs, and
// the editor's own line budget rests on that. A caller that overrides LightGizmoStyle::segments
// scales every emitter with it: writing n for clamp(style.segments, MIN_CIRCLE_SEGMENTS,
// MAX_CIRCLE_SEGMENTS), the point emits 3n, the directional n + DIRECTIONAL_RAYS and the spot
// 2n + SPOT_RIM_RAYS, so the general bound is the MAX of those three -- which is 3n for every
// n >= 4 and is 11 at n == 3, where the ray counts still dominate. GZ17 asserts both halves rather
// than the stronger claim that is not true.
inline constexpr std::uint32_t LIGHT_GIZMO_MAX_LINES_PER_ENTITY = 3U * LIGHT_GIZMO_CIRCLE_SEGMENTS;

// ---- style ---------------------------------------------------------------------------------------
// LINEAR colour, like everything the batch consumes: the HDR target is linear and 3.6.3's resolve
// encodes. The caller sets `color` per light from its OWN tint table; this header states NO palette
// (the editor owns colour -- axis_palette.hpp's rule, one layer over).
struct LightGizmoStyle {
    Vec4 color{0.62F, 0.64F, 0.68F, 0.90F};
    // The spot's INNER cone circle, as a multiple of color.w. A documented EXPECTATION that it is
    // <= 1, never a clamp: a caller who inverts it gets what they asked for (DebugGridStyle's rule).
    float innerAlphaScale = 0.55F;
    std::uint32_t segments = LIGHT_GIZMO_CIRCLE_SEGMENTS;
    DebugDepth depth = DebugDepth::Overlay;
    bool operator==(const LightGizmoStyle&) const = default;
};

// ---- the three emitters ---------------------------------------------------------------------------
// Each returns the number of LINES THE BATCH ACCEPTED -- read off the batch (lineCount() after minus
// before), never a running sum of what the emitter MEANT to push, which is the only form that reports
// the truth when the budget runs out mid-wireCircle.

struct DirectionalGizmoParams {
    Vec3 origin{};
    Vec3 direction{0.0F, 0.0F, -1.0F};  // the entity's -Z WORLD axis; a zero or non-finite one emits nothing
    LightGizmoStyle style{};
    bool operator==(const DirectionalGizmoParams&) const = default;
};

// A disc of radius LIGHT_GIZMO_DIRECTIONAL_RADIUS in the plane through `origin` perpendicular to the
// aim, plus LIGHT_GIZMO_DIRECTIONAL_RAYS parallel rays of length LIGHT_GIZMO_DIRECTIONAL_LENGTH
// starting ON emitted disc vertices. A directional light has no position-dependent extent, so both
// magnitudes are pure chrome and neither is asserted by value anywhere.
[[nodiscard]] std::uint32_t emitDirectionalLightGizmo(DebugDrawBatch& batch, const DirectionalGizmoParams& params);

struct PointGizmoParams {
    Vec3 center{};
    // <= 0 or non-finite emits NOTHING. PointLight::range is documented "> 0 by convention (not
    // validated)", so a non-positive one is the ordinary state of a hand-edited component.
    float range = 10.0F;
    LightGizmoStyle style{};
    bool operator==(const PointGizmoParams&) const = default;
};

// One wireSphere at `range` -- three great circles on the world axes, 3 * segments lines.
[[nodiscard]] std::uint32_t emitPointLightGizmo(DebugDrawBatch& batch, const PointGizmoParams& params);

struct SpotGizmoParams {
    Vec3 apex{};
    Vec3 direction{0.0F, 0.0F, -1.0F};
    float range = 10.0F;
    float innerConeRadians = 0.0F;
    float outerConeRadians = 0.0F;
    LightGizmoStyle style{};
    bool operator==(const SpotGizmoParams&) const = default;
};

// THE RIM IS ON THE RANGE SPHERE. The outer circle sits at axial distance range*cos(outer) with
// radius range*sin(outer), so every rim point is EXACTLY `range` from the apex and every rim ray is
// exactly `range` long. The flat range*tan(outer) form is NOT used: outerConeRadians is clamped to
// exactly HALF_PI by the component (spot_light.hpp's AERO_RANGE) and tan(HALF_PI) is infinite there.
//
// The INNER circle is emitted only when inner < outer. That MIRRORS resolveSpotCone's fold of
// inner >= outer into a hard edge at outer -- it does NOT read it: a SpotCone carries no bit saying
// its delta was floored, so there is nothing to carry. GZ8 is what ties the two, by driving the same
// angles through both.
//
// DEGENERATE BUT LEGAL, each with a stated picture rather than a rejection:
//   outer == 0            -> no circle (radius 0 would be ONE wireCircle rejection); a single axis
//                            line of length `range` instead, which is what a cone with no aperture is
//   outer == HALF_PI      -> a great circle of radius `range` THROUGH the apex; the rays have length
//                            `range` and lie in that plane. Finite, drawn, correct
//   inner >= outer        -> outer only
//   non-finite angle      -> NOTHING, and no rejection: resolveSpotCone calls that light OFF, and an
//                            off light draws no cone
//   range <= 0 / non-finite -> NOTHING
[[nodiscard]] std::uint32_t emitSpotLightGizmo(DebugDrawBatch& batch, const SpotGizmoParams& params);

}  // namespace engine::render
