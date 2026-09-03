// tests/render_debug_grid_test.cpp — task E.1.2: the ground-grid emitter, entirely without a GPU.
//
// EVERY CASE HERE IS UNGATED. There is no device, no window, no shader toolchain and no #if of any
// kind in this file -- the emitter is pure arithmetic, so everything about it is assertable without
// one, which is the point of putting it in engine/render rather than in the editor.
//
// <aero/render/debug_grid.hpp> IS DELIBERATELY NOT INCLUDED. GR1's whole claim is that the UMBRELLA
// carries it, and with both includes present that case passes on a seeded umbrella and proves
// nothing -- the DD23 pattern, restated because it is the kind of thing that gets "tidied" away.
#include <aero/render/render.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Vec3;
using engine::Vec4;
namespace rd = engine::render;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// The umbrella header and the emitter's own source, reached through AERO_SHADERS_SRC_DIR -- the ONE
// route into the source tree aero_tests already has (tests/CMakeLists.txt, target-wide). NOT a new
// compile definition: this task is held to two added source lines in that file.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";
constexpr std::string_view DEBUG_GRID_SOURCE_PATH = AERO_SHADERS_SRC_DIR "/../engine/render/src/debug_grid.cpp";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy GR22 -- and the banner
// of debug_grid.cpp names `std::pow` and `std::log10` in the sentence saying they are absent, which
// is exactly the trap this strips away.
[[nodiscard]] std::string strippedSourceAt(std::string_view absolutePath) {
    std::ifstream file{std::string{absolutePath}};
    std::string out;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        out += line;
        out += '\n';
    }
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// A batch big enough that nothing is ever dropped, so a count is the count EMITTED rather than the
// count that happened to fit. GR20 is the one case that deliberately uses a small one.
[[nodiscard]] rd::DebugDrawBatch roomyBatch() { return rd::DebugDrawBatch{rd::DebugDrawBudget{}}; }

// The pose the editor really opens with (EditorCamera's defaults: distance 8, pitch -20 degrees,
// pivot at the origin, far 1000), written once so twelve cases cannot drift apart.
[[nodiscard]] rd::DebugGridParams defaultPose() {
    constexpr float DISTANCE = 8.0F;
    constexpr float PITCH = -20.0F;
    const float eyeY = DISTANCE * std::sin(-PITCH * std::numbers::pi_v<float> / 180.0F);
    const float eyeZ = std::sqrt((DISTANCE * DISTANCE) - (eyeY * eyeY));
    return rd::DebugGridParams{.eye = Vec3{0.0F, eyeY, eyeZ}, .focus = Vec3::zero(), .farPlane = 1000.0F};
}

// A pose whose viewScale is EXACTLY `scale`: the eye is directly above the focus, so both arms of
// the max() are `scale` and no arithmetic rounds. Every self-similarity case uses this, because
// `viewScale' == 10 * viewScale` has to be BIT-exact for the claim to be about the emitter rather
// than about float multiplication.
[[nodiscard]] rd::DebugGridParams poseAtScale(float scale, Vec3 focus = Vec3::zero(), float farPlane = 1.0e6F) {
    return rd::DebugGridParams{.eye = Vec3{focus.x, focus.y + scale, focus.z}, .focus = focus, .farPlane = farPlane};
}

// Every vertex the emitter pushed into the Tested bucket, as a copy -- so a case can sort, scan or
// memcmp it without holding a span into a batch that is about to be reused.
[[nodiscard]] std::vector<rd::DebugLineVertex> testedVertices(const rd::DebugDrawBatch& batch) {
    const std::span<const rd::DebugLineVertex> span = batch.lineVertices(rd::DebugDepth::Tested);
    return std::vector<rd::DebugLineVertex>{span.begin(), span.end()};
}

[[nodiscard]] std::uint8_t alphaByte(std::uint32_t rgba) { return static_cast<std::uint8_t>((rgba >> 24U) & 0xFFU); }
[[nodiscard]] std::uint32_t rgbBits(std::uint32_t rgba) { return rgba & 0x00FFFFFFU; }

}  // namespace

TEST_CASE("render debug grid: the umbrella header carries debug_grid.hpp (GR1)") {
    // TWO ARMS, and only one of them is real -- the DD23 honesty, restated. The NAMING arm below is
    // NOT a compile failure: this TU includes render.hpp, which includes debug_draw.hpp, and
    // debug_grid.hpp includes debug_draw.hpp too -- but nothing else in aero_tests includes
    // debug_grid.hpp, so deleting the umbrella's line WOULD break this TU. That makes the naming arm
    // genuine here, unlike DD23's. The SOURCE-TEXT arm is kept anyway because it also refuses a
    // COMMENTED-OUT include, which a compile cannot distinguish from a missing one in a header that
    // some other TU happens to pull in.
    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());  // non-vacuity: the path resolved and the file was read
    CHECK(contains(umbrella, "#include <aero/render/debug_grid.hpp>"));
    // ...and the search can say NO, so a reader that matched everything could not fake the line above.
    CHECK_FALSE(contains(umbrella, "#include <aero/render/does_not_exist.hpp>"));

    [[maybe_unused]] const rd::DebugGridStyle style{};
    [[maybe_unused]] const rd::DebugGridParams params{};
    [[maybe_unused]] const rd::DebugGridCadence cadence{};
    CHECK(rd::DEBUG_GRID_MAX_LINES > 0U);
}

TEST_CASE("render debug grid: the defaults are exactly what the editor calls with (GR2)") {
    const rd::DebugGridStyle style{};
    CHECK(style.lineColor.x == 0.55F);
    CHECK(style.lineColor.y == 0.56F);
    CHECK(style.lineColor.z == 0.58F);
    CHECK(style.minorAlpha == 0.16F);
    CHECK(style.majorAlpha == 0.42F);
    // The stand-ins: red-ish X, blue-ish Z, so a caller who takes the defaults still gets the
    // convention. The EDITOR overrides both from axis_palette.hpp -- AX1 is what keeps that pair
    // honest, and these two are deliberately NOT the palette's values.
    CHECK(style.axisXColor.x > style.axisXColor.z);
    CHECK(style.axisZColor.z > style.axisZColor.x);

    const rd::DebugGridParams params{};
    CHECK((params.eye == Vec3::zero()));
    CHECK((params.focus == Vec3::zero()));
    CHECK(params.farPlane == 1000.0F);  // EditorCamera::DEFAULT_FAR
    CHECK(params.drawAxes);
    CHECK((params.style == rd::DebugGridStyle{}));

    const rd::DebugGridCadence cadence{};
    CHECK_FALSE(cadence.valid);
    CHECK(cadence.viewScale == 0.0F);
    CHECK(cadence.level == 0);
    CHECK(cadence.fraction == 0.0F);
}

TEST_CASE("render debug grid: the constants' relationships hold and the cap is DERIVED (GR3)") {
    // THE CAP IS ARITHMETIC ON THE CONSTANTS, recomputed here rather than compared to 2368. A
    // literal on either side would make this case agree with itself; recomputing means a changed
    // RADIUS_CELLS moves both sides together and a HAND-EDITED cap moves only one.
    const std::uint32_t derived =
        rd::DEBUG_GRID_CADENCES * 2U * ((2U * rd::DEBUG_GRID_RADIUS_CELLS) + 1U) * rd::DEBUG_GRID_FADE_SEGMENTS +
        (2U * rd::DEBUG_GRID_FADE_SEGMENTS);
    CHECK(rd::DEBUG_GRID_MAX_LINES == derived);
    CHECK(rd::DEBUG_GRID_MAX_LINES < rd::DebugDrawBudget{}.maxLines);  // room left for E.2.3

    CHECK(rd::DEBUG_GRID_CADENCES == 3U);  // the whole continuity argument needs three
    CHECK(rd::DEBUG_GRID_BASE == 10.0F);   // metric decades
    CHECK(rd::DEBUG_GRID_FADE_INNER > 0.0F);
    CHECK(rd::DEBUG_GRID_FADE_INNER < 1.0F);  // the fade band has to be non-empty
    CHECK(rd::DEBUG_GRID_FAR_FRACTION > 0.0F);
    CHECK(rd::DEBUG_GRID_FAR_FRACTION < 1.0F);  // < 1, so the fade completes INSIDE the frustum
    CHECK(rd::DEBUG_GRID_MIN_LEVEL < rd::DEBUG_GRID_MAX_LEVEL);
    CHECK(rd::DEBUG_GRID_MIN_VIEW_SCALE < rd::DEBUG_GRID_MAX_VIEW_SCALE);
    CHECK(rd::DEBUG_GRID_MIN_VIEW_SCALE > 0.0F);
    CHECK(rd::DEBUG_GRID_FADE_SEGMENTS >= 2U);  // one segment cannot express a fade ALONG a line
    CHECK(rd::DEBUG_GRID_RADIUS_CELLS >= 1U);
    CHECK(rd::DEBUG_GRID_PLANE_HEIGHT == 0.0F);

    // A DEFAULT-VALUE check, not a clamp: a caller who inverts these gets what they asked for, and
    // the emitter stays linear. The expectation is documented on the struct; this pins the default.
    CHECK(rd::DebugGridStyle{}.majorAlpha > rd::DebugGridStyle{}.minorAlpha);
}

