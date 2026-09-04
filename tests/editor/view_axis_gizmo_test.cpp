// tests/editor/view_axis_gizmo_test.cpp — task E.1.3: the view-axis gizmo's tier-0 battery. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: identical with AERO_REQUIRE_GPU unset and
// set, and NO `#if` of any kind (the 3.6.3 rule -- four cases once shipped inside a file-level #if
// with everything green while the one arm that mattered never ran).
//
// EVERY CASE ASSERTS A RELATIONSHIP -- an ordering, a sign, an inclusion, an invariance -- rather
// than a tuning magnitude, so retuning any of the seven VIEW_AXIS_* constants or VIEW_SNAP_SECONDS
// reddens nothing. That is 2.3.1's design promise applied to this task's constants.
#include <aero/editor/axis_palette.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/view_axis_gizmo.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using engine::Vec2;
using engine::Vec3;
using engine::editor::EditorCamera;
using engine::editor::VIEW_AXIS_BALL_RADIUS_POINTS;
using engine::editor::VIEW_AXIS_CENTER_RADIUS_POINTS;
using engine::editor::VIEW_AXIS_COUNT;
using engine::editor::VIEW_AXIS_HALF_EXTENT_POINTS;
using engine::editor::VIEW_AXIS_MARGIN_POINTS;
using engine::editor::VIEW_AXIS_MIN_IMAGE_POINTS;
using engine::editor::VIEW_AXIS_RING_RADIUS_POINTS;
using engine::editor::ViewAxis;
using engine::editor::ViewAxisBall;
using engine::editor::ViewAxisHit;
using engine::editor::viewAxisIsPositive;
using engine::editor::viewAxisLabel;
using engine::editor::ViewAxisLayout;
using engine::editor::viewAxisLayout;
using engine::editor::viewAxisPaletteKey;
using engine::editor::ViewAxisPick;
using engine::editor::viewAxisPickAt;
using engine::editor::viewAxisRect;

namespace {

// A generous image, comfortably above VIEW_AXIS_MIN_IMAGE_POINTS in both axes.
constexpr Vec2 IMAGE_ORIGIN{100.0F, 50.0F};
constexpr Vec2 IMAGE_SIZE{900.0F, 600.0F};

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

// The six enumerators, in ViewAxis order, so a loop can name them.
constexpr std::array<ViewAxis, 6> ALL_AXES{ViewAxis::PosX, ViewAxis::NegX, ViewAxis::PosY,
                                           ViewAxis::NegY, ViewAxis::PosZ, ViewAxis::NegZ};

// Puts the camera in an exactly-known orientation. The pose functions are COMMIT 3's; this file's
// layout cases predate them, so they set yaw/pitch directly through the camera's own clamped
// setters -- which is also what makes `right()`/`up()`/`forward()` the real thing rather than a
// hand-built basis.
[[nodiscard]] EditorCamera cameraAt(float yaw, float pitch) {
    EditorCamera camera;
    camera.setYaw(yaw);
    camera.setPitch(pitch);
    return camera;
}

[[nodiscard]] const ViewAxisBall& ballFor(const ViewAxisLayout& layout, ViewAxis axis) {
    return layout.balls[static_cast<std::size_t>(axis)];
}

}  // namespace

