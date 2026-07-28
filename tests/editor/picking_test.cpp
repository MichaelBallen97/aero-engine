// tests/editor/picking_test.cpp — task 2.3.2: the screen mapping, the basis ray, the local-box hit
// test, the world pick walk and the pure click decision. Tier-0 and UNGATED. SEVENTH TU of
// aero_editor_shell_test (no DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -- shell_test.cpp supplies
// main()). Must pass identically with AERO_REQUIRE_GPU unset and set.
//
// Cases 1-6 land in step 1 (the pure screen mapping, the basis ray and the local-box slab test);
// cases 7-13 (the world pick walk, the click decision and the Selection write) are appended in step 2.
#include <aero/editor/picking.hpp>
#include <aero/scene/transform.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

using engine::Mat4;
using engine::Quat;
using engine::Transform;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::editor::EditorCamera;
using engine::editor::projectToViewport;
using engine::editor::Ray;
using engine::editor::rayLocalBoxHit;
using engine::editor::viewportNdc;
using engine::editor::viewportRay;

namespace {

constexpr float EPS = 1.0e-4F;
constexpr float INF_F = std::numeric_limits<float>::infinity();
// NEVER spell this `NAN` -- <cmath> defines NAN as a macro.
constexpr float QUIET_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr Vec2 VIEWPORT_POINTS{800.0F, 600.0F};

// A camera with CLOSED-FORM geometry: pivot at the origin, yaw 0, pitch 0, distance 10
//   -> position() == {0,0,10}, forward() == {0,0,-1}, right() == {1,0,0}, up() == {0,1,0}
// (position() == pivot - forward()*distance, EditorCamera's INV-1). Every setter clamps through
// clampState(), and 0/0/10 all sit inside their clamps, so the pose lands exactly.
[[nodiscard]] EditorCamera testCamera() {
    EditorCamera camera;
    camera.setPivot(Vec3::zero());
    camera.setYaw(0.0F);
    camera.setPitch(0.0F);
    camera.setDistance(10.0F);
    return camera;
}

}  // namespace

TEST_CASE("picking: viewportNdc maps points to NDC with y flipped (AC-1)") {
    const Vec2 origin{100.0F, 50.0F};
    const Vec2 size{800.0F, 400.0F};

    SUBCASE("centre") {
        const Vec2 ndc = viewportNdc(Vec2{500.0F, 250.0F}, origin, size);
        CHECK(std::abs(ndc.x) < EPS);
        CHECK(std::abs(ndc.y) < EPS);
    }
    SUBCASE("corners") {
        const Vec2 topLeft = viewportNdc(origin, origin, size);
        CHECK(std::abs(topLeft.x - (-1.0F)) < EPS);
        CHECK(std::abs(topLeft.y - 1.0F) < EPS);
        const Vec2 bottomRight = viewportNdc(Vec2{900.0F, 450.0F}, origin, size);
        CHECK(std::abs(bottomRight.x - 1.0F) < EPS);
        CHECK(std::abs(bottomRight.y - (-1.0F)) < EPS);
    }
    SUBCASE("the y flip, as a sign") {
        const Vec2 above = viewportNdc(Vec2{500.0F, 200.0F}, origin, size);
        CHECK(above.y > 0.0F);
        const Vec2 below = viewportNdc(Vec2{500.0F, 300.0F}, origin, size);
        CHECK(below.y < 0.0F);
    }
    SUBCASE("linearity") {
        const Vec2 a = viewportNdc(Vec2{300.0F, 150.0F}, origin, size);
        CHECK(std::abs(a.x - (-0.5F)) < EPS);
        CHECK(std::abs(a.y - 0.5F) < EPS);
        const Vec2 b = viewportNdc(Vec2{700.0F, 350.0F}, origin, size);
        CHECK(std::abs(b.x - 0.5F) < EPS);
        CHECK(std::abs(b.y - (-0.5F)) < EPS);
    }
    SUBCASE("E6 totality") {
        CHECK(viewportNdc(Vec2{500.0F, 250.0F}, origin, Vec2{0.0F, 0.0F}) == Vec2::zero());
        CHECK(viewportNdc(Vec2{500.0F, 250.0F}, origin, Vec2{-800.0F, 400.0F}) == Vec2::zero());
        CHECK(viewportNdc(Vec2{500.0F, 250.0F}, origin, Vec2{QUIET_NAN, 400.0F}) == Vec2::zero());
        CHECK(viewportNdc(Vec2{500.0F, 250.0F}, origin, Vec2{INF_F, 400.0F}) == Vec2::zero());
        CHECK(viewportNdc(Vec2{QUIET_NAN, 250.0F}, origin, size) == Vec2::zero());
    }
}

