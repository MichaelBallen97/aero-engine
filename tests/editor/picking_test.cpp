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
#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <vector>

using engine::Entity;
using engine::Guid;
using engine::Mat4;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::editor::Aabb;
using engine::editor::applyPickAction;
using engine::editor::CLIP_W_EPSILON;
using engine::editor::EditorCamera;
using engine::editor::MeshBoundsKey;
using engine::editor::MeshBoundsLookup;
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

TEST_CASE("picking: projectToViewport rejects a FINITE point whose viewport mapping overflows (E4)") {
    const EditorCamera camera = testCamera();
    const Mat4 viewProj = camera.projectionMatrix(1.0F) * camera.viewMatrix();
    // 5e37 world units off-axis, one unit in front of the eye. Everything up to and INCLUDING the NDC
    // stays finite -- only ndcToViewportPoints' multiply by viewportWidth/2 overflows to +inf. That is
    // precisely why the E4 guard exists: ImDrawList consumes a +inf happily and corrupts the whole
    // frame's vertex buffer. The round-trip case above only ever feeds this function points that
    // project cleanly, so without this case the guard has no coverage at all.
    const Vec3 worldPoint{5.0e37F, 0.0F, 9.0F};
    const Vec4 clip = viewProj * engine::toVec4(worldPoint, 1.0F);

    // ANTI-VACUITY: prove that neither the input nor the near-plane test is what rejects this, so a
    // green CHECK_FALSE below can only come from the finiteness guard.
    REQUIRE(std::isfinite(worldPoint.x));
    REQUIRE(std::isfinite(clip.x));
    REQUIRE(std::isfinite(clip.w));
    REQUIRE(clip.w > CLIP_W_EPSILON);
    REQUIRE(std::isfinite(clip.x / clip.w));

    Vec2 out{12345.0F, 54321.0F};
    CHECK_FALSE(projectToViewport(viewProj, worldPoint, VIEWPORT_POINTS, out));
    CHECK(out.x == 12345.0F);  // left UNTOUCHED on a rejection, like rayLocalBoxHit's outT
    CHECK(out.y == 54321.0F);
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

// ---- task 3.1.5: the Aabb overload (PK1-PK7) ----------------------------------------------------
// The min/max generalisation, and the one every real mesh uses. Its whole reason for existing is that
// a ZERO-THICKNESS axis is LEGAL -- the flat Plane primitive's own shape -- so the precondition is
// min <= max, never min < max. Written BEFORE the overload existed, which is what retires V5's
// "unproven on a degenerate box" note: PK6 and PK7 are the degenerate cases, and they were the first
// thing typed.

TEST_CASE("picking: the Aabb overload's slab battery (PK1-PK5)") {
    const Aabb box{Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};

    SUBCASE("PK1 axis-aligned entry hit") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, box, t));
        CHECK(std::abs(t - 9.5F) < EPS);
    }
    SUBCASE("PK2 aimed away is a miss, outT untouched") {
        float t = 12345.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, 1.0F}, box, t));
        CHECK(t == 12345.0F);
    }
    SUBCASE("PK3 entry hits ONLY -- an origin inside the box is a miss") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, box, t));
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.25F, 0.25F, 0.25F}, engine::normalize(Vec3{1.0F, 1.0F, 1.0F}), box, t));
    }
    SUBCASE("PK4 parallel and OUTSIDE a slab") {
        float t = 0.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{2.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, box, t));
    }
    SUBCASE("PK5 parallel and INSIDE a slab constrains nothing") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.25F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, box, t));
        CHECK(std::abs(t - 9.5F) < EPS);
    }
}

TEST_CASE("picking: a ZERO-THICKNESS axis is legal and hittable from above (PK6, retiring V5)") {
    // The flat Plane primitive's own box. A precondition of min < max instead of min <= max rejects
    // this box outright and makes every plane in every scene unclickable -- which is S33b.
    const Aabb flat{Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, 0.5F}};
    CHECK(flat.valid());

    SUBCASE("straight down onto the middle") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.0F, 4.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, flat, t));
        CHECK(std::abs(t - 4.0F) < EPS);
    }
    SUBCASE("straight up from below") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{0.1F, -3.0F, -0.2F}, Vec3{0.0F, 1.0F, 0.0F}, flat, t));
        CHECK(std::abs(t - 3.0F) < EPS);
    }
    SUBCASE("obliquely from above, landing inside") {
        float t = 0.0F;
        CHECK(rayLocalBoxHit(Vec3{-0.4F, 1.0F, 0.0F}, engine::normalize(Vec3{0.4F, -1.0F, 0.0F}), flat, t));
    }
    SUBCASE("down, but OUTSIDE the quad") {
        float t = 12345.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{3.0F, 4.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, flat, t));
        CHECK(t == 12345.0F);
    }
}