TEST_CASE("editor view-axis gizmo: the vocabulary's layout pins and the D10 relationship (VA1)") {
    // RELATIONSHIPS, never magnitudes. Retuning RING/BALL/CENTER/MARGIN must redden nothing here --
    // what is asserted is what the DESIGN rests on, not what the numbers happen to be today.
    static_assert(sizeof(ViewAxis) == 1);     // performance-enum-size, --warnings-as-errors on Linux
    static_assert(sizeof(ViewAxisHit) == 1);  // ditto
    static_assert(VIEW_AXIS_COUNT == 6);

    SUBCASE("the centre badge is SMALLER than a ball -- D10's whole argument") {
        // A ball collapsed onto the widget centre (the canonical view, where the axis you look down
        // projects to zero offset) must still leave an annulus that reaches the BALL rather than the
        // badge. VA7's second arm is the behavioural form of this; here it is the invariant.
        CHECK(VIEW_AXIS_CENTER_RADIUS_POINTS < VIEW_AXIS_BALL_RADIUS_POINTS);
    }
    SUBCASE("the half-extent IS the ring plus one ball radius") {
        // DERIVED, not transcribed: if this ever became an independent literal, the rect and the
        // balls could disagree and a ball would poke outside the press-claim rect.
        CHECK(VIEW_AXIS_HALF_EXTENT_POINTS == VIEW_AXIS_RING_RADIUS_POINTS + VIEW_AXIS_BALL_RADIUS_POINTS);
    }
}

TEST_CASE("editor view-axis gizmo: the layout's screen mapping, and the y NEGATION (VA2)") {
    // THE DISCRIMINATOR FOR SEED S1. ImGui's +y is screen-DOWN and the camera's up() is world-UP, so
    // the y term must be negated -- the same flip viewportNdc performs in the other direction
    // (picking.cpp:56-57). Dropping the `-` puts every ball on the wrong side of the ring and the
    // widget silently becomes a mirror of the camera.
    const EditorCamera camera = cameraAt(engine::radians(30.0F), engine::radians(-20.0F));  // the shipped default pose
    const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
    REQUIRE(layout.visible);

    SUBCASE("six balls, one per enumerator, in ViewAxis order") {
        for (std::size_t i = 0; i < VIEW_AXIS_COUNT; ++i) {
            CAPTURE(i);
            CHECK((layout.balls[i].axis == ALL_AXES[i]));
        }
    }
    SUBCASE("every ball lies ON the ring -- the projection is orthographic onto a fixed circle") {
        for (const ViewAxisBall& ball : layout.balls) {
            CAPTURE(static_cast<int>(ball.axis));
            // <= rather than ==: an axis pointing partly at the camera projects INSIDE the ring.
            CHECK(engine::length(ball.offsetPoints) <= VIEW_AXIS_RING_RADIUS_POINTS + 1.0e-4F);
        }
    }
    SUBCASE("+X lands on the +x screen side and +Y lands on the SCREEN-UP (-y) side") {
        // At yaw +30 deg the world +X axis still has a positive component along the camera's right(),
        // and at pitch -20 deg the world +Y axis is above the horizon -- so this is a claim about
        // the SIGN of the mapping, which is exactly what the negation decides.
        CHECK(ballFor(layout, ViewAxis::PosX).offsetPoints.x > 0.0F);
        CHECK(ballFor(layout, ViewAxis::NegX).offsetPoints.x < 0.0F);
        CHECK(ballFor(layout, ViewAxis::PosY).offsetPoints.y < 0.0F);  // SCREEN-UP: the negation
        CHECK(ballFor(layout, ViewAxis::NegY).offsetPoints.y > 0.0F);
    }
    SUBCASE("+X and -X are exact opposites about the centre, in BOTH components") {
        // Anti-vacuity for the sign arm above: a mapping that clamped or dropped a component would
        // still satisfy "the sign is right" for one of the pair.
        // EXACT ==, no tolerance: dot({1,0,0}, right) is right.x and dot({-1,0,0}, right) is -right.x
        // bit for bit, and both are then scaled by the same RING constant -- the arithmetic really is
        // exact, so a tolerance here would let a genuine asymmetry through.
        const Vec2 posX = ballFor(layout, ViewAxis::PosX).offsetPoints;
        const Vec2 negX = ballFor(layout, ViewAxis::NegX).offsetPoints;
        CHECK(posX.x == -negX.x);
        CHECK(posX.y == -negX.y);
    }
}

