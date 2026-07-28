// tests/editor/scene_bounds_test.cpp — task 2.3.1: Aabb + entityBounds/selectionBounds/sceneBounds,
// tier-0 and UNGATED. Sixth TU of aero_editor_shell_test (no DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here
// -- shell_test.cpp supplies main()). Must pass identically with AERO_REQUIRE_GPU unset and set.
//
// Case 12b's LogFixture is copied from tests/log_test.cpp's idiom (the console_model_test.cpp
// precedent) -- declared FIRST in its case so it destructs LAST, after the LogSinkScope.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

using engine::Entity;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::Aabb;
using engine::editor::entityBounds;
using engine::editor::LOCAL_MESH_HALF_EXTENT;
using engine::editor::sceneBounds;
using engine::editor::selectionBounds;

namespace {
constexpr float EPS = 1.0e-4F;
}  // namespace

TEST_CASE("scene_bounds: Aabb algebra") {
    SUBCASE("empty() is invalid") { CHECK_FALSE(Aabb::empty().valid()); }

    SUBCASE("a point box is valid, radius 0, size zero") {
        Aabb box = Aabb::empty();
        box.expand(Vec3{1.0F, 2.0F, 3.0F});
        CHECK(box.valid());
        CHECK(box.radius() == 0.0F);
        CHECK(box.size() == Vec3::zero());
        CHECK(box.center() == Vec3{1.0F, 2.0F, 3.0F});
    }

    SUBCASE("expand with an invalid box is a no-op") {
        Aabb box = Aabb::empty();
        box.expand(Vec3{1.0F, 1.0F, 1.0F});
        const Aabb before = box;
        box.expand(Aabb::empty());
        CHECK(box.min == before.min);
        CHECK(box.max == before.max);
    }

    SUBCASE("a known box") {
        const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{1.0F, 2.0F, 3.0F}};
        CHECK(box.valid());
        CHECK(box.center() == Vec3::zero());
        CHECK(box.size() == Vec3{2.0F, 4.0F, 6.0F});
        CHECK(std::abs(box.radius() - std::sqrt(56.0F) / 2.0F) < EPS);
    }

    SUBCASE("an infinite corner is invalid (C2/AC-18)") {
        const float inf = std::numeric_limits<float>::infinity();
        const Aabb box{Vec3::zero(), Vec3{inf, 0.0F, 0.0F}};
        CHECK_FALSE(box.valid());
    }
}

TEST_CASE("scene_bounds: a single MeshRenderer entity at the origin is [-0.5, 0.5]^3") {
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e) != nullptr);

    const Aabb box = entityBounds(w, e, /*includeDescendants=*/false);
    CHECK(
        engine::approxEquals(box.min, Vec3{-LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT}));
    CHECK(engine::approxEquals(box.max, Vec3{LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT}));
}

TEST_CASE("scene_bounds: translated, rotated, non-uniformly scaled") {
    World w;

    SUBCASE("45deg about Y grows X/Z, keeps Y") {
        const Entity e = w.create();
        REQUIRE(
            w.add<Transform>(e, Transform{Vec3::zero(), engine::fromAxisAngle(Vec3::unitY(), engine::radians(45.0F)),
                                          Vec3::one()}) != nullptr);
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        const Aabb box = entityBounds(w, e, false);
        const float expectedXZ = LOCAL_MESH_HALF_EXTENT * std::numbers::sqrt2_v<float>;
        CHECK(std::abs(box.max.x - expectedXZ) < EPS);
        CHECK(std::abs(box.max.z - expectedXZ) < EPS);
        CHECK(std::abs(box.max.y - LOCAL_MESH_HALF_EXTENT) < EPS);
    }

    SUBCASE("non-uniform scale {2,1,0.5}") {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e, Transform{Vec3::zero(), Quat::identity(), Vec3{2.0F, 1.0F, 0.5F}}) != nullptr);
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        const Aabb box = entityBounds(w, e, false);
        CHECK(engine::approxEquals(box.max, Vec3{1.0F, 0.5F, 0.25F}));
        CHECK(engine::approxEquals(box.min, Vec3{-1.0F, -0.5F, -0.25F}));
    }

    SUBCASE("negative scale {-1,1,1} gives the SAME box, not an inverted one (E20)") {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e, Transform{Vec3::zero(), Quat::identity(), Vec3{-1.0F, 1.0F, 1.0F}}) != nullptr);
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        const Aabb box = entityBounds(w, e, false);
        CHECK(engine::approxEquals(box.min,
                                   Vec3{-LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT}));
        CHECK(engine::approxEquals(box.max,
                                   Vec3{LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT}));
    }

    SUBCASE("zero scale collapses to a point") {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e, Transform{Vec3{5.0F, 6.0F, 7.0F}, Quat::identity(), Vec3::zero()}) != nullptr);
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        const Aabb box = entityBounds(w, e, false);
        CHECK(engine::approxEquals(box.min, Vec3{5.0F, 6.0F, 7.0F}));
        CHECK(engine::approxEquals(box.max, Vec3{5.0F, 6.0F, 7.0F}));
    }
}