TEST_CASE("picking: a ray exactly EDGE-ON to the flat plane is total (PK7)") {
    const Aabb flat{Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, 0.5F}};
    float t = 0.0F;

    SUBCASE("in the plane's own y, aimed along -Z: parallel to the zero-thickness slab AND inside it") {
        // The 0 * inf -> NaN path the |d| < DIR_EPSILON branch exists to keep out. y == 0 is exactly on
        // both slab planes at once, which is the input a strict min < max ladder cannot express at all.
        CHECK(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, flat, t));
        CHECK(std::abs(t - 9.5F) < EPS);
    }
    SUBCASE("edge-on but OFFSET in y: parallel and outside") {
        float miss = 12345.0F;
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.25F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, flat, miss));
        CHECK(miss == 12345.0F);
    }
    SUBCASE("an origin ON the quad is an origin INSIDE the box -- a miss, not a hit at 0") {
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, flat, t));
    }
}

// The case above drives rayLocalBoxHit with RAY-level arguments only. AC-7's other two clauses -- a
// zero scale (a singular model matrix) and a non-finite Transform -- are properties of an ENTITY, and
// nothing else in this file builds a degenerate one and hands it to pickEntity. Without this case the
// E5 asymmetry has only one of its two halves proved: selection_overlay_test.cpp asserts that a
// zero-scaled entity still DRAWS its 12 segments, while "you cannot CLICK a zero-volume object" is
// asserted nowhere.
TEST_CASE("picking: a degenerate or non-finite ENTITY is never picked (AC-7/E5)") {
    const EditorCamera camera = testCamera();
    // Every subcase clicks the centre, ndc {0,0}, where a SANE entity at the world origin is picked --
    // the first subcase proves exactly that. So a CHECK_FALSE(hit()) below reports the degeneracy and
    // not a badly aimed ray.

    SUBCASE("ANTI-VACUITY: the same entity with a SANE transform IS picked") {
        World w;
        const Entity sane = makeMesh(w, Vec3::zero());
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK(r.entity == sane);
        CHECK(std::abs(r.distance - 9.5F) < 1.0e-3F);
    }
    SUBCASE("zero scale on every axis -- a singular model matrix") {
        World w;
        (void)makeMesh(w, Vec3::zero(), Quat::identity(), Vec3::zero());
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK_FALSE(r.hit());
        CHECK(std::isfinite(r.distance));
    }
    SUBCASE("zero scale on ONE axis -- singular too, and geometrically ON the ray") {
        // The collapsed box is a unit quad at local z = 0 that the -Z ray passes straight through, so
        // this is not a miss for want of geometry: it is a miss because zero VOLUME is not pickable.
        World w;
        (void)makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{1.0F, 1.0F, 0.0F});
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK_FALSE(r.hit());
        CHECK(std::isfinite(r.distance));
    }
    SUBCASE("a non-finite position") {
        World w;
        (void)makeMesh(w, Vec3{INF_F, 0.0F, 0.0F});
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK_FALSE(r.hit());
        CHECK(std::isfinite(r.distance));
    }
    SUBCASE("a non-finite scale") {
        World w;
        (void)makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{QUIET_NAN, 1.0F, 1.0F});
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK_FALSE(r.hit());
        CHECK(std::isfinite(r.distance));
    }
    SUBCASE("a TINY but non-degenerate entity stays pickable") {
        // The other half of the determinant contract, and the half that regresses silently: the guard
        // rejects zero VOLUME, never small OBJECTS. A uniform 1e-4 scale has det 1e-12 -- eight orders
        // of magnitude above DETERMINANT_EPSILON -- and must still be clickable. Raising that epsilon
        // "to be safe" would make small objects quietly unselectable in the editor, and reddens here.
        World w;
        const Entity tiny = makeMesh(w, Vec3::zero(), Quat::identity(), Vec3{1.0e-4F, 1.0e-4F, 1.0e-4F});
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK(r.entity == tiny);
        CHECK_FALSE(r.isPoint);
        CHECK(std::abs(r.distance - 10.0F) < 1.0e-3F);  // the eye is 10 away; the box is 1e-4 across
    }
    SUBCASE("a degenerate entity NEARER than a sane one never shadows it") {
        // The walk must SKIP the degenerate candidate and keep going, not abandon the click.
        World w;
        (void)makeMesh(w, Vec3{0.0F, 0.0F, 4.0F}, Quat::identity(), Vec3::zero());
        const Entity sane = makeMesh(w, Vec3::zero());
        const PickResult r = pickEntity(w, camera, requestAt(Vec2::zero()));
        CHECK(r.entity == sane);
        CHECK(std::abs(r.distance - 9.5F) < 1.0e-3F);
    }
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
    SUBCASE("equal screen distance ties break on the LOWEST index, whatever eachEntity's order") {
        // Both candidates sit at the EXACT SAME world point as the click itself, which makes their
        // screenDistance BIT-IDENTICAL (both zero) by construction -- a floating round trip through
        // the projection matrix could never be trusted to reproduce an exact tie, but two identical
        // calls with identical inputs are deterministic under IEEE arithmetic.
        //
        // CREATION ORDER ALONE CANNOT EXERCISE THE TIE-BREAK. eachEntity walks creation order, which
        // is also index order, so the lower index is visited FIRST and wins on the plain
        // `screenDistance < bestScreenDistance` arm -- `e.index < point.entity.index` is never the
        // decider and deleting it changes nothing. Recycling a destroyed slot inverts the visit
        // order, exactly as case 8 above does for the MESH tie-break: destroy() swaps the erased
        // entity with the last in-use element, so [a,b,c] - destroy(b) -> [a,c,b], and the next
        // create() recycles b's index with a bumped generation.
        const Vec3 clickWorld{2.0F, 0.0F, 0.0F};
        const Entity decoy = makeMesh(w, Vec3{0.0F, 0.0F, -50.0F});  // index 0; this ray misses it
        const Entity doomed = w.create();                            // index 1
        const Entity high = makePoint(w, clickWorld);                // index 2
        REQUIRE(w.destroy(doomed));
        const Entity low = makePoint(w, clickWorld);  // index 1 recycled, generation 2

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

        const PickResult r = pickEntity(w, camera, requestAt(ndcOf(camera, 1.0F, clickWorld)));
        CHECK(r.entity == low);  // dropping the index tie-break returns `high` and reddens here
        CHECK(low.index < high.index);
        (void)decoy;
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

// ================================================================================================
// task 3.1.5 (PK8-PK12): the reference pick arm, the invalid-box refusal, the two overloads agreeing,
// and the drop placement helper.
// ================================================================================================

namespace {
[[nodiscard]] Guid pickMeshGuid(std::uint64_t ordinal) { return Guid{ordinal, 0xC0FFEEULL}; }

// An entity holding a REFERENCE rather than a primitive selector.
[[nodiscard]] Entity makeReferenced(World& world, Vec3 position, Guid mesh, std::uint32_t meshIndex) {
    const Entity e = world.create();
    world.add<Transform>(e, Transform{.position = position});
    world.add<MeshRenderer>(e, MeshRenderer{.mesh = mesh, .meshIndex = meshIndex});
    return e;
}
}  // namespace

TEST_CASE("picking: a LARGE mesh is picked where the old half-unit box missed (PK8)") {
    // S31's witness: a localBoundsFor that answered primitiveLocalBounds(0) on the reference arm makes
    // this click a miss. The click lands 3 units off the entity's origin, well outside [-0.5, 0.5]^3
    // and well inside the referenced box.
    const EditorCamera camera = testCamera();
    constexpr float ASPECT = 4.0F / 3.0F;
    World world;
    const Entity e = makeReferenced(world, Vec3::zero(), pickMeshGuid(1), 0);

    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{pickMeshGuid(1), 0}, Aabb{Vec3{-4.0F, -4.0F, -1.0F}, Vec3{4.0F, 4.0F, 1.0F}});

    const PickRequest request{.ndc = ndcOf(camera, ASPECT, Vec3{3.0F, 0.0F, 0.0F}),
                              .aspect = ASPECT,
                              .viewportSizePoints = VIEWPORT_POINTS,
                              .meshBounds = &lookup};

    SUBCASE("with the lookup published, the big box is hit") {
        const PickResult result = pickEntity(world, camera, request);
        REQUIRE(result.hit());
        CHECK(result.entity == e);
        CHECK_FALSE(result.isPoint);
    }
    SUBCASE("the SAME click against the cube's box would be a mesh miss") {
        // The control: the identical entity as a PRIMITIVE, whose box really is [-0.5, 0.5]^3.
        World primitiveWorld;
        makeMesh(primitiveWorld, Vec3::zero());
        const PickResult result = pickEntity(primitiveWorld, camera, request);
        CHECK_FALSE(result.hit());
    }
}

