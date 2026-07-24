// tests/camera_test.cpp — task 1.3.3: engine::Camera, its projection/view matrices, and its
// built-in registration. Tier-0 throughout: no GPU, no reflect-gen, no generated code, no files, no
// randomness. Rides aero_tests unconditionally and passes identically with
// -DAERO_REFLECT_TOOLS=OFF — the structural proof that the component has zero codegen dependency.

#include <aero/core/math.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <type_traits>
#include <utility>

using engine::Camera;
using engine::Entity;
using engine::Mat4;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;

TEST_CASE("camera: layout") {
    static_assert(std::is_trivially_copyable_v<Camera>);
    static_assert(std::is_standard_layout_v<Camera>);
    static_assert(std::is_aggregate_v<Camera>);
    static_assert(sizeof(Camera) == 3 * sizeof(float));
}

TEST_CASE("camera: default value") {
    const Camera c{};
    CHECK(c.fovYRadians == engine::radians(60.0F));
    CHECK(c.nearPlane == 0.1F);
    CHECK(c.farPlane == 100.0F);

    CHECK(Camera{} == Camera{});
    Camera other{};
    other.farPlane = 50.0F;
    CHECK_FALSE(Camera{} == other);
}

TEST_CASE("camera: projectionMatrix matches perspective() with the given aspect (AC-2)") {
    const Camera c{};

    for (const float aspect : {16.0F / 9.0F, 1.0F, 0.5F}) {
        CAPTURE(aspect);
        const Mat4 projected = engine::projectionMatrix(c, aspect);
        const Mat4 expected = engine::perspective(c.fovYRadians, aspect, c.nearPlane, c.farPlane);
        CHECK(projected == expected);  // exact — same call, same inputs
    }

    // Aspect is genuinely applied, not a tautology: perspective() sets col0.x = f/aspect,
    // col1.y = f, so col0.x * aspect == col1.y for any aspect.
    const float aspect = 1.777F;
    const Mat4 m = engine::projectionMatrix(c, aspect);
    CHECK(engine::approxEquals(m.columns[0].x * aspect, m.columns[1].y));
}

TEST_CASE("camera: viewMatrix (AC-3)") {
    World w;

    SUBCASE("translated-only camera: the world origin maps to the camera's local origin") {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e, Transform{Vec3{3.0F, 4.0F, 5.0F}, Quat::identity(), Vec3::one()}) != nullptr);

        const Vec3 camWorldPos = engine::transformPoint(engine::worldMatrix(w, e), Vec3::zero());
        const Vec3 mapped = engine::transformPoint(engine::viewMatrix(w, e), camWorldPos);
        CHECK(engine::approxEquals(mapped, Vec3::zero()));
    }

    SUBCASE("rotated + translated: view * world is the identity") {
        const Entity e = w.create();
        const Transform t{Vec3{1.0F, 2.0F, 3.0F}, engine::fromAxisAngle(Vec3::unitY(), engine::radians(37.0F)),
                          Vec3::one()};
        REQUIRE(w.add<Transform>(e, t) != nullptr);

        CHECK(engine::approxEquals(engine::viewMatrix(w, e) * engine::worldMatrix(w, e), Mat4::identity()));
    }

    SUBCASE("dead entity -> identity, silently") {
        const Entity dead = w.create();
        REQUIRE(w.destroy(dead));
        CHECK(engine::viewMatrix(w, dead) == Mat4::identity());
    }

    SUBCASE("never-transformed entity -> identity, silently") {
        const Entity e = w.create();
        CHECK(engine::viewMatrix(w, e) == Mat4::identity());
    }

    SUBCASE("moved-from World -> identity, silently") {
        std::optional<World> mw;
        mw.emplace();
        const Entity e = mw->create();
        const World movedTo = std::move(*mw);
        CHECK(movedTo.entityCount() == 1);
        CHECK(engine::viewMatrix(*mw, e) == Mat4::identity());
    }
}

TEST_CASE("camera: registration (AC-4)") {
    World w;
    CHECK(w.findComponentType("engine::Camera").valid());
    CHECK(w.findComponentType("engine::Camera") == engine::componentTypeId<Camera>());

    const Entity e = w.create();
    auto* added = w.add<Camera>(e, Camera{engine::radians(90.0F), 0.5F, 200.0F});
    REQUIRE(added != nullptr);
    CHECK(added->fovYRadians == engine::radians(90.0F));
    CHECK(w.has<Camera>(e));

    auto* got = w.get<Camera>(e);
    REQUIRE(got != nullptr);
    got->nearPlane = 1.0F;
    CHECK(w.get<Camera>(e)->nearPlane == 1.0F);

    CHECK(w.componentCount<Camera>() == 1);
    CHECK(w.remove<Camera>(e));
    CHECK_FALSE(w.has<Camera>(e));
    CHECK(w.componentCount<Camera>() == 0);
}