TEST_CASE("scene_bounds: includeDescendants") {
    World w;
    const Entity parent = w.create();
    REQUIRE(w.add<Transform>(parent) != nullptr);
    REQUIRE(w.add<MeshRenderer>(parent) != nullptr);

    const Entity child = w.create();
    REQUIRE(w.add<Transform>(child, Transform{Vec3{10.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(child) != nullptr);
    REQUIRE(w.setParent(child, parent));

    const Aabb withDescendants = entityBounds(w, parent, true);
    CHECK(std::abs(withDescendants.min.x - (-LOCAL_MESH_HALF_EXTENT)) < EPS);
    CHECK(std::abs(withDescendants.max.x - (10.0F + LOCAL_MESH_HALF_EXTENT)) < EPS);

    const Aabb parentOnly = entityBounds(w, parent, false);
    CHECK(engine::approxEquals(parentOnly.min,
                               Vec3{-LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT, -LOCAL_MESH_HALF_EXTENT}));
    CHECK(engine::approxEquals(parentOnly.max,
                               Vec3{LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT, LOCAL_MESH_HALF_EXTENT}));

    // A two-level chain -- parent scale composition.
    const Entity grandchild = w.create();
    REQUIRE(w.add<Transform>(grandchild, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(grandchild) != nullptr);
    REQUIRE(w.setParent(grandchild, child));
    const Aabb withGrandchild = entityBounds(w, parent, true);
    CHECK(std::abs(withGrandchild.max.x - (11.0F + LOCAL_MESH_HALF_EXTENT)) < EPS);
}

TEST_CASE("scene_bounds: no MeshRenderer -> a point; no Transform -> a point at the origin") {
    World w;
    SUBCASE("no MeshRenderer (E15)") {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e, Transform{Vec3{4.0F, 5.0F, 6.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        const Aabb box = entityBounds(w, e, false);
        CHECK(engine::approxEquals(box.min, Vec3{4.0F, 5.0F, 6.0F}));
        CHECK(engine::approxEquals(box.max, Vec3{4.0F, 5.0F, 6.0F}));
    }
    SUBCASE("no Transform, and no MeshRenderer either -> a point at the origin (E19)") {
        // worldMatrix() falls back to identity when the entity carries no Transform, so its
        // "world translation" is the origin -- this is the no-MeshRenderer point contribution (E15)
        // at its untransformed extreme, not a separate code path.
        const Entity e = w.create();
        const Aabb box = entityBounds(w, e, false);
        CHECK(engine::approxEquals(box.min, Vec3::zero()));
        CHECK(engine::approxEquals(box.max, Vec3::zero()));
    }
}

TEST_CASE("scene_bounds: selectionBounds over a mixed span") {
    World w;
    const Entity a = w.create();
    REQUIRE(w.add<Transform>(a, Transform{Vec3{-5.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(a) != nullptr);
    const Entity b = w.create();
    REQUIRE(w.add<Transform>(b, Transform{Vec3{5.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(b) != nullptr);

    const Entity destroyed = w.create();
    REQUIRE(w.destroy(destroyed));

    const std::vector<Entity> mixed = {a, Entity{}, destroyed, b};
    const Aabb box = selectionBounds(w, mixed);
    CHECK(std::abs(box.min.x - (-5.0F - LOCAL_MESH_HALF_EXTENT)) < EPS);
    CHECK(std::abs(box.max.x - (5.0F + LOCAL_MESH_HALF_EXTENT)) < EPS);

    const std::vector<Entity> allDead = {Entity{}, destroyed};
    CHECK_FALSE(selectionBounds(w, allDead).valid());
}

TEST_CASE("scene_bounds: sceneBounds (case 7, incl. C2's moved-from arm)") {
    SUBCASE("a populated World matches an independent flat loop over the same entities") {
        World w;
        std::vector<Entity> all;
        for (int i = 0; i < 5; ++i) {
            const Entity e = w.create();
            REQUIRE(w.add<Transform>(e, Transform{Vec3{static_cast<float>(i) * 3.0F, 0.0F, 0.0F}, Quat::identity(),
                                                  Vec3::one()}) != nullptr);
            REQUIRE(w.add<MeshRenderer>(e) != nullptr);
            all.push_back(e);
        }
        const Aabb walked = sceneBounds(w);
        Aabb flat = Aabb::empty();
        for (const Entity e : all) {
            flat.expand(entityBounds(w, e, false));
        }
        CHECK(engine::approxEquals(walked.min, flat.min));
        CHECK(engine::approxEquals(walked.max, flat.max));
    }

    SUBCASE("an empty World -> Aabb::empty()") {
        const World w;
        CHECK_FALSE(sceneBounds(w).valid());
    }

    SUBCASE("entities but no MeshRenderer components -> Aabb::empty()") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        CHECK_FALSE(sceneBounds(w).valid());
    }

    SUBCASE("a moved-from World -> Aabb::empty() (C2 -- the ONE state where registered() fires)") {
        // Wrapped in std::optional (the shell_test.cpp/transform_test.cpp precedent): moving *source
        // rather than a bare local, and reading the moved-from state back through source-> afterward,
        // is what keeps bugprone-use-after-move from flagging a deliberate moved-from-state assertion.
        std::optional<World> source;
        source.emplace();
        const World movedTo(std::move(*source));
        CHECK_FALSE(sceneBounds(*source).valid());
        (void)movedTo;
    }
}

TEST_CASE("scene_bounds: a 200-entity chain terminates, visits each node once (walkForest contract)") {
    World w;
    constexpr int CHAIN_LENGTH = 200;
    std::vector<Entity> chain;
    chain.reserve(CHAIN_LENGTH);
    Entity previous{};
    for (int i = 0; i < CHAIN_LENGTH; ++i) {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(
                    e, Transform{Vec3{static_cast<float>(i), 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        if (previous.valid()) {
            REQUIRE(w.setParent(e, previous));
        }
        chain.push_back(e);
        previous = e;
    }

    const Aabb walked = entityBounds(w, chain.front(), /*includeDescendants=*/true);
    Aabb flat = Aabb::empty();
    for (const Entity e : chain) {
        flat.expand(entityBounds(w, e, false));
    }
    CHECK(engine::approxEquals(walked.min, flat.min));
    CHECK(engine::approxEquals(walked.max, flat.max));
}

namespace {
// case 12b: a TU-local component NO registerComponent call ever touches -- the F12 asymmetry proof
// the whole "never call each<T> here" rule rests on.
struct NeverRegistered {
    int value = 0;
};

// console_model_test.cpp:466-485's idiom, copied (not shared -- the 2.2.4/F27 rule): console=false,
// Trace floor, LogFixture declared FIRST so it destructs LAST, after the LogSinkScope.
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};
}  // namespace

TEST_CASE("scene_bounds: case 12b -- has/get are silent for an unregistered type, each<T> is not (F12)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST, after the scope below
    const engine::editor::LogSinkScope scope;

    World w;
    const Entity e = w.create();

    CHECK_FALSE(w.has<NeverRegistered>(e));
    CHECK(w.get<NeverRegistered>(e) == nullptr);
    std::vector<engine::editor::LogEntry> afterHasGet;
    scope.sink()->take(afterHasGet);
    CHECK(afterHasGet.empty());

    w.each<NeverRegistered>([](Entity /*entity*/, NeverRegistered& /*value*/) {});
    std::vector<engine::editor::LogEntry> afterEach;
    scope.sink()->take(afterEach);
    CHECK_FALSE(afterEach.empty());
}
