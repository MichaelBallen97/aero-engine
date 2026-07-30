// tests/editor/structural_commands_test.cpp -- task 2.4.2: the component-structure commands (step 5:
// X15-X16) and the five structural entity commands (step 6: X1-X14, X17-X20). Fourteenth TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated, and REFLECTION-FREE (AC-8): no line below
// names entt::meta or reflect-gen -- every case drives a REAL CommandStack with a real
// CommandContext{world, selection, roots}, no panel, no ImGui.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_commands.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using engine::Camera;
using engine::ComponentTypeId;
using engine::Entity;
using engine::LogLevel;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::AddComponentCommand;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::LogEntry;
using engine::editor::LogSinkScope;
using engine::editor::RemoveComponentCommand;
using engine::editor::RootOrder;
using engine::editor::Selection;
using engine::scene::internal::registerComponent;

namespace {

// File-local copies, per the established one-per-TU rule.
[[nodiscard]] std::size_t countAtLevel(const std::vector<LogEntry>& records, LogLevel level) {
    return static_cast<std::size_t>(
        std::count_if(records.begin(), records.end(), [level](const LogEntry& e) { return e.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// X16(b)'s probe: a genuinely empty (tag) type the private ComponentSnapshot store cannot mirror
// (F10/H3 -- see scene_snapshot_test.cpp's N6/N11 comments for the full reasoning).
struct X16Tag {};

}  // namespace

TEST_CASE("structural_commands: AddComponentCommand (X15/AC-12..AC-15/A5)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    const ComponentTypeId cameraId = world.findComponentType("engine::Camera");
    REQUIRE(cameraId.valid());

    SUBCASE("push adds; undo removes; redo re-adds") {
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(world.has<Camera>(e));
        REQUIRE(stack.undo(ctx));
        CHECK_FALSE(world.has<Camera>(e));
        REQUIRE(stack.redo(ctx));
        CHECK(world.has<Camera>(e));
    }

    SUBCASE("the label is \"Add <Short>\", byte-identical across push/undo/redo") {
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(stack.undoLabel() == "Add Camera");
        REQUIRE(stack.undo(ctx));
        CHECK(stack.redoLabel() == "Add Camera");
        REQUIRE(stack.redo(ctx));
        CHECK(stack.undoLabel() == "Add Camera");
    }

    SUBCASE("push onto a type already present: one ERROR + one WARN (A5), nothing recorded, untouched") {
        const LogFixture fixture;
        const LogSinkScope scope;
        std::vector<LogEntry> records;
        REQUIRE(world.add<Camera>(e, Camera{.fovYRadians = 1.5F}) != nullptr);
        CommandStack stack;
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        CHECK_FALSE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Error) == 1);  // component_ops' own refusal ERROR
        CHECK(countAtLevel(records, LogLevel::Warn) == 1);   // the stack's own WARN
        CHECK(stack.count() == 0);
        REQUIRE(world.has<Camera>(e));
        CHECK(world.get<Camera>(e)->fovYRadians == doctest::Approx(1.5F));  // untouched
    }

    SUBCASE("a dead target: false, nothing recorded") {
        REQUIRE(world.destroy(e));
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<AddComponentCommand>(e, cameraId, "engine::Camera")));
        CHECK(stack.count() == 0);
    }
}

TEST_CASE("structural_commands: RemoveComponentCommand (X16/AC-12..AC-15/D18/E10)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    const Entity e = world.create();
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());

    SUBCASE("undo restores BOTH non-default field values exactly (S14's discriminator)") {
        const Transform t{.position = Vec3{7.0F, 8.0F, 9.0F}, .scale = Vec3{3.0F, 3.0F, 3.0F}};
        REQUIRE(world.add<Transform>(e, t) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK_FALSE(world.has<Transform>(e));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        CHECK(*world.get<Transform>(e) == t);
    }

    // A5-shaped finding, own to this task rather than the spec's literal wording: D18's own "capture
    // BEFORE removing" reasoning means an UNMIRRORABLE type (F10 -- none of the five engine built-ins
    // is a tag; see scene_snapshot_test.cpp's N6/N11) is refused ENTIRELY, never silently zeroed. The
    // spec/plan's literal "(b) a tag component round-trips by presence" is unreachable today for a
    // non-built-in type (D24 says so of the sibling SubtreeSnapshot case); this proves the refusal is
    // safe rather than claiming a round trip the current architecture cannot deliver.
    SUBCASE("a component type this snapshot cannot mirror is refused entirely -- nothing removed (D18/H3)") {
        const ComponentTypeId tagId = registerComponent<X16Tag>(world, "X16.Tag");
        REQUIRE(tagId.valid());
        world.add<X16Tag>(e);
        REQUIRE(world.has<X16Tag>(e));
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, tagId, "X16.Tag")));
        CHECK(world.has<X16Tag>(e));  // untouched
        CHECK(stack.count() == 0);
    }

    SUBCASE("undo -> redo -> undo: each redo re-captures the CURRENT value, not the original") {
        REQUIRE(world.add<Transform>(e, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        world.get<Transform>(e)->position = Vec3{2.0F, 0.0F, 0.0F};  // an intervening write (A19)

        REQUIRE(stack.redo(ctx));  // must capture {2,0,0} fresh, not replay the original {1,0,0}
        CHECK_FALSE(world.has<Transform>(e));
        REQUIRE(stack.undo(ctx));
        REQUIRE(world.has<Transform>(e));
        CHECK(world.get<Transform>(e)->position == Vec3{2.0F, 0.0F, 0.0F});
    }

    SUBCASE("an absent component: redo returns false, removes nothing") {
        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK(stack.count() == 0);
    }

    SUBCASE("the label is \"Remove <Short>\"") {
        REQUIRE(world.add<Transform>(e) != nullptr);
        CommandStack stack;
        REQUIRE(stack.push(ctx, std::make_unique<RemoveComponentCommand>(e, transformId, "engine::Transform")));
        CHECK(stack.undoLabel() == "Remove Transform");
    }
}
