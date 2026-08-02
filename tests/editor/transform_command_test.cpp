// tests/editor/transform_command_test.cpp -- task 2.4.1: TransformCommand, wrapping the transform_ops
// seam. Twelfth TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass identically with
// AERO_REQUIRE_GPU unset and set.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
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

// A non-TransformCommand, for T6's cross-type merge-rejection arm. Nothing else needs it.
class OtherCommand final : public engine::editor::Command {
public:
    bool redo(engine::editor::CommandContext& /*context*/) override { return true; }
    bool undo(engine::editor::CommandContext& /*context*/) override { return true; }
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
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{w, selection, roots};
    const Entity e = w.create();
    const Transform before{.position = Vec3{1.0F, 2.0F, 3.0F},
                           .rotation = engine::fromAxisAngle(Vec3::unitZ(), engine::radians(20.0F)),
                           .scale = Vec3{2.0F, 3.0F, 4.0F}};
    const Transform after{.position = Vec3{5.0F, 6.0F, 7.0F},
                          .rotation = engine::fromAxisAngle(Vec3::unitX(), engine::radians(45.0F)),
                          .scale = Vec3{1.5F, 1.5F, 1.5F}};
    REQUIRE(w.add<Transform>(e, before) != nullptr);

    TransformCommand cmd{e, before, after};
    CHECK(cmd.redo(ctx));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == after);

    CHECK(cmd.undo(ctx));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == before);
}

TEST_CASE("transform_command: dead entity (T2/AC-13/D16)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("undo/redo return false, and no ERROR is emitted -- the discriminating half (S10)") {
        World w;
        engine::editor::Selection selection;
        engine::editor::RootOrder roots;
        engine::editor::CommandContext ctx{w, selection, roots};
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        const Transform before = *readTransform(w, e);
        const Transform after{.position = Vec3{9.0F, 9.0F, 9.0F}};
        TransformCommand cmd{e, before, after};
        REQUIRE(w.destroy(e));

        CHECK_FALSE(cmd.undo(ctx));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("driven through a CommandStack: exactly one WARN and zero ERRORs") {
        World w;
        engine::editor::Selection selection;
        engine::editor::RootOrder roots;
        engine::editor::CommandContext ctx{w, selection, roots};
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(e) != nullptr);
        const Transform before = *readTransform(w, e);
        const Transform after{.position = Vec3{9.0F, 9.0F, 9.0F}};
        engine::editor::CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(e, before, after)));
        REQUIRE(w.destroy(e));
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        CHECK(stack.undo(ctx));  // D20: the history still MOVES even though the command failed
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

// Code-review round (Gap 4): T3 used to have no LogFixture/LogSinkScope/ERROR assertion at all, so the
// "no AERO_LOG_ERROR" half of AC-13 was proven only by T2's DEAD-entity arm. Both of T3's original
// checks (CHECK_FALSE(cmd.redo(ctx)) and CHECK_FALSE(w.has<Transform>(e))) stay true with
// TransformCommand::write's `readTransform` guard removed (S10), because writeTransform ALSO returns
// false for a missing component -- it just also emits the AERO_LOG_ERROR the guard exists to avoid.
// Proven dead before this rewrite: seeding S10 left T3 green. The null-Entity{} arm of AC-13 (a target
// that never resolves at all, as opposed to one that used to exist) had no coverage anywhere in this TU.
TEST_CASE("transform_command: no Transform component (T3/AC-13)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("a live entity with no Transform: redo fails, no side effect, zero ERRORs (S10)") {
        World w;
        engine::editor::Selection selection;
        engine::editor::RootOrder roots;
        engine::editor::CommandContext ctx{w, selection, roots};
        const Entity e = w.create();  // bare entity, no Transform
        const Transform before{};
        const Transform after{.position = Vec3{1.0F, 1.0F, 1.0F}};
        TransformCommand cmd{e, before, after};

        CHECK_FALSE(cmd.redo(ctx));
        CHECK_FALSE(w.has<Transform>(e));  // nothing created as a side effect
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("a null Entity{}: redo and undo both fail, zero ERRORs -- the AC-13 arm T3 never covered") {
        World w;
        engine::editor::Selection selection;
        engine::editor::RootOrder roots;
        engine::editor::CommandContext ctx{w, selection, roots};
        const Entity e{};  // default-constructed: generation 0, never resolves to a live component
        const Transform before{};
        const Transform after{.position = Vec3{1.0F, 1.0F, 1.0F}};
        TransformCommand cmd{e, before, after};

        CHECK_FALSE(cmd.redo(ctx));
        CHECK_FALSE(cmd.undo(ctx));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        AERO_LOG_ERROR("transform_command_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
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
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{w, selection, roots};
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);

    engine::editor::CommandStack stack;
    stack.breakMergeChain();

    Transform previous = *readTransform(w, e);
    const Transform p0 = previous;
    for (int i = 1; i <= 49; ++i) {
        Transform next = previous;
        next.position = Vec3{static_cast<float>(i), 0.0F, 0.0F};
        REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(e, previous, next)));
        previous = next;
    }
    const Transform p49 = previous;

    CHECK(stack.count() == 1);
    CHECK(stack.appliedCount() == 1);
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p49);

    REQUIRE(stack.undo(ctx));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p0);  // the drag START, not one frame back

    REQUIRE(stack.redo(ctx));
    REQUIRE(readTransform(w, e).has_value());
    CHECK(*readTransform(w, e) == p49);
}

