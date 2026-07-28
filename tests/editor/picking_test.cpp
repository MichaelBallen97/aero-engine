// tests/editor/picking_test.cpp — task 2.3.2: the screen mapping, the basis ray, the local-box hit
// test, the world pick walk and the pure click decision. Tier-0 and UNGATED. SEVENTH TU of
// aero_editor_shell_test (no DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -- shell_test.cpp supplies
// main()). Must pass identically with AERO_REQUIRE_GPU unset and set.
//
// Cases 1-6 (step 1) are the pure screen mapping, the basis ray and the local-box slab test; cases
// 7-13 (step 2, appended here) are the world pick walk, the click decision and the Selection write.
//
// Case 11's LogFixture follows scene_bounds_test.cpp's case-12b idiom -- declared FIRST in its case
// so it destructs LAST, after the LogSinkScope.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/picking.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using engine::Entity;
using engine::Mat4;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::editor::applyPickAction;
using engine::editor::EditorCamera;
using engine::editor::PickAction;
using engine::editor::pickEntity;
using engine::editor::PickRequest;
using engine::editor::PickResult;
using engine::editor::pickSelectionAction;
using engine::editor::projectToViewport;
using engine::editor::Ray;
using engine::editor::rayLocalBoxHit;
using engine::editor::Selection;
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

// "Click exactly where this world point projects." Built from the SAME view-projection pickEntity
// derives, so a case reads as an intent ("click the cube's centre") rather than as arithmetic.
[[nodiscard]] Vec2 ndcOf(const EditorCamera& camera, float aspect, Vec3 worldPoint) {
    const Vec4 clip = camera.projectionMatrix(aspect) * camera.viewMatrix() * engine::toVec4(worldPoint, 1.0F);
    return Vec2{clip.x / clip.w, clip.y / clip.w};
}

// Nudge an NDC by a screen-space offset in POINTS. ndcToViewportPoints scales by size/2, so the
// inverse scales by 2/size; y is negated because NDC y is UP and points are DOWN.
[[nodiscard]] Vec2 offsetNdcByPoints(Vec2 ndc, Vec2 deltaPoints, Vec2 sizePoints) {
    return Vec2{ndc.x + (2.0F * deltaPoints.x / sizePoints.x), ndc.y - (2.0F * deltaPoints.y / sizePoints.y)};
}

[[nodiscard]] Entity makeMesh(World& world, Vec3 position, Quat rotation = Quat::identity(), Vec3 scale = Vec3::one()) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position, .rotation = rotation, .scale = scale});
    world.add<MeshRenderer>(e, MeshRenderer{});
    return e;
}

// A Transform-only entity: entityBounds(..., false) documents this as the single POINT at its world
// translation (F3), which is exactly D5's pick point.
[[nodiscard]] Entity makePoint(World& world, Vec3 position) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position});
    return e;
}

[[nodiscard]] PickRequest requestAt(Vec2 ndc) {
    return PickRequest{.ndc = ndc, .aspect = 1.0F, .viewportSizePoints = VIEWPORT_POINTS};
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

TEST_CASE("picking: a rotated cube is MISSED where its world AABB would be hit (AC-6/D2)") {
    // NOT a Y rotation: a Y-rotated cube viewed along -Z has EXACTLY the same silhouette as its own
    // world AABB (for every world x in [-0.7071, 0.7071] some z lies inside the rotated footprint),
    // so no -Z ray can discriminate the two and the case would be vacuous. A Z rotation puts the
    // difference in the XY plane, where the -Z ray can see it.
    World w;
    const EditorCamera camera = testCamera();
    const Entity cube = makeMesh(w, Vec3::zero(), engine::fromAxisAngle(Vec3::unitZ(), engine::radians(45.0F)));

    SUBCASE("a ray at the centre HITS") {
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == cube);
        CHECK_FALSE(r.isPoint);
        CHECK(std::abs(r.distance - 9.5F) < 1.0e-3F);
    }
    SUBCASE("a ray inside the world AABB but OUTSIDE the cube MISSES -- S3's discriminator") {
        // The rotated cube's world AABB is [-0.7071, 0.7071]^2 x [-0.5, 0.5]. World {0.6, 0.6, 0} is
        // INSIDE it and OUTSIDE the cube (local x = 0.8485 > 0.5). An AABB test enters at z = 0.5
        // with x = y = 0.57 and HITS; the OBB test gives tMin 9.534 > tMax 5.91 and MISSES.
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3{0.6F, 0.6F, 0.0F})));
        CHECK_FALSE(r.hit());
    }
}

