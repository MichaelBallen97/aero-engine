// tests/editor/scene_bounds_test.cpp — task 2.3.1: Aabb + entityBounds/selectionBounds/sceneBounds,
// tier-0 and UNGATED. Sixth TU of aero_editor_shell_test (no DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here
// -- shell_test.cpp supplies main()). Must pass identically with AERO_REQUIRE_GPU unset and set.
//
// Case 12b's LogFixture is copied from tests/log_test.cpp's idiom (the console_model_test.cpp
// precedent) -- declared FIRST in its case so it destructs LAST, after the LogSinkScope.
#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

using engine::Entity;
using engine::Guid;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::Aabb;
using engine::editor::aabbCorner;
using engine::editor::entityBounds;
using engine::editor::localBoundsFor;
using engine::editor::MeshBoundsKey;
using engine::editor::MeshBoundsLookup;
using engine::editor::primitiveLocalBounds;
using engine::editor::sceneBounds;
using engine::editor::selectionBounds;

namespace {
constexpr float EPS = 1.0e-4F;
// Every case in this file builds a DEFAULT MeshRenderer, i.e. primitive 0 == Cube, whose local box is
// [-0.5, 0.5]^3. Pinned as a LITERAL here rather than read back from primitiveLocalBounds: a case that
// compares the walk's output against the very function the walk calls cannot see them agree on a wrong
// value. LB1 is the case that pins primitiveLocalBounds(0) itself, against this same literal.
constexpr float CUBE_HALF = 0.5F;
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
    CHECK(engine::approxEquals(box.min, Vec3{-CUBE_HALF, -CUBE_HALF, -CUBE_HALF}));
    CHECK(engine::approxEquals(box.max, Vec3{CUBE_HALF, CUBE_HALF, CUBE_HALF}));
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
        const float expectedXZ = CUBE_HALF * std::numbers::sqrt2_v<float>;
        CHECK(std::abs(box.max.x - expectedXZ) < EPS);
        CHECK(std::abs(box.max.z - expectedXZ) < EPS);
        CHECK(std::abs(box.max.y - CUBE_HALF) < EPS);
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
        CHECK(engine::approxEquals(box.min, Vec3{-CUBE_HALF, -CUBE_HALF, -CUBE_HALF}));
        CHECK(engine::approxEquals(box.max, Vec3{CUBE_HALF, CUBE_HALF, CUBE_HALF}));
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
    CHECK(std::abs(withDescendants.min.x - (-CUBE_HALF)) < EPS);
    CHECK(std::abs(withDescendants.max.x - (10.0F + CUBE_HALF)) < EPS);

    const Aabb parentOnly = entityBounds(w, parent, false);
    CHECK(engine::approxEquals(parentOnly.min, Vec3{-CUBE_HALF, -CUBE_HALF, -CUBE_HALF}));
    CHECK(engine::approxEquals(parentOnly.max, Vec3{CUBE_HALF, CUBE_HALF, CUBE_HALF}));