TEST_CASE("transform_command: interleaved entities do not merge (T9)") {
    World w;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{w, selection, roots};
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
    REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(a, aBefore, aAfter)));
    REQUIRE(stack.push(ctx, std::make_unique<TransformCommand>(b, bBefore, bAfter)));  // chain still open

    CHECK(stack.count() == 2);

    REQUIRE(stack.undo(ctx));
    REQUIRE(stack.undo(ctx));
    REQUIRE(readTransform(w, a).has_value());
    REQUIRE(readTransform(w, b).has_value());
    CHECK(*readTransform(w, a) == aBefore);
    CHECK(*readTransform(w, b) == bBefore);
}

// T10 (Phase 2 audit): a stale incoming command does not merge.
//
// `mergeWith`'s own comment scoped its safety precisely -- the guard "could never be false in this
// task ... The first task that can write a Transform from outside the drag loop must add it (E9/H7)."
// That task landed immediately afterwards: 2.4.2's SetFieldCommand writes Transform.position /
// rotation / scale from the Inspector, Transform being one reflected component among five. The
// handoff was recorded and never re-read, which is the failure mode -- three review rounds each
// caught a defect in the code and none caught a claim about the code that had stopped being true.
//
// Merging keeps OUR before and takes THEIR after. That is only sound while the two are contiguous:
// if anything wrote the Transform between the two commands, our `before` skips that write, and undo
// restores a value the entity never held.
TEST_CASE("transform_command: a command whose before does not continue ours never merges (T10/E9/H7)") {
    engine::Transform a;
    a.position = {0.0F, 0.0F, 0.0F};
    engine::Transform b;
    b.position = {1.0F, 0.0F, 0.0F};
    engine::Transform outside;
    outside.position = {50.0F, 0.0F, 0.0F};  // an Inspector edit, a script, a future timeline scrub
    engine::Transform d;
    d.position = {2.0F, 0.0F, 0.0F};

    const engine::Entity target{1U, 1U};

    SUBCASE("contiguous: the next frame of the SAME drag still merges (AC-14 is not weakened)") {
        engine::editor::TransformCommand drag(target, a, b);
        const engine::editor::TransformCommand nextFrame(target, b, d);
        REQUIRE(drag.mergeWith(nextFrame));
        CHECK(drag.before().position.x == doctest::Approx(0.0F));  // ours, kept
        CHECK(drag.after().position.x == doctest::Approx(2.0F));   // theirs, taken
    }

    SUBCASE("stale: something wrote the Transform in between, so the chain is broken") {
        engine::editor::TransformCommand drag(target, a, b);
        const engine::editor::TransformCommand afterOutsideWrite(target, outside, d);
        CHECK_FALSE(drag.mergeWith(afterOutsideWrite));
        // Refusing leaves this command exactly as it was -- the caller records a second entry, so
        // undo walks b -> a and the outside write keeps its own history entry.
        CHECK(drag.before().position.x == doctest::Approx(0.0F));
        CHECK(drag.after().position.x == doctest::Approx(1.0F));
    }
}
