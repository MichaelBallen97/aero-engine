// tests/editor/editor_camera_test.cpp — task 2.3.1: the editor camera's tier-0 battery. Fifth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
//
// Step 2 landed section 6.1's case 8, the gesture matrix (AC-12/AC-13). Step 4 (this addition) lands
// cases 1-7 and 9-12, the EditorCamera model itself. Case 12's log arm follows the LogFixture-first
// idiom (console_model_test.cpp / scene_bounds_test.cpp's case 12b).
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using engine::Entity;
using engine::Mat4;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::editor::Aabb;
using engine::editor::CameraButton;
using engine::editor::CameraGesture;
using engine::editor::CameraGestureInput;
using engine::editor::CameraGestureState;
using engine::editor::CameraInput;
using engine::editor::EditorCamera;
using engine::editor::nextGesture;
using engine::editor::selectionBounds;

namespace {

constexpr float EPS = 1.0e-4F;

struct GestureRow {
    std::string name;
    CameraGestureState current;
    CameraGestureInput input;
    CameraGestureState expected;
};

// INV-5, in one place: finite everywhere, and every clamped field inside its own documented bound.
void checkPoseIsSane(const EditorCamera& camera) {
    using namespace engine::editor;  // the MIN_*/MAX_* bounds; test TU, not a header
    CHECK(std::isfinite(camera.pivot().x));
    CHECK(std::isfinite(camera.pivot().y));
    CHECK(std::isfinite(camera.pivot().z));
    CHECK(camera.distance() >= MIN_DISTANCE);
    CHECK(camera.distance() <= MAX_DISTANCE);
    CHECK(camera.yaw() > -engine::PI);
    CHECK(camera.yaw() <= engine::PI);
    CHECK(std::abs(camera.pitch()) <= MAX_PITCH);
    CHECK(camera.fovYRadians() >= MIN_FOV_Y);
    CHECK(camera.fovYRadians() <= MAX_FOV_Y);
    CHECK(camera.nearPlane() >= MIN_NEAR_PLANE);
    CHECK(camera.nearPlane() < camera.farPlane());
    CHECK(std::isfinite(camera.farPlane()));
    CHECK(camera.flySpeed() >= MIN_FLY_SPEED);
    CHECK(camera.flySpeed() <= MAX_FLY_SPEED);
}

[[nodiscard]] bool allFinite(const Mat4& matrix) {
    return std::all_of(matrix.columns.begin(), matrix.columns.end(), [](const Vec4& column) {
        return std::isfinite(column.x) && std::isfinite(column.y) && std::isfinite(column.z) && std::isfinite(column.w);
    });
}

}  // namespace

TEST_CASE("editor camera: enum layout (performance-enum-size pins)") {
    static_assert(sizeof(CameraGesture) == 1);
    static_assert(sizeof(CameraButton) == 1);
    static_assert(std::is_same_v<std::underlying_type_t<CameraGesture>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<CameraButton>, std::uint8_t>);
}