    // A two-level chain -- parent scale composition.
    const Entity grandchild = w.create();
    REQUIRE(w.add<Transform>(grandchild, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(grandchild) != nullptr);
    REQUIRE(w.setParent(grandchild, child));
    const Aabb withGrandchild = entityBounds(w, parent, true);
    CHECK(std::abs(withGrandchild.max.x - (11.0F + CUBE_HALF)) < EPS);
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
    CHECK(std::abs(box.min.x - (-5.0F - CUBE_HALF)) < EPS);
    CHECK(std::abs(box.max.x - (5.0F + CUBE_HALF)) < EPS);

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

// ================================================================================================
// task 3.1.5 (LB1-LB12): primitiveLocalBounds, aabbCorner, MeshBoundsLookup, localBoundsFor, and the
// defaulted lookup threaded through the three walks. Task 2.3.1's single knowingly-wrong half-extent
// constant is DELETED, not deprecated; these are what replaced it.
// ================================================================================================

namespace {
[[nodiscard]] Guid meshGuid(std::uint64_t ordinal) { return Guid{ordinal, 0xB0DEULL}; }
}  // namespace

TEST_CASE("scene_bounds: the cube's local box is [-0.5, 0.5]^3 (LB1)") {
    const Aabb cube = primitiveLocalBounds(0);
    CHECK(cube.valid());
    CHECK(engine::approxEquals(cube.min, Vec3{-0.5F, -0.5F, -0.5F}));
    CHECK(engine::approxEquals(cube.max, Vec3{0.5F, 0.5F, 0.5F}));
}

TEST_CASE("scene_bounds: the sphere's local box is the cube's (LB2)") {
    // The catalog's sphere RADIUS is 0.5, deliberately matching the cube's extent -- so this is an
    // equality about the catalog, not a copy-paste.
    const Aabb sphere = primitiveLocalBounds(1);
    CHECK(engine::approxEquals(sphere.min, Vec3{-0.5F, -0.5F, -0.5F}));
    CHECK(engine::approxEquals(sphere.max, Vec3{0.5F, 0.5F, 0.5F}));
}

TEST_CASE("scene_bounds: the PLANE's local box is FLAT (LB3)") {
    // The retirement of task 2.3.2's knowingly-fat plane pick box. A box that is 0.5 thick in Y here is
    // S33, and this is the only case that can see it.
    const Aabb plane = primitiveLocalBounds(2);
    CHECK(plane.valid());  // min <= max, never min < max -- a flat box is a LEGAL box
    CHECK(plane.min.y == 0.0F);
    CHECK(plane.max.y == 0.0F);
    CHECK(engine::approxEquals(plane.min, Vec3{-0.5F, 0.0F, -0.5F}));
    CHECK(engine::approxEquals(plane.max, Vec3{0.5F, 0.0F, 0.5F}));
    CHECK(plane.size() == Vec3{1.0F, 0.0F, 1.0F});
}

TEST_CASE("scene_bounds: an out-of-range primitive clamps to the CUBE box (LB4)") {
    // clampPrimitive clamps to Cube, so this does too -- never to Plane, and never wrapping to
    // primitive % 3, which for 7 would answer the plane's flat box.
    const Aabb cube = primitiveLocalBounds(0);
    for (const std::uint32_t primitive : {3U, 7U, 99U, 0xFFFFFFFFU}) {
        const Aabb box = primitiveLocalBounds(primitive);
        CHECK(box.min == cube.min);
        CHECK(box.max == cube.max);
    }
}

TEST_CASE("scene_bounds: localBoundsFor's PRIMITIVE arm is always engaged (LB5)") {
    // A nil `mesh` never consults the lookup at all, so the answer is the same with and without one.
    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, Aabb{Vec3{-9.0F, -9.0F, -9.0F}, Vec3{9.0F, 9.0F, 9.0F}});

    for (const std::uint32_t primitive : {0U, 1U, 2U, 42U}) {
        const MeshRenderer renderer{.primitive = primitive};
        const std::optional<Aabb> withoutLookup = localBoundsFor(renderer, nullptr);
        const std::optional<Aabb> withLookup = localBoundsFor(renderer, &lookup);
        REQUIRE(withoutLookup.has_value());
        REQUIRE(withLookup.has_value());
        CHECK(withoutLookup->min == primitiveLocalBounds(primitive).min);
        CHECK(withoutLookup->max == primitiveLocalBounds(primitive).max);
        CHECK(withLookup->min == withoutLookup->min);
        CHECK(withLookup->max == withoutLookup->max);
    }
}

TEST_CASE("scene_bounds: localBoundsFor's REFERENCE arm answers the looked-up box (LB6)") {
    const Aabb big{Vec3{-4.0F, -1.0F, -2.0F}, Vec3{6.0F, 3.0F, 2.0F}};
    const Aabb other{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}};
    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, big);
    lookup.set(MeshBoundsKey{meshGuid(1), 1}, other);
    REQUIRE(lookup.size() == 2);

    SUBCASE("meshIndex SELECTS, it is not decoration") {
        const std::optional<Aabb> zero = localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}, &lookup);
        const std::optional<Aabb> one = localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 1}, &lookup);
        REQUIRE(zero.has_value());
        REQUIRE(one.has_value());
        CHECK(zero->min == big.min);
        CHECK(zero->max == big.max);
        CHECK(one->min == other.min);
        CHECK(one->max == other.max);
        // ...and it is NOT the primitive box, which is what S31 (returning primitiveLocalBounds(0) on
        // this arm) would produce.
        CHECK(zero->max != primitiveLocalBounds(0).max);
    }
    SUBCASE("set on an existing key REPLACES") {
        lookup.set(MeshBoundsKey{meshGuid(1), 0}, other);
        CHECK(lookup.size() == 2);
        const std::optional<Aabb> box = localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}, &lookup);
        REQUIRE(box.has_value());
        CHECK(box->max == other.max);
    }
    SUBCASE("removeMesh takes EVERY meshIndex of one guid, and only that guid") {
        lookup.set(MeshBoundsKey{meshGuid(2), 0}, other);
        lookup.set(MeshBoundsKey{meshGuid(1), 5}, other);
        REQUIRE(lookup.size() == 4);
        lookup.removeMesh(meshGuid(1));
        CHECK(lookup.size() == 1);
        CHECK(lookup.find(MeshBoundsKey{meshGuid(1), 0}) == nullptr);
        CHECK(lookup.find(MeshBoundsKey{meshGuid(1), 1}) == nullptr);
        CHECK(lookup.find(MeshBoundsKey{meshGuid(1), 5}) == nullptr);
        CHECK(lookup.find(MeshBoundsKey{meshGuid(2), 0}) != nullptr);
    }
    SUBCASE("clear empties it") {
        lookup.clear();
        CHECK(lookup.size() == 0);
        CHECK_FALSE(localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}, &lookup).has_value());
    }
    SUBCASE("descending insertion still finds every key") {
        MeshBoundsLookup many;
        for (std::uint64_t ordinal = 8; ordinal >= 1; --ordinal) {
            many.set(MeshBoundsKey{meshGuid(ordinal), 0}, big);
        }
        REQUIRE(many.size() == 8);
        for (std::uint64_t ordinal = 1; ordinal <= 8; ++ordinal) {
            CHECK(many.find(MeshBoundsKey{meshGuid(ordinal), 0}) != nullptr);
        }
    }
}