TEST_CASE("picking: viewportRay closed forms (AC-2)") {
    const EditorCamera camera = testCamera();
    const float tanHalf = std::tan(camera.fovYRadians() * 0.5F);

    SUBCASE("centre ray equals forward, unit length") {
        const Ray ray = viewportRay(camera, Vec2::zero(), 1.0F);
        CHECK(engine::approxEquals(ray.origin, camera.position()));
        CHECK(engine::approxEquals(ray.direction, camera.forward(), 1.0e-4F));
        CHECK(std::abs(engine::length(ray.direction) - 1.0F) < EPS);
    }
    SUBCASE("aspect scales x, not y") {
        const float aspect = 1.6F;
        const Ray ray = viewportRay(camera, Vec2{1.0F, 0.0F}, aspect);
        const float expected = (aspect * tanHalf) / std::sqrt(1.0F + (aspect * tanHalf * aspect * tanHalf));
        CHECK(std::abs(engine::dot(ray.direction, camera.right()) - expected) < 1.0e-4F);
        CHECK(std::abs(engine::dot(ray.direction, camera.up())) < 1.0e-4F);
        CHECK(std::abs(engine::length(ray.direction) - 1.0F) < EPS);
    }
    SUBCASE("aspect scales the corner ratio exactly") {
        const Ray one = viewportRay(camera, Vec2{1.0F, 1.0F}, 1.0F);
        const Ray two = viewportRay(camera, Vec2{1.0F, 1.0F}, 2.0F);
        const float ratioOne = engine::dot(one.direction, camera.right()) / engine::dot(one.direction, camera.up());
        const float ratioTwo = engine::dot(two.direction, camera.right()) / engine::dot(two.direction, camera.up());
        CHECK(std::abs(ratioTwo - (2.0F * ratioOne)) < 1.0e-4F);
    }
    SUBCASE("hostile aspect/ndc totality") {
        const Ray a = viewportRay(camera, Vec2{QUIET_NAN, 0.0F}, 1.0F);
        CHECK(a.direction == Vec3::zero());
        CHECK(a.origin == camera.position());
        const Ray b = viewportRay(camera, Vec2::zero(), QUIET_NAN);
        CHECK(b.direction == Vec3::zero());
        CHECK(b.origin == camera.position());
        const Ray c = viewportRay(camera, Vec2::zero(), 0.0F);
        CHECK(c.direction == Vec3::zero());
        CHECK(c.origin == camera.position());
        const Ray d = viewportRay(camera, Vec2::zero(), INF_F);
        CHECK(d.direction == Vec3::zero());
        CHECK(d.origin == camera.position());
    }
}