TEST_CASE("editor view-axis gizmo: depth order, the front hemisphere, and their INDEPENDENCE (VA3)") {
    const EditorCamera camera = cameraAt(engine::radians(30.0F), engine::radians(-20.0F));
    const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
    REQUIRE(layout.visible);

    SUBCASE("drawOrder is a PERMUTATION of 0..5") {
        std::array<bool, VIEW_AXIS_COUNT> seen{};
        for (const std::uint8_t index : layout.drawOrder) {
            REQUIRE(index < VIEW_AXIS_COUNT);
            CHECK_FALSE(seen[index]);  // no index appears twice
            seen[index] = true;
        }
        for (const bool s : seen) {
            CHECK(s);  // ...and none is missing
        }
    }
    SUBCASE("depth is NON-INCREASING along drawOrder -- far drawn first") {
        for (std::size_t i = 1; i < VIEW_AXIS_COUNT; ++i) {
            CAPTURE(i);
            const float previous = layout.balls[layout.drawOrder[i - 1U]].depth;
            const float current = layout.balls[layout.drawOrder[i]].depth;
            CHECK(previous >= current);
        }
    }
    SUBCASE("the front hemisphere is EXACTLY {depth < 0}, and it is non-empty in both halves") {
        std::size_t inFront = 0;
        for (const ViewAxisBall& ball : layout.balls) {
            if (ball.depth < 0.0F) {
                ++inFront;
            }
        }
        // Three axes point toward the eye and three away at any non-degenerate pose. A ball at
        // exactly zero depth is edge-on and would break the tie either way -- the default pose has
        // none, which the strict count below is what proves.
        CHECK(inFront == 3U);
    }
    SUBCASE("SIGN and DEPTH come from DIFFERENT inputs -- rotate 180 deg and only depth moves") {
        // THE E.1.2 GR8 LESSON, applied: this arm changes the CAMERA and reads BOTH quantities back
        // off the layout, so it cannot be satisfied by a `positive` that was derived from `depth`.
        // A widget that dimmed the negative axes instead of the far ones would pass every other
        // subcase in this case and fail here.
        const EditorCamera turned = cameraAt(engine::radians(30.0F) + engine::PI, engine::radians(-20.0F));
        const ViewAxisLayout after = viewAxisLayout(turned, IMAGE_ORIGIN, IMAGE_SIZE);
        REQUIRE(after.visible);
        for (std::size_t i = 0; i < VIEW_AXIS_COUNT; ++i) {
            CAPTURE(i);
            // X and Z invert; Y is the yaw axis and its depth is unchanged by a yaw turn, so the
            // assertion is on the FOUR equatorial axes and Y is asserted to be UNMOVED.
            const bool equatorial = ALL_AXES[i] != ViewAxis::PosY && ALL_AXES[i] != ViewAxis::NegY;
            if (equatorial) {
                CHECK(layout.balls[i].depth == doctest::Approx(-after.balls[i].depth).epsilon(1.0e-5));
            } else {
                CHECK(layout.balls[i].depth == doctest::Approx(after.balls[i].depth).epsilon(1.0e-5));
            }
            CHECK(layout.balls[i].positive == after.balls[i].positive);  // sign never moves
        }
    }
}