TEST_CASE("scene_bounds: localBoundsFor answers NULLOPT for every unresolved cause (LB7)") {
    const Aabb box{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}};
    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, box);

    SUBCASE("a valid reference with NO lookup published yet") {
        CHECK_FALSE(localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}, nullptr).has_value());
    }
    SUBCASE("a guid the lookup does not hold") {
        CHECK_FALSE(localBoundsFor(MeshRenderer{.mesh = meshGuid(9), .meshIndex = 0}, &lookup).has_value());
    }
    SUBCASE("a STALE meshIndex on a guid it does hold") {
        CHECK_FALSE(localBoundsFor(MeshRenderer{.mesh = meshGuid(1), .meshIndex = 3}, &lookup).has_value());
    }
    SUBCASE("a NIL guid never reaches find, and find refuses one anyway") {
        // The primitive arm catches a nil `mesh` first, so this is defence in depth WITH a case:
        // MeshBoundsLookup::find answers nullptr for a nil guid whoever asks.
        CHECK(lookup.find(MeshBoundsKey{Guid{}, 0}) == nullptr);
        MeshBoundsLookup nilKeyed;
        nilKeyed.set(MeshBoundsKey{Guid{}, 0}, box);
        CHECK(nilKeyed.size() == 0);  // a nil guid is the NONE sentinel, never a key
    }
}