TEST_CASE("render debug grid: the default pose emits a real grid, under the cap (GR4)") {
    rd::DebugDrawBatch batch = roomyBatch();
    const std::uint32_t emitted = rd::emitDebugGrid(batch, defaultPose());
    CHECK(emitted > 0U);
    CHECK(emitted <= rd::DEBUG_GRID_MAX_LINES);
    CHECK(batch.lineCount() == emitted);  // nothing was dropped or rejected
    CHECK(batch.droppedLines() == 0U);
    CHECK(batch.rejectedLines() == 0U);
    // FADE_SEGMENTS lines per grid line, so the total is a multiple of it -- which is the structural
    // statement that every line really was subdivided rather than pushed whole.
    CHECK(emitted % rd::DEBUG_GRID_FADE_SEGMENTS == 0U);
    CHECK(batch.lineVertices(rd::DebugDepth::Tested).size() == 2U * emitted);
    // Measured on this tree: 2224 at the default pose. Recorded as a RANGE rather than an equality,
    // because it is a consequence of five constants and a camera and would redden on any retune --
    // GR3 is where the constants are pinned, and this is where "a real grid, not two lines" is.
    CHECK(emitted > 1000U);
}

TEST_CASE("render debug grid: everything lands in the Tested bucket and no billboard is pushed (GR5)") {
    rd::DebugDrawBatch batch = roomyBatch();
    const std::uint32_t emitted = rd::emitDebugGrid(batch, defaultPose());
    CHECK(batch.lineCount(rd::DebugDepth::Tested) == emitted);
    CHECK(batch.lineCount(rd::DebugDepth::Overlay) == 0U);  // "correctly occluded" is the deliverable
    CHECK(batch.lineVertices(rd::DebugDepth::Overlay).empty());
    CHECK(batch.billboardCount() == 0U);
    CHECK(batch.billboardCount(rd::DebugDepth::Tested) == 0U);
    CHECK(batch.billboardCount(rd::DebugDepth::Overlay) == 0U);
    CHECK(batch.droppedBillboards() == 0U);
    CHECK(batch.rejectedBillboards() == 0U);
}

TEST_CASE("render debug grid: a decade change scales the grid EXACTLY, and the one place it does not (GR6)") {
    // THE CLAIM D3 IS ACTUALLY ABOUT. Scaling eye, focus and farPlane by ten must leave the picture
    // identical and ten times bigger. Three arms, because the property is exact in three different
    // senses and MEASURING which is which is the whole value of the case.

    SUBCASE("ARM 1 -- the CADENCE STATE is bit-exactly self-similar") {
        // level >= -1 is the band where all three spacings scale exactly (arm 3 says why), and
        // `decade` here is the VIEW SCALE's decade, not the level: level == -1 is viewScale in
        // [1, 10), so the in-band decades start at 0. MEASURED on this tree, six mantissas each:
        // decade -1 gives level -2 and fails 6 of 6 rows (it IS arm 3's exception band); decades 0
        // through 4 give levels -1 through 3 and fail 0 of 6; decade 5 fails because 9e5 x 10
        // exceeds DEBUG_GRID_MAX_VIEW_SCALE and the scaled input is no longer a x10 of the first.
        for (const float mantissa : {1.0F, 2.0F, 3.0F, 5.0F, 7.0F, 9.0F}) {
            for (const int decade : {0, 1, 2, 3, 4}) {
                const float scale = mantissa * rd::debugGridPow10(decade);
                const rd::DebugGridParams small = poseAtScale(scale);
                rd::DebugGridParams large = poseAtScale(10.0F * scale);
                large.farPlane = 10.0F * small.farPlane;
                const rd::DebugGridCadence a = rd::debugGridCadence(small);
                const rd::DebugGridCadence b = rd::debugGridCadence(large);
                CAPTURE(mantissa);
                CAPTURE(decade);
                REQUIRE(a.valid);
                REQUIRE(b.valid);
                // The input really did scale exactly -- without this the case would be about float
                // multiplication rather than about the emitter.
                REQUIRE(b.viewScale == 10.0F * a.viewScale);
                CHECK(b.level == a.level + 1);
                CHECK(b.fraction == a.fraction);  // EXACT. f is the crossfade's whole state.
                for (std::uint32_t c = 0; c < rd::DEBUG_GRID_CADENCES; ++c) {
                    CAPTURE(c);
                    CHECK(b.spacing[c] == 10.0F * a.spacing[c]);
                    CHECK(b.radius[c] == 10.0F * a.radius[c]);  // INV-3: radius is a function of s
                    CHECK(b.weight[c] == a.weight[c]);          // INV-3: alpha is a function of f
                }
            }
        }
    }

    SUBCASE("ARM 2 -- the VERTEX STREAM matches, on a lattice-aligned pose") {
        // focus AT THE ORIGIN, deliberately: with the focus off the lattice, whether a line lands
        // exactly on the disc rim is a knife-edge that a x10 moves, and the count can differ by two
        // lines (MEASURED at focus.x = 17 and 250). That is a property of a disc, not a defect --
        // arm 1 is the claim that survives it, and this arm is the stronger claim where it holds.
        for (const float scale : {1.0F, 5.0F, 8.0F, 50.0F, 100.0F, 1000.0F}) {
            const rd::DebugGridParams small = poseAtScale(scale);
            rd::DebugGridParams large = poseAtScale(10.0F * scale);
            large.farPlane = 10.0F * small.farPlane;
            rd::DebugDrawBatch batchA = roomyBatch();
            rd::DebugDrawBatch batchB = roomyBatch();
            const std::uint32_t na = rd::emitDebugGrid(batchA, small);
            const std::uint32_t nb = rd::emitDebugGrid(batchB, large);
            CAPTURE(scale);
            REQUIRE(na == nb);  // identical LINE COUNT and emission order
            const std::vector<rd::DebugLineVertex> va = testedVertices(batchA);
            const std::vector<rd::DebugLineVertex> vb = testedVertices(batchB);
            REQUIRE(va.size() == vb.size());
            const float tolerance = 1.0e-6F * rd::debugGridCadence(large).radius[rd::DEBUG_GRID_CADENCES - 1U];
            for (std::size_t i = 0; i < va.size(); ++i) {
                CAPTURE(i);
                // THE PACKED COLOUR IS EXACT -- all four bytes, alpha included. Measured worst
                // deviation over 90 scale pairs: ZERO. The spec predicted "within 1"; it is 0.
                CHECK(va[i].rgba == vb[i].rgba);
                // Positions to a tolerance SCALED BY THE PICTURE, with the epsilon as part of the
                // assertion. Measured worst: 9.11e-08 x the coarsest radius, so 1e-6 is a >10x
                // margin and a real regression cannot hide under it.
                CHECK(std::fabs((10.0F * va[i].position.x) - vb[i].position.x) < tolerance);
                CHECK(std::fabs((10.0F * va[i].position.y) - vb[i].position.y) < tolerance);
                CHECK(std::fabs((10.0F * va[i].position.z) - vb[i].position.z) < tolerance);
            }
        }
    }

    SUBCASE("ARM 3 -- the ONE decade where the ladder is not exactly a x10 ladder, PINNED") {
        // The reason arm 1 and arm 2 are restricted to level >= -1, asserted rather than remembered.
        // debugGridPow10 iterates from 1.0F, so pow10(n+1) and 10*pow10(n) are the same float for
        // EVERY n in range EXCEPT n == -2 -- where a single ulp separates them and shifts a whole
        // family's k range. A case that asserts the boundary of its own claim is worth more than one
        // that pretends the boundary is not there.
        CHECK(rd::debugGridPow10(-1) != 10.0F * rd::debugGridPow10(-2));
        CHECK(rd::debugGridPow10(0) == 10.0F * rd::debugGridPow10(-1));
        CHECK(rd::debugGridPow10(0) == 1.0F);
        CHECK(rd::debugGridPow10(1) == 10.0F);
        CHECK(rd::debugGridPow10(2) == 100.0F);
        for (const int n : {-6, -5, -4, -3, -1, 0, 1, 2, 3, 4, 5, 6, 7}) {
            CAPTURE(n);
            CHECK(rd::debugGridPow10(n + 1) == 10.0F * rd::debugGridPow10(n));
        }
        // ...and the RUNNING-PRODUCT hazard the fresh-call rule exists for, measured rather than
        // asserted: a round trip down six decades and back does NOT return 1.0.
        float running = 1.0F;
        for (int i = 0; i < 6; ++i) {
            running /= 10.0F;
        }
        for (int i = 0; i < 6; ++i) {
            running *= 10.0F;
        }
        CHECK(running != 1.0F);
        CHECK(rd::debugGridPow10(0) == 1.0F);  // ...while a fresh call is exactly 1
        // The level clamp, both ends.
        CHECK(rd::debugGridPow10(rd::DEBUG_GRID_MIN_LEVEL - 5) == rd::debugGridPow10(rd::DEBUG_GRID_MIN_LEVEL));
        CHECK(rd::debugGridPow10(rd::DEBUG_GRID_MAX_LEVEL + 5) == rd::debugGridPow10(rd::DEBUG_GRID_MAX_LEVEL));
    }
}