TEST_CASE("editor view-axis gizmo: the axis you look DOWN collapses onto the centre (VA4)") {
    // A Front view: yaw 0, pitch 0 -> forward() is world -Z, so the +Z ball is directly BEHIND the
    // eye's gaze (nearest the viewer) and the -Z ball is directly away from it.
    const EditorCamera camera = cameraAt(0.0F, 0.0F);
    const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
    REQUIRE(layout.visible);

    const ViewAxisBall& posZ = ballFor(layout, ViewAxis::PosZ);
    const ViewAxisBall& negZ = ballFor(layout, ViewAxis::NegZ);

    CHECK(engine::length(posZ.offsetPoints) <= 1.0e-5F);  // collapsed: no screen offset at all
    CHECK(engine::length(negZ.offsetPoints) <= 1.0e-5F);
    // dot(+Z, forward) == dot(+Z, -Z) == -1: NEAREST. dot(-Z, forward) == +1: FARTHEST.
    CHECK(posZ.depth == doctest::Approx(-1.0).epsilon(1.0e-5));
    CHECK(negZ.depth == doctest::Approx(1.0).epsilon(1.0e-5));
    // ...and the four equatorial balls are still out on the ring, so "collapsed" means something.
    CHECK(engine::length(ballFor(layout, ViewAxis::PosX).offsetPoints) ==
          doctest::Approx(VIEW_AXIS_RING_RADIUS_POINTS).epsilon(1.0e-5));
}

TEST_CASE("editor view-axis gizmo: ONE visibility predicate, FOUR consequences (VA5)") {
    // D16. Nothing draws, nothing is hit, the rect is degenerate and the press claim is empty -- all
    // four out of viewAxisVisible(), not out of four comparisons that could drift apart. Seed S12
    // (viewAxisRect returning a full-image rect when invisible) and the sort's non-finite guard
    // (seed S15) are both discriminated here.
    const EditorCamera camera = cameraAt(engine::radians(30.0F), engine::radians(-20.0F));

    struct HiddenCase {
        const char* name;
        Vec2 origin;
        Vec2 size;
    };
    // Each row is invisible for its OWN reason, and the sub-threshold pair is split so ONE axis at a
    // time is under the minimum -- a predicate that tested only x, or only y, passes half of these.
    const std::array<HiddenCase, 6> hidden{{
        {"zero size", IMAGE_ORIGIN, Vec2::zero()},
        {"NaN size", IMAGE_ORIGIN, Vec2{NAN_F, NAN_F}},
        {"NaN origin", Vec2{NAN_F, 0.0F}, IMAGE_SIZE},
        {"narrow in x only", IMAGE_ORIGIN, Vec2{VIEW_AXIS_MIN_IMAGE_POINTS - 1.0F, IMAGE_SIZE.y}},
        {"narrow in y only", IMAGE_ORIGIN, Vec2{IMAGE_SIZE.x, VIEW_AXIS_MIN_IMAGE_POINTS - 1.0F}},
        {"negative size", IMAGE_ORIGIN, Vec2{-900.0F, -600.0F}},
    }};

    for (const HiddenCase& row : hidden) {
        CAPTURE(row.name);
        const ViewAxisLayout layout = viewAxisLayout(camera, row.origin, row.size);
        CHECK_FALSE(layout.visible);

        Vec2 rectMin{};
        Vec2 rectMax{};
        viewAxisRect(row.origin, row.size, rectMin, rectMax);
        CHECK_FALSE(rectMax.x > rectMin.x);  // DEGENERATE -- an empty rect owns nothing
        CHECK_FALSE(rectMax.y > rectMin.y);

        // ...and nothing is hit, not even at what WOULD have been the widget centre.
        CHECK((viewAxisPickAt(layout, layout.centerPoints).kind == ViewAxisHit::None));
        CHECK((viewAxisPickAt(layout, Vec2{rectMin.x, rectMin.y}).kind == ViewAxisHit::None));
    }

    SUBCASE("exactly AT the minimum in both axes is VISIBLE -- the threshold is inclusive") {
        // Anti-vacuity for the two sub-threshold rows: a predicate that hid EVERYTHING would satisfy
        // all six rows above and this arm is what says it does not.
        const ViewAxisLayout layout =
            viewAxisLayout(camera, IMAGE_ORIGIN, Vec2{VIEW_AXIS_MIN_IMAGE_POINTS, VIEW_AXIS_MIN_IMAGE_POINTS});
        CHECK(layout.visible);
    }
    SUBCASE("a POISONED camera hides the widget rather than sorting NaNs") {
        // std::sort with a comparator that is not a strict weak ordering is UNDEFINED BEHAVIOUR and
        // can read out of bounds -- not merely a wrong order. clampState deliberately leaves a
        // directly-set NaN in place for stateIsFinite() to sweep on the next update(), so this state
        // is reachable between a setYaw(NaN) and the next frame.
        EditorCamera poisoned;
        poisoned.setYaw(NAN_F);
        REQUIRE_FALSE(std::isfinite(poisoned.yaw()));  // anti-vacuity: the poison really landed
        const ViewAxisLayout layout = viewAxisLayout(poisoned, IMAGE_ORIGIN, IMAGE_SIZE);
        CHECK_FALSE(layout.visible);
        CHECK((viewAxisPickAt(layout, IMAGE_ORIGIN).kind == ViewAxisHit::None));
    }
}

