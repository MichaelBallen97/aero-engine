// tests/editor/transform_command_test.cpp -- task 2.4.1: TransformCommand, wrapping the transform_ops
// seam. Twelfth TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/transform_command.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// File-local: command_stack_test.cpp has its own copy (they are `static` there, one per TU by design
// -- do not promote either to a shared header for two users).
[[nodiscard]] std::size_t countAtLevel(const std::vector<engine::editor::LogEntry>& records, engine::LogLevel level) {
    return static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(), [level](const engine::editor::LogEntry& e) { return e.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// A non-TransformCommand, for T6's dynamic_cast arm. Nothing else needs it.
class OtherCommand final : public engine::editor::Command {
public:
    bool redo(engine::World& /*world*/) override { return true; }
    bool undo(engine::World& /*world*/) override { return true; }
    [[nodiscard]] std::string_view label() const noexcept override { return "other"; }
};

}  // namespace

using engine::Entity;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::readTransform;
using engine::editor::TRANSFORM_COMMAND_LABEL;
using engine::editor::TransformCommand;

TEST_CASE("transform_command: apply / revert (T1/AC-12)") {
    World w;
    const Entity e = w.create();
    const Transform before{.position = Vec3{1.0F, 2.0F, 3.0F},
                           .rotation = engine::fromAxisAngle(Vec3::unitZ(), engine::radians(20.0F)),
                           .scale = Vec3{2.0F, 3.0F, 4.0F}};
    const Transform after{.position = Vec3{5.0F, 6.0F, 7.0F},
                          .rotation = engine::fromAxisAngle(Vec3::unitX(), engine::radians(45.0F)),
                          .scale = Vec3{1.5F, 1.5F, 1.5F}};
    REQUIRE(w.add<Transform>(e, before) != nullptr);

    TransformCommand cmd{e, before, after};
    CHECK(cmd.redo(w));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == after);

    CHECK(cmd.undo(w));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == before);
}

TEST_CASE("transform_command: dead entity (T2/AC-13/D16)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("undo/redo return false, and no ERROR is emitted -- the discriminating half (S10)") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        const Transform before = *readTransform(w, e);
        const Transform after{.position = Vec3{9.0F, 9.0F, 9.0F}};
        TransformCommand cmd{e, before, after};
        REQUIRE(w.destroy(e));

        CHECK_FALSE(cmd.undo(w));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("driven through a CommandStack: exactly one WARN and zero ERRORs") {
        World w;
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        const Transform before = *readTransform(w, e);
        const Transform after{.position = Vec3{9.0F, 9.0F, 9.0F}};
        engine::editor::CommandStack stack;
        REQUIRE(stack.push(w, std::make_unique<TransformCommand>(e, before, after)));
        REQUIRE(w.destroy(e));
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        CHECK(stack.undo(w));  // D20: the history still MOVES even though the command failed
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        AERO_LOG_ERROR("transform_command_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
}

TEST_CASE("transform_command: no Transform component") {
    World w;
    const Entity e = w.create();  // bare entity, no Transform
    const Transform before{};
    const Transform after{.position = Vec3{1.0F, 1.0F, 1.0F}};
    TransformCommand cmd{e, before, after};

    CHECK_FALSE(cmd.redo(w));
    CHECK_FALSE(w.has<Transform>(e));  // nothing created as a side effect
}

TEST_CASE("transform_command: merge, same entity (T4/AC-14)") {
    const Entity e{1, 1};
    const Transform p0{.position = Vec3{0.0F, 0.0F, 0.0F}};
    const Transform p1{.position = Vec3{1.0F, 0.0F, 0.0F}};
    const Transform p2{.position = Vec3{2.0F, 0.0F, 0.0F}};

    TransformCommand a{e, p0, p1};
    const TransformCommand b{e, p1, p2};

    CHECK(a.mergeWith(b));
    CHECK(a.before() == p0);  // UNCHANGED -- S9's discriminator
    CHECK(a.after() == p2);
    CHECK(b.before() == p1);  // b untouched
    CHECK(b.after() == p2);
}

TEST_CASE("transform_command: merge, different entity") {
    const Entity e1{1, 1};
    const Entity e2{2, 1};
    const Transform p0{.position = Vec3{0.0F, 0.0F, 0.0F}};
    const Transform p1{.position = Vec3{1.0F, 0.0F, 0.0F}};
    const Transform p2{.position = Vec3{2.0F, 0.0F, 0.0F}};

    TransformCommand a{e1, p0, p1};
    const TransformCommand b{e2, p1, p2};

    CHECK_FALSE(a.mergeWith(b));
    CHECK(a.before() == p0);
    CHECK(a.after() == p1);
    CHECK(b.before() == p1);
    CHECK(b.after() == p2);
}

TEST_CASE("transform_command: merge, different command type (T6/S11)") {
    const Entity e{1, 1};
    const Transform p0{.position = Vec3{0.0F, 0.0F, 0.0F}};
    const Transform p1{.position = Vec3{1.0F, 0.0F, 0.0F}};
    TransformCommand a{e, p0, p1};
    const OtherCommand other;

    CHECK_FALSE(a.mergeWith(other));
    CHECK(a.before() == p0);
    CHECK(a.after() == p1);
}

TEST_CASE("transform_command: label (T7/AC-15)") {
    const Entity e{1, 1};
    const Transform t{};
    const TransformCommand cmd{e, t, t};
    CHECK(cmd.label() == TRANSFORM_COMMAND_LABEL);
    CHECK(cmd.label() == "Transform");
    CHECK_FALSE(cmd.label().empty());
}

TEST_CASE("transform_command: a simulated drag through a real stack (T8/AC-16)") {
    World w;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);

    engine::editor::CommandStack stack;
    stack.breakMergeChain();

    Transform previous = *readTransform(w, e);
    const Transform p0 = previous;
    for (int i = 1; i <= 49; ++i) {
        Transform next = previous;
        next.position = Vec3{static_cast<float>(i), 0.0F, 0.0F};
        REQUIRE(stack.push(w, std::make_unique<TransformCommand>(e, previous, next)));
        previous = next;
    }
    const Transform p49 = previous;

    CHECK(stack.count() == 1);
    CHECK(stack.appliedCount() == 1);
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p49);

    REQUIRE(stack.undo(w));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p0);  // the drag START, not one frame back

    REQUIRE(stack.redo(w));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p49);
}

TEST_CASE("transform_command: interleaved entities do not merge (T9)") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Transform aBefore{.position = Vec3{0.0F, 0.0F, 0.0F}};
    const Transform aAfter{.position = Vec3{1.0F, 0.0F, 0.0F}};
    const Transform bBefore{.position = Vec3{0.0F, 1.0F, 0.0F}};
    const Transform bAfter{.position = Vec3{0.0F, 2.0F, 0.0F}};
    REQUIRE(w.add<Transform>(a, aBefore) != nullptr);
    REQUIRE(w.add<Transform>(b, bBefore) != nullptr);

    engine::editor::CommandStack stack;
    stack.breakMergeChain();
    REQUIRE(stack.push(w, std::make_unique<TransformCommand>(a, aBefore, aAfter)));
    REQUIRE(stack.push(w, std::make_unique<TransformCommand>(b, bBefore, bAfter)));  // chain still open

    CHECK(stack.count() == 2);

    REQUIRE(stack.undo(w));
    REQUIRE(stack.undo(w));
    REQUIRE(readTransform(w, a).has_value());
    REQUIRE(readTransform(w, b).has_value());
    CHECK(*readTransform(w, a) == aBefore);
    CHECK(*readTransform(w, b) == bBefore);
}
