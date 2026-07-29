// tests/editor/transform_ops_test.cpp — task 2.3.3: the Transform read/write seam's tier-0 battery.
// Tenth TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
//
// Case T2's LogFixture follows picking_test.cpp's case-11 idiom (itself the scene_bounds_test.cpp
// case-12b idiom) -- declared FIRST in its scope so it destructs LAST, after the LogSinkScope.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

using engine::Entity;
using engine::fromAxisAngle;
using engine::radians;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::readTransform;
using engine::editor::writeTransform;

TEST_CASE("transform_ops: readTransform happy path (T1)") {
    World w;
    const Entity e = w.create();
    const Transform t{.position = Vec3{1.0F, 2.0F, 3.0F},
                      .rotation = fromAxisAngle(Vec3::unitZ(), radians(20.0F)),
                      .scale = Vec3{2.0F, 3.0F, 4.0F}};
    REQUIRE(w.add<Transform>(e, t) != nullptr);
    const std::optional<Transform> read = readTransform(w, e);
    REQUIRE(read.has_value());
    CHECK(*read == t);  // EXACT, not approx
}

namespace {
struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};
}  // namespace

TEST_CASE("transform_ops: readTransform rejections are SILENT (T2)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("Entity{}") {
        const World w;
        CHECK_FALSE(readTransform(w, Entity{}).has_value());
        scope.sink()->take(records);
        CHECK(records.empty());
    }

    SUBCASE("a destroyed entity") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        REQUIRE(w.destroy(e));
        CHECK_FALSE(readTransform(w, e).has_value());
        scope.sink()->take(records);
        CHECK(records.empty());
    }

    SUBCASE("an entity without a Transform") {
        World w;
        const Entity e = w.create();
        CHECK_FALSE(readTransform(w, e).has_value());
        scope.sink()->take(records);
        CHECK(records.empty());
    }

    SUBCASE("a moved-from World") {
        std::optional<World> source;
        source.emplace();
        const Entity e = source->create();
        REQUIRE(source->add<Transform>(e) != nullptr);
        const World movedTo(std::move(*source));
        CHECK_FALSE(readTransform(*source, e).has_value());
        scope.sink()->take(records);
        CHECK(records.empty());
        (void)movedTo;
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        AERO_LOG_ERROR("transform_ops_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
}

TEST_CASE("transform_ops: writeTransform happy path (T3)") {
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);
    const Transform value{.position = Vec3{5.0F, 6.0F, 7.0F},
                          .rotation = fromAxisAngle(Vec3::unitX(), radians(15.0F)),
                          .scale = Vec3{1.5F, 1.5F, 1.5F}};
    CHECK(writeTransform(w, e, value));
    const std::optional<Transform> read = readTransform(w, e);
    REQUIRE(read.has_value());
    CHECK(*read == value);
}

TEST_CASE("transform_ops: writeTransform rejections (T4)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;
    const Transform someValue{.position = Vec3{1.0F, 1.0F, 1.0F}};

    SUBCASE("Entity{}") {
        World w;
        const Entity sibling = w.create();
        REQUIRE(w.add<Transform>(sibling, Transform{.position = Vec3{9.0F, 9.0F, 9.0F}}) != nullptr);
        CHECK_FALSE(writeTransform(w, Entity{}, someValue));
        scope.sink()->take(records);
        CHECK(records.size() == 1);
        CHECK(w.get<Transform>(sibling)->position == Vec3{9.0F, 9.0F, 9.0F});  // no mutation ANYWHERE
    }

    SUBCASE("a destroyed entity") {
        World w;
        const Entity sibling = w.create();
        REQUIRE(w.add<Transform>(sibling, Transform{.position = Vec3{9.0F, 9.0F, 9.0F}}) != nullptr);
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        REQUIRE(w.destroy(e));
        CHECK_FALSE(writeTransform(w, e, someValue));
        scope.sink()->take(records);
        CHECK(records.size() == 1);
        CHECK(w.get<Transform>(sibling)->position == Vec3{9.0F, 9.0F, 9.0F});
    }

    SUBCASE("an entity without a Transform") {
        World w;
        const Entity sibling = w.create();
        REQUIRE(w.add<Transform>(sibling, Transform{.position = Vec3{9.0F, 9.0F, 9.0F}}) != nullptr);
        const Entity e = w.create();
        CHECK_FALSE(writeTransform(w, e, someValue));
        scope.sink()->take(records);
        CHECK(records.size() == 1);
        CHECK_FALSE(w.has<Transform>(e));
        CHECK(w.get<Transform>(sibling)->position == Vec3{9.0F, 9.0F, 9.0F});
    }

    SUBCASE("a moved-from World") {
        std::optional<World> source;
        source.emplace();
        const Entity e = source->create();
        REQUIRE(source->add<Transform>(e) != nullptr);
        const World movedTo(std::move(*source));
        CHECK_FALSE(writeTransform(*source, e, someValue));
        scope.sink()->take(records);
        CHECK(records.size() == 1);
        (void)movedTo;
    }
}

TEST_CASE("transform_ops: writeTransform does not create the component when absent (T5)") {
    World w;
    const Entity e = w.create();
    const std::size_t before = w.entityCount();
    CHECK_FALSE(writeTransform(w, e, Transform{}));
    CHECK_FALSE(w.has<Transform>(e));
    CHECK(w.entityCount() == before);
}

TEST_CASE("transform_ops: non-finite values are stored AS GIVEN (T6)") {
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Transform hostile{.position = Vec3{inf, nan, 0.0F}};
    CHECK(writeTransform(w, e, hostile));
    const std::optional<Transform> read = readTransform(w, e);
    REQUIRE(read.has_value());
    CHECK(read->position.x == inf);
    CHECK(std::isnan(read->position.y));
}

TEST_CASE("transform_ops: tools-independence -- no entt anywhere on this path (T7/AC-17)") {
    // A World with NO editor reflection registered round-trips identically. Structurally reinforced
    // by the task's grep that neither this TU nor transform_ops.cpp names entt:: (see §V7).
    World w;
    const Entity e = w.create();
    const Transform t{.position = Vec3{3.0F, 2.0F, 1.0F}};
    REQUIRE(w.add<Transform>(e, t) != nullptr);
    CHECK(readTransform(w, e) == t);
    CHECK(writeTransform(w, e, Transform{.position = Vec3{4.0F, 4.0F, 4.0F}}));
    CHECK(readTransform(w, e)->position == Vec3{4.0F, 4.0F, 4.0F});
}
