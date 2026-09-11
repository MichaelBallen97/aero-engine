// tests/editor/material_preview_rig_test.cpp — task E.2.4: the material preview's rig (PV1–PV12).
// TIER 0: pure, no GPU, no ImGui, no World, no #if of any kind. `main` comes from tests/test_main.cpp
// like every other TU on aero_editor_shell_test.
//
// EVERY MAGNITUDE IN MaterialPreviewRig IS A TUNING CONSTANT judged on the validation page, so nothing
// here asserts one. PV1 asserts the RELATIONSHIPS between them and PV12 checks the same relationships
// on the real matrices; a retune of orbitRadius / orbitSpeed / fovYDegrees reddens neither.

#include <aero/core/math.hpp>
#include <aero/editor/material_preview_rig.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <ostream>

using engine::Mat4;
using engine::Vec3;
using engine::Vec4;
using engine::editor::advanceMaterialPreviewOrbit;
using engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG;
using engine::editor::materialPreviewCamera;
using engine::editor::MaterialPreviewLighting;
using engine::editor::MaterialPreviewRig;
using engine::editor::materialPreviewView;
using engine::render::AmbientMode;
using engine::render::BackgroundMode;
using engine::render::CameraView;
using engine::render::EnvironmentData;
using engine::render::MeshInstance;
using engine::render::RenderView;

namespace {

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// The eye's distance from the world origin, which is the sphere's centre. Every framing relationship
// below is stated against this rather than against orbitRadius alone -- the camera sits ABOVE the
// equator, so the radius is not the distance.
[[nodiscard]] float eyeDistance(const MaterialPreviewRig& rig) {
    return std::sqrt((rig.orbitRadius * rig.orbitRadius) + (rig.orbitHeight * rig.orbitHeight));
}

// Element for element, so a failure names WHICH element moved rather than printing two matrices.
void checkMat4Equal(const Mat4& got, const Mat4& want) {
    for (std::size_t c = 0; c < 4; ++c) {
        CAPTURE(c);
        CHECK(got.columns.at(c).x == want.columns.at(c).x);
        CHECK(got.columns.at(c).y == want.columns.at(c).y);
        CHECK(got.columns.at(c).z == want.columns.at(c).z);
        CHECK(got.columns.at(c).w == want.columns.at(c).w);
    }
}

[[nodiscard]] bool mat4BitEqual(const Mat4& a, const Mat4& b) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (a.columns.at(c).x != b.columns.at(c).x || a.columns.at(c).y != b.columns.at(c).y ||
            a.columns.at(c).z != b.columns.at(c).z || a.columns.at(c).w != b.columns.at(c).w) {
            return false;
        }
    }
    return true;
}

// A sun whose every field is off its default, so a rig that dropped a field could not pass by
// agreeing with the default. `direction` is deliberately NON-UNIT: the rig must not re-normalise the
// bridge's number.
[[nodiscard]] engine::render::DirectionalLightData nonDefaultSun() {
    return {.direction = Vec3{2.0F, 0.0F, 0.0F},
            .color = Vec3{0.25F, 0.5F, 0.75F},
            .intensity = 2.75F,
            .castsShadows = false,
            .shadowBias = 0.007F,
            .shadowNormalBias = 0.09F,
            .shadowDistance = 42.0F};
}

[[nodiscard]] EnvironmentData nonDefaultEnvironment() {
    return {.backgroundMode = BackgroundMode::Solid,
            .skyColor = Vec3{0.11F, 0.22F, 0.33F},
            .horizonColor = Vec3{0.44F, 0.55F, 0.66F},
            .groundColor = Vec3{0.77F, 0.12F, 0.34F},
            .solidColor = Vec3{0.13F, 0.24F, 0.35F},
            .ambientMode = AmbientMode::Flat,
            .ambientColor = Vec3{0.9F, 0.8F, 0.7F},
            .ambientIntensity = 3.5F};
}

}  // namespace

