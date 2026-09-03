#pragma once
// Aero Engine — render::emitDebugGrid (task E.1.2): the editor's ground grid, as world-space lines.
//
// PURE ARITHMETIC. No GPU, no rhi type, no logging, no allocation, no static state, and the only
// libm functions it reaches are sqrt, floor and ceil -- all three of which IEEE-754 requires to be
// CORRECTLY ROUNDED, so this emitter is bit-deterministic on every lane. std::log10 and std::pow are
// deliberately absent (GR22 pins that as source text): neither is required to be correctly rounded,
// so their results differ between libm implementations, and every exact assertion in GR7, GR12 and
// GR6 would become a three-lane tolerance argument. docs/09 §13.7 already excludes
// render::sampleAnimation from the determinism contract for exactly that reason; this does not join
// it.
//
// THREE DECADES AT ONCE, AND THE GRID IS CONTINUOUS BY CONSTRUCTION. A cadence that SNAPS pops every
// time the camera dollies through a decade. This one draws s0 = 10^L, s1 = 10^(L+1), s2 = 10^(L+2)
// simultaneously with weights
//         w0 = a(1 - f)      w1 = b(1 - f) + a f      w2 = b f
// where a is the minor alpha, b the major alpha, and f in [0, 1] the position inside the decade.
// At f = 0 the weights are (a, b, 0) -- a fine grid with every tenth line stronger, which IS the
// major/minor look. As f -> 1 they become (0, a, b), and at that instant L increments: s1 becomes
// the new s0 (weight a), s2 becomes the new s1 (weight b), and the new s2 enters at weight 0. EVERY
// WORLD SPACING KEEPS ITS EXACT ALPHA ACROSS THE BOUNDARY. No threshold, no hysteresis. The radius
// R(s) = min(RADIUS_CELLS*s, FAR_FRACTION*farPlane) is a function of the SPACING ALONE, never of the
// level, so the extent is continuous for the same reason. GR8 and GR6 assert both halves.
//
// THE LATTICE IS WORLD-ABSOLUTE AND NOTHING IS SNAPPED. Every emitted line of cadence s lies at an
// exact multiple of s along the axis it is constant in (GR12), and the disc is CENTRED on the focus
// but not aligned to it. So the line at x = 0 really is the world's x = 0 -- which is what lets the
// axes replace it -- and nothing translates as the camera moves: lines enter and leave at the rim,
// where their alpha is already exactly zero.
//
// THE LINE COUNT IS BOUNDED BY THE CONSTANTS, NOT BY THE CAMERA. DEBUG_GRID_MAX_LINES below is
// derived, never a literal. The bound holds BY CONSTRUCTION rather than by arithmetic luck: the
// per-family index range is clamped to 2*RADIUS_CELLS and iterated with an INTEGER counter, because
// (a) the two divisions that produce kMin and kMax round independently and can each move floor/ceil
// by one at |centre/spacing| ~ 1e7, and (b) at |k| >= 2^24 the expression `k += 1.0F` is a NO-OP and
// a float-indexed loop NEVER TERMINATES -- measured at focus.x = 1e6 with spacing 0.01, which is an
// ordinary editor pose, not a pathological one.
//
// EDITOR CHROME, WITH EXACTLY ONE CALLER. This is a free function that mutates a batch it is handed:
// no global state, nothing registered, reachable only by being called. ViewportPanel::renderScene is
// the one call site in the whole tree, behind the viewport's Grid toggle, and I110's fourth subcase
// is what keeps it that way. It is never scene content, never serialized, never exported.
//
// NEAR AND FAR, stated rather than accidental. Near: nothing special -- a grid line closer than the
// near plane is clipped by the hardware, as any geometry is. Far: every radius is clamped to
// FAR_FRACTION * farPlane, and because the radial fade completes INSIDE that radius there is no hard
// edge at the far plane; the grid fades out before the frustum ends it.

#include <aero/core/math.hpp>
#include <aero/render/debug_draw.hpp>

#include <array>
#include <cstdint>