TEST_CASE("render debug grid: the cadence selected for a view scale, exactly (GR7)") {
    // EVERY EXPECTED SPACING IS RECOMPUTED WITH THE SAME LADDER THE EMITTER USES, never spelled as a
    // decade literal. `0.01F` is the nearest float to 0.01 and debugGridPow10(-2) is 0.0099999998 --
    // NOT the same float. A literal here would pin the wrong number and could redden on one lane
    // alone, which is the whole species of bug D4 exists to avoid.
    struct Row {
        float viewScale;
        int level;
        float fraction;
    };
    const std::array<Row, 7> rows{{
        {1.0F, -1, 0.0F},  // reference == pow10(-1) exactly -> f is exactly 0
        {10.0F, 0, 0.0F},
        {100.0F, 1, 0.0F},
        {1000.0F, 2, 0.0F},
        {50.0F, 0, 4.0F / 9.0F},  // reference 5 -> (5/1 - 1)/9
        {8.0F, -1, 7.0F / 9.0F},  // THE DEFAULT POSE: reference 0.8, s0 0.1, f = 7/9
        {5.0F, -1, 4.0F / 9.0F},
    }};
    for (const Row& row : rows) {
        CAPTURE(row.viewScale);
        const rd::DebugGridCadence cadence = rd::debugGridCadence(poseAtScale(row.viewScale));
        REQUIRE(cadence.valid);
        CHECK(cadence.viewScale == row.viewScale);
        CHECK(cadence.level == row.level);
        CHECK(cadence.spacing[0] == rd::debugGridPow10(row.level));
        CHECK(cadence.spacing[1] == rd::debugGridPow10(row.level + 1));
        CHECK(cadence.spacing[2] == rd::debugGridPow10(row.level + 2));
        // EXACT, NOT Approx. Measured: 0.8F / debugGridPow10(-1) is exactly 8.0F (0.8F IS 8 x 0.1F --
        // the same mantissa three binary exponents apart), and (8 - 1)/9 is bit-identical to
        // 7.0F/9.0F. Every row's fraction is arithmetic on two exact quantities, so a tolerance here
        // would be slack hiding a real property.
        CHECK(cadence.fraction == row.fraction);
    }

    SUBCASE("the VIEW-SCALE clamp is the binding one -- the LEVEL clamp is never reached") {
        // MEASURED, and worth asserting because it looks like the opposite is true: viewScale clamps
        // to [1e-3, 1e6], so reference clamps to [1e-4, 1e5] and the REACHABLE level range is
        // [-4, 5] -- strictly inside the ladder's own [-6, 9]. DEBUG_GRID_MIN_LEVEL and
        // DEBUG_GRID_MAX_LEVEL exist to BOUND THE ITERATION COUNT so the emitter terminates whatever
        // reference is, including after a retune of the view-scale clamps; they are not a behaviour
        // anyone sees. Asserting the RELATIONSHIP is what keeps that true after a retune.
        const rd::DebugGridCadence tiny = rd::debugGridCadence(poseAtScale(1.0e-9F));
        REQUIRE(tiny.valid);
        CHECK(tiny.viewScale == rd::DEBUG_GRID_MIN_VIEW_SCALE);  // the view-scale clamp bit
        CHECK(tiny.level > rd::DEBUG_GRID_MIN_LEVEL);            // measured: -4, against a floor of -6
        CHECK(tiny.spacing[0] == rd::debugGridPow10(tiny.level));
        // f is 1.3e-8 here rather than 0, because reference (the float nearest 1e-4) is NOT
        // debugGridPow10(-4) (which is 9.99999902e-05). That is precisely why every expected spacing
        // above is recomputed with the ladder instead of spelled as a decade literal.
        CHECK(tiny.fraction >= 0.0F);
        CHECK(tiny.fraction < 0.001F);

        const rd::DebugGridCadence huge = rd::debugGridCadence(poseAtScale(1.0e12F, Vec3::zero(), 1.0e9F));
        REQUIRE(huge.valid);
        CHECK(huge.viewScale == rd::DEBUG_GRID_MAX_VIEW_SCALE);
        CHECK(huge.level < rd::DEBUG_GRID_MAX_LEVEL);  // measured: 5, against a ceiling of 9
        CHECK(huge.fraction == 0.0F);                  // reference 1e5 == debugGridPow10(5)
        CHECK(huge.spacing[0] == rd::debugGridPow10(huge.level));
    }
}

TEST_CASE("render debug grid: the weights are CONTINUOUS across a decade boundary (GR8)") {
    // THE ALGEBRA THAT MAKES "stays legible at every zoom" A PROPERTY. As f -> 1 at level L the
    // triple is (0, a, b); at f = 0 of level L+1 it is (a, b, 0). The SAME WORLD SPACING therefore
    // keeps the SAME alpha across the boundary, and the two spacings that appear and disappear do so
    // at EXACTLY zero. There is no threshold and no hysteresis, so there is nothing to tune wrong.
    const rd::DebugGridStyle style{};
    const float a = style.minorAlpha;
    const float b = style.majorAlpha;

    // f is driven directly through viewScale, so this is the real function rather than a restatement
    // of the formula. reference = viewScale/10; f = (reference/s0 - 1)/9, so f = 0 at viewScale =
    // 10*s0 and f -> 1 as viewScale -> 100*s0.
    const rd::DebugGridCadence low = rd::debugGridCadence(poseAtScale(10.0F));  // level 0, f == 0
    REQUIRE(low.valid);
    REQUIRE(low.fraction == 0.0F);
    CHECK(low.weight[0] == a);
    CHECK(low.weight[1] == b);
    CHECK(low.weight[2] == 0.0F);

    // The top of the SAME decade, reached one ulp below the flip: viewScale 99.99 is still level 0.
    const rd::DebugGridCadence high = rd::debugGridCadence(poseAtScale(99.99F));
    REQUIRE(high.valid);
    REQUIRE(high.level == low.level);
    CHECK(high.fraction > 0.99F);

    // ...and the decade above, at f == 0.
    const rd::DebugGridCadence next = rd::debugGridCadence(poseAtScale(100.0F));
    REQUIRE(next.valid);
    REQUIRE(next.level == low.level + 1);
    REQUIRE(next.fraction == 0.0F);

    // THE SHIFT. Each world spacing keeps its alpha: s1 becomes the new s0 (weight a) and s2 becomes
    // the new s1 (weight b). Written at f == 1 exactly, computed from the rule with the SAME a and b
    // the emitter used, so this asserts the identity rather than re-deriving the numbers.
    const float w0AtOne = a * (1.0F - 1.0F);
    const float w1AtOne = (b * (1.0F - 1.0F)) + (a * 1.0F);
    const float w2AtOne = b * 1.0F;
    CHECK(w0AtOne == 0.0F);            // the finest cadence LEAVES at exactly zero alpha
    CHECK(w1AtOne == next.weight[0]);  // s1 -> the new s0, at alpha a, EXACTLY
    CHECK(w2AtOne == next.weight[1]);  // s2 -> the new s1, at alpha b, EXACTLY
    CHECK(next.weight[2] == 0.0F);     // the new coarsest ENTERS at exactly zero alpha

    // AND THE EXTENT IS CONTINUOUS FOR THE SAME REASON: R is a function of the SPACING alone, so the
    // disc a given world spacing is drawn over is unchanged across the boundary. This is INV-3, and
    // a seed that makes the radius depend on viewScale or on level reddens exactly here.
    CHECK(next.spacing[0] == high.spacing[1]);
    CHECK(next.radius[0] == high.radius[1]);
    CHECK(next.spacing[1] == high.spacing[2]);
    CHECK(next.radius[1] == high.radius[2]);
}

