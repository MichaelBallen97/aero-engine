// tests/editor/field_command_test.cpp -- task 2.4.2: SetFieldCommand driven against REAL generated
// entt::meta (InspectorProbe). Second TU of aero_editor_inspector_test, whose only OTHER TU
// (inspector_test.cpp) defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN -- this TU must NOT (A7). GATED on
// AERO_REFLECT_TOOLS by the target itself. Tier-0: no GPU, no window, no ImGui context.
#include <aero/core/guid.hpp>  // task 3.1.5: the eighth FieldValue alternative
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_commands.hpp>
#include <aero/editor/component_ops.hpp>  // task 3.1.5: writeComponentField, for IR6's setup
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/internal/world_access.hpp>

#include "inspector_probe.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>  // task 3.1.5: IR6 reads the field back through readComponentField
#include <ostream>   // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using engine::ComponentTypeId;
using engine::Entity;
using engine::LogLevel;
using engine::Quat;
using engine::Vec3;
using engine::World;
using engine::editor::Command;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::FieldValue;
using engine::editor::LogEntry;
using engine::editor::LogSinkScope;
using engine::editor::readComponentField;
using engine::editor::RootOrder;
using engine::editor::Selection;
using engine::editor::SetFieldCommand;
using engine::scene::internal::registerComponent;

// Forward-declared here; DEFINED by the GENERATED aero_editor_inspector_test.aggregator.gen.cpp
// (cmake/reflect.cmake's aero_reflect_generate(), task 1.1.4) -- the SAME aggregator
// inspector_test.cpp already calls. Declaring it a second time in a second TU is legal (a function
// declaration, not a definition) and costs nothing new in the build graph.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_all_aero_editor_inspector_test();

namespace {

ComponentTypeId registerProbe(World& world) { return registerComponent<InspectorProbe>(world, "InspectorProbe"); }

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

// P3's cross-type merge-rejection arm. Nothing else needs it.
class OtherCommand final : public Command {
public:
    bool redo(CommandContext& /*context*/) override { return true; }
    bool undo(CommandContext& /*context*/) override { return true; }
    [[nodiscard]] std::string_view label() const noexcept override { return "other"; }
};

}  // namespace

TEST_CASE("field_command: every FieldKind writes `after` on redo, `before` on undo, exactly (P1/AC-12)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    SUBCASE("Bool") {
        SetFieldCommand cmd{e, probeId, "enabled", "InspectorProbe", FieldValue{false}, FieldValue{true}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "enabled");
        REQUIRE(after.has_value());
        CHECK(std::get<bool>(*after) == true);
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "enabled");
        REQUIRE(before.has_value());
        CHECK(std::get<bool>(*before) == false);
    }

    SUBCASE("Int") {
        SetFieldCommand cmd{
            e, probeId, "gear", "InspectorProbe", FieldValue{std::int64_t{5}}, FieldValue{std::int64_t{-3}}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "gear");
        REQUIRE(after.has_value());
        CHECK(std::get<std::int64_t>(*after) == -3);
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "gear");
        REQUIRE(before.has_value());
        CHECK(std::get<std::int64_t>(*before) == 5);
    }

    SUBCASE("UInt") {
        SetFieldCommand cmd{
            e, probeId, "tiny", "InspectorProbe", FieldValue{std::uint64_t{10}}, FieldValue{std::uint64_t{200}}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "tiny");
        REQUIRE(after.has_value());
        CHECK(std::get<std::uint64_t>(*after) == 200);
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "tiny");
        REQUIRE(before.has_value());
        CHECK(std::get<std::uint64_t>(*before) == 10);
    }

    SUBCASE("Float") {
        SetFieldCommand cmd{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{5.5}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "speed");
        REQUIRE(after.has_value());
        CHECK(std::get<double>(*after) == doctest::Approx(5.5));
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "speed");
        REQUIRE(before.has_value());
        CHECK(std::get<double>(*before) == doctest::Approx(1.0));
    }

    SUBCASE("Vec3") {
        const Vec3 beforeV{0.1F, 0.2F, 0.3F};
        const Vec3 afterV{0.9F, 0.8F, 0.7F};
        SetFieldCommand cmd{e, probeId, "tint", "InspectorProbe", FieldValue{beforeV}, FieldValue{afterV}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "tint");
        REQUIRE(after.has_value());
        CHECK(std::get<Vec3>(*after) == afterV);
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "tint");
        REQUIRE(before.has_value());
        CHECK(std::get<Vec3>(*before) == beforeV);
    }

    SUBCASE("Quat") {
        const Quat beforeQ{};
        const Quat afterQ = engine::fromAxisAngle(Vec3::unitY(), engine::radians(45.0F));
        SetFieldCommand cmd{e, probeId, "aim", "InspectorProbe", FieldValue{beforeQ}, FieldValue{afterQ}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "aim");
        REQUIRE(after.has_value());
        CHECK(std::get<Quat>(*after) == afterQ);
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "aim");
        REQUIRE(before.has_value());
        CHECK(std::get<Quat>(*before) == beforeQ);
    }

    SUBCASE("String") {
        SetFieldCommand cmd{
            e, probeId, "label", "InspectorProbe", FieldValue{std::string("Alpha")}, FieldValue{std::string("Beta")}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "label");
        REQUIRE(after.has_value());
        CHECK(std::get<std::string>(*after) == "Beta");
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "label");
        REQUIRE(before.has_value());
        CHECK(std::get<std::string>(*before) == "Alpha");
    }

    SUBCASE("Guid") {
        // task 3.1.5. This case's title says EVERY FieldKind, so the eighth arrives here rather than
        // in a case of its own: SetFieldCommand needed no change at all for it (it carries
        // FieldValue by value, and a Guid is trivially copyable), and that is exactly what this
        // subcase proves.
        const engine::Guid beforeG{0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL};
        const engine::Guid afterG{0xFEEDFACECAFEBEEFULL, 0x0123456789ABCDEFULL};
        SetFieldCommand cmd{e, probeId, "asset", "InspectorProbe", FieldValue{beforeG}, FieldValue{afterG}};
        CHECK(cmd.redo(ctx));
        auto after = readComponentField(world, e, probeId, "asset");
        REQUIRE(after.has_value());
        CHECK((std::get<engine::Guid>(*after) == afterG));
        CHECK(cmd.undo(ctx));
        auto before = readComponentField(world, e, probeId, "asset");
        REQUIRE(before.has_value());
        CHECK((std::get<engine::Guid>(*before) == beforeG));
    }
}