TEST_CASE("editor: the preview rig's constants stand in the right RELATIONSHIPS (PV1)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const float d = eyeDistance(rig);
    CAPTURE(d);

    // The eye is OUTSIDE the unit sphere it looks at.
    CHECK(rig.orbitRadius > 1.0F);
    CHECK(d > 1.0F);
    // The near plane is nearer than the sphere's closest point and the far plane is beyond its
    // farthest, so the whole sphere is inside the frustum's depth range.
    CHECK(rig.nearPlane < d - 1.0F);
    CHECK(rig.farPlane > d + 1.0F);
    CHECK(rig.nearPlane > 0.0F);
    CHECK(rig.farPlane > rig.nearPlane);
    // The sphere fits the VERTICAL field of view: its angular diameter seen from the eye is
    // 2*asin(1/d), and the fov must exceed it.
    const float angularDiameter = 2.0F * std::asin(1.0F / d);
    CAPTURE(angularDiameter);
    CHECK(angularDiameter < engine::radians(rig.fovYDegrees));
    // The orbit turns, in the positive direction, and the fov is a legal perspective fov.
    CHECK(rig.orbitSpeed > 0.0F);
    CHECK(rig.fovYDegrees > 0.0F);
    CHECK(rig.fovYDegrees < 180.0F);
    // The default IS the default -- so DEFAULT_MATERIAL_PREVIEW_RIG and MaterialPreviewRig{} can
    // never drift apart.
    CHECK(DEFAULT_MATERIAL_PREVIEW_RIG == MaterialPreviewRig{});
}

TEST_CASE("editor: the preview camera's eye is the orbit's own parametrisation (PV2)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const std::array<float, 4> angles{0.0F, engine::HALF_PI, 0.7F, 3.9F};
    for (const float a : angles) {
        CAPTURE(a);
        const CameraView camera = materialPreviewCamera(rig, a, 1.0F);
        // The SAME expression on both sides, so exactness is by construction rather than by rounding
        // -- neither sin nor cos is required by IEEE-754 to be correctly rounded, and this case makes
        // no claim that they are.
        CHECK(camera.eyePosition.x == rig.orbitRadius * std::cos(a));
        CHECK(camera.eyePosition.y == rig.orbitHeight);
        CHECK(camera.eyePosition.z == rig.orbitRadius * std::sin(a));
    }
    // a = 0 is the one angle where the value is arithmetic rather than a libm result: cos(0) is
    // exactly 1 and sin(0) is exactly 0 on every conforming implementation.
    const CameraView atZero = materialPreviewCamera(rig, 0.0F, 1.0F);
    CHECK(atZero.eyePosition == Vec3{rig.orbitRadius, rig.orbitHeight, 0.0F});
}

TEST_CASE("editor: the preview camera IS engine::lookAt and engine::perspective, exactly (PV3)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const float angle = 0.7F;
    const float aspect = 1.75F;
    const CameraView camera = materialPreviewCamera(rig, angle, aspect);

    // NOT an epsilon: both sides are ONE computation, the engine's own functions called with the same
    // arguments. An epsilon here would let a rig that used a different up vector or a different target
    // pass on a nearly-identical matrix.
    const Vec3 eye{rig.orbitRadius * std::cos(angle), rig.orbitHeight, rig.orbitRadius * std::sin(angle)};
    checkMat4Equal(camera.view, engine::lookAt(eye, Vec3::zero(), Vec3{0.0F, 1.0F, 0.0F}));
    checkMat4Equal(camera.proj,
                   engine::perspective(engine::radians(rig.fovYDegrees), aspect, rig.nearPlane, rig.farPlane));
}