TEST_CASE("render debug grid: at f == 0 the weights are exactly (minor, major, 0) (GR9)") {
    // THE MAJOR/MINOR LOOK THE DELIVERABLE ASKS FOR, and it is not tuned to be -- it falls out of
    // the weight rule at the bottom of every decade.
    for (const int decade : {-1, 0, 1, 2, 3}) {
        CAPTURE(decade);
        const rd::DebugGridCadence cadence = rd::debugGridCadence(poseAtScale(10.0F * rd::debugGridPow10(decade)));
        REQUIRE(cadence.valid);
        REQUIRE(cadence.fraction == 0.0F);
        CHECK(cadence.weight[0] == rd::DebugGridStyle{}.minorAlpha);
        CHECK(cadence.weight[1] == rd::DebugGridStyle{}.majorAlpha);
        CHECK(cadence.weight[2] == 0.0F);
    }
    SUBCASE("a custom style flows through unchanged -- the rule is linear in a and b") {
        rd::DebugGridParams params = poseAtScale(10.0F);
        params.style.minorAlpha = 0.3F;
        params.style.majorAlpha = 0.7F;
        const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
        REQUIRE(cadence.fraction == 0.0F);
        CHECK(cadence.weight[0] == 0.3F);
        CHECK(cadence.weight[1] == 0.7F);
        CHECK(cadence.weight[2] == 0.0F);
    }
}

TEST_CASE("render debug grid: every vertex sits EXACTLY on the plane (GR10)") {
    // BIT-EXACT, no tolerance: the y coordinate is assigned from a constant and never computed, so
    // anything but == would be slack. A seed that wires the plane height to eye.y reddens here.
    for (const float scale : {0.05F, 1.0F, 8.0F, 250.0F, 5000.0F}) {
        for (const Vec3 focus : {Vec3::zero(), Vec3{3.0F, 11.0F, -7.0F}, Vec3{-400.0F, -2.0F, 400.0F}}) {
            CAPTURE(scale);
            const rd::DebugGridParams params = poseAtScale(scale, focus);
            rd::DebugDrawBatch batch = roomyBatch();
            REQUIRE(rd::emitDebugGrid(batch, params) > 0U);
            for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
                CHECK(vertex.position.y == rd::DEBUG_GRID_PLANE_HEIGHT);
            }
        }
    }
}

TEST_CASE("render debug grid: the fade reaches EXACTLY zero at every line's two ends (GR11)") {
    // THE PACKED BYTE, not the float. The radius round trip goes through two square roots -- the
    // chord endpoint's distance is sqrt(o^2 + (sqrt(R^2 - o^2))^2) -- and leaves a residual fade of
    // ~1e-7, which quantises to 0. Asserting the float would need a tolerance; asserting the BYTE is
    // exact, and the byte is what the GPU consumes.
    rd::DebugDrawBatch batch = roomyBatch();
    const std::uint32_t emitted = rd::emitDebugGrid(batch, defaultPose());
    REQUIRE(emitted > 0U);
    const std::vector<rd::DebugLineVertex> vertices = testedVertices(batch);
    REQUIRE(vertices.size() == 2U * emitted);

    // Every SEGMENTS-line run is one grid line: vertex 0 of the run and vertex 2*SEGMENTS-1 are its
    // two extreme ends. Measured over 550 800 lines across a 4000-pose sweep: ZERO ends with a
    // non-zero alpha byte.
    constexpr std::size_t STRIDE = std::size_t{2} * rd::DEBUG_GRID_FADE_SEGMENTS;
    REQUIRE(vertices.size() % STRIDE == 0U);
    std::size_t interiorLit = 0;
    for (std::size_t base = 0; base + STRIDE <= vertices.size(); base += STRIDE) {
        CAPTURE(base);
        CHECK(alphaByte(vertices[base].rgba) == 0U);
        CHECK(alphaByte(vertices[base + STRIDE - 1U].rgba) == 0U);
        for (std::size_t i = 1; i + 1U < STRIDE; ++i) {
            if (alphaByte(vertices[base + i].rgba) != 0U) {
                ++interiorLit;
            }
        }
    }
    // ANTI-VACUITY, and it is the arm that discriminates FADE_SEGMENTS == 1: if a line were not
    // subdivided it would have no interior vertices at all, so "both ends are 0" would be the whole
    // line and the grid would be invisible. Something in the middle must be lit.
    CHECK(interiorLit > 0U);

    SUBCASE("and every vertex is inside its cadence's disc") {
        // A RELATIVE epsilon, with the epsilon as part of the assertion: the radius round trip is
        // two square roots, and the measured worst overshoot over a 4000-pose sweep is
        // (r - R2)/R2 = 3.37e-05. 1e-4 is a ~3x margin; a real escape from the disc is orders of
        // magnitude larger.
        const rd::DebugGridCadence cadence = rd::debugGridCadence(defaultPose());
        const float coarsest = cadence.radius[rd::DEBUG_GRID_CADENCES - 1U];
        for (const rd::DebugLineVertex& vertex : vertices) {
            const float dx = vertex.position.x - defaultPose().focus.x;
            const float dz = vertex.position.z - defaultPose().focus.z;
            CHECK(std::sqrt((dx * dx) + (dz * dz)) <= coarsest * 1.0001F);
        }
    }
}

TEST_CASE("render debug grid: every line lies on the WORLD lattice, bit-exactly (GR12)") {
    // THE CASE THE FRESH-pow10 RULE EXISTS FOR. Each emitted line's constant coordinate must be an
    // exact float multiple of one of the three spacings -- so `x == 1` really is at 1. A running
    // product, a std::pow, or a `10 * s[c-1]` shortcut all redden here, because each produces a
    // spacing that is one ulp away from the one the expectation is built from.
    //
    // The expectation is RECOMPUTED with the same ladder and the same single multiply the emitter
    // uses. Nothing is spelled as a decade literal anywhere in this case.
    for (const float scale : {0.5F, 8.0F, 137.0F}) {
        for (const Vec3 focus : {Vec3::zero(), Vec3{2.5F, 0.0F, -9.25F}}) {
            CAPTURE(scale);
            rd::DebugGridParams params = poseAtScale(scale, focus);
            params.drawAxes = false;  // the axes sit at 0, which is a multiple of everything -- so
                                      // turning them off is what makes this case discriminate
            const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
            REQUIRE(cadence.valid);
            rd::DebugDrawBatch batch = roomyBatch();
            REQUIRE(rd::emitDebugGrid(batch, params) > 0U);

            for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
                // A vertex is on a family-B line iff its x is constant along the line; rather than
                // track families, assert the WEAKER-LOOKING but equivalent claim that at least one
                // of x and z is an exact lattice coordinate of some cadence -- every emitted vertex
                // has exactly one constant axis by construction.
                bool onLattice = false;
                for (std::uint32_t c = 0; c < rd::DEBUG_GRID_CADENCES && !onLattice; ++c) {
                    const float spacing = cadence.spacing[c];
                    for (const float coordinate : {vertex.position.x, vertex.position.z}) {
                        // k recomputed by the inverse, then the FORWARD multiply re-applied: the
                        // claim is `coordinate == k * spacing` for an integral k, and comparing the
                        // product is what makes it bit-exact rather than a division tolerance.
                        const float k = std::round(coordinate / spacing);
                        if (k * spacing == coordinate) {
                            onLattice = true;
                            break;
                        }
                    }
                }
                CAPTURE(vertex.position.x);
                CAPTURE(vertex.position.z);
                CHECK(onLattice);
            }
        }
    }
}