TEST_CASE("scene_bounds: an UNRESOLVED reference contributes a POINT at its world translation (LB8)") {
    // AC-34's bounds half: an entity mid-load is framed at its origin, exactly as an entity with no
    // MeshRenderer at all is -- not as a phantom unit cube, and not as nothing.
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{Vec3{4.0F, 5.0F, 6.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}) != nullptr);

    const Aabb box = entityBounds(w, e, /*includeDescendants=*/false);
    CHECK(box.valid());
    CHECK(engine::approxEquals(box.min, Vec3{4.0F, 5.0F, 6.0F}));
    CHECK(engine::approxEquals(box.max, Vec3{4.0F, 5.0F, 6.0F}));

    // ...and once the box IS published, the same entity frames its real extent.
    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, Aabb{Vec3{-2.0F, -2.0F, -2.0F}, Vec3{2.0F, 2.0F, 2.0F}});
    const Aabb resolved = entityBounds(w, e, false, &lookup);
    CHECK(engine::approxEquals(resolved.min, Vec3{2.0F, 3.0F, 4.0F}));
    CHECK(engine::approxEquals(resolved.max, Vec3{6.0F, 7.0F, 8.0F}));
}

TEST_CASE("scene_bounds: all three walks take the defaulted lookup and are unchanged by a null one (LB9)") {
    World w;
    const Entity a = w.create();
    REQUIRE(w.add<Transform>(a, Transform{Vec3{-3.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(a) != nullptr);
    const Entity b = w.create();
    REQUIRE(w.add<Transform>(b, Transform{Vec3{3.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(b, MeshRenderer{.primitive = 1}) != nullptr);
    const std::vector<Entity> both{a, b};

    const Aabb entityDefaulted = entityBounds(w, a, false);
    const Aabb entityExplicit = entityBounds(w, a, false, nullptr);
    CHECK(entityDefaulted.min == entityExplicit.min);
    CHECK(entityDefaulted.max == entityExplicit.max);

    const Aabb selectionDefaulted = selectionBounds(w, both);
    const Aabb selectionExplicit = selectionBounds(w, both, nullptr);
    CHECK(selectionDefaulted.min == selectionExplicit.min);
    CHECK(selectionDefaulted.max == selectionExplicit.max);

    const Aabb sceneDefaulted = sceneBounds(w);
    const Aabb sceneExplicit = sceneBounds(w, nullptr);
    CHECK(sceneDefaulted.min == sceneExplicit.min);
    CHECK(sceneDefaulted.max == sceneExplicit.max);

    // ...and the values are the pre-3.1.5 ones: two cubes at +/-3.
    CHECK(std::abs(sceneDefaulted.min.x - (-3.0F - CUBE_HALF)) < EPS);
    CHECK(std::abs(sceneDefaulted.max.x - (3.0F + CUBE_HALF)) < EPS);
}

TEST_CASE("scene_bounds: aabbCorner enumerates all 8 corners, bit 0 = X, bit 1 = Y, bit 2 = Z (LB10)") {
    const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{10.0F, 20.0F, 30.0F}};
    CHECK(aabbCorner(box, 0) == Vec3{-1.0F, -2.0F, -3.0F});
    CHECK(aabbCorner(box, 1) == Vec3{10.0F, -2.0F, -3.0F});
    CHECK(aabbCorner(box, 2) == Vec3{-1.0F, 20.0F, -3.0F});
    CHECK(aabbCorner(box, 3) == Vec3{10.0F, 20.0F, -3.0F});
    CHECK(aabbCorner(box, 4) == Vec3{-1.0F, -2.0F, 30.0F});
    CHECK(aabbCorner(box, 5) == Vec3{10.0F, -2.0F, 30.0F});
    CHECK(aabbCorner(box, 6) == Vec3{-1.0F, 20.0F, 30.0F});
    CHECK(aabbCorner(box, 7) == Vec3{10.0F, 20.0F, 30.0F});
    // The 8 corners are DISTINCT and their union is exactly the box.
    Aabb rebuilt = Aabb::empty();
    for (std::size_t i = 0; i < 8; ++i) {
        rebuilt.expand(aabbCorner(box, i));
    }
    CHECK(rebuilt.min == box.min);
    CHECK(rebuilt.max == box.max);
    // A flat box's corners collapse in pairs but stay finite -- the plane's own case.
    const Aabb flat = primitiveLocalBounds(2);
    CHECK(aabbCorner(flat, 0).y == 0.0F);
    CHECK(aabbCorner(flat, 2).y == 0.0F);
    CHECK(aabbCorner(flat, 0) == aabbCorner(flat, 2));
}

TEST_CASE("scene_bounds: a referenced box rides through a non-uniform scale and a rotation (LB11)") {
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3{2.0F, 1.0F, 0.5F}}) !=
            nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}) != nullptr);

    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, Aabb{Vec3{-3.0F, -1.0F, -4.0F}, Vec3{3.0F, 1.0F, 4.0F}});

    const Aabb box = entityBounds(w, e, false, &lookup);
    CHECK(engine::approxEquals(box.min, Vec3{1.0F - 6.0F, -1.0F, -2.0F}));
    CHECK(engine::approxEquals(box.max, Vec3{1.0F + 6.0F, 1.0F, 2.0F}));

    SUBCASE("45deg about Y grows X/Z from the REFERENCED extents, not the cube's") {
        auto* const transform = w.get<Transform>(e);
        REQUIRE(transform != nullptr);
        transform->position = Vec3::zero();
        transform->rotation = engine::fromAxisAngle(Vec3::unitY(), engine::radians(45.0F));
        transform->scale = Vec3::one();
        const Aabb rotated = entityBounds(w, e, false, &lookup);
        const float expected = (3.0F + 4.0F) * std::numbers::sqrt2_v<float> / 2.0F;
        CHECK(std::abs(rotated.max.x - expected) < EPS);
        CHECK(std::abs(rotated.max.z - expected) < EPS);
        CHECK(std::abs(rotated.max.y - 1.0F) < EPS);
    }
}