TEST_CASE("editor: the preview camera's aspect reaches ONLY columns[0].x, and a bad one falls back (PV4)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const Mat4 atOne = materialPreviewCamera(rig, 0.7F, 1.0F).proj;

    SUBCASE("a legal aspect moves exactly one element") {
        const Mat4 atTwo = materialPreviewCamera(rig, 0.7F, 2.0F).proj;
        CHECK(atTwo.columns.at(0).x != atOne.columns.at(0).x);  // the one that MUST move
        for (std::size_t c = 0; c < 4; ++c) {
            CAPTURE(c);
            if (c != 0) {
                CHECK(atTwo.columns.at(c).x == atOne.columns.at(c).x);
            }
            CHECK(atTwo.columns.at(c).y == atOne.columns.at(c).y);
            CHECK(atTwo.columns.at(c).z == atOne.columns.at(c).z);
            CHECK(atTwo.columns.at(c).w == atOne.columns.at(c).w);
        }
    }

    SUBCASE("every degenerate aspect takes the 1.0 fallback, element for element") {
        // The NaN arm is what the NEGATED `>` buys: `aspect > 0` is false for a NaN, so the fallback
        // fires. A `!(aspect <= 0)` spelling -- which reads like the same thing -- would ACCEPT it.
        const std::array<float, 5> bad{0.0F, -1.0F, NAN_F, INF_F, -INF_F};
        for (const float aspect : bad) {
            CAPTURE(aspect);
            checkMat4Equal(materialPreviewCamera(rig, 0.7F, aspect).proj, atOne);
        }
    }
}

TEST_CASE("editor: the preview view carries the scene's Environment VERBATIM (PV5)") {
    const MaterialPreviewLighting lighting{.environment = nonDefaultEnvironment()};
    std::array<MeshInstance, 1> instances{};
    const RenderView view =
        materialPreviewView(materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.7F, 1.0F), lighting, instances);

    // Field for field -- no clamp, no re-normalisation, no mode translation. These are the BRIDGE's
    // numbers and must stay them; that is what makes PX's byte-identity possible at all. The two enum
    // comparisons go in DOUBLE parentheses (doctest's decomposer, the toString trap's cousin).
    const EnvironmentData& want = lighting.environment;
    CHECK((view.environment.backgroundMode == want.backgroundMode));
    CHECK(view.environment.skyColor == want.skyColor);
    CHECK(view.environment.horizonColor == want.horizonColor);
    CHECK(view.environment.groundColor == want.groundColor);
    CHECK(view.environment.solidColor == want.solidColor);
    CHECK((view.environment.ambientMode == want.ambientMode));
    CHECK(view.environment.ambientColor == want.ambientColor);
    CHECK(view.environment.ambientIntensity == want.ambientIntensity);
    // Anti-vacuity: the seed really is non-default, so "verbatim" is a claim about the copy.
    CHECK_FALSE(view.environment == EnvironmentData{});
}

TEST_CASE("editor: the preview view carries the scene's sun VERBATIM, non-unit direction included (PV6)") {
    const MaterialPreviewLighting lighting{.sun = nonDefaultSun(), .hasSun = true};
    std::array<MeshInstance, 1> instances{};
    const RenderView view =
        materialPreviewView(materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.7F, 1.0F), lighting, instances);

    const engine::render::DirectionalLightData& want = lighting.sun;
    CHECK(view.directional.direction == want.direction);
    CHECK(view.directional.color == want.color);
    CHECK(view.directional.intensity == want.intensity);
    CHECK(view.directional.castsShadows == want.castsShadows);
    CHECK(view.directional.shadowBias == want.shadowBias);
    CHECK(view.directional.shadowNormalBias == want.shadowNormalBias);
    CHECK(view.directional.shadowDistance == want.shadowDistance);
    // The direction was passed NON-UNIT on purpose and STAYS non-unit: the rig does not re-normalise
    // what the bridge handed it. A rig that normalised would make the parity A/B approximate.
    CHECK(engine::lengthSquared(view.directional.direction) == 4.0F);

    // ...and the three fields nothing else in this task assigns.
    CHECK(view.hasCamera);
    CHECK(view.points.empty());
    CHECK(view.spots.empty());
}