TEST_CASE("render debug grid: with drawAxes, NO cadence emits a line at k == 0 (GR13)") {
    // Two coplanar, depth-write-free lines on the same pixels composited in push order is an artefact
    // whose appearance depends on nothing anyone chose. So the k == 0 line is skipped in BOTH
    // families of ALL THREE cadences and the axis takes it.
    rd::DebugGridParams params = poseAtScale(8.0F, Vec3{0.3F, 0.0F, -0.4F});
    params.drawAxes = true;
    params.style.axisXColor = Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    params.style.axisZColor = Vec4{0.0F, 0.0F, 1.0F, 1.0F};
    params.style.lineColor = Vec3{0.5F, 0.5F, 0.5F};
    rd::DebugDrawBatch batch = roomyBatch();
    REQUIRE(rd::emitDebugGrid(batch, params) > 0U);

    // Count the lines whose constant coordinate is EXACTLY zero. With drawAxes on, the only ones are
    // the two axes -- so there are exactly 2 * FADE_SEGMENTS such lines, and their colour is an axis
    // colour rather than the grid grey.
    const std::vector<rd::DebugLineVertex> vertices = testedVertices(batch);
    constexpr std::size_t STRIDE = std::size_t{2} * rd::DEBUG_GRID_FADE_SEGMENTS;
    std::size_t zeroRuns = 0;
    for (std::size_t base = 0; base + STRIDE <= vertices.size(); base += STRIDE) {
        const rd::DebugLineVertex& a = vertices[base];
        const rd::DebugLineVertex& b = vertices[base + 1U];
        const bool constantX = a.position.x == b.position.x;
        const float constantCoord = constantX ? a.position.x : a.position.z;
        if (constantCoord != 0.0F) {
            continue;
        }
        ++zeroRuns;
        // ...and it carries an AXIS colour, not the grid's grey. The alpha varies along the line, so
        // only the RGB bits are compared -- which is exactly what packDebugColor puts in the low 24.
        const std::uint32_t red = rgbBits(rd::packDebugColor(params.style.axisXColor));
        const std::uint32_t blue = rgbBits(rd::packDebugColor(params.style.axisZColor));
        CHECK((rgbBits(a.rgba) == red || rgbBits(a.rgba) == blue));
    }
    CHECK(zeroRuns == 2U);  // exactly two axes, and no grid line joined them
}

TEST_CASE("render debug grid: with drawAxes off the k == 0 lines COME BACK and no axis is drawn (GR14)") {
    // The other arm of D8, and it is what makes GR13 mean something: there is no gap in either
    // configuration. Turning the axes off restores the grid's own lines rather than leaving a hole.
    rd::DebugGridParams params = poseAtScale(8.0F, Vec3{0.3F, 0.0F, -0.4F});
    params.style.axisXColor = Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    params.style.axisZColor = Vec4{0.0F, 0.0F, 1.0F, 1.0F};
    params.style.lineColor = Vec3{0.5F, 0.5F, 0.5F};
    params.drawAxes = false;

    rd::DebugDrawBatch batch = roomyBatch();
    REQUIRE(rd::emitDebugGrid(batch, params) > 0U);
    const std::vector<rd::DebugLineVertex> vertices = testedVertices(batch);
    constexpr std::size_t STRIDE = std::size_t{2} * rd::DEBUG_GRID_FADE_SEGMENTS;

    const std::uint32_t red = rgbBits(rd::packDebugColor(params.style.axisXColor));
    const std::uint32_t blue = rgbBits(rd::packDebugColor(params.style.axisZColor));
    std::size_t zeroRuns = 0;
    for (std::size_t base = 0; base + STRIDE <= vertices.size(); base += STRIDE) {
        const rd::DebugLineVertex& a = vertices[base];
        const rd::DebugLineVertex& b = vertices[base + 1U];
        const float constantCoord = (a.position.x == b.position.x) ? a.position.x : a.position.z;
        if (constantCoord == 0.0F) {
            ++zeroRuns;
        }
    }
    // Three cadences x two families, all of them now present at k == 0.
    CHECK(zeroRuns == static_cast<std::size_t>(rd::DEBUG_GRID_CADENCES) * 2U);
    // ...and NO vertex carries an axis colour at all.
    for (const rd::DebugLineVertex& vertex : vertices) {
        CHECK(rgbBits(vertex.rgba) != red);
        CHECK(rgbBits(vertex.rgba) != blue);
    }
}

TEST_CASE("render debug grid: the axes run along X and Z, carry their colours, and go LAST (GR15)") {
    rd::DebugGridParams params = defaultPose();
    params.style.axisXColor = Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    params.style.axisZColor = Vec4{0.0F, 0.0F, 1.0F, 1.0F};
    rd::DebugDrawBatch batch = roomyBatch();
    const std::uint32_t emitted = rd::emitDebugGrid(batch, params);
    REQUIRE(emitted > 0U);
    const std::vector<rd::DebugLineVertex> vertices = testedVertices(batch);
    constexpr std::size_t STRIDE = std::size_t{2} * rd::DEBUG_GRID_FADE_SEGMENTS;
    REQUIRE(vertices.size() >= 2U * STRIDE);

    // THE LAST TWO RUNS ARE THE TWO AXES, in that order: X then Z. Push order is the contract
    // (GR24), and this is the half of it that a picture can see -- the axes are drawn over the grid.
    const std::size_t xBase = vertices.size() - (2U * STRIDE);
    const std::size_t zBase = vertices.size() - STRIDE;

    // The X axis: constant z == 0, spanning x.
    CHECK(vertices[xBase].position.z == 0.0F);
    CHECK(vertices[xBase + STRIDE - 1U].position.z == 0.0F);
    CHECK(vertices[xBase].position.x < vertices[xBase + STRIDE - 1U].position.x);  // +X direction
    CHECK(rgbBits(vertices[xBase + (STRIDE / 2U)].rgba) == rgbBits(rd::packDebugColor(params.style.axisXColor)));

    // The Z axis: constant x == 0, spanning z.
    CHECK(vertices[zBase].position.x == 0.0F);
    CHECK(vertices[zBase + STRIDE - 1U].position.x == 0.0F);
    CHECK(vertices[zBase].position.z < vertices[zBase + STRIDE - 1U].position.z);  // +Z direction
    CHECK(rgbBits(vertices[zBase + (STRIDE / 2U)].rgba) == rgbBits(rd::packDebugColor(params.style.axisZColor)));

    SUBCASE("and they span the COARSEST radius, not the finest") {
        // If the axis radius were taken from radius[0] the axes would be a stub in the middle of a
        // grid 100x wider. The half-length is the chord at the disc centre, which for a focus at the
        // origin IS the coarsest radius.
        const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
        const float coarsest = cadence.radius[rd::DEBUG_GRID_CADENCES - 1U];
        CHECK(vertices[xBase + STRIDE - 1U].position.x == doctest::Approx(static_cast<double>(coarsest)).epsilon(1e-5));
        CHECK(vertices[xBase + STRIDE - 1U].position.x > cadence.radius[0] * 2.0F);
    }
}