TEST_CASE("scene_bounds: sceneBounds over a mixed world -- primitives, references, and neither (LB12)") {
    World w;
    // a cube at -10
    const Entity cube = w.create();
    REQUIRE(w.add<Transform>(cube, Transform{Vec3{-10.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(cube) != nullptr);
    // a RESOLVED reference at +10, twice the cube's reach
    const Entity referenced = w.create();
    REQUIRE(w.add<Transform>(referenced, Transform{Vec3{10.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(referenced, MeshRenderer{.mesh = meshGuid(1), .meshIndex = 0}) != nullptr);
    // an UNRESOLVED reference at +30 -- a point, so it still extends the box, just not by a box
    const Entity loading = w.create();
    REQUIRE(w.add<Transform>(loading, Transform{Vec3{30.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<MeshRenderer>(loading, MeshRenderer{.mesh = meshGuid(2), .meshIndex = 0}) != nullptr);
    // a light-like entity: a Transform and NO MeshRenderer -- sceneBounds skips it entirely
    const Entity light = w.create();
    REQUIRE(w.add<Transform>(light, Transform{Vec3{500.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);

    MeshBoundsLookup lookup;
    lookup.set(MeshBoundsKey{meshGuid(1), 0}, Aabb{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}});

    const Aabb box = sceneBounds(w, &lookup);
    CHECK(std::abs(box.min.x - (-10.0F - CUBE_HALF)) < EPS);  // the cube's own half extent
    CHECK(std::abs(box.max.x - 30.0F) < EPS);                 // the loading entity, as a POINT
    CHECK(box.max.x < 500.0F);                                // the no-MeshRenderer entity is skipped
    CHECK(std::abs(box.max.y - 1.0F) < EPS);                  // the RESOLVED box, not the cube's 0.5
}