TEST_CASE("picking: an UNRESOLVED reference falls through to the screen disc (PK9)") {
    // AC-34: an entity mid-load stays selectable. The mesh arm must NOT return unconditionally, or a
    // dropped model is unclickable for the whole of its load and the user cannot undo it by selecting
    // it.
    const EditorCamera camera = testCamera();
    constexpr float ASPECT = 4.0F / 3.0F;
    World world;
    const Entity loading = makeReferenced(world, Vec3::zero(), pickMeshGuid(1), 0);

    const PickRequest request{
        .ndc = ndcOf(camera, ASPECT, Vec3::zero()), .aspect = ASPECT, .viewportSizePoints = VIEWPORT_POINTS};

    SUBCASE("no lookup at all") {
        const PickResult result = pickEntity(world, camera, request);
        REQUIRE(result.hit());
        CHECK(result.entity == loading);
        CHECK(result.isPoint);  // the DISC, exactly as a light is picked
    }
    SUBCASE("a lookup that does not hold this guid") {
        MeshBoundsLookup lookup;
        lookup.set(MeshBoundsKey{pickMeshGuid(9), 0}, Aabb{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}});
        PickRequest withLookup = request;
        withLookup.meshBounds = &lookup;
        const PickResult result = pickEntity(world, camera, withLookup);
        REQUIRE(result.hit());
        CHECK(result.entity == loading);
        CHECK(result.isPoint);
    }
    SUBCASE("...and a click well outside the disc still misses, so the disc is a DISC") {
        const PickRequest far{.ndc = offsetNdcByPoints(request.ndc, Vec2{120.0F, 0.0F}, VIEWPORT_POINTS),
                              .aspect = ASPECT,
                              .viewportSizePoints = VIEWPORT_POINTS};
        CHECK_FALSE(pickEntity(world, camera, far).hit());
    }
    SUBCASE("once the box IS published, the same entity is picked as a MESH") {
        MeshBoundsLookup lookup;
        lookup.set(MeshBoundsKey{pickMeshGuid(1), 0}, Aabb{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}});
        PickRequest withLookup = request;
        withLookup.meshBounds = &lookup;
        const PickResult result = pickEntity(world, camera, withLookup);
        REQUIRE(result.hit());
        CHECK(result.entity == loading);
        CHECK_FALSE(result.isPoint);
    }
}