TEST_CASE("render debug grid: a focus beyond the coarsest radius emits NO axis vertex (GR16)") {
    // CORRECT RATHER THAN TOLERATED: the axes mark the origin, and from out here the origin is not
    // in the picture. Stated in the header, asserted here.
    rd::DebugGridParams params = poseAtScale(8.0F, Vec3{5000.0F, 0.0F, 5000.0F});
    params.style.axisXColor = Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    params.style.axisZColor = Vec4{0.0F, 0.0F, 1.0F, 1.0F};
    const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
    REQUIRE(cadence.valid);
    REQUIRE(cadence.radius[rd::DEBUG_GRID_CADENCES - 1U] < 5000.0F);  // the premise, asserted

    rd::DebugDrawBatch batch = roomyBatch();
    REQUIRE(rd::emitDebugGrid(batch, params) > 0U);  // there is still a grid out here
    const std::uint32_t red = rgbBits(rd::packDebugColor(params.style.axisXColor));
    const std::uint32_t blue = rgbBits(rd::packDebugColor(params.style.axisZColor));
    for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
        CHECK(rgbBits(vertex.rgba) != red);
        CHECK(rgbBits(vertex.rgba) != blue);
    }
}

TEST_CASE("render debug grid: the far-plane clamp bites where it should and NOWHERE ELSE (GR17)") {
    // TWO ARMS, and the NEGATIVE one is what makes the positive one mean something. MEASURED: at the
    // default pose the radii are (2.4, 24, 240) against 0.9 * 1000 = 900, so NOTHING is clamped and
    // halving the far plane changes nothing at all. A one-armed "halving farPlane strictly reduces
    // the count" reddens immediately at the pose anyone actually uses.

    SUBCASE("the clamp does NOT bite at the default pose: bit-identical radii and count") {
        const rd::DebugGridParams nearPose = poseAtScale(8.0F, Vec3::zero(), 1000.0F);
        const rd::DebugGridParams halfPose = poseAtScale(8.0F, Vec3::zero(), 500.0F);
        const rd::DebugGridCadence a = rd::debugGridCadence(nearPose);
        const rd::DebugGridCadence b = rd::debugGridCadence(halfPose);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        for (std::uint32_t c = 0; c < rd::DEBUG_GRID_CADENCES; ++c) {
            CAPTURE(c);
            // The premise, asserted rather than assumed: every radius is the SPACING arm.
            CHECK(a.radius[c] == static_cast<float>(rd::DEBUG_GRID_RADIUS_CELLS) * a.spacing[c]);
            CHECK(a.radius[c] == b.radius[c]);
        }
        rd::DebugDrawBatch batchA = roomyBatch();
        rd::DebugDrawBatch batchB = roomyBatch();
        CHECK(rd::emitDebugGrid(batchA, nearPose) == rd::emitDebugGrid(batchB, halfPose));
    }

    SUBCASE("the clamp DOES bite far out: every radius halves and the count strictly falls") {
        const rd::DebugGridParams nearPose = poseAtScale(8000.0F, Vec3::zero(), 1000.0F);
        const rd::DebugGridParams halfPose = poseAtScale(8000.0F, Vec3::zero(), 500.0F);
        const rd::DebugGridCadence a = rd::debugGridCadence(nearPose);
        const rd::DebugGridCadence b = rd::debugGridCadence(halfPose);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        for (std::uint32_t c = 0; c < rd::DEBUG_GRID_CADENCES; ++c) {
            CAPTURE(c);
            // The premise: every radius is the FAR-PLANE arm here.
            CHECK(a.radius[c] == rd::DEBUG_GRID_FAR_FRACTION * 1000.0F);
            CHECK(b.radius[c] == rd::DEBUG_GRID_FAR_FRACTION * 500.0F);
            CHECK(b.radius[c] < a.radius[c]);
        }
        rd::DebugDrawBatch batchA = roomyBatch();
        rd::DebugDrawBatch batchB = roomyBatch();
        const std::uint32_t nearCount = rd::emitDebugGrid(batchA, nearPose);
        const std::uint32_t halfCount = rd::emitDebugGrid(batchB, halfPose);
        CHECK(halfCount < nearCount);  // measured 144 < 272
        CHECK(halfCount > 0U);         // ...and it did not collapse to nothing
    }

    SUBCASE("the fade completes INSIDE the clamped radius -- there is no hard edge at the far plane") {
        // FAR_FRACTION < 1 is what buys this, and GR3 pins that. Here: the outermost vertex of the
        // outermost line is at alpha 0 AND inside 0.9 * farPlane, so the grid has already faded out
        // before the frustum would have cut it.
        const rd::DebugGridParams params = poseAtScale(8000.0F, Vec3::zero(), 1000.0F);
        rd::DebugDrawBatch batch = roomyBatch();
        REQUIRE(rd::emitDebugGrid(batch, params) > 0U);
        for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
            const float radius =
                std::sqrt((vertex.position.x * vertex.position.x) + (vertex.position.z * vertex.position.z));
            CHECK(radius <= rd::DEBUG_GRID_FAR_FRACTION * 1000.0F * 1.0001F);
        }
    }
}

TEST_CASE("render debug grid: a non-finite camera emits NOTHING and rejects NOTHING (GR18)") {
    // A BAD CAMERA IS NOT A BAD PUSH. The batch's two counters mean different things and this case
    // is what keeps them from being conflated: zero lines AND zero rejections, with the batch left
    // exactly as it was found.
    const std::array<rd::DebugGridParams, 9> bad{{
        {.eye = Vec3{NAN_F, 1.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = 1000.0F},
        {.eye = Vec3{0.0F, INF_F, 0.0F}, .focus = Vec3::zero(), .farPlane = 1000.0F},
        {.eye = Vec3{0.0F, 1.0F, -INF_F}, .focus = Vec3::zero(), .farPlane = 1000.0F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3{NAN_F, 0.0F, 0.0F}, .farPlane = 1000.0F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3{0.0F, INF_F, 0.0F}, .farPlane = 1000.0F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = NAN_F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = INF_F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = 0.0F},
        {.eye = Vec3{0.0F, 8.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = -1.0F},
    }};
    for (std::size_t i = 0; i < bad.size(); ++i) {
        CAPTURE(i);
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitDebugGrid(batch, bad[i]) == 0U);
        CHECK(batch.lineCount() == 0U);
        CHECK(batch.rejectedLines() == 0U);  // NOT a rejection -- nothing was ever pushed
        CHECK(batch.droppedLines() == 0U);
        CHECK(batch.empty());
        CHECK_FALSE(rd::debugGridCadence(bad[i]).valid);
        CHECK((rd::debugGridCadence(bad[i]) == rd::DebugGridCadence{}));  // every field zeroed
    }
    SUBCASE("a POISONED batch is still usable afterwards -- the emitter left no state behind") {
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitDebugGrid(batch, bad[0]) == 0U);
        CHECK(rd::emitDebugGrid(batch, defaultPose()) > 0U);  // the very next call works
    }
}

TEST_CASE("render debug grid: TOTAL over an adversarial sweep, and always under the cap (GR19)") {
    // THE SWEEP THAT FOUND THE NON-TERMINATING LOOP. Two families of probe, each named because each
    // caught something:
    //   * focus at 1e6 with a small viewScale -- the quotient reaches ~1e8, where `k += 1.0F` is a
    //     NO-OP. A float-indexed loop NEVER RETURNS here, so a regression is a HANG rather than a
    //     failure, and ctest's timeout is what would report it.
    //   * focus at 1e38 -- kMin and kMax are both +inf, `inf <= inf` is true, and `inf - inf` is NaN.
    //     A cast of that to an unsigned is UB that UBSan traps.
    // Neither is exotic: 1e6 units out with a close orbit is "fly across the level, then inspect
    // something", and 1e38 is merely FINITE, which is all the totality gate promises to survive.
    for (int decade = -7; decade <= 9; ++decade) {
        for (const float mantissa : {1.0F, 3.0F, 9.9F}) {
            const float scale = mantissa * std::pow(10.0F, static_cast<float>(decade));
            for (const float magnitude : {0.0F, 0.5F, 3.7F, 100.0F, 12345.678F, 1.0e6F, 1.0e10F, 1.0e20F, 1.0e38F}) {
                for (const float farPlane : {1.0e-3F, 1.0F, 1000.0F, 1.0e6F}) {
                    for (const bool axes : {true, false}) {
                        CAPTURE(decade);
                        CAPTURE(mantissa);
                        CAPTURE(magnitude);
                        CAPTURE(farPlane);
                        CAPTURE(axes);
                        rd::DebugGridParams params = poseAtScale(scale, Vec3{magnitude, 0.0F, -magnitude}, farPlane);
                        params.drawAxes = axes;
                        rd::DebugDrawBatch batch = roomyBatch();
                        const std::uint32_t emitted = rd::emitDebugGrid(batch, params);
                        // THE CAP, which is what DEBUG_GRID_MAX_LINES claims to be. Measured worst
                        // over this sweep: 2304 against a bound of 2368.
                        CHECK(emitted <= rd::DEBUG_GRID_MAX_LINES);
                        CHECK(batch.lineCount() == emitted);
                        CHECK(batch.droppedLines() == 0U);
                        for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
                            CHECK(std::isfinite(vertex.position.x));
                            CHECK(std::isfinite(vertex.position.y));
                            CHECK(std::isfinite(vertex.position.z));
                        }
                    }
                }
            }
        }
    }
    // ANTI-VACUITY: a sweep that emitted nothing everywhere would satisfy every assertion above.
    rd::DebugDrawBatch witness = roomyBatch();
    CHECK(rd::emitDebugGrid(witness, defaultPose()) > 1000U);
}