// task E.2.4 (code-review round): AC-10 names FOUR degenerate inputs -- a NaN delta, a negative
// delta, a non-positive or non-finite aspect, and A NON-FINITE SCENE COLOUR. PV11 covers the first
// two and PV4 the third; nothing covered the fourth, because every other arm here and every PX arm
// seeds a FINITE colour. The claim is the same one PV5 and PV6 make for finite values -- the rig
// copies the bridge's numbers and does not touch them -- but it has to be made where a future
// "defensive" sanitiser would bite, which is precisely the non-finite case.
//
// WHY IT MATTERS THAT THIS IS TESTED HERE AND NOT BY PX: a sanitiser added inside
// materialPreviewView would leave every PX arm green, because side A IS the rig and would sanitise
// while side B -- SceneRenderer over the same World -- would pass the number through untouched. The
// byte-identity would break in the one direction PX structurally cannot see.
TEST_CASE("editor: the preview view carries a NON-FINITE scene colour verbatim too (PV13)") {
    MaterialPreviewLighting lighting{.environment = nonDefaultEnvironment(), .sun = nonDefaultSun()};
    lighting.environment.skyColor.x = NAN_F;
    lighting.environment.ambientColor.y = INF_F;
    lighting.sun.color.z = -INF_F;
    lighting.sun.direction.x = NAN_F;

    std::array<MeshInstance, 1> instances{};
    const RenderView view =
        materialPreviewView(materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.7F, 1.0F), lighting, instances);

    // Asserted with std::isnan / std::isinf, never with ==, which is false for every NaN (PV11's own
    // rule). A NaN that ARRIVED is not a NaN the rig MANUFACTURED -- that is the distinction AC-10
    // draws, and it is the reason this asserts propagation rather than refusal.
    CHECK(std::isnan(view.environment.skyColor.x));
    CHECK(std::isinf(view.environment.ambientColor.y));
    CHECK(view.environment.ambientColor.y > 0.0F);
    CHECK(std::isinf(view.directional.color.z));
    CHECK(view.directional.color.z < 0.0F);
    CHECK(std::isnan(view.directional.direction.x));

    // Anti-vacuity: the FINITE fields beside them are untouched, so this is a statement about the
    // non-finite components and not about a view that was wholesale discarded.
    CHECK(view.environment.skyColor.y == nonDefaultEnvironment().skyColor.y);
    CHECK(view.directional.intensity == nonDefaultSun().intensity);

    // And the rig still manufactures nothing of its own: the CAMERA is finite even though the
    // lighting is not, because the two share no arithmetic.
    CHECK(std::isfinite(view.camera.eyePosition.x));
    CHECK(std::isfinite(view.camera.eyePosition.y));
    CHECK(std::isfinite(view.camera.eyePosition.z));
    CHECK(std::isfinite(view.instances[0].mvp.columns[0].x));
}

TEST_CASE("editor: every mvp is (proj * view) * model, bit for bit (PV7)") {
    const CameraView camera = materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.7F, 1.6F);
    std::array<MeshInstance, 3> instances{};
    instances.at(0).model = engine::translation(Vec3{1.5F, -2.25F, 0.75F});
    instances.at(1).model = engine::scaling(Vec3{2.0F, 0.5F, 3.25F});
    instances.at(2).model = engine::toMat4(engine::fromAxisAngle(Vec3::unitY(), engine::radians(37.0F)));

    const RenderView view = materialPreviewView(camera, MaterialPreviewLighting{}, instances);
    CHECK(view.instances.size() == 3);

    const Mat4 viewProj = camera.proj * camera.view;
    std::size_t associationsThatDiffer = 0;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        CAPTURE(i);
        // BIT-equal, not approximately equal: this is the association claim, and an epsilon would let
        // proj * (view * model) pass (E.2.2's fl(1e-4)^2 species).
        checkMat4Equal(instances.at(i).mvp, viewProj * instances.at(i).model);
        if (!mat4BitEqual(camera.proj * (camera.view * instances.at(i).model), instances.at(i).mvp)) {
            ++associationsThatDiffer;
        }
    }
    // THE ARM'S OWN LIVENESS, MEASURED rather than assumed. If the two associations happen to round
    // identically for all three models the arm above asserts the geometry and not the association, and
    // PX1's byte-identity is the standing witness for that seed instead. Reported, never failed.
    if (associationsThatDiffer == 0) {
        MESSAGE(
            "PV7: proj*(view*model) is bit-equal to (proj*view)*model for all three models here; "
            "the association arm is vacuous and PX1 is the standing witness (seed S8)");
    } else {
        MESSAGE("PV7: the two associations differ for " << associationsThatDiffer << " of 3 models");
    }
}