TEST_CASE("field_command: Clear on a Guid row is one undoable entry (task 3.1.5, IR6)") {
    // What the inspector's Clear button pushes, through the REAL stack rather than by calling redo()
    // by hand: CommandStack::push APPLIES (2.4.1's D5), so the write below happens inside the
    // command and nowhere else. Undo must restore the exact prior guid -- a Clear that could not be
    // undone would be the one destructive edit in a panel where everything else is reversible.
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const engine::Guid bound{0x00A1B2C3D4E5F607ULL, 0x1122334455667788ULL};
    REQUIRE(engine::editor::writeComponentField(world, e, probeId, "asset", FieldValue{bound}));

    const std::optional<FieldValue> beforeValue = readComponentField(world, e, probeId, "asset");
    REQUIRE(beforeValue.has_value());
    CHECK(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "asset", "InspectorProbe", *beforeValue,
                                                            FieldValue{engine::Guid{}})));
    {
        const std::optional<FieldValue> cleared = readComponentField(world, e, probeId, "asset");
        REQUIRE(cleared.has_value());
        REQUIRE(std::holds_alternative<engine::Guid>(*cleared));
        CHECK_FALSE(std::get<engine::Guid>(*cleared).valid());
    }
    CHECK(stack.undo(ctx));
    {
        const std::optional<FieldValue> restored = readComponentField(world, e, probeId, "asset");
        REQUIRE(restored.has_value());
        REQUIRE(std::holds_alternative<engine::Guid>(*restored));
        CHECK((std::get<engine::Guid>(*restored) == bound));
    }
    // And forward again, so the entry is a real two-way history entry rather than a one-shot.
    CHECK(stack.redo(ctx));
    {
        const std::optional<FieldValue> againCleared = readComponentField(world, e, probeId, "asset");
        REQUIRE(againCleared.has_value());
        CHECK_FALSE(std::get<engine::Guid>(*againCleared).valid());
    }
}

TEST_CASE("field_command: the numeric clamp survives the command layer, both directions (P2/F17)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    // `tiny` is a plain uint8_t with no AERO_RANGE: 300 must clamp to the TYPE's own max (255), never
    // wrap the way an unguarded static_cast would (300 % 256 == 44).
    SetFieldCommand cmd{
        e, probeId, "tiny", "InspectorProbe", FieldValue{std::uint64_t{300}}, FieldValue{std::uint64_t{300}}};
    REQUIRE(cmd.redo(ctx));
    auto afterVal = readComponentField(world, e, probeId, "tiny");
    REQUIRE(afterVal.has_value());
    CHECK(std::get<std::uint64_t>(*afterVal) == 255);

    REQUIRE(cmd.undo(ctx));
    auto beforeVal = readComponentField(world, e, probeId, "tiny");
    REQUIRE(beforeVal.has_value());
    CHECK(std::get<std::uint64_t>(*beforeVal) == 255);  // undo restores the STORED (clamped) value
}