TEST_CASE("render debug grid: against a nearly-full batch it returns what was ACCEPTED (GR20)") {
    // The overflow policy is the batch's, not the emitter's: refusal happens at PUSH time, in PUSH
    // order, and the result is a deterministic PARTIAL grid rather than an all-or-nothing failure.
    // The emitter returns the number the batch ACCEPTED, so a caller can tell the two apart.
    rd::DebugDrawBatch batch{{.maxLines = 100U, .maxBillboards = 16U}};
    for (std::uint32_t i = 0; i < 99U; ++i) {
        REQUIRE(batch.line(Vec3::zero(), Vec3{1.0F, 0.0F, 0.0F}, Vec4{1.0F, 1.0F, 1.0F, 1.0F}));
    }
    REQUIRE(batch.lineCount() == 99U);

    const std::uint32_t accepted = rd::emitDebugGrid(batch, defaultPose());
    CHECK(accepted == 1U);               // exactly the one line there was room for
    CHECK(batch.lineCount() == 100U);    // the budget, exactly
    CHECK(batch.droppedLines() > 0U);    // and the rest are counted as DROPS...
    CHECK(batch.rejectedLines() == 0U);  // ...never as rejections: every push was legal
}

TEST_CASE("render debug grid: two emits with the same params are BYTE-IDENTICAL (GR21)") {
    // PURITY, observed rather than claimed: no static state, no cache, no allocation-order
    // dependence. std::memcmp over the whole vertex span, which is the strongest form available --
    // DebugLineVertex is standard-layout, trivially copyable and has ZERO tail padding (DD1).
    for (const rd::DebugGridParams params :
         {defaultPose(), poseAtScale(137.0F, Vec3{-8.5F, 0.0F, 3.25F}), poseAtScale(0.02F, Vec3{1.0F, 0.0F, 1.0F})}) {
        rd::DebugDrawBatch first = roomyBatch();
        rd::DebugDrawBatch second = roomyBatch();
        const std::uint32_t a = rd::emitDebugGrid(first, params);
        const std::uint32_t b = rd::emitDebugGrid(second, params);
        REQUIRE(a == b);
        const std::span<const rd::DebugLineVertex> va = first.lineVertices(rd::DebugDepth::Tested);
        const std::span<const rd::DebugLineVertex> vb = second.lineVertices(rd::DebugDepth::Tested);
        REQUIRE(va.size() == vb.size());
        REQUIRE_FALSE(va.empty());  // anti-vacuity: memcmp of two empty spans is trivially equal
        CHECK(std::memcmp(va.data(), vb.data(), va.size_bytes()) == 0);
    }
    SUBCASE("...and a SECOND emit into the SAME batch appends rather than replacing") {
        // Nothing about the emitter is idempotent on a batch, and it must not pretend to be: it is
        // an immediate-mode push, and flush() is what drains it.
        rd::DebugDrawBatch batch = roomyBatch();
        const std::uint32_t once = rd::emitDebugGrid(batch, defaultPose());
        const std::uint32_t twice = rd::emitDebugGrid(batch, defaultPose());
        CHECK(once == twice);
        CHECK(batch.lineCount() == once + twice);
    }
}

TEST_CASE("render debug grid: the emitter reaches NO libm function but sqrt/floor/ceil (GR22)") {
    // THE DETERMINISM CLAIM, PINNED AS SOURCE TEXT, because there is no behaviour to observe: a
    // std::log10 implementation and this iteration agree to six significant digits on every input
    // anyone would try by hand, and disagree in the last bits on some lane somewhere -- which is
    // exactly the failure that costs three days. The DD26 reader: COMMENT-STRIPPED, so the banner's
    // own sentence naming std::pow and std::log10 as absent cannot satisfy the search.
    const std::string source = strippedSourceAt(DEBUG_GRID_SOURCE_PATH);
    REQUIRE_FALSE(source.empty());  // non-vacuity, first direction: the path resolved

    // (a) the four banned tokens
    CHECK_FALSE(contains(source, "log10"));
    CHECK_FALSE(contains(source, "std::log"));
    CHECK_FALSE(contains(source, "std::pow"));
    CHECK_FALSE(contains(source, "std::exp"));
    CHECK_FALSE(contains(source, "std::sin"));
    CHECK_FALSE(contains(source, "std::cos"));

    // (b) non-vacuity, second direction: the three ADMITTED ones ARE present, so a reader that
    //     matched nothing could not fake the six lines above.
    CHECK(contains(source, "std::sqrt"));
    CHECK(contains(source, "std::floor"));
    CHECK(contains(source, "std::ceil"));

    // (c) ...and the search can say NO to something that is genuinely absent, which is the third
    //     direction and the one a stripped-to-empty reader would still pass without.
    CHECK_FALSE(contains(source, "std::this_is_not_a_function"));
}