TEST_CASE("editor camera: nextGesture matches D5's three rules exactly (AC-12, AC-13)") {
    const CameraGestureState none{};
    const CameraGestureState orbitLeft{.gesture = CameraGesture::Orbit, .button = CameraButton::Left};

    const std::vector<GestureRow> rows = {
        // --- START, hovered + fresh press: one row per rule-3 clause + its near-misses ---
        // `...Down` is set alongside `...Pressed` in every row below, matching real ImGui semantics
        // (a `IsMouseClicked` edge this frame always coincides with `IsMouseDown` being true THIS
        // frame too) -- this is what lets S8 (dropping the fresh-press requirement, `...Down` instead
        // of `...Pressed`) actually discriminate every START row, not just the two-button E5 row.
        {"Alt+LMB -> Orbit/Left",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .alt = true},
         {.gesture = CameraGesture::Orbit, .button = CameraButton::Left}},
        {"Alt+Cmd+LMB -> Pan/Left (beats Orbit)",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .alt = true, .ctrlOrCmd = true},
         {.gesture = CameraGesture::Pan, .button = CameraButton::Left}},
        {"MMB -> Pan/Middle",
         none,
         {.hovered = true, .middleDown = true, .middlePressed = true},
         {.gesture = CameraGesture::Pan, .button = CameraButton::Middle}},
        {"RMB -> Fly/Right",
         none,
         {.hovered = true, .rightDown = true, .rightPressed = true},
         {.gesture = CameraGesture::Fly, .button = CameraButton::Right}},
        {"Alt+RMB -> Dolly/Right (beats Fly)",
         none,
         {.hovered = true, .rightDown = true, .rightPressed = true, .alt = true},
         {.gesture = CameraGesture::Dolly, .button = CameraButton::Right}},

        // --- AC-13: plain LMB starts NOTHING, at any hover state ---
        {"plain LMB, hovered, no Alt -> None", none, {.hovered = true, .leftDown = true, .leftPressed = true}, none},
        {"plain LMB + ctrlOrCmd alone, hovered -> None",
         none,
         {.hovered = true, .leftDown = true, .leftPressed = true, .ctrlOrCmd = true},
         none},

        // --- not hovered + fresh press -> None, for every button/modifier combination ---
        {"not hovered, Alt+LMB -> None",
         none,
         {.hovered = false, .leftDown = true, .leftPressed = true, .alt = true},
         none},
        {"not hovered, RMB -> None", none, {.hovered = false, .rightDown = true, .rightPressed = true}, none},
        {"not hovered, MMB -> None", none, {.hovered = false, .middleDown = true, .middlePressed = true}, none},

        // --- CONTINUE ---
        {"continuing orbit, not hovered -> unchanged (E3)",
         orbitLeft,
         {.hovered = false, .leftDown = true, .alt = true},
         orbitLeft},
        {"continuing orbit, Alt released -> unchanged (E6, D5 rule 1)",
         orbitLeft,
         {.hovered = true, .leftDown = true, .alt = false},
         orbitLeft},

        // --- END ---
        {"orbit ends: button released, hovered -> None", orbitLeft, {.hovered = true, .leftDown = false}, none},
        {"orbit ends: button released, not hovered -> None", orbitLeft, {.hovered = false, .leftDown = false}, none},

        // --- E5: two buttons at once ---
        {"orbiting + RMB fresh press -> still Orbit/Left (rule 1 wins)",
         orbitLeft,
         {.hovered = true, .leftDown = true, .rightPressed = true, .alt = true},
         orbitLeft},
        {"LMB released, RMB held (no fresh press) -> None",
         orbitLeft,
         {.hovered = true, .leftDown = false, .rightDown = true},
         none},
    };

    for (const GestureRow& row : rows) {
        CAPTURE(row.name);
        const CameraGestureState actual = nextGesture(row.current, row.input);
        CHECK(actual.gesture == row.expected.gesture);
        CHECK(actual.button == row.expected.button);
    }
}

TEST_CASE("editor camera: nextGesture's Alt-after-press row (S8's discriminator)") {
    // AC-13, explicit: a plain LMB press must NEVER retroactively become an orbit, even if Alt is
    // pressed on a LATER frame while the button is still held -- rule 3 needs a FRESH press.
    const CameraGestureInput pressFrame{.hovered = true, .leftDown = true, .leftPressed = true};
    const CameraGestureState afterPress = nextGesture(CameraGestureState{}, pressFrame);
    CHECK(afterPress.gesture == CameraGesture::None);

    const CameraGestureInput heldWithAltNow{.hovered = true, .leftDown = true, .leftPressed = false, .alt = true};
    const CameraGestureState afterAltAppears = nextGesture(afterPress, heldWithAltNow);
    CHECK(afterAltAppears.gesture == CameraGesture::None);
    CHECK(afterAltAppears.button == CameraButton::None);
}

TEST_CASE("editor camera: nextGesture is idempotent for a continuing gesture") {
    const CameraGestureState orbitLeft{.gesture = CameraGesture::Orbit, .button = CameraButton::Left};
    const CameraGestureInput held{.hovered = true, .leftDown = true, .alt = true};
    const CameraGestureState once = nextGesture(orbitLeft, held);
    const CameraGestureState twice = nextGesture(once, held);
    CHECK(once.gesture == twice.gesture);
    CHECK(once.button == twice.button);
}

// ==================================================================================================
// §6.1 cases 1-7, 9-12: the EditorCamera model itself (task 2.3.1 step 4).
// ==================================================================================================