TEST_CASE("picking: pickEntity selects among entities (AC-8/E15)") {
    World w;
    const EditorCamera camera = testCamera();

    SUBCASE("nearest of three collinear cubes wins") {
        const Entity near = makeMesh(w, Vec3{0.0F, 0.0F, 0.0F});
        const Entity mid = makeMesh(w, Vec3{0.0F, 0.0F, -5.0F});
        const Entity far = makeMesh(w, Vec3{0.0F, 0.0F, -10.0F});
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == near);
        CHECK(std::abs(r.distance - 9.5F) < 1.0e-3F);
        (void)mid;
        (void)far;
    }
    SUBCASE("destroy the nearest and the middle wins") {
        const Entity near = makeMesh(w, Vec3{0.0F, 0.0F, 0.0F});
        const Entity mid = makeMesh(w, Vec3{0.0F, 0.0F, -5.0F});
        const Entity far = makeMesh(w, Vec3{0.0F, 0.0F, -10.0F});
        REQUIRE(w.destroy(near));
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == mid);
        CHECK(std::abs(r.distance - 14.5F) < 1.0e-3F);
        (void)far;
    }
    SUBCASE("parenting: the child's WORLD transform is what gets clicked") {
        const Entity parent = makePoint(w, Vec3{3.0F, 0.0F, 0.0F});
        const Entity child = makeMesh(w, Vec3{0.0F, 2.0F, 0.0F});
        REQUIRE(w.setParent(child, parent));
        // The child's world position is {3,2,0}: clicking there picks the child.
        const PickResult hit = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3{3.0F, 2.0F, 0.0F})));
        CHECK(hit.entity == child);
        // Where the child would be WITHOUT the parent transform -- the parent's own projection is far
        // outside the 8-point radius at this distance, so it never competes and this is a clean miss.
        const PickResult miss = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3{0.0F, 2.0F, 0.0F})));
        CHECK_FALSE(miss.hit());
    }
    SUBCASE("dead and null handles are skipped") {
        const Entity doomed = makeMesh(w, Vec3::zero());
        REQUIRE(w.destroy(doomed));
        const PickResult afterDeath = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK_FALSE(afterDeath.hit());  // a null Entity{} needs no special case: pickEntity only ever
                                        // sees what eachEntity yields, and a dead handle is never in it
        const Entity behind = makeMesh(w, Vec3{0.0F, 0.0F, -5.0F});
        const PickResult afterLive = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3{0.0F, 0.0F, -5.0F})));
        CHECK(afterLive.entity == behind);
    }
    SUBCASE("two co-located cubes tie-break on the LOWEST index, whatever eachEntity's order (D16/E15)") {
        // eachEntity walks es.data()[0 .. free_list()) (engine/scene/src/world.cpp:264-278), which is
        // CREATION order when nothing has been destroyed -- and creation order is ALSO index order, so
        // the `e.index < mesh.entity.index` branch would never be entered and S4 could not redden.
        // Recycling one slot inverts the visit order: destroy() swaps the erased entity with the last
        // in-use element, so [a,b,c] - destroy(b) -> [a,c,b], and the next create() recycles b's index
        // with a bumped generation. The walk then yields index 0, index 2, index 1.
        const Entity decoy = makeMesh(w, Vec3{0.0F, 0.0F, -50.0F});  // index 0, far away
        const Entity doomed = w.create();                            // index 1
        const Entity high = makeMesh(w, Vec3::zero());               // index 2
        REQUIRE(w.destroy(doomed));
        const Entity low = makeMesh(w, Vec3::zero());  // index 1 recycled, generation 2

        // ANTI-VACUITY: assert the inversion exists. If EnTT's packing ever changes, this REQUIRE
        // fails LOUDLY instead of the case passing for the wrong reason.
        std::vector<std::uint32_t> visited;
        w.eachEntity([&](Entity e) { visited.push_back(e.index); });
        REQUIRE(visited.size() == 3);
        const auto highPos = std::find(visited.begin(), visited.end(), high.index);
        const auto lowPos = std::find(visited.begin(), visited.end(), low.index);
        REQUIRE(highPos != visited.end());
        REQUIRE(lowPos != visited.end());
        REQUIRE(highPos < lowPos);  // a HIGHER index is visited BEFORE a lower one

        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == low);  // S4 (dropping the tie-break) returns `high` and reddens here
        CHECK(low.index < high.index);
        (void)decoy;
    }
}

TEST_CASE("picking: pickEntity's distance is in WORLD units, not local units (AC-5/D2, arm B)") {
    // A 10x-scaled cube at the origin has its front face at world z = 5 -> t = 5 from the eye at z = 10.
    // A UNIT cube at {0,0,5.5} has its front face at z = 6 -> t = 4. The unit cube is NEARER and must
    // win. Normalising the local direction reports the scaled cube at t = 0.5 -- a tenth -- so it
    // wrongly wins, which is S2 at the pickEntity seed site.
    World w;
    const EditorCamera camera = testCamera();
    const Entity big = makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{10.0F, 10.0F, 10.0F});
    const Entity small = makeMesh(w, Vec3{0.0F, 0.0F, 5.5F});
    const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
    CHECK(r.entity == small);
    CHECK(std::abs(r.distance - 4.0F) < 1.0e-3F);
    (void)big;
}