TEST_CASE("editor view-axis gizmo: the hit test hits what it draws, and refuses what it does not (VA6)") {
    const EditorCamera camera = cameraAt(engine::radians(30.0F), engine::radians(-20.0F));
    const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
    REQUIRE(layout.visible);

    SUBCASE("the exact centre of each ball picks THAT ball") {
        for (const ViewAxisBall& ball : layout.balls) {
            CAPTURE(static_cast<int>(ball.axis));
            const Vec2 at = layout.centerPoints + ball.offsetPoints;
            const ViewAxisPick pick = viewAxisPickAt(layout, at);
            CHECK((pick.kind == ViewAxisHit::Axis));
            CHECK((pick.axis == ball.axis));
        }
    }
    SUBCASE("just OUTSIDE a ball's radius picks nothing") {
        // Taken on the +X ball, along the direction pointing AWAY from the widget centre, so the
        // probe cannot accidentally land on the centre badge or on another ball.
        const ViewAxisBall& posX = ballFor(layout, ViewAxis::PosX);
        const Vec2 outward = engine::normalizeOrZero(posX.offsetPoints);
        REQUIRE(engine::lengthSquared(outward) > 0.0F);
        const Vec2 at = layout.centerPoints + posX.offsetPoints + (outward * (VIEW_AXIS_BALL_RADIUS_POINTS + 0.5F));
        CHECK((viewAxisPickAt(layout, at).kind == ViewAxisHit::None));
    }
    SUBCASE("the widget centre picks Center") {
        CHECK((viewAxisPickAt(layout, layout.centerPoints).kind == ViewAxisHit::Center));
    }
    SUBCASE("inside the rect but on no target picks None") {
        // The rect's top-left CORNER: inside the press-claim box (which is what makes the press
        // chrome) but on neither the ring nor the badge, since the ring's radius is smaller than the
        // half-extent by a whole ball radius.
        Vec2 rectMin{};
        Vec2 rectMax{};
        viewAxisRect(IMAGE_ORIGIN, IMAGE_SIZE, rectMin, rectMax);
        CHECK((viewAxisPickAt(layout, Vec2{rectMin.x + 0.5F, rectMin.y + 0.5F}).kind == ViewAxisHit::None));
    }
    SUBCASE("a NaN mouse position picks None") {
        // The negated `<=` idiom throughout the ladder: every comparison with NaN is false, so the
        // POSITIVE form would have ACCEPTED this and returned whichever target it tested first.
        CHECK((viewAxisPickAt(layout, Vec2{NAN_F, NAN_F}).kind == ViewAxisHit::None));
        CHECK((viewAxisPickAt(layout, Vec2{layout.centerPoints.x, NAN_F}).kind == ViewAxisHit::None));
    }
}