TEST_CASE("field_command: mergeWith keys on entity+type+field, all three required (P3/AC-14/S7/S8)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    const Entity other = world.create();
    world.addRaw(probeId, other, nullptr);
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());

    SUBCASE("same entity+type+field merges; before stays ours, after becomes theirs") {
        SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        const SetFieldCommand b{e, probeId, "speed", "InspectorProbe", FieldValue{2.0}, FieldValue{3.0}};
        CHECK(a.mergeWith(b));
        CHECK(std::get<double>(a.before()) == doctest::Approx(1.0));
        CHECK(std::get<double>(a.after()) == doctest::Approx(3.0));
    }

    SUBCASE("a different field refuses") {
        SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        const SetFieldCommand b{e, probeId, "mass", "InspectorProbe", FieldValue{2.0}, FieldValue{3.0}};
        CHECK_FALSE(a.mergeWith(b));
        CHECK(std::get<double>(a.before()) == doctest::Approx(1.0));
        CHECK(std::get<double>(a.after()) == doctest::Approx(2.0));
    }

    SUBCASE("a different ComponentTypeId refuses") {
        SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        const SetFieldCommand b{e, transformId, "speed", "engine::Transform", FieldValue{2.0}, FieldValue{3.0}};
        CHECK_FALSE(a.mergeWith(b));
        CHECK(std::get<double>(a.before()) == doctest::Approx(1.0));
        CHECK(std::get<double>(a.after()) == doctest::Approx(2.0));
    }

    SUBCASE("a different entity refuses") {
        SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        const SetFieldCommand b{other, probeId, "speed", "InspectorProbe", FieldValue{2.0}, FieldValue{3.0}};
        CHECK_FALSE(a.mergeWith(b));
        CHECK(std::get<double>(a.before()) == doctest::Approx(1.0));
        CHECK(std::get<double>(a.after()) == doctest::Approx(2.0));
    }

    SUBCASE("a cross-type Command refuses") {
        SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        const OtherCommand cross;
        CHECK_FALSE(a.mergeWith(cross));
        CHECK(std::get<double>(a.before()) == doctest::Approx(1.0));
        CHECK(std::get<double>(a.after()) == doctest::Approx(2.0));
    }
}

TEST_CASE("field_command: a 50-step simulated drag collapses to one entry (P4/AC-14, the T8 shape)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    // `mass` (a plain double with no AERO_RANGE) rather than `speed` -- speed's AERO_RANGE(0, 10)
    // would clamp a 49-step climb long before it reached this case's values.
    CommandStack stack;
    stack.breakMergeChain();
    double previous = 1.0;
    const double start = previous;
    for (int i = 1; i <= 49; ++i) {
        const double next = previous + 1.0;
        REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe",
                                                                  FieldValue{previous}, FieldValue{next})));
        previous = next;
    }
    const double last = previous;

    CHECK(stack.count() == 1);
    CHECK(stack.appliedCount() == 1);
    auto atEnd = readComponentField(world, e, probeId, "mass");
    REQUIRE(atEnd.has_value());
    CHECK(std::get<double>(*atEnd) == doctest::Approx(last));

    REQUIRE(stack.undo(ctx));
    auto afterUndo = readComponentField(world, e, probeId, "mass");
    REQUIRE(afterUndo.has_value());
    CHECK(std::get<double>(*afterUndo) == doctest::Approx(start));  // the drag START, not one frame back
}