TEST_CASE("picking: point candidates by screen radius (AC-9/D5)") {
    World w;
    const EditorCamera camera = testCamera();

    SUBCASE("dead centre hits, 20 points away misses, 4 points away still hits") {
        const Entity light = makePoint(w, Vec3{2.0F, 0.0F, 0.0F});
        const Vec2 ndc = ndcOf(camera, 1.0F, Vec3{2.0F, 0.0F, 0.0F});

        const PickResult centre = pickEntity(w, camera, requestAt(ndc));
        CHECK(centre.entity == light);
        CHECK(centre.isPoint);
        CHECK(std::abs(centre.distance - engine::length(Vec3{2.0F, 0.0F, 0.0F} - camera.position())) < 1.0e-3F);

        const PickResult farAway =
            pickEntity(w, camera, requestAt(offsetNdcByPoints(ndc, Vec2{20.0F, 0.0F}, VIEWPORT_POINTS)));
        CHECK_FALSE(farAway.hit());

        const PickResult stillInside =
            pickEntity(w, camera, requestAt(offsetNdcByPoints(ndc, Vec2{4.0F, 0.0F}, VIEWPORT_POINTS)));
        CHECK(stillInside.entity == light);
    }
    SUBCASE("a light behind the camera cannot be picked") {
        (void)makePoint(w, Vec3{0.0F, 0.0F, 20.0F});
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK_FALSE(r.hit());
    }
    SUBCASE("the nearer-on-screen candidate wins regardless of index order") {
        const Vec3 clickWorld{2.0F, 0.0F, 0.0F};
        // ~3.1 points away at this depth -- inside the 8-point radius, so it is a REAL competitor.
        const Entity far = makePoint(w, Vec3{2.0F, 0.06F, 0.0F});  // created FIRST -- the LOWER index
        const Entity near = makePoint(w, clickWorld);              // created SECOND -- the HIGHER index
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, clickWorld)));
        CHECK(r.entity == near);  // wins on DISTANCE despite the higher index
        (void)far;
    }
    SUBCASE("equal screen distance ties break on the LOWEST index") {
        // Both candidates sit at the EXACT SAME world point as the click itself, which makes their
        // screenDistance BIT-IDENTICAL (both zero) by construction -- a floating round trip through
        // the projection matrix could never be trusted to reproduce an exact tie, but two identical
        // calls with identical inputs are deterministic under IEEE arithmetic.
        const Vec3 clickWorld{2.0F, 0.0F, 0.0F};
        const Entity low = makePoint(w, clickWorld);   // created FIRST -- the LOWER index
        const Entity high = makePoint(w, clickWorld);  // created SECOND, SAME position -- the tie
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, clickWorld)));
        CHECK(r.entity == low);
        (void)high;
    }
}

TEST_CASE("picking: the depth rule decides between a mesh hit and a point candidate (AC-10/D5)") {
    World w;
    const EditorCamera camera = testCamera();
    const Entity cube = makeMesh(w, Vec3::zero());  // front face z = 0.5, t = 9.5 from the eye

    SUBCASE("a light IN FRONT of the cube wins") {
        const Entity light = makePoint(w, Vec3{0.0F, 0.0F, 2.0F});  // eye distance 8 <= 9.5
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == light);
        CHECK(r.isPoint);
        (void)cube;
    }
    SUBCASE("a light BEHIND the cube loses to the cube") {
        const Entity light = makePoint(w, Vec3{0.0F, 0.0F, -2.0F});  // eye distance 12 > 9.5
        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, Vec3::zero())));
        CHECK(r.entity == cube);
        CHECK_FALSE(r.isPoint);
        (void)light;
    }
}

namespace {
// A TU-local component NO registerComponent call ever touches -- the positive control below.
struct NeverRegistered {
    int value = 0;
};
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};
}  // namespace