TEST_CASE("editor: the preview view opts OUT of shadows and IN to culling (PV8)") {
    std::array<MeshInstance, 1> instances{};
    const RenderView view = materialPreviewView(materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.0F, 1.0F),
                                                MaterialPreviewLighting{}, instances);
    // Three separate CHECKs, because shadowsEnabled is the field a careless edit drops and a compound
    // assertion would not name it.
    CHECK_FALSE(view.shadowsEnabled);
    CHECK(view.cullingEnabled);
    CHECK_FALSE(view.shadow.valid);
}

TEST_CASE("editor: the preview view BORROWS its instances and writes through them (PV9)") {
    std::array<MeshInstance, 2> instances{};
    instances.at(0).model = engine::translation(Vec3{4.0F, 0.0F, 0.0F});
    instances.at(1).model = Mat4::identity();
    const Mat4 beforeCall = instances.at(0).mvp;

    const RenderView view = materialPreviewView(materialPreviewCamera(DEFAULT_MATERIAL_PREVIEW_RIG, 0.0F, 1.0F),
                                                MaterialPreviewLighting{}, instances);

    // BORROWED, never copied -- the RenderView span rule.
    CHECK(view.instances.data() == instances.data());
    CHECK(view.instances.size() == instances.size());
    // ...and the loop really wrote through the CALLER's array, which is what makes the borrow useful.
    CHECK_FALSE(mat4BitEqual(instances.at(0).mvp, beforeCall));
}

TEST_CASE("editor: the orbit advances exactly, wraps into range, and stays there (PV10)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;

    SUBCASE("below the wrap the result is EXACTLY the sum the pre-E.2.4 body computed") {
        const float angle = 0.4F;
        const float dt = 1.0F / 60.0F;
        CHECK(advanceMaterialPreviewOrbit(angle, dt, rig) == angle + (dt * rig.orbitSpeed));
    }

    SUBCASE("a step across the wrap lands in [0, TWO_PI) and is bit-equal to the subtraction") {
        // Hand-picked so the sum lands in [TWO_PI, 2*TWO_PI) -- the ONLY range one ordinary step can
        // reach. Sterbenz makes `next - TWO_PI` exact there, and fmod returns exactly that, which is
        // why the pre-E.2.4 picture did not move when the wrap became fmod. Pinned so a future
        // "simplification" cannot move it either.
        const float angle = engine::TWO_PI - 0.001F;
        const float dt = 1.0F;
        const float next = angle + (dt * rig.orbitSpeed);
        REQUIRE(next >= engine::TWO_PI);
        REQUIRE(next < 2.0F * engine::TWO_PI);
        const float wrapped = advanceMaterialPreviewOrbit(angle, dt, rig);
        CHECK(wrapped == next - engine::TWO_PI);
        CHECK(wrapped >= 0.0F);
        CHECK(wrapped < engine::TWO_PI);
    }

    SUBCASE("a single huge step still lands in range") {
        const float wrapped = advanceMaterialPreviewOrbit(0.0F, 1e6F, rig);
        CHECK(std::isfinite(wrapped));
        CHECK(wrapped >= 0.0F);
        CHECK(wrapped < engine::TWO_PI);
    }

    SUBCASE("ten thousand accumulated 1/60-second steps never leave the range") {
        float angle = 0.0F;
        for (int i = 0; i < 10000; ++i) {
            angle = advanceMaterialPreviewOrbit(angle, 1.0F / 60.0F, rig);
            REQUIRE(angle >= 0.0F);
            REQUIRE(angle < engine::TWO_PI);
        }
        // Anti-vacuity: the orbit actually TURNED rather than sitting at zero for ten thousand steps.
        CHECK(angle != 0.0F);
    }
}

TEST_CASE("editor: the orbit refuses to MANUFACTURE a NaN (PV11)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const float angle = 1.25F;

    SUBCASE("every degenerate delta returns the angle unchanged, bit-exactly") {
        const std::array<float, 4> bad{NAN_F, -1.0F, INF_F, -INF_F};
        for (const float dt : bad) {
            CAPTURE(dt);
            CHECK(advanceMaterialPreviewOrbit(angle, dt, rig) == angle);
        }
    }

    SUBCASE("garbage in stays garbage out -- this function does not sanitise its caller's state") {
        // Deliberate and documented: refusing to MAKE a NaN is not the same promise as cleaning one up.
        // Asserted with std::isnan, never with ==, which is false for every NaN.
        CHECK(std::isnan(advanceMaterialPreviewOrbit(NAN_F, 1.0F / 60.0F, rig)));
    }
}