TEST_CASE("render debug grid: viewScale is the MAX of two arms, and each arm is discriminated (GR23)") {
    // THREE PROBES, each of which fails under a DIFFERENT one-line regression. A single probe would
    // pass under both `distance only` and `height only` for half the poses anyone tries.
    SUBCASE("distance dominates: orbiting close to the ground") {
        // eye 10 units from the focus but only 0.5 above the plane. `height only` would give 0.5.
        const rd::DebugGridCadence cadence = rd::debugGridCadence(
            {.eye = Vec3{10.0F, 0.5F, 0.0F}, .focus = Vec3{0.0F, 0.5F, 0.0F}, .farPlane = 1000.0F});
        REQUIRE(cadence.valid);
        CHECK(cadence.viewScale == 10.0F);
    }
    SUBCASE("height dominates: flying high with the pivot dragged along") {
        // THE FLY-MODE CASE. EditorCamera's fly mode translates the pivot WITH the eye, so distance()
        // stays small however high you go -- `distance only` would give 0.5 at 10 units up, which is
        // a 0.001-metre cadence for a scene a hundred metres wide.
        const rd::DebugGridCadence cadence = rd::debugGridCadence(
            {.eye = Vec3{0.0F, 10.0F, 0.0F}, .focus = Vec3{0.0F, 9.5F, 0.0F}, .farPlane = 1000.0F});
        REQUIRE(cadence.valid);
        CHECK(cadence.viewScale == 10.0F);
    }
    SUBCASE("the tie: both arms agree, and the max is not a tie-break bug") {
        const rd::DebugGridCadence cadence =
            rd::debugGridCadence({.eye = Vec3{0.0F, 10.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = 1000.0F});
        REQUIRE(cadence.valid);
        CHECK(cadence.viewScale == 10.0F);
    }
    SUBCASE("below the plane: |eye.y| is used, so the grid is identical seen from underneath") {
        const rd::DebugGridCadence above =
            rd::debugGridCadence({.eye = Vec3{0.0F, 12.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = 1000.0F});
        const rd::DebugGridCadence below =
            rd::debugGridCadence({.eye = Vec3{0.0F, -12.0F, 0.0F}, .focus = Vec3::zero(), .farPlane = 1000.0F});
        REQUIRE(above.valid);
        REQUIRE(below.valid);
        CHECK(below.viewScale == above.viewScale);
        CHECK(below.level == above.level);
        CHECK(below.fraction == above.fraction);
    }
}

TEST_CASE("render debug grid: the emission ORDER is cadence-major, A then B, k ascending (GR24)") {
    // ORDER IS THE CONTRACT. Nothing depends on it for correctness -- the Tested bucket is one draw
    // and depth write is off -- but a stable order is what makes GR6's vertex arm and GR21's memcmp
    // mean anything, and what makes a diff of two frames readable.
    //
    // THE CADENCE IS RECOVERED FROM POSITION IN THE SEQUENCE, NEVER FROM COORDINATE DIVISIBILITY,
    // AND THAT IS A MEASURED CONSTRAINT RATHER THAN A TASTE. "Which of the three spacings is this
    // coordinate an exact float multiple of" CANNOT name the cadence, in either scan direction,
    // because the three lattices NEST in float32. Measured at the default pose, where
    // spacing = (0.100000001, 1, 10):
    //
    //     coord    multiple of s0 / s1 / s2     finest-first   coarsest-first   truth
    //      -2.3         yes   no    no               0               0            0
    //       -23         YES  yes    no               0               1            1
    //        10         YES  yes   YES               0               2            1   <-- defeats BOTH
    //      -230         YES  yes   yes               0               2            2
    //         0         yes  yes   yes               0               2      0, 1 AND 2
    //
    // Two mechanisms, both measured rather than reasoned. (1) `round(k / 0.1F) * 0.1F` returns
    // EXACTLY `k` for the integers the coarser cadences produce: at k = -23 the product's error is
    // 3.427e-07 against a half-ulp of 9.537e-07 at that magnitude, so it rounds straight back -- the
    // ulp that "should" separate the lattices is not there. (2) 10.0 is an exact multiple of ALL
    // THREE spacings at once, so it defeats finest-first and coarsest-first SIMULTANEOUSLY and there
    // is no third direction to try. And with drawAxes == false the k == 0 line is emitted by EVERY
    // cadence, so coord == 0 belongs to all three at the same time and no single answer is even
    // correct. DO NOT "simplify" this back to a divisibility test: it is not a fragile heuristic,
    // it is an impossible one.
    //
    // WHAT WORKS, and why it is stronger. Each (cadence, family) block restarts at the disc's
    // NEGATIVE rim, so coord drops at every block boundary and strictly ascends inside one.
    // Splitting on that yields exactly 2 * CADENCES blocks and the block INDEX names the cadence.
    // The block COUNT carries the ascending claim as well: one out-of-order run inside a family
    // would split that family in two and the count would exceed six.
    for (const bool axes : {true, false}) {
        CAPTURE(axes);
        rd::DebugGridParams params = defaultPose();
        params.drawAxes = axes;
        const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
        REQUIRE(cadence.valid);
        rd::DebugDrawBatch batch = roomyBatch();
        REQUIRE(rd::emitDebugGrid(batch, params) > 0U);
        const std::vector<rd::DebugLineVertex> vertices = testedVertices(batch);
        constexpr std::size_t STRIDE = std::size_t{2} * rd::DEBUG_GRID_FADE_SEGMENTS;
        REQUIRE(vertices.size() % STRIDE == 0U);
        const std::size_t runs = vertices.size() / STRIDE;

        // The two axes are the LAST two runs when they are drawn (GR15 pins that), so the grid's own
        // blocks are everything before them. Excluded here rather than special-cased in the walk.
        const std::size_t axisRuns = axes ? std::size_t{2} : std::size_t{0};
        REQUIRE(runs > axisRuns);
        const std::size_t gridRuns = runs - axisRuns;
        // ANTI-VACUITY: measured 276 grid runs with the axes on and 282 with them off, so a walk
        // over a handful of runs means the emitter collapsed rather than that the order held.
        CHECK(gridRuns > 100U);

        // Read every run's family FROM THE GEOMETRY -- a family-B line is constant in x, a family-A
        // line is constant in z -- and its constant coordinate. Nothing here is inferred from the
        // position in the sequence; that is what the block index is for, below.
        std::vector<int> runFamily;
        std::vector<float> runCoord;
        runFamily.reserve(gridRuns);
        runCoord.reserve(gridRuns);
        for (std::size_t run = 0; run < gridRuns; ++run) {
            const std::size_t base = run * STRIDE;
            const int family = (vertices[base].position.x == vertices[base + 1U].position.x) ? 1 : 0;
            runFamily.push_back(family);
            runCoord.push_back((family == 1) ? vertices[base].position.x : vertices[base].position.z);
        }

        // SPLIT wherever coord fails to STRICTLY ASCEND. That is the whole structural signal.
        std::vector<std::size_t> blockOf(gridRuns, 0);
        std::size_t blocks = 0;
        for (std::size_t run = 0; run < gridRuns; ++run) {
            if (run == 0 || !(runCoord[run] > runCoord[run - 1U])) {
                ++blocks;
            }
            blockOf[run] = blocks - 1U;
        }
        // EXACTLY 2 * CADENCES BLOCKS. REQUIRE rather than CHECK for two reasons: a wrong count
        // makes every assertion below noise, and `block / 2` would index cadence.spacing out of
        // range. This one number is also the k-ascending claim -- see the banner.
        REQUIRE(blocks == std::size_t{2} * rd::DEBUG_GRID_CADENCES);

        for (std::size_t run = 0; run < gridRuns; ++run) {
            const std::size_t block = blockOf[run];
            CAPTURE(run);
            CAPTURE(block);
            CAPTURE(runCoord[run]);
            // Family A on even blocks, B on odd: within one cadence, A is pushed before B.
            CHECK(runFamily[run] == static_cast<int>(block % 2U));
            // ...and the block INDEX names the cadence, which is what ties the lattice claim to the
            // ORDER rather than to a coordinate the three lattices all contain.
            const float spacing = cadence.spacing[block / 2U];
            const float k = std::round(runCoord[run] / spacing);
            CHECK(k * spacing == runCoord[run]);
        }

        if (axes) {
            // ...and the two runs excluded above really ARE the axes, so the exclusion cannot be
            // quietly dropping two grid lines: both sit at the constant coordinate zero.
            for (std::size_t run = gridRuns; run < runs; ++run) {
                const std::size_t base = run * STRIDE;
                const int family = (vertices[base].position.x == vertices[base + 1U].position.x) ? 1 : 0;
                CAPTURE(run);
                CHECK(((family == 1) ? vertices[base].position.x : vertices[base].position.z) == 0.0F);
            }
        }
    }
}

TEST_CASE("render debug grid: emitDebugGrid and debugGridCadence make ONE decision (GR25)") {
    // THE SEAM'S WHOLE POINT. If the emitter recomputed the cadence internally rather than calling
    // this function, every assertion in GR6-GR9, GR17 and GR23 would be about a second
    // implementation that agrees with the first only by review. This case is what ties them.
    for (const rd::DebugGridParams params :
         {defaultPose(), poseAtScale(0.02F), poseAtScale(4200.0F), poseAtScale(8.0F, Vec3{9000.0F, 0.0F, 0.0F})}) {
        const rd::DebugGridCadence cadence = rd::debugGridCadence(params);
        rd::DebugDrawBatch batch = roomyBatch();
        const std::uint32_t emitted = rd::emitDebugGrid(batch, params);
        CHECK(cadence.valid == (emitted > 0U));
        if (!cadence.valid) {
            continue;
        }
        // Every emitted grid line's constant coordinate is a multiple of one of THESE THREE
        // spacings, and every emitted alpha is bounded by the largest of THESE THREE weights. Two
        // independent implementations would agree on neither for long.
        const float maxWeight = std::max({cadence.weight[0], cadence.weight[1], cadence.weight[2],
                                          params.style.axisXColor.w, params.style.axisZColor.w});
        for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
            CHECK(static_cast<float>(alphaByte(vertex.rgba)) / 255.0F <= maxWeight + 0.005F);
        }
    }
    SUBCASE("purity: calling the cadence twice returns the identical struct") {
        const rd::DebugGridParams params = defaultPose();
        CHECK((rd::debugGridCadence(params) == rd::debugGridCadence(params)));
    }
}