TEST_CASE("picking: the round trip NDC -> ray -> project -> NDC holds off-axis (AC-3)") {
    // Deliberately off-axis so a basis/matrix disagreement cannot hide behind an identity pose.
    EditorCamera camera;
    camera.setYaw(engine::radians(35.0F));
    camera.setPitch(engine::radians(-15.0F));

    // std::array, not a braced range-for -- that needs <initializer_list> on libstdc++.
    const std::array<float, 2> aspects{1.0F, 0.5F};
    const std::array<Vec2, 5> ndcSamples{{{0.0F, 0.0F}, {-1.0F, 1.0F}, {1.0F, 1.0F}, {-1.0F, -1.0F}, {1.0F, -1.0F}}};

    for (const float aspect : aspects) {
        for (const Vec2 ndc : ndcSamples) {
            const Mat4 viewProj = camera.projectionMatrix(aspect) * camera.viewMatrix();
            const Ray ray = viewportRay(camera, ndc, aspect);
            REQUIRE(engine::lengthSquared(ray.direction) > 0.0F);
            const Vec3 world = ray.origin + (ray.direction * 5.0F);
            const Vec4 clip = viewProj * engine::toVec4(world, 1.0F);
            REQUIRE(clip.w > 0.0F);
            // D4's basis and D14's projection are inverses.
            CHECK(std::abs((clip.x / clip.w) - ndc.x) < 1.0e-3F);
            CHECK(std::abs((clip.y / clip.w) - ndc.y) < 1.0e-3F);
            Vec2 points{};
            REQUIRE(projectToViewport(viewProj, world, VIEWPORT_POINTS, points));
            CHECK(engine::approxEquals(viewportNdc(points, Vec2::zero(), VIEWPORT_POINTS), ndc, 1.0e-3F));
        }
    }
}

TEST_CASE("picking: rayLocalBoxHit core geometry (AC-4/D3)") {
    constexpr float H = 0.5F;
    SUBCASE("entry hit") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
        CHECK(std::abs(t - 9.5F) < EPS);
    }
    SUBCASE("aimed away") {
        float t = 12345.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, 1.0F}, H, t));
        CHECK(t == 12345.0F);
    }
    SUBCASE("box entirely behind") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, -10.0F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
    }
    SUBCASE("origin inside, axis-aligned") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
    }
    SUBCASE("origin inside, diagonal") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.25F, 0.25F, 0.25F}, engine::normalize(Vec3{1.0F, 1.0F, 1.0F}), H, t));
    }
    SUBCASE("origin exactly on a face") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 0.5F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
    }
    SUBCASE("parallel and outside") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{2.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
    }
    SUBCASE("parallel and inside") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.25F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, H, t));
        CHECK(std::abs(t - 9.5F) < EPS);
    }
}

TEST_CASE("picking: rayLocalBoxHit totality against hostile inputs (AC-7)") {
    float t = 12345.0F;
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3::zero(), 0.5F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{QUIET_NAN, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.5F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{INF_F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.5F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{QUIET_NAN, 0.0F, -1.0F}, 0.5F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.0F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, -0.5F, t));
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, QUIET_NAN, t));
    CHECK(t == 12345.0F);
}

TEST_CASE("picking: rayLocalBoxHit's t is in WORLD units, not local units (AC-5/D2, arm A)") {
    const Mat4 unscaled =
        engine::localMatrix(Transform{.position = Vec3::zero(), .rotation = Quat::identity(), .scale = Vec3::one()});
    const Mat4 scaled = engine::localMatrix(
        Transform{.position = Vec3::zero(), .rotation = Quat::identity(), .scale = Vec3{10.0F, 10.0F, 10.0F}});
    const Mat4 invUnscaled = engine::inverse(unscaled);
    const Mat4 invScaled = engine::inverse(scaled);

    const Vec3 worldOrigin{0.0F, 0.0F, 100.0F};
    const Vec3 worldDir{0.0F, 0.0F, -1.0F};

    float tUnscaled = 0.0F;
    REQUIRE(rayLocalBoxHit(engine::transformPoint(invUnscaled, worldOrigin),
                           engine::transformDirection(invUnscaled, worldDir), 0.5F, tUnscaled));
    float tScaled = 0.0F;
    REQUIRE(rayLocalBoxHit(engine::transformPoint(invScaled, worldOrigin),
                           engine::transformDirection(invScaled, worldDir), 0.5F, tScaled));

    CHECK(std::abs(tUnscaled - 99.5F) < 1.0e-3F);
    CHECK(std::abs(tScaled - 95.0F) < 1.0e-3F);  // the 10x box's front face is at world z = 5
    CHECK(tScaled > 90.0F);  // normalising localDir reports 9.5 -- a TENTH. S2 reddens exactly here.
}