namespace engine::render {

// ---- the tuning constants (the EditorCamera posture: named, so retuning is a ONE-LINE change, and
//      every tier-0 case asserts a RELATIONSHIP rather than a magnitude) --------------------------
inline constexpr std::uint32_t DEBUG_GRID_CADENCES = 3;       // fading / steady / entering
inline constexpr float DEBUG_GRID_BASE = 10.0F;               // metric decades
inline constexpr float DEBUG_GRID_TARGET_DIVISIONS = 10.0F;   // cells across one viewScale
inline constexpr std::uint32_t DEBUG_GRID_RADIUS_CELLS = 24;  // cells from the centre to the rim
inline constexpr std::uint32_t DEBUG_GRID_FADE_SEGMENTS = 8;  // subdivisions per grid line
inline constexpr float DEBUG_GRID_FADE_INNER = 0.55F;         // the fade starts at this x radius
inline constexpr float DEBUG_GRID_FAR_FRACTION = 0.9F;        // radius ceiling as a fraction of farPlane
inline constexpr float DEBUG_GRID_MIN_VIEW_SCALE = 1.0e-3F;
inline constexpr float DEBUG_GRID_MAX_VIEW_SCALE = 1.0e6F;
inline constexpr int DEBUG_GRID_MIN_LEVEL = -6;
inline constexpr int DEBUG_GRID_MAX_LEVEL = 9;
inline constexpr float DEBUG_GRID_PLANE_HEIGHT = 0.0F;  // world Y of the ground plane

// DERIVED, NEVER A LITERAL: CADENCES x {lines along X, lines along Z} x (2*CELLS + 1) x SEGMENTS,
// plus the two axes. An UPPER bound: the disc clip and the far-plane clamp only ever reduce it, and
// the k == 0 skip removes CADENCES*2*SEGMENTS more whenever drawAxes is true. Measured: the default
// editor pose emits 2224 lines and the worst pose over an adversarial sweep emits 2304.
inline constexpr std::uint32_t DEBUG_GRID_MAX_LINES =
    DEBUG_GRID_CADENCES * 2U * ((2U * DEBUG_GRID_RADIUS_CELLS) + 1U) * DEBUG_GRID_FADE_SEGMENTS +
    (2U * DEBUG_GRID_FADE_SEGMENTS);
static_assert(DEBUG_GRID_MAX_LINES < DebugDrawBudget{}.maxLines,
              "the whole grid must fit E.1.1's DEFAULT line budget with room left for E.2.3");

// ---- style -------------------------------------------------------------------------------------
// LINEAR colours, like everything the batch consumes: the HDR target is linear and 3.6.3's resolve
// encodes. `lineColor` is shared by all three cadences and only the ALPHA distinguishes them --
// varying the hue as well would make the continuity argument above two-dimensional for no gain.
struct DebugGridStyle {
    Vec3 lineColor{0.55F, 0.56F, 0.58F};
    float minorAlpha = 0.16F;  // `a` in the weight rule
    float majorAlpha = 0.42F;  // `b`. majorAlpha > minorAlpha is a documented EXPECTATION, not a
                               // clamp: a caller who inverts them gets what they asked for.
    // DELIBERATELY ROUND STAND-INS, and NOT a second copy of the palette. The editor ALWAYS
    // overrides these from editor/include/aero/editor/axis_palette.hpp, which is the single source
    // for anything the editor draws. They exist so a sample or a test that takes the defaults still
    // gets a red X and a blue Z. Two authoritative copies of one colour is exactly the drift AX1
    // exists to prevent, so there is only one -- and it is not here.
    Vec4 axisXColor{0.75F, 0.05F, 0.05F, 0.85F};
    Vec4 axisZColor{0.05F, 0.20F, 0.75F, 0.85F};
    bool operator==(const DebugGridStyle&) const = default;
};

struct DebugGridParams {
    Vec3 eye{};                // camera position, world space
    Vec3 focus{};              // what it orbits / looks at, world space (EditorCamera::pivot())
    float farPlane = 1000.0F;  // the camera's far plane; every radius is clamped against it
    DebugGridStyle style{};
    bool drawAxes = true;
    bool operator==(const DebugGridParams&) const = default;
};

// ---- the cadence -------------------------------------------------------------------------------
// THE DECISION, EXPOSED. emitDebugGrid is implemented on top of this, so the test tier and the
// picture see ONE decision rather than two that agree by review -- the sanitizeTonemapParams
// posture. It also exists because a caller cannot otherwise know WHICH cadence is being drawn:
// samples/phase-E-debug-draw prints it, and GR6/GR7/GR8/GR9/GR17/GR23 assert it directly instead of
// inferring three alphas from three quantised bytes.
//
// `valid == false` means the totality gate refused (a non-finite eye, focus or farPlane, or a
// farPlane <= 0) and EVERY OTHER FIELD IS ZERO. A bad camera is not an error to report; it is a
// frame with no grid in it.
struct DebugGridCadence {
    float viewScale = 0.0F;                            // max(|eye - focus|, |eye.y - PLANE_HEIGHT|), after the clamp
    int level = 0;                                     // the decade exponent of the FINEST of the three cadences
    float fraction = 0.0F;                             // `f` in [0, 1]: the position inside the decade
    std::array<float, DEBUG_GRID_CADENCES> spacing{};  // 10^level, 10^(level+1), 10^(level+2)
    std::array<float, DEBUG_GRID_CADENCES> weight{};   // the weight rule, one alpha per cadence
    std::array<float, DEBUG_GRID_CADENCES> radius{};   // min(RADIUS_CELLS*s, FAR_FRACTION*farPlane)
    bool valid = false;
    bool operator==(const DebugGridCadence&) const = default;
};

// PURE, TOTAL, noexcept. Reaches sqrt (through length) and nothing else from libm.
[[nodiscard]] DebugGridCadence debugGridCadence(const DebugGridParams& params) noexcept;

// The decade ladder, exposed so a test can recompute an expected spacing with the SAME iteration the
// emitter uses instead of spelling a decade literal -- `0.01F` is the nearest float to 0.01 and
// `1.0F/10.0F/10.0F` is NOT the same float, so a literal would pin the wrong number and could redden
// on one lane alone. Clamps its argument to [DEBUG_GRID_MIN_LEVEL, DEBUG_GRID_MAX_LEVEL].
[[nodiscard]] float debugGridPow10(int exponent) noexcept;

// ---- the emitter -------------------------------------------------------------------------------
// TOTAL. Pushes into the batch's `Tested` bucket ONLY, and pushes no billboard. Returns the number of
// LINES THE BATCH ACCEPTED -- which is below the number emitted iff the batch was already near its
// budget, and the batch's own droppedLines() is what says so. Non-finite input in eye, focus or
// farPlane (or farPlane <= 0) emits NOTHING, returns 0 and adds NO rejection: a bad camera is not a
// bad push. ALLOCATION-FREE: every line is built in a stack array and pushed with one lines(span)
// call.
[[nodiscard]] std::uint32_t emitDebugGrid(DebugDrawBatch& batch, const DebugGridParams& params);

}  // namespace engine::render