TEST_CASE("picking: pickEntity emits NO log record on any path (AC-11/INV-5)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("a populated scene") {
        World w;
        const EditorCamera camera = testCamera();
        (void)makeMesh(w, Vec3::zero());
        (void)makeMesh(w, Vec3{2.0F, 0.0F, 0.0F});
        (void)makePoint(w, Vec3{-2.0F, 0.0F, 0.0F});
        (void)pickEntity(w, camera, requestAt(Vec2::zero()));
        (void)pickEntity(w, camera, requestAt(Vec2{0.9F, -0.9F}));  // a miss, too
        scope.sink()->take(records);
        CHECK(records.empty());
    }
    SUBCASE("an empty World") {
        const World w;
        (void)pickEntity(w, testCamera(), requestAt(Vec2::zero()));
        scope.sink()->take(records);
        CHECK(records.empty());
    }
    SUBCASE("a MOVED-FROM World") {
        // std::optional + reading back through source-> is what keeps bugprone-use-after-move from
        // flagging a deliberate moved-from assertion (the scene_bounds_test.cpp precedent).
        std::optional<World> source;
        source.emplace();
        const World movedTo(std::move(*source));
        (void)pickEntity(*source, testCamera(), requestAt(Vec2::zero()));
        scope.sink()->take(records);
        CHECK(records.empty());
        (void)movedTo;
    }
    SUBCASE("ANTI-VACUITY: the sink IS listening -- each<T> over an unregistered type DOES log") {
        // Without this arm, "zero records" would be green even if the callback had never been
        // installed. It is also the F15 asymmetry in one place: has/get are silent, each<T> is not.
        World w;
        const Entity e = w.create();
        CHECK_FALSE(w.has<NeverRegistered>(e));
        scope.sink()->take(records);
        CHECK(records.empty());
        w.each<NeverRegistered>([](Entity /*entity*/, NeverRegistered& /*value*/) {});
        std::vector<engine::editor::LogEntry> afterEach;
        scope.sink()->take(afterEach);
        CHECK_FALSE(afterEach.empty());
    }
}

TEST_CASE("picking: pickSelectionAction covers all sixteen rows of the D9 table (AC-12/D9)") {
    struct ActionRow {
        bool hit;
        bool ctrlOrCmd;
        bool shift;
        PickAction expected;
    };
    constexpr std::array<ActionRow, 8> ROWS{{
        {true, false, false, PickAction::Select},
        {true, true, false, PickAction::Toggle},
        {true, false, true, PickAction::Add},
        {true, true, true, PickAction::Toggle},    // Ctrl/Cmd BEFORE Shift
        {false, false, false, PickAction::Clear},  // the universal "click empty space to deselect"
        {false, true, false, PickAction::None},    // a modifier-held MISS never clears
        {false, false, true, PickAction::None},
        {false, true, true, PickAction::None},
    }};
    for (const ActionRow& row : ROWS) {
        CAPTURE(static_cast<int>(row.expected));
        CHECK(pickSelectionAction(row.hit, false, row.ctrlOrCmd, row.shift) == row.expected);
        CHECK(pickSelectionAction(row.hit, true, row.ctrlOrCmd, row.shift) == row.expected);
    }
}

TEST_CASE("picking: applyPickAction is the one place a PickAction becomes a Selection mutation") {
    const Entity a{1, 1};
    const Entity b{2, 1};
    const Entity c{3, 1};

    SUBCASE("Select replaces the whole selection") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        applyPickAction(selection, PickAction::Select, c);
        CHECK(selection.count() == 1);
        CHECK(selection.contains(c));
        CHECK(selection.primary() == c);
    }
    SUBCASE("Toggle appends an absent entity and it becomes primary") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        applyPickAction(selection, PickAction::Toggle, c);
        CHECK(selection.contains(c));
        CHECK(selection.primary() == c);
    }
    SUBCASE("Toggle removes an already-present entity") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        const std::size_t before = selection.count();
        applyPickAction(selection, PickAction::Toggle, b);
        CHECK_FALSE(selection.contains(b));
        CHECK(selection.count() == before - 1);
    }
    SUBCASE("Add appends an absent entity and it becomes primary") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        applyPickAction(selection, PickAction::Add, c);
        CHECK(selection.contains(c));
        CHECK(selection.primary() == c);
    }
    SUBCASE("Add on an already-present entity is a no-op (D9)") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        const std::size_t countBefore = selection.count();
        const Entity primaryBefore = selection.primary();
        applyPickAction(selection, PickAction::Add, a);
        CHECK(selection.count() == countBefore);
        CHECK(selection.primary() == primaryBefore);
    }
    SUBCASE("Clear empties the selection") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        applyPickAction(selection, PickAction::Clear, c);
        CHECK(selection.empty());
    }
    SUBCASE("None touches nothing") {
        Selection selection;
        selection.set(a);
        selection.add(b);
        const std::size_t countBefore = selection.count();
        const Entity primaryBefore = selection.primary();
        const std::vector<Entity> entitiesBefore(selection.entities().begin(), selection.entities().end());
        applyPickAction(selection, PickAction::None, c);
        CHECK(selection.count() == countBefore);
        CHECK(selection.primary() == primaryBefore);
        const std::vector<Entity> entitiesAfter(selection.entities().begin(), selection.entities().end());
        CHECK(entitiesAfter == entitiesBefore);
    }
}