TEST_CASE("editor camera: case 1 -- defaults (D8)") {
    using namespace engine::editor;  // DEFAULT_* / MIN_*/MAX_* constants; test TU, not a header (D4)
    const EditorCamera camera;

    CHECK(camera.pivot() == DEFAULT_PIVOT);
    CHECK(camera.distance() == DEFAULT_DISTANCE);
    CHECK(camera.yaw() == DEFAULT_YAW);
    CHECK(camera.pitch() == DEFAULT_PITCH);
    CHECK(camera.fovYRadians() == DEFAULT_FOV_Y);
    CHECK(camera.nearPlane() == DEFAULT_NEAR);
    CHECK(camera.farPlane() == DEFAULT_FAR);
    CHECK(camera.flySpeed() == DEFAULT_FLY_SPEED);

    // position() independently derived from the D16/D17 formula the header documents -- NOT by
    // calling camera.rotation()/forward() (which would be tautological), but by composing the same
    // documented quaternion product directly here.
    const engine::Quat expectedRotation =
        engine::fromAxisAngle(Vec3::unitY(), DEFAULT_YAW) * engine::fromAxisAngle(Vec3::unitX(), DEFAULT_PITCH);
    const Vec3 expectedForward = expectedRotation * Vec3{0.0F, 0.0F, -1.0F};
    const Vec3 expectedPosition = DEFAULT_PIVOT - expectedForward * DEFAULT_DISTANCE;
    CHECK(engine::approxEquals(camera.position(), expectedPosition));

    // INV-1: viewMatrix() maps position() to the origin.
    CHECK(engine::approxEquals(engine::transformPoint(camera.viewMatrix(), camera.position()), Vec3::zero()));
    // INV-2: right() is always horizontal.
    CHECK(std::abs(camera.right().y) < EPS);
    // INV-5: the pose starts inside its own bounds.
    CHECK(camera.distance() >= MIN_DISTANCE);
    CHECK(camera.distance() <= MAX_DISTANCE);
    CHECK(camera.nearPlane() < camera.farPlane());
}