TEST_CASE("field_command: the D15 guards are silent -- zero ERRORs of the command's own (P5)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const LogSinkScope scope;
    std::vector<LogEntry> records;

    SUBCASE("a dead entity") {
        World world;
        aero_reflect_register_all_aero_editor_inspector_test();
        const ComponentTypeId probeId = registerProbe(world);
        REQUIRE(probeId.valid());
        const Entity e = world.create();
        world.addRaw(probeId, e, nullptr);
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        SetFieldCommand cmd{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        REQUIRE(world.destroy(e));
        scope.sink()->take(records);
        records.clear();  // LogSink::take requires `out` empty on entry

        CHECK_FALSE(cmd.redo(ctx));
        CHECK_FALSE(cmd.undo(ctx));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Error) == 0);
    }

    SUBCASE("a removed component") {
        World world;
        aero_reflect_register_all_aero_editor_inspector_test();
        const ComponentTypeId probeId = registerProbe(world);
        REQUIRE(probeId.valid());
        const Entity e = world.create();
        world.addRaw(probeId, e, nullptr);
        Selection selection;
        RootOrder roots;
        CommandContext ctx{world, selection, roots};
        SetFieldCommand cmd{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
        REQUIRE(world.removeRaw(probeId, e));
        scope.sink()->take(records);
        records.clear();

        CHECK_FALSE(cmd.redo(ctx));
        scope.sink()->take(records);
        CHECK(countAtLevel(records, LogLevel::Error) == 0);
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        AERO_LOG_ERROR("field_command_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
}

TEST_CASE("field_command: the label is \"InspectorProbe.<field>\" (P6/AC-15)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    SetFieldCommand a{e, probeId, "speed", "InspectorProbe", FieldValue{1.0}, FieldValue{2.0}};
    CHECK(a.label() == "InspectorProbe.speed");
    CHECK_FALSE(a.label().empty());
    const SetFieldCommand b{e, probeId, "speed", "InspectorProbe", FieldValue{2.0}, FieldValue{3.0}};
    REQUIRE(a.mergeWith(b));
    CHECK(a.label() == "InspectorProbe.speed");  // unchanged by a merge (built ONCE, D14)
}

TEST_CASE("field_command: a std::string field owns its value, independent of the caller's buffer (P7/INV-2)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    std::string beforeBuf = "Alpha";
    std::string afterBuf = "Beta";
    SetFieldCommand cmd{e, probeId, "label", "InspectorProbe", FieldValue{beforeBuf}, FieldValue{afterBuf}};
    beforeBuf.clear();
    afterBuf.clear();

    CHECK(cmd.redo(ctx));
    auto afterVal = readComponentField(world, e, probeId, "label");
    REQUIRE(afterVal.has_value());
    CHECK(std::get<std::string>(*afterVal) == "Beta");

    CHECK(cmd.undo(ctx));
    auto beforeVal = readComponentField(world, e, probeId, "label");
    REQUIRE(beforeVal.has_value());
    CHECK(std::get<std::string>(*beforeVal) == "Alpha");
}

TEST_CASE("field_command: the merge-chain ordering law -- OPEN before the push, CLOSE after (P8/D17/AC-16)") {
    // The tier-0 policy pin for inspector_panel.cpp's seven-arm gate (task 2.4.2 R-1): the panel itself
    // is unreachable from any test target (src-private, ImGui-bound), so this drives the CALL SEQUENCE
    // a release frame produces, directly against a real CommandStack. Human row 6 is the only proof the
    // panel actually applies the policy pinned here.
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};

    SUBCASE("Arm 1 -- the correct order: OPEN before, CLOSE after -> one entry, undo reaches the drag start") {
        CommandStack stack;
        stack.breakMergeChain();  // the OPEN edge, correctly BEFORE this gesture's first push
        double v = 1.0;
        const double start = v;
        for (int i = 0; i < 3; ++i) {  // three drag frames
            const double next = v + 1.0;
            REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe",
                                                                      FieldValue{v}, FieldValue{next})));
            v = next;
        }
        // The release frame: push THEN close -- the correct order (D17).
        const double released = v + 1.0;
        REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe", FieldValue{v},
                                                                  FieldValue{released})));
        stack.breakMergeChain();

        CHECK(stack.count() == 1);
        REQUIRE(stack.undo(ctx));
        auto afterUndo = readComponentField(world, e, probeId, "mass");
        REQUIRE(afterUndo.has_value());
        CHECK(std::get<double>(*afterUndo) == doctest::Approx(start));  // the drag START, not one frame back
    }

    SUBCASE("Arm 2 -- the DEFECT, documented: CLOSE moved BEFORE the release frame's push -> two entries") {
        CommandStack stack;
        stack.breakMergeChain();
        double v = 1.0;
        for (int i = 0; i < 3; ++i) {
            const double next = v + 1.0;
            REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe",
                                                                      FieldValue{v}, FieldValue{next})));
            v = next;
        }
        // This arm exists to PIN the bug the code above must not have, not to bless it: the CLOSE edge
        // runs BEFORE the release frame's push instead of after -- 2.4.1's own code-review-round defect,
        // reproduced here on purpose.
        const double released = v + 1.0;
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe", FieldValue{v},
                                                                  FieldValue{released})));

        CHECK(stack.count() == 2);
    }

    SUBCASE("Arm 3 -- the OPEN edge on the wrong side: two gestures wrongly collapse into one entry") {
        CommandStack stack;
        // Gesture 1: OPEN correctly before its push.
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe", FieldValue{1.0},
                                                                  FieldValue{2.0})));
        // Gesture 2's OPEN edge, moved AFTER its first push instead of before: the push below runs
        // while gesture 1's chain is still open, so it wrongly MERGES into gesture 1's entry -- the
        // breakMergeChain() call that follows is too late to prevent it.
        REQUIRE(stack.push(ctx, std::make_unique<SetFieldCommand>(e, probeId, "mass", "InspectorProbe", FieldValue{2.0},
                                                                  FieldValue{3.0})));
        stack.breakMergeChain();

        CHECK(stack.count() == 1);  // two GESTURES should be two entries; the ordering defect collapses them
    }
}