TEST_CASE("editor view-axis gizmo: the hit ladder's two orderings (VA7)") {
    SUBCASE("NEAR beats FAR where two balls overlap") {
        // THE DISCRIMINATOR FOR SEED S2 (sorting drawOrder ascending). At a pose where +X and -X are
        // nearly edge-on their two balls sit almost on top of each other; the one in FRONT (smaller
        // depth) must win, because it is the one drawn last and therefore the one visible.
        //
        // Yaw is chosen so |dot(+X, right())| is small -- the two X balls collapse toward the centre
        // together -- while their DEPTHS stay well separated.
        const EditorCamera camera = cameraAt(engine::HALF_PI, 0.0F);  // looking down world -X
        const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
        REQUIRE(layout.visible);
        const ViewAxisBall& posX = ballFor(layout, ViewAxis::PosX);
        const ViewAxisBall& negX = ballFor(layout, ViewAxis::NegX);
        // Anti-vacuity: the two really are on top of one another, and really differ in depth.
        REQUIRE(engine::length(posX.offsetPoints - negX.offsetPoints) < VIEW_AXIS_BALL_RADIUS_POINTS);
        REQUIRE(posX.depth < negX.depth);
        // A probe just outside the centre badge, on the ring's own axis, reaches both balls.
        const Vec2 at = layout.centerPoints + Vec2{0.0F, VIEW_AXIS_CENTER_RADIUS_POINTS + 1.0F};
        const ViewAxisPick pick = viewAxisPickAt(layout, at);
        CHECK((pick.kind == ViewAxisHit::Axis));
        CHECK((pick.axis == ViewAxis::PosX));  // the NEARER of the two
    }
    SUBCASE("the CENTRE beats a collapsed ball, and the annulus still reaches the ball") {
        // D10's second ordering, and the reason CENTER_RADIUS < BALL_RADIUS. In a Front view the
        // +Z ball is exactly on the widget centre; a ladder that tested balls first would make the
        // projection toggle unreachable forever.
        const EditorCamera camera = cameraAt(0.0F, 0.0F);
        const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
        REQUIRE(layout.visible);
        REQUIRE(engine::length(ballFor(layout, ViewAxis::PosZ).offsetPoints) <= 1.0e-5F);  // really collapsed

        CHECK((viewAxisPickAt(layout, layout.centerPoints).kind == ViewAxisHit::Center));
        // ...and one point past the badge's edge the collapsed ball takes over: the 2-point annulus.
        const Vec2 annulus = layout.centerPoints + Vec2{VIEW_AXIS_CENTER_RADIUS_POINTS + 1.0F, 0.0F};
        const ViewAxisPick pick = viewAxisPickAt(layout, annulus);
        CHECK((pick.kind == ViewAxisHit::Axis));
        CHECK((pick.axis == ViewAxis::PosZ));  // the NEAREST collapsed ball, not -Z behind it
    }
}

TEST_CASE("editor view-axis gizmo: the rect sits in the image's corner and AGREES with the layout (VA8)") {
    Vec2 rectMin{};
    Vec2 rectMax{};
    viewAxisRect(IMAGE_ORIGIN, IMAGE_SIZE, rectMin, rectMax);

    SUBCASE("wholly inside the image") {
        CHECK(rectMin.x >= IMAGE_ORIGIN.x);
        CHECK(rectMin.y >= IMAGE_ORIGIN.y);
        CHECK(rectMax.x <= IMAGE_ORIGIN.x + IMAGE_SIZE.x);
        CHECK(rectMax.y <= IMAGE_ORIGIN.y + IMAGE_SIZE.y);
    }
    SUBCASE("inset by exactly MARGIN from the top-right corner") {
        CHECK(rectMax.x == IMAGE_ORIGIN.x + IMAGE_SIZE.x - VIEW_AXIS_MARGIN_POINTS);
        CHECK(rectMin.y == IMAGE_ORIGIN.y + VIEW_AXIS_MARGIN_POINTS);
    }
    SUBCASE("square, 2 x HALF_EXTENT on a side") {
        CHECK(rectMax.x - rectMin.x == 2.0F * VIEW_AXIS_HALF_EXTENT_POINTS);
        CHECK(rectMax.y - rectMin.y == 2.0F * VIEW_AXIS_HALF_EXTENT_POINTS);
    }
    SUBCASE("its centre IS layout.centerPoints -- the two functions cannot disagree") {
        // The press claim reads viewAxisRect and the draw reads viewAxisLayout. If these two ever
        // parted company you could click a ball that the rect did not claim, or claim a press where
        // nothing is drawn.
        const EditorCamera camera = cameraAt(engine::radians(30.0F), engine::radians(-20.0F));
        const ViewAxisLayout layout = viewAxisLayout(camera, IMAGE_ORIGIN, IMAGE_SIZE);
        REQUIRE(layout.visible);
        CHECK((rectMin.x + rectMax.x) * 0.5F == layout.centerPoints.x);
        CHECK((rectMin.y + rectMax.y) * 0.5F == layout.centerPoints.y);
    }
}