TEST_CASE("picking: an INVALID box is a miss and leaves outT untouched (PK10)") {
    float t = 12345.0F;
    SUBCASE("min > max on one axis") {
        const Aabb inverted{Vec3{1.0F, -0.5F, -0.5F}, Vec3{-1.0F, 0.5F, 0.5F}};
        CHECK_FALSE(inverted.valid());
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, inverted, t));
    }
    SUBCASE("Aabb::empty()'s inverted sentinel") {
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, Aabb::empty(), t));
    }
    SUBCASE("a non-finite corner") {
        const Aabb infinite{Vec3{-INF_F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, infinite, t));
        const Aabb nanBox{Vec3{QUIET_NAN, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};
        CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, nanBox, t));
    }
    CHECK(t == 12345.0F);
}

TEST_CASE("picking: the half-extent overload DELEGATES to the Aabb one, answer for answer (PK11)") {
    // The half-extent overload's own published battery above is UNMOVED -- that is the first half of
    // AC-32. This is the second: the two forms cannot drift, because there is only one slab ladder.
    const std::array<Vec3, 6> origins{Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -10.0F}, Vec3{0.0F, 0.0F, 0.0F},
                                      Vec3{2.0F, 0.0F, 10.0F}, Vec3{0.25F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, 0.5F}};
    const std::array<Vec3, 3> directions{Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 0.0F, 1.0F},
                                         engine::normalize(Vec3{1.0F, 1.0F, 1.0F})};
    for (const float h : {0.5F, 1.0F, 3.0F}) {
        const Aabb box{Vec3{-h, -h, -h}, Vec3{h, h, h}};
        for (const Vec3 origin : origins) {
            for (const Vec3 direction : directions) {
                float viaExtent = -1.0F;
                float viaBox = -2.0F;
                const bool hitExtent = rayLocalBoxHit(origin, direction, h, viaExtent);
                const bool hitBox = rayLocalBoxHit(origin, direction, box, viaBox);
                CHECK(hitExtent == hitBox);
                if (hitExtent) {
                    CHECK(viaExtent == viaBox);
                }
            }
        }
    }
    // ...and the half-extent form keeps its STRICTER precondition: h <= 0 is refused before a box
    // exists, where the Aabb form would happily accept the point box {0,0,0}..{0,0,0}.
    float t = 0.0F;
    CHECK_FALSE(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.0F, t));
    CHECK(rayLocalBoxHit(Vec3{0.0F, 0.0F, 10.0F}, Vec3{0.0F, 0.0F, -1.0F}, Aabb{Vec3::zero(), Vec3::zero()}, t));
}