TEST_CASE("editor: the unit sphere is inside the frustum on the REAL matrices (PV12)") {
    const MaterialPreviewRig rig = DEFAULT_MATERIAL_PREVIEW_RIG;
    const CameraView camera = materialPreviewCamera(rig, 0.7F, 1.0F);
    const Mat4 viewProj = camera.proj * camera.view;

    // PV1's relationship, checked where it actually matters: fourteen points spread over the UNIT
    // SPHERE -- six axis poles and eight normalised diagonals -- all project inside the viewport with
    // a positive w.
    //
    // THE SAMPLE IS THE SPHERE, NOT ITS BOUNDING BOX, and the distinction is measured rather than
    // stylistic: the box's corners sit at sqrt(3) from the origin, an angular radius of
    // asin(sqrt(3)/d) = 32.4 degrees from this eye against a 30-degree half field of view, so the box
    // is NOT framed. Projected through these very matrices at angle 0.7, corner (+1,-1,+1) lands at
    // |clip.y/clip.w| = 1.096 and is off-screen -- ONE of the eight, the next-nearest being
    // (+1,+1,-1) at |clip.x/clip.w| = 0.888. One is enough: the plan's "every box corner is inside"
    // assertion is false, which is why this samples the sphere instead. (The 32.4-degree figure is the
    // worst case over the whole sphere of radius sqrt(3); it does not say how many CORNERS miss, and
    // an earlier draft of this comment wrongly inferred four from it.)
    // The preview draws a sphere, PV1's relationship is
    // 2*asin(1/d) < fovY (18.0 degrees against 30), and this is that relationship on the real
    // matrices. Asserting the box would assert something the rig never promised.
    const std::array<Vec3, 6> poles{Vec3::unitX(),  -Vec3::unitX(), Vec3::unitY(),
                                    -Vec3::unitY(), Vec3::unitZ(),  -Vec3::unitZ()};
    std::array<Vec3, 14> samples{};
    for (std::size_t i = 0; i < poles.size(); ++i) {
        samples.at(i) = poles.at(i);
    }
    for (std::size_t corner = 0; corner < 8; ++corner) {
        const Vec3 direction{(corner & 1U) != 0U ? 1.0F : -1.0F, (corner & 2U) != 0U ? 1.0F : -1.0F,
                             (corner & 4U) != 0U ? 1.0F : -1.0F};
        samples.at(poles.size() + corner) = engine::normalize(direction);
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CAPTURE(i);
        const Vec3 point = samples.at(i);
        REQUIRE(engine::approxEquals(engine::lengthSquared(point), 1.0F, 1e-5F));  // it IS on the sphere
        const Vec4 clip = viewProj * Vec4{point.x, point.y, point.z, 1.0F};
        CHECK(clip.w > 0.0F);
        CHECK(std::abs(clip.x / clip.w) < 1.0F);
        CHECK(std::abs(clip.y / clip.w) < 1.0F);
    }

    // Anti-vacuity: the predicate can say NO. A point 20 units behind the sphere fails at least one of
    // the three, so the fourteen passes above are a statement about the framing.
    const Vec4 farBehind = viewProj * Vec4{0.0F, 0.0F, 20.0F, 1.0F};
    const bool inFront = farBehind.w > 0.0F;
    const bool insideX = std::abs(farBehind.x / farBehind.w) < 1.0F;
    const bool insideY = std::abs(farBehind.y / farBehind.w) < 1.0F;
    // ONE named bool, never CHECK_FALSE(a && b && c): doctest's decomposer hard-errors on a compound
    // expression with "Expression Too Complex Please Rewrite As Binary Comparison!".
    const bool inside = inFront && insideX && insideY;
    CHECK_FALSE(inside);
}
