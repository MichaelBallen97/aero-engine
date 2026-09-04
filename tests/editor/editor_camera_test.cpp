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

// Every field of the camera's public pose in one snapshot. Comparing the WHOLE pose bit-for-bit is
// what makes "the poisoned input was IGNORED" distinguishable from "the state went non-finite and
// reset() restored the default pose" -- from a DEFAULT-constructed camera those two outcomes are
// identical, which is why every totality row below starts from offDefaultCamera().
struct Pose {
    Vec3 pivot;
    float distance = 0.0F;
    float yaw = 0.0F;
    float pitch = 0.0F;
    float fovY = 0.0F;
    float nearPlane = 0.0F;
    float farPlane = 0.0F;
    float flySpeed = 0.0F;
};

[[nodiscard]] Pose poseOf(const EditorCamera& camera) {
    return Pose{camera.pivot(),       camera.distance(),  camera.yaw(),      camera.pitch(),
                camera.fovYRadians(), camera.nearPlane(), camera.farPlane(), camera.flySpeed()};
}

// A camera deliberately OFF the D8 default pose -- orbited (yaw + pitch) and dollied (distance) --
// while leaving the pivot exactly at the origin, so a fly's `pivot = eye + forward*distance` round
// trip stays bit-exact. reset() lands on the default pose, so yaw/pitch/distance differing from
// DEFAULT_* is what makes an unwanted reset visible at all.
[[nodiscard]] EditorCamera offDefaultCamera() {
    EditorCamera camera;
    camera.update(CameraInput{.dragDelta = Vec2{37.0F, -21.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
    camera.update(CameraInput{.wheelNotches = 2.0F}, 1.0F / 60.0F);
    return camera;
}

// AC-18's "rejected WITHOUT corrupting state", asserted field by field and EXACTLY: an ignored input
// must leave the pose untouched, not merely finite.
void checkPoseUnchanged(const EditorCamera& camera, const Pose& before) {
    CHECK(camera.pivot() == before.pivot);
    CHECK(camera.distance() == before.distance);
    CHECK(camera.yaw() == before.yaw);
    CHECK(camera.pitch() == before.pitch);
    CHECK(camera.fovYRadians() == before.fovY);
    CHECK(camera.nearPlane() == before.nearPlane);
    CHECK(camera.farPlane() == before.farPlane);
    CHECK(camera.flySpeed() == before.flySpeed);
}

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
    CHECK(camera.yaw() == DEFAULT_YAW_RADIANS);
    CHECK(camera.pitch() == DEFAULT_PITCH_RADIANS);
    CHECK(camera.fovYRadians() == DEFAULT_FOV_Y);
    CHECK(camera.nearPlane() == DEFAULT_NEAR);
    CHECK(camera.farPlane() == DEFAULT_FAR);
    CHECK(camera.flySpeed() == DEFAULT_FLY_SPEED);

    // position() independently derived from the D16/D17 formula the header documents -- NOT by
    // calling camera.rotation()/forward() (which would be tautological), but by composing the same
    // documented quaternion product directly here.
    const engine::Quat expectedRotation = engine::fromAxisAngle(Vec3::unitY(), DEFAULT_YAW_RADIANS) *
                                          engine::fromAxisAngle(Vec3::unitX(), DEFAULT_PITCH_RADIANS);
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

TEST_CASE("editor camera: case 9's independent framing check -- the box lands inside the frustum (AC-16)") {
    using namespace engine::editor;
    // Case 9 re-derives radius/sin(halfFov) from the same inputs the implementation uses. This is the
    // INDEPENDENT half section 6.1 case 9 asks for, and it goes through a different code path
    // entirely: push the eight corners of the framed box through projectionMatrix(aspect) *
    // viewMatrix() and require every one to land inside the clip volume -- x,y in [-1,1] and
    // z in [0,1], because ADR-005 pins clip Z to [0,1], right-handed, Y-up, -Z forward, with NO Y
    // flip for Vulkan (SDL converts behind the scenes; flipping here would double-flip).
    const Aabb box{Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};
    const std::vector<float> aspects = {0.5F, 1.0F, 2.0F};  // 0.5 is narrow -> the fovX-tighter branch

    for (const float aspect : aspects) {
        CAPTURE(aspect);
        EditorCamera camera;
        camera.focusOn(box, aspect);
        const Mat4 viewProj = camera.projectionMatrix(aspect) * camera.viewMatrix();

        float widest = 0.0F;
        for (int corner = 0; corner < 8; ++corner) {
            CAPTURE(corner);
            const Vec3 point{(corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
                             (corner & 4) != 0 ? box.max.z : box.min.z};
            const Vec4 clip = viewProj * Vec4{point.x, point.y, point.z, 1.0F};
            REQUIRE(clip.w > 0.0F);  // in front of the eye at all -- and the divide below is safe
            const Vec3 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
            CHECK(std::abs(ndc.x) <= 1.0F);
            CHECK(std::abs(ndc.y) <= 1.0F);
            CHECK(ndc.z >= 0.0F);
            CHECK(ndc.z <= 1.0F);
            widest = std::max({widest, std::abs(ndc.x), std::abs(ndc.y)});
        }
        // ... and the box subtends EXACTLY the angle FOCUS_MARGIN asks for. The framing puts a sphere
        // of radius r*FOCUS_MARGIN tangent to the frustum, so the box's OWN bounding sphere (radius
        // r) subtends asin(sin(halfFov)/FOCUS_MARGIN), and no corner can project past
        // tan(that)/tan(halfFov) on the tighter screen axis. Derived from FOCUS_MARGIN, the fov and
        // the aspect -- never from camera.distance() -- so retuning a constant moves the bound WITH
        // the implementation (R-4) while a wrong distance formula still reddens it.
        const float halfY = 0.5F * camera.fovYRadians();
        const float halfX = std::atan(std::tan(halfY) * aspect);
        const float halfFov = std::min(halfY, halfX);
        const float expectedWidest = std::tan(std::asin(std::sin(halfFov) / FOCUS_MARGIN)) / std::tan(halfFov);
        CAPTURE(widest);
        CAPTURE(expectedWidest);
        CHECK(widest <= expectedWidest);
        // The cube's corners sit ON its bounding sphere, so they must come CLOSE to that bound rather
        // than shrink to a dot: without this, every assertion above would pass just as happily for a
        // camera parked at MAX_DISTANCE.
        CHECK(widest > 0.5F * expectedWidest);
    }
}

TEST_CASE("editor camera: case 10 -- totality (AC-18, E11, E12, E22) [C4]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float posInf = std::numeric_limits<float>::infinity();
    const float negInf = -posInf;

    const std::vector<float> poisons = {nan, posInf, negInf};
    const std::vector<CameraGesture> gestures = {CameraGesture::None, CameraGesture::Orbit, CameraGesture::Pan,
                                                 CameraGesture::Dolly, CameraGesture::Fly};

    // AC-18 says a poisoned input is rejected WITHOUT CORRUPTING STATE, which is strictly stronger
    // than "the camera ends up finite": from a DEFAULT-constructed camera, "the input was ignored"
    // and "the state went non-finite and reset() restored the default pose" are INDISTINGUISHABLE --
    // and the reset path is exactly the one an unguarded +inf takes. Every camera below therefore
    // starts OFF the default pose, and the whole pose is compared field by field.
    for (const CameraGesture gesture : gestures) {
        CAPTURE(static_cast<int>(gesture));
        for (const float poison : poisons) {
            CAPTURE(poison);
            {
                INFO("dragDelta.x");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.dragDelta = Vec2{poison, 1.0F}, .gesture = gesture}, 1.0F / 60.0F);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
            }
            {
                INFO("dragDelta.y");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.dragDelta = Vec2{1.0F, poison}, .gesture = gesture}, 1.0F / 60.0F);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
            }
            {
                INFO("wheelNotches");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.wheelNotches = poison, .gesture = gesture}, 1.0F / 60.0F);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
            }
            {
                INFO("viewportHeightPoints");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.viewportHeightPoints = poison, .gesture = gesture}, 1.0F / 60.0F);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
            }
            {
                INFO("deltaSeconds");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.gesture = gesture}, poison);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
            }
            {
                INFO("all at once");
                EditorCamera camera = offDefaultCamera();
                const Pose before = poseOf(camera);
                camera.update(CameraInput{.dragDelta = Vec2{poison, poison},
                                          .wheelNotches = poison,
                                          .viewportHeightPoints = poison,
                                          .gesture = gesture},
                              poison);
                checkPoseUnchanged(camera, before);
                checkPoseIsSane(camera);
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
        CHECK(camera.yaw() != engine::editor::DEFAULT_YAW_RADIANS);
    }

    SUBCASE("a non-finite or <=0 aspect falls back to 1.0 in projectionMatrix") {
        const EditorCamera camera;
        const Mat4 withOne = camera.projectionMatrix(1.0F);
        // NaN and 0.0F discriminate NOTHING on their own: the spec's negated-`>` spelling rejects
        // both (`NaN > 0` is false, `0 > 0` is false). They are kept as coverage, not as proof.
        CHECK(engine::approxEquals(camera.projectionMatrix(nan), withOne));
        CHECK(engine::approxEquals(camera.projectionMatrix(0.0F), withOne));
        CHECK(engine::approxEquals(camera.projectionMatrix(-2.0F), withOne));
        // +inf IS the row C4 exists for: `+inf > 0.0F` is TRUE, so the negated-`>` spelling passes it
        // straight through and proj[0][0] becomes 1/(tan(halfY)*inf) == 0 instead of 1/tan(halfY).
        CHECK(engine::approxEquals(camera.projectionMatrix(posInf), withOne));
        CHECK(engine::approxEquals(camera.projectionMatrix(negInf), withOne));
        // ... and the fallback is not vacuous: a genuinely different aspect gives a different matrix.
        CHECK_FALSE(engine::approxEquals(camera.projectionMatrix(2.0F), withOne));
    }

    SUBCASE("+inf deltaSeconds falls back to exactly 0.0F [C4]") {
        // Under the negated-`>` spelling `dt` stays +inf, `pivot += normalizeOrZero(move) * (speed *
        // inf)` poisons the pose, and the finiteness sweep RESETS the camera -- which is not
        // "rejected without corrupting state". Starting off the default pose is what makes that
        // reset visible; from a default camera the reset is invisible and every assertion passes.
        const CameraInput flyForward{.gesture = CameraGesture::Fly, .moveForward = true, .fast = true};
        EditorCamera poisoned = offDefaultCamera();
        const Pose before = poseOf(poisoned);
        poisoned.update(flyForward, posInf);
        checkPoseUnchanged(poisoned, before);

        // ... and it is exactly the pose a dt of 0.0F -- the documented fallback VALUE -- produces.
        EditorCamera zeroDelta = offDefaultCamera();
        zeroDelta.update(flyForward, 0.0F);
        checkPoseUnchanged(zeroDelta, before);

        // Not vacuous: a real dt does move the camera, so the two rows above assert something.
        EditorCamera moving = offDefaultCamera();
        moving.update(flyForward, 1.0F);
        CHECK(moving.pivot() != before.pivot);
    }

    SUBCASE("+inf viewportHeightPoints falls back to exactly 1.0F [C4]") {
        // Under the negated-`>` spelling `height` stays +inf, worldPerPoint becomes
        // 2*distance*tan(fovY/2)/inf == 0, and a pan drag moves NOTHING -- silently finite, silently
        // wrong. The specified fallback is 1.0F, so the poisoned pan must land on the 1.0F pan.
        const CameraInput panDrag{
            .dragDelta = Vec2{12.0F, -7.0F}, .viewportHeightPoints = posInf, .gesture = CameraGesture::Pan};
        EditorCamera poisoned = offDefaultCamera();
        poisoned.update(panDrag, 1.0F / 60.0F);

        CameraInput unitHeight = panDrag;
        unitHeight.viewportHeightPoints = 1.0F;
        EditorCamera reference = offDefaultCamera();
        reference.update(unitHeight, 1.0F / 60.0F);

        CHECK(poisoned.pivot() == reference.pivot());
        // Not vacuous: the fallback pan genuinely moves the pivot off where it started.
        CHECK(poisoned.pivot() != offDefaultCamera().pivot());
        checkPoseIsSane(poisoned);
    }
}