TEST_CASE("editor view-axis gizmo: label, palette key and sign are total and agree pairwise (VA9)") {
    SUBCASE("the three labels are X, Y, Z and a pair shares one") {
        CHECK(viewAxisLabel(ViewAxis::PosX) == 'X');
        CHECK(viewAxisLabel(ViewAxis::NegX) == 'X');
        CHECK(viewAxisLabel(ViewAxis::PosY) == 'Y');
        CHECK(viewAxisLabel(ViewAxis::NegY) == 'Y');
        CHECK(viewAxisLabel(ViewAxis::PosZ) == 'Z');
        CHECK(viewAxisLabel(ViewAxis::NegZ) == 'Z');
    }
    SUBCASE("the palette key agrees with the label, and colours the pair identically") {
        CHECK((viewAxisPaletteKey(ViewAxis::PosX) == engine::editor::Axis::X));
        CHECK((viewAxisPaletteKey(ViewAxis::NegX) == engine::editor::Axis::X));
        CHECK((viewAxisPaletteKey(ViewAxis::PosY) == engine::editor::Axis::Y));
        CHECK((viewAxisPaletteKey(ViewAxis::NegY) == engine::editor::Axis::Y));
        CHECK((viewAxisPaletteKey(ViewAxis::PosZ) == engine::editor::Axis::Z));
        CHECK((viewAxisPaletteKey(ViewAxis::NegZ) == engine::editor::Axis::Z));
        // The whole reason the key exists: -X is drawn in X's colour, differing only in FILL.
        CHECK((engine::editor::axisColorSrgbBytes(viewAxisPaletteKey(ViewAxis::NegX)) ==
               engine::editor::axisColorSrgbBytes(viewAxisPaletteKey(ViewAxis::PosX))));
        CHECK((engine::editor::axisColorSrgbBytes(viewAxisPaletteKey(ViewAxis::PosY)) !=
               engine::editor::axisColorSrgbBytes(viewAxisPaletteKey(ViewAxis::PosX))));
    }
    SUBCASE("the sign splits the six into three and three") {
        CHECK(viewAxisIsPositive(ViewAxis::PosX));
        CHECK(viewAxisIsPositive(ViewAxis::PosY));
        CHECK(viewAxisIsPositive(ViewAxis::PosZ));
        CHECK_FALSE(viewAxisIsPositive(ViewAxis::NegX));
        CHECK_FALSE(viewAxisIsPositive(ViewAxis::NegY));
        CHECK_FALSE(viewAxisIsPositive(ViewAxis::NegZ));
    }
    SUBCASE("an out-of-range cast is DEFINED for all three") {
        // The trailing return after each total switch. Not UB, not a read past an array.
        const auto outOfRange = static_cast<ViewAxis>(9);
        CHECK(viewAxisLabel(outOfRange) == 'X');
        CHECK((viewAxisPaletteKey(outOfRange) == engine::editor::Axis::X));
        CHECK(viewAxisIsPositive(outOfRange));
    }
}