TEST_CASE("picking: dropPlacementPoint's four arms, and it is TOTAL (PK12)") {
    using engine::editor::DROP_FALLBACK_DISTANCE;
    using engine::editor::dropPlacementPoint;

    SUBCASE("arm 4: a downward ray meets y = 0 at the expected point") {
        const Ray ray{.origin = Vec3{2.0F, 10.0F, -3.0F}, .direction = engine::normalize(Vec3{0.0F, -1.0F, 0.0F})};
        const Vec3 point = dropPlacementPoint(ray);
        CHECK(std::abs(point.y) < EPS);
        CHECK(std::abs(point.x - 2.0F) < EPS);
        CHECK(std::abs(point.z - (-3.0F)) < EPS);
    }
    SUBCASE("arm 2: PARALLEL to the ground plane -> the fixed fallback distance") {
        const Ray ray{.origin = Vec3{0.0F, 5.0F, 0.0F}, .direction = Vec3{0.0F, 0.0F, -1.0F}};
        const Vec3 point = dropPlacementPoint(ray);
        CHECK(std::abs(point.y - 5.0F) < EPS);  // still at the eye's height -- the plane is never met
        CHECK(std::abs(point.z - (-DROP_FALLBACK_DISTANCE)) < EPS);
    }
    SUBCASE("arm 3: the plane is BEHIND the eye -> the same fallback, never a negative t") {
        // Looking UP from above the plane: t = -10/+1 = -10, which arm 3 rejects. A body that returned
        // origin + dir * t unconditionally (S30) lands 10 units BEHIND the camera, under the floor.
        const Ray ray{.origin = Vec3{0.0F, 10.0F, 0.0F}, .direction = Vec3{0.0F, 1.0F, 0.0F}};
        const Vec3 point = dropPlacementPoint(ray);
        CHECK(std::abs(point.y - (10.0F + DROP_FALLBACK_DISTANCE)) < EPS);
        CHECK(point.y > ray.origin.y);  // IN FRONT of the eye, which the rejected t is not
    }
    SUBCASE("arm 1: an unbuildable (zero) direction -> the ray origin") {
        const Ray ray{.origin = Vec3{7.0F, 8.0F, 9.0F}, .direction = Vec3::zero()};
        CHECK(dropPlacementPoint(ray) == Vec3{7.0F, 8.0F, 9.0F});
    }
    SUBCASE("arm 4's finiteness re-check: a finite t can still produce an infinite point") {
        const Ray ray{.origin = Vec3{0.0F, 1.0e30F, 0.0F}, .direction = engine::normalize(Vec3{1.0F, -1.0e-20F, 0.0F})};
        const Vec3 point = dropPlacementPoint(ray);
        CHECK(std::isfinite(point.x));
        CHECK(std::isfinite(point.y));
        CHECK(std::isfinite(point.z));
    }
    SUBCASE("every result is FINITE, including from the four NDC corners of a real viewport") {
        const EditorCamera camera = testCamera();
        constexpr float ASPECT = 4.0F / 3.0F;
        for (const Vec2 ndc :
             {Vec2{-1.0F, -1.0F}, Vec2{1.0F, -1.0F}, Vec2{-1.0F, 1.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F}}) {
            const Vec3 point = dropPlacementPoint(viewportRay(camera, ndc, ASPECT));
            CHECK(std::isfinite(point.x));
            CHECK(std::isfinite(point.y));
            CHECK(std::isfinite(point.z));
        }
    }
}