// ==================================================================================================
// clampState()'s setter-reached arms. setYaw/setPitch/setFovYRadians/setNearPlane/setFarPlane/
// setFlySpeed had ZERO call sites anywhere in editor/ or tests/, so four of clampState()'s seven
// statements -- the fov, near, far and flySpeed clamps -- were dead to the entire suite: deleting all
// four left it green. C7, one of the plan's four load-bearing corrections, was therefore unproven.
// ==================================================================================================

TEST_CASE("editor camera: every setter clamps through clampState (INV-5, C7)") {
    using namespace engine::editor;

    SUBCASE("setFovYRadians clamps into [MIN_FOV_Y, MAX_FOV_Y]") {
        EditorCamera camera;
        camera.setFovYRadians(0.0F);
        CHECK(camera.fovYRadians() == MIN_FOV_Y);
        camera.setFovYRadians(10.0F);  // ~573 degrees
        CHECK(camera.fovYRadians() == MAX_FOV_Y);
    }

    SUBCASE("setNearPlane floors at MIN_NEAR_PLANE") {
        EditorCamera camera;
        camera.setNearPlane(0.0F);
        CHECK(camera.nearPlane() == MIN_NEAR_PLANE);
        camera.setNearPlane(-5.0F);
        CHECK(camera.nearPlane() == MIN_NEAR_PLANE);
        CHECK(camera.nearPlane() < camera.farPlane());
    }

    SUBCASE("setFarPlane floors at exactly nearPlane() + MIN_DEPTH_RANGE") {
        EditorCamera camera;
        camera.setFarPlane(0.0F);
        // EXACTLY that sum, not merely "greater than near": at ordinary magnitudes MIN_DEPTH_RANGE is
        // the wider of the two floors, and a hypothetical nextafter-only floor would give 0.10000001
        // where this gives 0.101 -- collapsing the usable depth range.
        CHECK(camera.farPlane() == camera.nearPlane() + MIN_DEPTH_RANGE);
        camera.setFarPlane(-1.0e6F);
        CHECK(camera.farPlane() == camera.nearPlane() + MIN_DEPTH_RANGE);
        CHECK(camera.nearPlane() < camera.farPlane());
    }

    SUBCASE("setFlySpeed clamps into [MIN_FLY_SPEED, MAX_FLY_SPEED]") {
        EditorCamera camera;
        camera.setFlySpeed(0.0F);
        CHECK(camera.flySpeed() == MIN_FLY_SPEED);
        camera.setFlySpeed(1.0e6F);
        CHECK(camera.flySpeed() == MAX_FLY_SPEED);
    }

    SUBCASE("setDistance clamps into [MIN_DISTANCE, MAX_DISTANCE]") {
        EditorCamera camera;
        camera.setDistance(0.0F);
        CHECK(camera.distance() == MIN_DISTANCE);
        camera.setDistance(1.0e9F);
        CHECK(camera.distance() == MAX_DISTANCE);
    }

    SUBCASE("setPitch clamps into +-MAX_PITCH") {
        EditorCamera camera;
        camera.setPitch(10.0F);
        CHECK(camera.pitch() == MAX_PITCH);
        camera.setPitch(-10.0F);
        CHECK(camera.pitch() == -MAX_PITCH);
    }

    SUBCASE("setYaw wraps into the HALF-OPEN interval (-pi, pi]") {
        EditorCamera camera;
        camera.setYaw(100.0F);
        CHECK(camera.yaw() > -engine::PI);
        CHECK(camera.yaw() <= engine::PI);
        CHECK(camera.yaw() != 100.0F);
        // -pi is the open end: it must come back as +pi exactly, never as -pi (AC-10, exactly).
        camera.setYaw(-engine::PI);
        CHECK(camera.yaw() == engine::PI);
    }

    SUBCASE("setPivot is unclamped and round-trips exactly") {
        EditorCamera camera;
        const Vec3 target{12.5F, -3.25F, 0.75F};
        camera.setPivot(target);
        CHECK(camera.pivot() == target);
    }

    SUBCASE("a non-finite value through ANY setter is swept sane by the next update()") {
        // The setters deliberately do NOT reset: std::clamp does not sanitize NaN (`v < lo` and
        // `hi < v` are both false for it), and case 10's direct-poison row depends on the poison
        // SURVIVING until the next update(). So the contract proved here is the pair -- poison a
        // setter, then ONE update() lands a finite pose inside every INV-5 bound.
        struct FloatSetterRow {
            std::string name;
            void (EditorCamera::*apply)(float) noexcept;
        };
        const std::vector<FloatSetterRow> setters = {
            {"setDistance", &EditorCamera::setDistance},   {"setYaw", &EditorCamera::setYaw},
            {"setPitch", &EditorCamera::setPitch},         {"setFovYRadians", &EditorCamera::setFovYRadians},
            {"setNearPlane", &EditorCamera::setNearPlane}, {"setFarPlane", &EditorCamera::setFarPlane},
            {"setFlySpeed", &EditorCamera::setFlySpeed}};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float posInf = std::numeric_limits<float>::infinity();
        const std::vector<float> poisons = {nan, posInf, -posInf};

        for (const FloatSetterRow& row : setters) {
            CAPTURE(row.name);
            for (const float poison : poisons) {
                CAPTURE(poison);
                EditorCamera camera;
                (camera.*row.apply)(poison);
                camera.update(CameraInput{}, 1.0F / 60.0F);
                checkPoseIsSane(camera);
            }
        }

        for (const float poison : poisons) {
            CAPTURE(poison);
            EditorCamera camera;
            camera.setPivot(Vec3{poison, 0.0F, -poison});
            camera.update(CameraInput{}, 1.0F / 60.0F);
            checkPoseIsSane(camera);
        }
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

// ---- task E.1.3: the projection mode ------------------------------------------------------------

TEST_CASE("editor camera: case 13 -- the projection mode round-trips and changes NOTHING else (PM1)") {
    using engine::editor::ProjectionMode;
    static_assert(sizeof(ProjectionMode) == 1);  // performance-enum-size

    EditorCamera camera;
    CHECK((camera.projectionMode() == ProjectionMode::Perspective));  // the default

    SUBCASE("it round-trips both ways") {
        camera.setProjectionMode(ProjectionMode::Orthographic);
        CHECK((camera.projectionMode() == ProjectionMode::Orthographic));
        camera.setProjectionMode(ProjectionMode::Perspective);
        CHECK((camera.projectionMode() == ProjectionMode::Perspective));
    }
    SUBCASE("reset() restores Perspective") {
        // reset() IS the D8 default pose, so a mode surviving it would be a state the defaults do not
        // describe -- and the F-focus fallback path calls reset() for a user who flew into the void.
        camera.setProjectionMode(ProjectionMode::Orthographic);
        camera.reset();
        CHECK((camera.projectionMode() == ProjectionMode::Perspective));
    }
    SUBCASE("setProjectionMode changes NOTHING else -- the whole pose is byte-equal across the call") {
        // It does not call clampState(), because it clamps no quantity; this is what says so. Started
        // from an OFF-DEFAULT pose so "unchanged" is distinguishable from "reset to the defaults".
        camera.setPivot(Vec3{3.0F, -2.0F, 7.0F});
        camera.setDistance(21.5F);
        camera.setYaw(0.75F);
        camera.setPitch(-0.4F);
        camera.setFovYRadians(engine::radians(42.0F));
        camera.setNearPlane(0.25F);
        camera.setFarPlane(555.0F);
        camera.setFlySpeed(3.5F);

        const Vec3 pivot = camera.pivot();
        const float distance = camera.distance();
        const float yaw = camera.yaw();
        const float pitch = camera.pitch();
        const float fov = camera.fovYRadians();
        const float nearPlane = camera.nearPlane();
        const float farPlane = camera.farPlane();
        const float flySpeed = camera.flySpeed();

        camera.setProjectionMode(ProjectionMode::Orthographic);

        CHECK(camera.pivot() == pivot);
        CHECK(camera.distance() == distance);
        CHECK(camera.yaw() == yaw);
        CHECK(camera.pitch() == pitch);
        CHECK(camera.fovYRadians() == fov);
        CHECK(camera.nearPlane() == nearPlane);
        CHECK(camera.farPlane() == farPlane);
        CHECK(camera.flySpeed() == flySpeed);
    }
}

TEST_CASE("editor camera: case 14 -- orthoHalfHeight is distance * tan(fovY/2) (PM2)") {
    EditorCamera camera;

    SUBCASE("computed independently, EXACTLY equal") {
        // Exact ==: the same two reads, the same two operations, in the same order. A tolerance here
        // would let a different-but-close formula through, which is the whole thing being pinned.
        const float expected = camera.distance() * std::tan(0.5F * camera.fovYRadians());
        CHECK(camera.orthoHalfHeight() == expected);
    }
    SUBCASE("it scales LINEARLY with distance") {
        // A relationship rather than a magnitude: this is what makes a dolly change the ortho zoom,
        // and it is why the toggle stays continuous as the camera moves.
        const float atEight = camera.orthoHalfHeight();
        camera.setDistance(camera.distance() * 2.0F);
        CHECK(camera.orthoHalfHeight() == doctest::Approx(atEight * 2.0F).epsilon(1.0e-6));
        camera.setDistance(camera.distance() * 0.25F);
        CHECK(camera.orthoHalfHeight() == doctest::Approx(atEight * 0.5F).epsilon(1.0e-6));
    }
    SUBCASE("it is unaffected by aspect -- aspect widens the WIDTH, never the height") {
        const float half = camera.orthoHalfHeight();
        const Mat4 wide = camera.projectionMatrix(3.0F);
        const Mat4 narrow = camera.projectionMatrix(0.4F);
        (void)wide;
        (void)narrow;
        CHECK(camera.orthoHalfHeight() == half);
    }
}

TEST_CASE("editor camera: case 15 -- the two projections are STRUCTURALLY distinct (PM3)") {
    using engine::editor::ProjectionMode;
    EditorCamera camera;
    constexpr float ASPECT = 16.0F / 9.0F;

    const Mat4 perspectiveProj = camera.projectionMatrix(ASPECT);
    camera.setProjectionMode(ProjectionMode::Orthographic);
    const Mat4 orthoProj = camera.projectionMatrix(ASPECT);

    SUBCASE("the ortho bottom row is EXACTLY (0,0,0,1) and the perspective one is not") {
        // Mat4 is COLUMN-major, so the bottom row is the .w of each of the four columns.
        CHECK(orthoProj.columns[0].w == 0.0F);
        CHECK(orthoProj.columns[1].w == 0.0F);
        CHECK(orthoProj.columns[2].w == 0.0F);
        CHECK(orthoProj.columns[3].w == 1.0F);
        // The perspective one puts -1 in columns[2].w: that is the whole divide.
        CHECK(perspectiveProj.columns[2].w != 0.0F);
    }
    SUBCASE("clip.w is INDEPENDENT OF THE POINT in ortho -- F3, and that is what makes the gate vacuous") {
        // THE REASON FOUR "in front of the eye" GATES GO VACUOUS, stated as the property that is
        // actually exact on this tree. `w == 1` is NOT: EditorCamera::viewMatrix() is
        // inverse(compose(...)), a GENERAL cofactor inverse rather than an affine one, so its
        // bottom-right entry comes back one ulp low -- MEASURED at 0.99999994 for the default pose,
        // constant across every point. What IS exact, and what the vacuity rests on, is that the
        // composed bottom row's x/y/z are exactly zero, so w does not depend on the world point at
        // all and `w > CLIP_W_EPSILON` therefore answers the same thing everywhere.
        const Mat4 orthoViewProj = orthoProj * camera.viewMatrix();
        const Mat4 perspViewProj = perspectiveProj * camera.viewMatrix();
        CHECK(orthoViewProj.columns[0].w == 0.0F);
        CHECK(orthoViewProj.columns[1].w == 0.0F);
        CHECK(orthoViewProj.columns[2].w == 0.0F);

        const std::vector<Vec3> points{
            Vec3::zero(),
            Vec3{1.0F, 2.0F, 3.0F},
            Vec3{-40.0F, 12.0F, 900.0F},
            Vec3{0.0F, 0.0F, -5.0F},
            Vec3{100.0F, -100.0F, -1000.0F},
            Vec3{1.0e6F, 1.0e6F, 1.0e6F},
        };
        const float referenceW = (orthoViewProj * Vec4{0.0F, 0.0F, 0.0F, 1.0F}).w;
        CHECK(std::abs(referenceW - 1.0F) < 1.0e-6F);  // one ulp low, not a different number
        bool anyPerspectiveWDiffers = false;
        for (const Vec3& point : points) {
            CAPTURE(point.x);
            CAPTURE(point.y);
            CAPTURE(point.z);
            const Vec4 orthoClip = orthoViewProj * Vec4{point.x, point.y, point.z, 1.0F};
            CHECK(orthoClip.w == referenceW);  // EXACT, and the point ranges over 1e6 in every axis
            const Vec4 perspClip = perspViewProj * Vec4{point.x, point.y, point.z, 1.0F};
            anyPerspectiveWDiffers = anyPerspectiveWDiffers || (perspClip.w != referenceW);
        }
        CHECK(anyPerspectiveWDiffers);  // anti-vacuity: w really varies with the point in the OTHER mode
    }
}

TEST_CASE("editor camera: case 16 -- the mode toggle is continuous on the PIVOT PLANE (PM4)") {
    using engine::editor::ProjectionMode;
    // D11, in its corrected form. It is NOT the pivot POINT that is invariant -- it is the whole
    // PLANE at view-space depth -distance, off-axis points included, because
    // halfHeight == distance * tan(fovY/2) makes the two mappings algebraically identical there.
    // Only ndc.z differs. Writing the negative arm as "an off-axis point moves" would have compared
    // two values that are equal by algebra and called the equality a bug.
    EditorCamera camera;
    camera.setYaw(engine::radians(37.0F));
    camera.setPitch(engine::radians(-11.0F));
    camera.setPivot(Vec3{2.0F, 1.0F, -3.0F});
    constexpr float ASPECT = 16.0F / 9.0F;

    const Mat4 perspViewProj = camera.projectionMatrix(ASPECT) * camera.viewMatrix();
    camera.setProjectionMode(ProjectionMode::Orthographic);
    const Mat4 orthoViewProj = camera.projectionMatrix(ASPECT) * camera.viewMatrix();

    const Vec3 pivot = camera.pivot();
    const Vec3 right = camera.right();
    const Vec3 up = camera.up();
    const Vec3 forward = camera.forward();

    // Both NDCs come from the MATRICES, never from the extent formula -- so the two sides of the
    // identity have genuinely different sources (E.1.2's GR8 rule).
    const auto ndcOf = [](const Mat4& viewProj, Vec3 point) {
        const Vec4 clip = viewProj * Vec4{point.x, point.y, point.z, 1.0F};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    };

    SUBCASE("five points ON the pivot plane agree in x/y and DISAGREE in z") {
        const std::vector<Vec3> onPlane{
            pivot,
            pivot + (right * 1.5F) + (up * 0.75F),
            pivot - (right * 2.25F) + (up * 1.1F),
            pivot + (right * 0.3F) - (up * 2.0F),
            pivot - (right * 1.0F) - (up * 1.0F),
        };
        for (const Vec3& point : onPlane) {
            CAPTURE(point.x);
            CAPTURE(point.y);
            CAPTURE(point.z);
            const Vec3 persp = ndcOf(perspViewProj, point);
            const Vec3 ortho = ndcOf(orthoViewProj, point);
            CHECK(std::abs(persp.x - ortho.x) < 1.0e-5F);
            CHECK(std::abs(persp.y - ortho.y) < 1.0e-5F);
            // ...and ndc.z is the one thing that DOES move: perspective's depth is nonlinear.
            CHECK(std::abs(persp.z - ortho.z) > 1.0e-3F);
        }
    }
    SUBCASE("a point off-axis AND at a different depth genuinely MOVES") {
        // The honest negative arm. `forward * c` with c != 0 takes it off the invariant plane, which
        // is the only thing that makes the two projections disagree in x/y at all.
        const Vec3 offPlane = pivot + (right * 1.5F) + (up * 0.75F) + (forward * 3.0F);
        const Vec3 persp = ndcOf(perspViewProj, offPlane);
        const Vec3 ortho = ndcOf(orthoViewProj, offPlane);
        const float moved = std::max(std::abs(persp.x - ortho.x), std::abs(persp.y - ortho.y));
        CHECK(moved > 1.0e-2F);
    }
    SUBCASE("the near and far planes map to ndc.z 0 and 1 in ortho") {
        // ADR-005's [0,1] clip volume, which is what makes `clip.z > 0` the correct ortho near-plane
        // gate the picking predicates adopt.
        const Vec3 eye = camera.position();
        const Vec3 atNear = eye + (forward * camera.nearPlane());
        const Vec3 atFar = eye + (forward * camera.farPlane());
        CHECK(std::abs(ndcOf(orthoViewProj, atNear).z - 0.0F) < 1.0e-4F);
        CHECK(std::abs(ndcOf(orthoViewProj, atFar).z - 1.0F) < 1.0e-4F);
    }
}

TEST_CASE("editor camera: case 17 -- the ortho arm is TOTAL, and focusOn frames in BOTH modes (PM5)") {
    using engine::editor::ProjectionMode;

    SUBCASE("a POISONED fov yields a finite matrix instead of a Debug abort") {
        // THE GUARD THE PERSPECTIVE ARM DOES NOT NEED. clampState leaves a directly-set NaN in
        // fovYValue for stateIsFinite() to sweep on the NEXT update(), and ortho() asserts
        // right > left / top > bottom -- both of which carry fovY. Without the finiteness guard this
        // is a SIGABRT in the Debug lanes, not a wrong number.
        EditorCamera camera;
        camera.setProjectionMode(ProjectionMode::Orthographic);
        camera.setFovYRadians(std::numeric_limits<float>::quiet_NaN());
        REQUIRE_FALSE(std::isfinite(camera.fovYRadians()));  // anti-vacuity: the poison really landed
        const Mat4 proj = camera.projectionMatrix(1.0F);
        for (const Vec4& column : proj.columns) {
            CHECK(std::isfinite(column.x));
            CHECK(std::isfinite(column.y));
            CHECK(std::isfinite(column.z));
            CHECK(std::isfinite(column.w));
        }
    }
    SUBCASE("focusOn frames the box inside the clip volume in BOTH modes") {
        // Case 9's independent framing check, wrapped in a two-mode loop. focusOn fits the TIGHTER
        // axis from the fov, and orthoHalfHeight is built from the same fov and the distance focusOn
        // just chose -- so the fit survives the substitution rather than needing its own formula.
        const Aabb box{Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};
        for (const ProjectionMode mode : {ProjectionMode::Perspective, ProjectionMode::Orthographic}) {
            for (const float aspect : {0.5F, 1.0F, 2.0F}) {
                CAPTURE(static_cast<int>(mode));
                CAPTURE(aspect);
                EditorCamera camera;
                camera.setProjectionMode(mode);
                camera.focusOn(box, aspect);
                const Mat4 viewProj = camera.projectionMatrix(aspect) * camera.viewMatrix();
                for (int corner = 0; corner < 8; ++corner) {
                    CAPTURE(corner);
                    const Vec3 point{(corner & 1) != 0 ? box.max.x : box.min.x,
                                     (corner & 2) != 0 ? box.max.y : box.min.y,
                                     (corner & 4) != 0 ? box.max.z : box.min.z};
                    const Vec4 clip = viewProj * Vec4{point.x, point.y, point.z, 1.0F};
                    REQUIRE(clip.w > 0.0F);
                    const Vec3 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
                    CHECK(std::abs(ndc.x) <= 1.0F);
                    CHECK(std::abs(ndc.y) <= 1.0F);
                    CHECK(ndc.z >= 0.0F);
                    CHECK(ndc.z <= 1.0F);
                }
            }
        }
    }
}