TEST_CASE("editor camera: case 2 -- orbit changes yaw/pitch only (AC-5)") {
    EditorCamera camera;
    const Vec3 pivotBefore = camera.pivot();
    const float distanceBefore = camera.distance();

    camera.update(CameraInput{.dragDelta = Vec2{40.0F, 25.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);

    CHECK(camera.pivot() == pivotBefore);
    CHECK(camera.distance() == distanceBefore);
}

TEST_CASE("editor camera: case 3 -- orbit signs (AC-9) [S1]") {
    EditorCamera camera;
    const float forwardXBefore = camera.forward().x;
    const float forwardYBefore = camera.forward().y;
    const float positionYBefore = camera.position().y;

    SUBCASE("a +x drag turns the view right (forward().x increases)") {
        camera.update(CameraInput{.dragDelta = Vec2{50.0F, 0.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
        CHECK(camera.forward().x > forwardXBefore);
    }
    SUBCASE("a +y drag turns it down (forward().y decreases) and raises the eye (position().y increases)") {
        camera.update(CameraInput{.dragDelta = Vec2{0.0F, 50.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
        CHECK(camera.forward().y < forwardYBefore);
        CHECK(camera.position().y > positionYBefore);
    }
}

TEST_CASE("editor camera: case 4 -- pitch clamp / yaw wrap (AC-10) [S2]") {
    using namespace engine::editor;
    EditorCamera camera;

    SUBCASE("10000 points of +y drag pins the pitch clamp exactly") {
        for (int i = 0; i < 10000; ++i) {
            camera.update(CameraInput{.dragDelta = Vec2{0.0F, 1.0F}, .gesture = CameraGesture::Orbit}, 0.0F);
        }
        CHECK(camera.pitch() == -MAX_PITCH);
    }

    SUBCASE("10000 points of +x drag keeps yaw finite and in (-pi, pi]; right().y stays ~0") {
        for (int i = 0; i < 10000; ++i) {
            camera.update(CameraInput{.dragDelta = Vec2{1.0F, 0.0F}, .gesture = CameraGesture::Orbit}, 0.0F);
            if (i % 1000 == 0) {
                CHECK(std::abs(camera.right().y) < EPS);
            }
        }
        CHECK(std::isfinite(camera.yaw()));
        CHECK(camera.yaw() > -engine::PI);
        CHECK(camera.yaw() <= engine::PI);
    }
}

TEST_CASE("editor camera: case 5 -- pan scaling (AC-6) [S3]") {
    EditorCamera camera;
    const float yawBefore = camera.yaw();
    const float pitchBefore = camera.pitch();

    SUBCASE("doubling distance doubles the pivot translation") {
        // NEVER `near`/`far` as identifiers -- <windows.h> #defines both as empty macros (camera.hpp's
        // own D8 rationale applies here too).
        camera.setDistance(5.0F);
        const Vec3 pivotBeforeClose = camera.pivot();
        camera.update(
            CameraInput{.dragDelta = Vec2{10.0F, 0.0F}, .viewportHeightPoints = 500.0F, .gesture = CameraGesture::Pan},
            1.0F / 60.0F);
        const float closeTranslation = engine::length(camera.pivot() - pivotBeforeClose);

        EditorCamera distantCamera;
        distantCamera.setDistance(10.0F);
        const Vec3 pivotBeforeDistant = distantCamera.pivot();
        distantCamera.update(
            CameraInput{.dragDelta = Vec2{10.0F, 0.0F}, .viewportHeightPoints = 500.0F, .gesture = CameraGesture::Pan},
            1.0F / 60.0F);
        const float distantTranslation = engine::length(distantCamera.pivot() - pivotBeforeDistant);

        CHECK(std::abs(distantTranslation - 2.0F * closeTranslation) < EPS);
    }

    SUBCASE("halving viewportHeightPoints doubles the translation") {
        EditorCamera tall;
        const Vec3 pivotBeforeTall = tall.pivot();
        tall.update(
            CameraInput{.dragDelta = Vec2{10.0F, 0.0F}, .viewportHeightPoints = 500.0F, .gesture = CameraGesture::Pan},
            1.0F / 60.0F);
        const float tallTranslation = engine::length(tall.pivot() - pivotBeforeTall);

        EditorCamera narrow;
        const Vec3 pivotBeforeNarrow = narrow.pivot();
        narrow.update(
            CameraInput{.dragDelta = Vec2{10.0F, 0.0F}, .viewportHeightPoints = 250.0F, .gesture = CameraGesture::Pan},
            1.0F / 60.0F);
        const float narrowTranslation = engine::length(narrow.pivot() - pivotBeforeNarrow);

        CHECK(std::abs(narrowTranslation - 2.0F * tallTranslation) < EPS);
    }

    SUBCASE("direction: -right for +x, +up for +y; distance/yaw/pitch exactly unchanged") {
        const Vec3 rightDir = camera.right();
        const Vec3 upDir = camera.up();
        const float distanceBefore = camera.distance();
        camera.update(
            CameraInput{.dragDelta = Vec2{10.0F, 10.0F}, .viewportHeightPoints = 500.0F, .gesture = CameraGesture::Pan},
            1.0F / 60.0F);
        // pivot moved opposite `right` and along `up` -- project onto both axes and check signs.
        const Vec3 delta = camera.pivot();  // started at DEFAULT_PIVOT == zero()
        CHECK(engine::dot(delta, rightDir) < 0.0F);
        CHECK(engine::dot(delta, upDir) > 0.0F);
        CHECK(camera.distance() == distanceBefore);
        CHECK(camera.yaw() == yawBefore);
        CHECK(camera.pitch() == pitchBefore);
    }
}

TEST_CASE("editor camera: case 6 -- dolly (AC-7)") {
    using namespace engine::editor;
    EditorCamera camera;
    const Vec3 pivotBefore = camera.pivot();
    const float yawBefore = camera.yaw();
    const float pitchBefore = camera.pitch();

    SUBCASE("multiplicative and composable: one big step ~= many small steps") {
        EditorCamera once;
        once.update(CameraInput{.wheelNotches = 5.0F}, 1.0F / 60.0F);

        EditorCamera stepwise;
        for (int i = 0; i < 5; ++i) {
            stepwise.update(CameraInput{.wheelNotches = 1.0F}, 1.0F / 60.0F);
        }
        CHECK(std::abs(once.distance() - stepwise.distance()) < EPS);
    }

    SUBCASE("clamped at both ends") {
        EditorCamera zoomedIn;
        for (int i = 0; i < 500; ++i) {
            zoomedIn.update(CameraInput{.wheelNotches = 1.0F}, 1.0F / 60.0F);
        }
        CHECK(zoomedIn.distance() == MIN_DISTANCE);

        EditorCamera zoomedOut;
        for (int i = 0; i < 500; ++i) {
            zoomedOut.update(CameraInput{.wheelNotches = -1.0F}, 1.0F / 60.0F);
        }
        CHECK(zoomedOut.distance() == MAX_DISTANCE);
    }

    SUBCASE("pivot/yaw/pitch exactly unchanged; scroll-up decreases distance") {
        camera.update(CameraInput{.wheelNotches = 1.0F}, 1.0F / 60.0F);
        CHECK(camera.pivot() == pivotBefore);
        CHECK(camera.yaw() == yawBefore);
        CHECK(camera.pitch() == pitchBefore);
        CHECK(camera.distance() < DEFAULT_DISTANCE);
    }

    SUBCASE("the drag form {+10,-10} applies TWICE the notches of {+10,0} (the (d.x-d.y) form)") {
        // dragNotches(d) = (d.x - d.y) * DOLLY_NOTCHES_PER_POINT is private, so the notch count isn't
        // directly observable -- but applyDolly is MULTIPLICATIVE (distance *= STEP^-n), so doubling n
        // squares the distance RATIO rather than doubling it. {+10,-10} -> 20 "notch units";
        // {+10,0} -> 10 -- exactly double -- so diagonalRatio must equal singleRatio^2.
        EditorCamera diagonal;
        diagonal.update(CameraInput{.dragDelta = Vec2{10.0F, -10.0F}, .gesture = CameraGesture::Dolly}, 1.0F / 60.0F);
        EditorCamera single;
        single.update(CameraInput{.dragDelta = Vec2{10.0F, 0.0F}, .gesture = CameraGesture::Dolly}, 1.0F / 60.0F);
        const float diagonalRatio = DEFAULT_DISTANCE / diagonal.distance();
        const float singleRatio = DEFAULT_DISTANCE / single.distance();
        CHECK(std::abs(diagonalRatio - singleRatio * singleRatio) < 1.0e-3F);
    }

    SUBCASE("the wheel dollies under every gesture but Fly") {
        EditorCamera none;
        none.update(CameraInput{.wheelNotches = 1.0F, .gesture = CameraGesture::None}, 1.0F / 60.0F);
        EditorCamera orbiting;
        orbiting.update(CameraInput{.wheelNotches = 1.0F, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
        EditorCamera panning;
        panning.update(CameraInput{.wheelNotches = 1.0F, .gesture = CameraGesture::Pan}, 1.0F / 60.0F);
        CHECK(none.distance() == orbiting.distance());
        CHECK(none.distance() == panning.distance());

        EditorCamera flying;
        const float flySpeedBefore = flying.flySpeed();
        flying.update(CameraInput{.wheelNotches = 1.0F, .gesture = CameraGesture::Fly}, 1.0F / 60.0F);
        CHECK(flying.distance() == DEFAULT_DISTANCE);  // bit-identical: the wheel never reached applyDolly
        CHECK(flying.flySpeed() != flySpeedBefore);
    }
}

TEST_CASE("editor camera: case 7 -- fly (AC-8, AC-11) [S4, S10]") {
    using namespace engine::editor;

    SUBCASE("a fly-look drag leaves position() invariant while forward() changes [S4]") {
        EditorCamera camera;
        const Vec3 positionBefore = camera.position();
        const Vec3 forwardBefore = camera.forward();
        camera.update(CameraInput{.dragDelta = Vec2{30.0F, 15.0F}, .gesture = CameraGesture::Fly}, 1.0F / 60.0F);
        CHECK(engine::approxEquals(camera.position(), positionBefore, 1.0e-3F));
        CHECK_FALSE(engine::approxEquals(camera.forward(), forwardBefore, 1.0e-3F));
    }

    SUBCASE("W for 1s at the default speed moves exactly flySpeed units along pre-call forward()") {
        EditorCamera camera;
        const Vec3 positionBefore = camera.position();
        const Vec3 forwardBefore = camera.forward();
        constexpr int STEPS = 60;
        for (int i = 0; i < STEPS; ++i) {
            camera.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true}, 1.0F / 60.0F);
        }
        const Vec3 expected = positionBefore + forwardBefore * DEFAULT_FLY_SPEED;
        CHECK(engine::approxEquals(camera.position(), expected, 1.0e-2F));
    }

    SUBCASE("Shift multiplies speed by FLY_FAST_MULTIPLIER") {
        EditorCamera normal;
        const Vec3 normalStart = normal.position();
        const Vec3 normalForward = normal.forward();
        normal.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true}, 1.0F);

        EditorCamera fast;
        const Vec3 fastStart = fast.position();
        fast.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true, .fast = true}, 1.0F);

        const float normalDist = engine::length(normal.position() - normalStart);
        const float fastDist = engine::length(fast.position() - fastStart);
        CHECK(std::abs(fastDist - FLY_FAST_MULTIPLIER * normalDist) < 1.0e-2F);
        (void)normalForward;
        (void)fastStart;
    }

    SUBCASE("Q/E move along WORLD +-Y regardless of pitch; X/Z unchanged") {
        EditorCamera steep;
        // Pitch the camera steeply via orbit first.
        steep.update(CameraInput{.dragDelta = Vec2{0.0F, 400.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
        const Vec3 before = steep.position();
        steep.update(CameraInput{.gesture = CameraGesture::Fly, .moveUp = true}, 1.0F);
        CHECK(std::abs(steep.position().x - before.x) < EPS);
        CHECK(std::abs(steep.position().z - before.z) < EPS);
        CHECK(steep.position().y > before.y);
    }

    SUBCASE("W+D together move exactly speed*dt, not sqrt(2)*speed*dt") {
        EditorCamera camera;
        const Vec3 before = camera.position();
        camera.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true, .moveRight = true}, 1.0F);
        const float travelled = engine::length(camera.position() - before);
        CHECK(std::abs(travelled - DEFAULT_FLY_SPEED) < 1.0e-2F);
    }

    SUBCASE("the wheel changes flySpeed and not distance while flying") {
        EditorCamera camera;
        camera.update(CameraInput{.wheelNotches = 3.0F, .gesture = CameraGesture::Fly}, 1.0F / 60.0F);
        CHECK(camera.distance() == DEFAULT_DISTANCE);
        CHECK(camera.flySpeed() != DEFAULT_FLY_SPEED);
    }

    SUBCASE("frame-rate independence: 20 steps at dt=1/20 ~= 120 steps at dt=1/120 [S10]") {
        EditorCamera coarse;
        const Vec3 coarseStart = coarse.position();
        for (int i = 0; i < 20; ++i) {
            coarse.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true}, 1.0F / 20.0F);
        }
        EditorCamera fine;
        const Vec3 fineStart = fine.position();
        for (int i = 0; i < 120; ++i) {
            fine.update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true}, 1.0F / 120.0F);
        }
        const float coarseTravel = engine::length(coarse.position() - coarseStart);
        const float fineTravel = engine::length(fine.position() - fineStart);
        CHECK(std::abs(coarseTravel - fineTravel) < 1.0e-2F);
    }

    SUBCASE("extreme pitch during a fly-look: position() stays invariant (clampState-between-halves)") {
        EditorCamera camera;
        const Vec3 positionBefore = camera.position();
        // A huge drag.y drives the pitch straight into its clamp within applyFly.
        camera.update(CameraInput{.dragDelta = Vec2{0.0F, 100000.0F}, .gesture = CameraGesture::Fly}, 1.0F / 60.0F);
        CHECK(engine::approxEquals(camera.position(), positionBefore, 1.0e-2F));
    }
}

TEST_CASE("editor camera: case 9 -- focus (AC-16) [S11]") {
    using namespace engine::editor;
    const Aabb unitBox{Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};

    SUBCASE("focusOn a unit box preserves yaw/pitch and sets pivot/distance") {
        EditorCamera camera;
        camera.update(CameraInput{.dragDelta = Vec2{20.0F, 10.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
        const float yawBefore = camera.yaw();
        const float pitchBefore = camera.pitch();

        camera.focusOn(unitBox, 1.0F);
        CHECK(camera.yaw() == yawBefore);
        CHECK(camera.pitch() == pitchBefore);
        CHECK(engine::approxEquals(camera.pivot(), unitBox.center()));

        const float radius = std::max(unitBox.radius() * FOCUS_MARGIN, FOCUS_MIN_RADIUS);
        const float halfFov = 0.5F * camera.fovYRadians();  // aspect == 1 -> horizontal == vertical
        const float expectedDistance = radius / std::sin(halfFov);
        CHECK(std::abs(camera.distance() - expectedDistance) < 1.0e-3F);
    }

    SUBCASE("a tall/narrow aspect (0.5) fits the HORIZONTAL axis and pulls further back [S11]") {
        EditorCamera square;
        square.focusOn(unitBox, 1.0F);
        EditorCamera tall;
        tall.focusOn(unitBox, 0.5F);
        CHECK(tall.distance() > square.distance());
    }

    SUBCASE("an invalid Aabb leaves the camera bit-identical") {
        EditorCamera camera;
        const EditorCamera before = camera;
        camera.focusOn(Aabb::empty(), 1.0F);
        CHECK(camera.pivot() == before.pivot());
        CHECK(camera.distance() == before.distance());
        CHECK(camera.yaw() == before.yaw());
        CHECK(camera.pitch() == before.pitch());
    }

    SUBCASE("a point box floors at FOCUS_MIN_RADIUS (E15)") {
        EditorCamera camera;
        const Aabb point{Vec3::zero(), Vec3::zero()};
        camera.focusOn(point, 1.0F);
        const float expectedDistance = FOCUS_MIN_RADIUS / std::sin(0.5F * camera.fovYRadians());
        CHECK(std::abs(camera.distance() - expectedDistance) < 1.0e-3F);
    }

    SUBCASE("a huge box clamps at MAX_DISTANCE (E16)") {
        EditorCamera camera;
        const Aabb huge{Vec3{-1.0e8F, -1.0e8F, -1.0e8F}, Vec3{1.0e8F, 1.0e8F, 1.0e8F}};
        camera.focusOn(huge, 1.0F);
        CHECK(camera.distance() == MAX_DISTANCE);
    }
}

TEST_CASE("editor camera: case 10 -- totality (AC-18, E11, E12, E22) [C4]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float posInf = std::numeric_limits<float>::infinity();
    const float negInf = -posInf;

    const std::vector<float> poisons = {nan, posInf, negInf};
    const std::vector<CameraGesture> gestures = {CameraGesture::None, CameraGesture::Orbit, CameraGesture::Pan,
                                                 CameraGesture::Dolly, CameraGesture::Fly};

    for (const CameraGesture gesture : gestures) {
        CAPTURE(static_cast<int>(gesture));
        for (const float poison : poisons) {
            {
                EditorCamera camera;
                camera.update(CameraInput{.dragDelta = Vec2{poison, 1.0F}, .gesture = gesture}, 1.0F / 60.0F);
                CHECK(std::isfinite(camera.distance()));
                CHECK(camera.distance() > 0.0F);
                CHECK(camera.nearPlane() < camera.farPlane());
            }
            {
                EditorCamera camera;
                camera.update(CameraInput{.dragDelta = Vec2{1.0F, poison}, .gesture = gesture}, 1.0F / 60.0F);
                CHECK(std::isfinite(camera.distance()));
            }
            {
                EditorCamera camera;
                camera.update(CameraInput{.wheelNotches = poison, .gesture = gesture}, 1.0F / 60.0F);
                CHECK(std::isfinite(camera.distance()));
            }
            {
                EditorCamera camera;
                camera.update(CameraInput{.viewportHeightPoints = poison, .gesture = gesture}, 1.0F / 60.0F);
                CHECK(std::isfinite(camera.distance()));
            }
            {
                EditorCamera camera;
                camera.update(CameraInput{.gesture = gesture}, poison);
                CHECK(std::isfinite(camera.distance()));
            }
            {
                // All at once.
                EditorCamera camera;
                camera.update(CameraInput{.dragDelta = Vec2{poison, poison},
                                          .wheelNotches = poison,
                                          .viewportHeightPoints = poison,
                                          .gesture = gesture},
                              poison);
                CHECK(std::isfinite(camera.distance()));
                CHECK(camera.distance() > 0.0F);
                CHECK(camera.nearPlane() < camera.farPlane());
            }
        }
    }

    SUBCASE("a directly-poisoned state resets on the next update") {
        EditorCamera camera;
        camera.setDistance(std::numeric_limits<float>::quiet_NaN());
        camera.update(CameraInput{}, 1.0F / 60.0F);
        CHECK(camera.distance() == engine::editor::DEFAULT_DISTANCE);

        camera.setPivot(Vec3{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
        camera.update(CameraInput{}, 1.0F / 60.0F);
        CHECK(camera.pivot() == engine::editor::DEFAULT_PIVOT);
    }

    SUBCASE("deltaSeconds == 0 moves nothing but still orbits (E12)") {
        EditorCamera camera;
        const Vec3 pivotBefore = camera.pivot();
        camera.update(CameraInput{.dragDelta = Vec2{20.0F, 0.0F}, .gesture = CameraGesture::Orbit}, 0.0F);
        CHECK(camera.pivot() == pivotBefore);
        CHECK(camera.yaw() != engine::editor::DEFAULT_YAW);
    }

    SUBCASE("a non-finite or <=0 aspect falls back to 1.0 in projectionMatrix") {
        const EditorCamera camera;
        const Mat4 withNan = camera.projectionMatrix(nan);
        const Mat4 withZero = camera.projectionMatrix(0.0F);
        const Mat4 withOne = camera.projectionMatrix(1.0F);
        CHECK(engine::approxEquals(withNan, withOne));
        CHECK(engine::approxEquals(withZero, withOne));
    }
}

TEST_CASE("editor camera: near < far survives every magnitude (INV-5, C7's MIN_DEPTH_RANGE floor)") {
    using namespace engine::editor;
    // MIN_DEPTH_RANGE (1.0e-3F) is below half an ULP of a float at magnitudes >= 32768, so
    // `near + MIN_DEPTH_RANGE == near` EXACTLY there: with that floor alone, setNearPlane(40000.0F)
    // leaves near == far. Nothing downstream survives it -- perspective() asserts zFar > zNear in
    // Debug and divides by zFar - zNear == 0 in Release, returning a matrix carrying +-inf -- and
    // stateIsFinite() cannot see it, because BOTH members are perfectly finite. Only this sweep can.
    const std::vector<float> nearValues = {MIN_NEAR_PLANE, 0.1F,     1.0F,   100.0F,  20000.0F, 32767.0F,
                                           32768.0F,       40000.0F, 1.0e6F, 1.0e20F, 1.0e30F};

    for (const float value : nearValues) {
        CAPTURE(value);
        EditorCamera camera;
        camera.setNearPlane(value);
        CHECK(camera.nearPlane() >= MIN_NEAR_PLANE);
        CHECK(std::isfinite(camera.farPlane()));
        // REQUIRE, not CHECK: perspective() asserts zFar > zNear, so letting the case run on past a
        // broken floor would abort the whole binary instead of naming the magnitude that failed.
        REQUIRE(camera.nearPlane() < camera.farPlane());
        // perspective()'s own `-(zFar*zNear)/(zFar-zNear)` term overflows a float once zNear passes
        // ~sqrt(FLT_MAX) (~1.8e19), however correct the floor is -- that is a limit of the projection
        // formula, not of this clamp, and a near plane of 1e20 is far past any real use. Ask for a
        // finite projection only where the formula is representable at all.
        if (value < 1.0e19F) {
            CHECK(allFinite(camera.projectionMatrix(1.5F)));
        }
    }

    SUBCASE("the one input with no finite answer: near == FLT_MAX leaves +inf, which update() sweeps") {
        // No float is greater than FLT_MAX, so `near < far` is unreachable with a finite far. far
        // therefore lands on +inf -- which, unlike near == far, IS visible to stateIsFinite(), so the
        // next update() resets the camera instead of leaving a silently degenerate projection.
        EditorCamera camera;
        camera.setNearPlane(std::numeric_limits<float>::max());
        CHECK(camera.nearPlane() < camera.farPlane());
        CHECK_FALSE(std::isfinite(camera.farPlane()));
        camera.update(CameraInput{}, 1.0F / 60.0F);
        CHECK(camera.nearPlane() == DEFAULT_NEAR);
        CHECK(camera.farPlane() == DEFAULT_FAR);
        checkPoseIsSane(camera);
    }
}

TEST_CASE("editor camera: case 11 -- viewMatrix/projectionMatrix") {
    EditorCamera camera;
    camera.update(CameraInput{.dragDelta = Vec2{15.0F, 8.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);

    const Mat4 model = engine::compose({.translation = camera.position(), .rotation = camera.rotation()});
    CHECK(engine::approxEquals(camera.viewMatrix() * model, Mat4::identity()));

    const Mat4 proj = camera.projectionMatrix(1.5F);
    const Mat4 expectedProj = engine::perspective(camera.fovYRadians(), 1.5F, camera.nearPlane(), camera.farPlane());
    CHECK(engine::approxEquals(proj, expectedProj));
}

namespace {
// case 12's LogFixture: declared FIRST in its case so it destructs LAST, after the LogSinkScope
// (console_model_test.cpp / scene_bounds_test.cpp's shared idiom).
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};
}  // namespace

TEST_CASE("editor camera: case 12 -- no log output across a focusSelection-shaped sequence (AC-17, E18) [S9b]") {
    const LogFixture fixture;  // declared FIRST: destructs LAST, after the scope below
    const engine::editor::LogSinkScope scope;

    const World empty;
    (void)engine::editor::sceneBounds(empty);

    World withEntitiesNoMeshes;
    for (int i = 0; i < 3; ++i) {
        const Entity e = withEntitiesNoMeshes.create();
        (void)withEntitiesNoMeshes.add<engine::Transform>(e);
    }
    (void)engine::editor::sceneBounds(withEntitiesNoMeshes);

    // Wrapped in std::optional (the shell_test.cpp/scene_bounds_test.cpp precedent): moving *source
    // rather than a bare local, and reading the moved-from state back through source-> afterward, is
    // what keeps bugprone-use-after-move from flagging a deliberate moved-from-state assertion.
    std::optional<World> movedFromSource;
    movedFromSource.emplace();
    const World movedTo(std::move(*movedFromSource));
    (void)engine::editor::sceneBounds(*movedFromSource);
    (void)movedTo;

    const Entity destroyed = withEntitiesNoMeshes.create();
    (void)withEntitiesNoMeshes.destroy(destroyed);
    const std::vector<Entity> mixed = {Entity{}, destroyed};
    (void)selectionBounds(withEntitiesNoMeshes, mixed);

    EditorCamera camera;
    camera.focusOn(Aabb::empty(), 1.0F);
    camera.setDistance(std::numeric_limits<float>::quiet_NaN());
    camera.update(CameraInput{.dragDelta = Vec2{std::numeric_limits<float>::infinity(), 0.0F}}, 1.0F / 60.0F);

    std::vector<engine::editor::LogEntry> out;
    scope.sink()->take(out);
    CHECK(out.empty());
}
