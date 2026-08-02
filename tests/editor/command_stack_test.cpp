// tests/editor/command_stack_test.cpp -- task 2.4.1: the Command interface and the bounded
// CommandStack's five algorithms. Eleventh TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0/ungated: must pass
// identically with AERO_REQUIRE_GPU unset and set.
#include <aero/core/log.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Instrumentation the FAKE writes into. Owned by the CASE, never by the command: Command deletes copy
// AND move (AC-1), so a FakeCommand can only live on the heap inside the stack, and the stack destroys
// it -- sometimes at its OWN destruction. Declaring a CommandLog BEFORE the CommandStack in every case
// is what makes ~FakeCommand's write land in a live object (the LogFixture ordering rule).
struct CommandLog {
    int redoCalls = 0;
    int undoCalls = 0;
    int destroyCalls = 0;
    int mergeCalls = 0;
    // The label of the LAST command handed to mergeWith. A string_view onto a literal, NOT &incoming:
    // on a successful merge the incoming command is destroyed inside push(), so a saved pointer's value
    // is indeterminate afterwards, while a literal's view stays valid forever.
    std::string_view lastMergeLabel;
};

class FakeCommand final : public engine::editor::Command {
public:
    // `log` MUST outlive this command.
    FakeCommand(CommandLog& log, std::string_view name) noexcept : sink(&log), text(name) {}
    ~FakeCommand() override { ++sink->destroyCalls; }
    FakeCommand(const FakeCommand&) = delete;
    FakeCommand& operator=(const FakeCommand&) = delete;
    FakeCommand(FakeCommand&&) = delete;
    FakeCommand& operator=(FakeCommand&&) = delete;

    bool redo(engine::editor::CommandContext& /*context*/) override {
        ++sink->redoCalls;
        return redoResult;
    }
    bool undo(engine::editor::CommandContext& /*context*/) override {
        ++sink->undoCalls;
        return undoResult;
    }
    [[nodiscard]] std::string_view label() const noexcept override { return text; }
    bool mergeWith(const engine::editor::Command& incoming) override {
        ++sink->mergeCalls;
        sink->lastMergeLabel = incoming.label();
        return mergeResult;
    }

    // Settable verdicts -- the whole point of the double. Set through the raw pointer captured BEFORE
    // std::move'ing the unique_ptr into push(); the stack keeps the object alive.
    bool redoResult = true;
    bool undoResult = true;
    bool mergeResult = false;

private:
    CommandLog* sink;       // non-owning; caller-owned, outlives the stack
    std::string_view text;  // a literal at every construction site
};

// Count by LEVEL, never records.size(): AERO_LOG_DEBUG is compiled out under NDEBUG (log.hpp:137-143),
// so a successful undo contributes a record in Debug and none in Release. Counting by level makes every
// assertion below identical on both presets (plan A5).
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

}  // namespace

using engine::editor::COMMAND_HISTORY_CAPACITY;
using engine::editor::CommandStack;

TEST_CASE("command_stack: a fresh stack is empty and clean (C1)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandStack stack;
    CHECK(stack.count() == 0);
    CHECK(stack.appliedCount() == 0);
    CHECK(stack.capacity() == COMMAND_HISTORY_CAPACITY);
    CHECK_FALSE(stack.canUndo());
    CHECK_FALSE(stack.canRedo());
    CHECK(stack.undoLabel().empty());
    CHECK(stack.redoLabel().empty());
    CHECK(stack.isClean());
    CHECK_FALSE(stack.undo(ctx));
    CHECK_FALSE(stack.redo(ctx));

    const CommandLog log;
    CHECK(log.redoCalls == 0);
    CHECK(log.undoCalls == 0);
    CHECK(log.destroyCalls == 0);
    CHECK(log.mergeCalls == 0);
}

TEST_CASE("command_stack: push applies exactly once (C2/AC-2)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog log;
    CommandStack stack;
    CHECK(stack.push(ctx, std::make_unique<FakeCommand>(log, "fake")));
    CHECK(log.redoCalls == 1);  // not 0, not 2
    CHECK(log.undoCalls == 0);
    CHECK(stack.canUndo());
    CHECK_FALSE(stack.canRedo());
    CHECK(stack.appliedCount() == 1);
    CHECK(stack.count() == 1);
    CHECK(stack.undoLabel() == "fake");
    CHECK(stack.redoLabel().empty());
    CHECK_FALSE(stack.isClean());
}

TEST_CASE("command_stack: undo -> redo round trip (C3/AC-4)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog log;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(log, "fake")));

    CHECK(stack.undo(ctx));
    CHECK(log.undoCalls == 1);
    CHECK(stack.appliedCount() == 0);
    CHECK_FALSE(stack.canUndo());
    CHECK(stack.canRedo());
    CHECK(stack.undoLabel().empty());
    CHECK(stack.redoLabel() == "fake");

    CHECK(stack.redo(ctx));
    CHECK(log.redoCalls == 2);
    CHECK(stack.appliedCount() == 1);
    CHECK(stack.undoLabel() == "fake");
    CHECK(stack.redoLabel().empty());
}

TEST_CASE("command_stack: a recording push truncates the redo branch (C4/AC-3)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandLog logC;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));  // A's mergeWith is false
    REQUIRE(stack.undo(ctx));
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));

    CHECK(logB.destroyCalls == 1);
    CHECK(stack.count() == 2);
    CHECK(stack.appliedCount() == 2);
    CHECK_FALSE(stack.canRedo());
    CHECK(stack.undoLabel() == "C");
}

TEST_CASE("command_stack: merge collapses into the top (C5/AC-6)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandStack stack;
    auto a = std::make_unique<FakeCommand>(logA, "A");
    FakeCommand* const aPtr = a.get();  // the stack keeps A alive; this stays valid
    REQUIRE(stack.push(ctx, std::move(a)));
    aPtr->mergeResult = true;

    CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    CHECK(stack.count() == 1);
    CHECK(stack.appliedCount() == 1);
    CHECK(logA.mergeCalls == 1);
    CHECK(logA.lastMergeLabel == "B");
    CHECK(logB.redoCalls == 1);  // redo ran BEFORE the merge
    CHECK(logB.destroyCalls == 1);
    CHECK(stack.undoLabel() == "A");
}

TEST_CASE("command_stack: breakMergeChain prevents a merge (C6/AC-7)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandStack stack;
    auto a = std::make_unique<FakeCommand>(logA, "A");
    FakeCommand* const aPtr = a.get();
    REQUIRE(stack.push(ctx, std::move(a)));
    aPtr->mergeResult = true;
    stack.breakMergeChain();

    CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    CHECK(stack.count() == 2);
    CHECK(logA.mergeCalls == 0);
    CHECK(logB.destroyCalls == 0);
}

TEST_CASE("command_stack: undo/redo/clear/setClean each break the chain (C7/AC-7)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};

    SUBCASE("undo") {
        // Code-review round (Gap 2): the single-entry construction below this comment used to be the
        // whole arm -- push A, undo A, push B -- and it CANNOT discriminate S2 (a bugged `undo()` that
        // forgets `mergeOpen = false;`), because undoing the stack's SOLE entry drops `applied` to 0,
        // and push B's own step 3 (truncate `history[applied, size)`) then destroys A before the merge
        // guard's `applied > 0` term is ever reached -- the seed and the fix look identical. A TWO-entry
        // construction (push A, break the chain, push B, undo B, push C) leaves A applied and B
        // redoable: with the bug, `mergeOpen` stays true after undoing B, and push C's merge guard finds
        // `applied > 0` true and A's `mergeWith` gets called, wrongly folding C into A.
        CommandLog logA;
        CommandLog logB;
        CommandLog logC;
        CommandStack stack;
        auto a = std::make_unique<FakeCommand>(logA, "A");
        FakeCommand* const aPtr = a.get();
        REQUIRE(stack.push(ctx, std::move(a)));
        aPtr->mergeResult = true;
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        REQUIRE(stack.undo(ctx));
        CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));
        CHECK(logA.mergeCalls == 0);
        CHECK(stack.count() == 2);  // B's redo branch is truncated by C; A survives untouched
    }

    SUBCASE("redo") {
        CommandLog logA;
        CommandLog logB;
        CommandStack stack;
        auto a = std::make_unique<FakeCommand>(logA, "A");
        FakeCommand* const aPtr = a.get();
        REQUIRE(stack.push(ctx, std::move(a)));
        aPtr->mergeResult = true;
        REQUIRE(stack.undo(ctx));
        REQUIRE(stack.redo(ctx));
        CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        CHECK(logA.mergeCalls == 0);
        CHECK(stack.count() == 2);
    }

    SUBCASE("clear") {
        CommandLog logA;
        CommandLog logB;
        CommandStack stack;
        auto a = std::make_unique<FakeCommand>(logA, "A");
        FakeCommand* const aPtr = a.get();
        REQUIRE(stack.push(ctx, std::move(a)));
        aPtr->mergeResult = true;
        stack.clear();
        CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        CHECK(logA.mergeCalls == 0);
        CHECK(stack.count() == 1);
    }

    SUBCASE("setClean") {
        CommandLog logA;
        CommandLog logB;
        CommandStack stack;
        auto a = std::make_unique<FakeCommand>(logA, "A");
        FakeCommand* const aPtr = a.get();
        REQUIRE(stack.push(ctx, std::move(a)));
        aPtr->mergeResult = true;
        stack.setClean();
        CHECK(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        CHECK(logA.mergeCalls == 0);
        CHECK(stack.count() == 2);
    }
}

TEST_CASE("command_stack: capacity evicts from the front (C8/AC-8)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandLog logC;
    CommandStack stack{2};
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));

    CHECK(logA.destroyCalls == 1);
    CHECK(logB.destroyCalls == 0);
    CHECK(stack.count() == 2);
    CHECK(stack.appliedCount() == 2);
    CHECK(stack.capacity() == 2);

    CHECK(stack.undo(ctx));
    CHECK(stack.undo(ctx));
    CHECK(stack.appliedCount() == 0);

    const int undoCallsB = logB.undoCalls;
    const int undoCallsC = logC.undoCalls;
    CHECK_FALSE(stack.undo(ctx));  // a third undo does nothing -- A is gone
    CHECK(logB.undoCalls == undoCallsB);
    CHECK(logC.undoCalls == undoCallsC);
}

TEST_CASE("command_stack: capacity is clamped to at least 1 (C9/AC-8)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;  // declared BEFORE the stack: the LogFixture ordering rule (§A12)
    CommandLog logB;
    CommandStack stack{0};
    CHECK(stack.capacity() == 1);

    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));

    CHECK(stack.count() == 1);
    CHECK(logA.destroyCalls == 1);  // the FIRST is evicted, not the second
    CHECK(logB.destroyCalls == 0);
    CHECK(stack.undoLabel() == "B");
}

TEST_CASE("command_stack: a failed push records nothing (C10/AC-2/D21/E7)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    REQUIRE(stack.undo(ctx));
    scope.sink()->take(records);
    records.clear();  // A6: LogSink::take requires `out` empty on entry

    auto b = std::make_unique<FakeCommand>(logB, "B");
    b->redoResult = false;
    CHECK_FALSE(stack.push(ctx, std::move(b)));
    CHECK(stack.count() == 1);
    CHECK(stack.canRedo());  // the redo branch (A) survives
    CHECK(logB.destroyCalls == 1);

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);
}

TEST_CASE("command_stack: a failed undo still consumes its step (C11/AC-5/D20)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandStack stack;
    auto a = std::make_unique<FakeCommand>(logA, "A");
    FakeCommand* const aPtr = a.get();
    REQUIRE(stack.push(ctx, std::move(a)));
    aPtr->undoResult = false;
    scope.sink()->take(records);
    records.clear();

    CHECK(stack.undo(ctx));
    CHECK(stack.appliedCount() == 0);
    CHECK(stack.canRedo());
    CHECK(logA.undoCalls == 1);

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);
}

TEST_CASE("command_stack: clear() (C12/AC-9)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandLog logC;
    CommandLog logD;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    stack.clear();

    CHECK(logA.destroyCalls == 1);
    CHECK(logB.destroyCalls == 1);
    CHECK(stack.count() == 0);
    CHECK(stack.appliedCount() == 0);
    CHECK(stack.isClean());
    CHECK_FALSE(stack.undo(ctx));

    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));
    stack.breakMergeChain();  // the chain was broken by clear() up to here; keep it broken across the
                              // seam so D records independently rather than attempting a merge into C
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logD, "D")));
    CHECK(stack.count() == 2);
    CHECK(logC.mergeCalls == 0);
}

TEST_CASE("command_stack: clean tracking (C13/AC-9)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    CHECK_FALSE(stack.isClean());

    stack.setClean();
    CHECK(stack.isClean());

    REQUIRE(stack.undo(ctx));
    CHECK_FALSE(stack.isClean());

    REQUIRE(stack.redo(ctx));
    CHECK(stack.isClean());

    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    CHECK_FALSE(stack.isClean());
}

TEST_CASE("command_stack: the clean position shifts, then becomes unreachable (C14/AC-9/E17)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};

    SUBCASE("arm 1: the clean position survives one shift and stays reachable") {
        CommandLog logA;
        CommandLog logB;
        CommandLog logC;
        CommandStack stack{2};
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
        stack.setClean();  // clean = 1
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));  // clean = 1, applied = 2
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));  // trim evicts A; clean 1->0
        CHECK_FALSE(stack.isClean());

        REQUIRE(stack.undo(ctx));
        REQUIRE(stack.undo(ctx));
        CHECK(stack.appliedCount() == 0);
        CHECK(stack.isClean());  // position 0 now denotes "after A", which IS the saved state
    }

    SUBCASE("arm 2: the clean position is evicted and becomes permanently unreachable") {
        CommandLog logA;
        CommandLog logB;
        CommandLog logC;
        CommandLog logD;
        CommandStack stack{2};
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
        stack.setClean();  // clean = 1
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));  // trim: clean 1->0
        stack.breakMergeChain();
        REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logD, "D")));  // trim evicts B; clean -> nullopt

        CHECK_FALSE(stack.isClean());
        CHECK(stack.appliedCount() == 2);
        REQUIRE(stack.undo(ctx));
        CHECK_FALSE(stack.isClean());
        REQUIRE(stack.undo(ctx));
        CHECK_FALSE(stack.isClean());
    }
}

TEST_CASE("command_stack: push(ctx, nullptr) (C15/AC-11/E8)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};

    SUBCASE("a null command changes nothing and logs nothing") {
        const LogFixture fixture;
        const engine::editor::LogSinkScope scope;
        std::vector<engine::editor::LogEntry> records;

        CommandStack stack;
        CHECK_FALSE(stack.push(ctx, nullptr));
        CHECK(stack.count() == 0);
        CHECK(stack.appliedCount() == 0);
        scope.sink()->take(records);
        CHECK(records.empty());
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        const LogFixture fixture;
        const engine::editor::LogSinkScope scope;
        std::vector<engine::editor::LogEntry> records;

        AERO_LOG_ERROR("command_stack_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
}

TEST_CASE("command_stack: labels (C16/AC-10)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandStack stack;
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));

    CHECK(stack.undoLabel() == "B");
    CHECK(stack.redoLabel().empty());

    REQUIRE(stack.undo(ctx));
    CHECK(stack.undoLabel() == "A");
    CHECK(stack.redoLabel() == "B");

    REQUIRE(stack.undo(ctx));
    CHECK(stack.undoLabel().empty());
    CHECK(stack.redoLabel() == "A");

    REQUIRE(stack.redo(ctx));
    REQUIRE(stack.redo(ctx));
    CHECK(stack.undoLabel() == "B");
    CHECK(stack.redoLabel().empty());
}

TEST_CASE("command_stack: destruction is complete, no leak (C17)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog log;  // ONE shared log, declared BEFORE the inner scope
    {
        CommandStack stack{8};
        for (int i = 0; i < 200; ++i) {
            auto cmd = std::make_unique<FakeCommand>(log, "cmd");
            REQUIRE(stack.push(ctx, std::move(cmd)));  // mergeResult defaults to false: never merges
        }
        CHECK(stack.count() == 8);
        CHECK(log.destroyCalls == 192);
    }
    CHECK(log.destroyCalls == 200);  // ASan/LSan is the second net, not the first
}

TEST_CASE("command_stack: the drag call sequence, end to end (C18/AC-16/AC-17)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    // `logs` declared BEFORE the stack: the LogFixture ordering rule (§A12) -- each FakeCommand's
    // sink is one of these, heap-owned, and must outlive the stack that destroys the commands.
    std::vector<std::unique_ptr<CommandLog>> logs;
    logs.reserve(10);
    for (int i = 0; i < 10; ++i) {
        logs.push_back(std::make_unique<CommandLog>());
    }
    CommandStack stack;

    stack.breakMergeChain();
    for (int i = 0; i < 5; ++i) {
        auto cmd = std::make_unique<FakeCommand>(*logs[static_cast<std::size_t>(i)], "drag");
        cmd->mergeResult = true;
        REQUIRE(stack.push(ctx, std::move(cmd)));
    }

    stack.breakMergeChain();
    for (int i = 5; i < 10; ++i) {
        auto cmd = std::make_unique<FakeCommand>(*logs[static_cast<std::size_t>(i)], "drag");
        cmd->mergeResult = true;
        REQUIRE(stack.push(ctx, std::move(cmd)));
    }

    CHECK(stack.count() == 2);
    CHECK(stack.appliedCount() == 2);

    REQUIRE(stack.undo(ctx));
    REQUIRE(stack.undo(ctx));
    CHECK(stack.appliedCount() == 0);
    CHECK(stack.canRedo());

    CHECK(logs[0]->mergeCalls == 4);
    CHECK(logs[5]->mergeCalls == 4);
    for (std::size_t i = 1; i < 5; ++i) {
        CHECK(logs[i]->destroyCalls == 1);
    }
    for (std::size_t i = 6; i < 10; ++i) {
        CHECK(logs[i]->destroyCalls == 1);
    }
}

// Code-review round (Gap 3): the mirror of C11, on the `redo()` side. AC-5 covers both directions, but
// C11 only ever drives `undo()` with a failing command -- nothing in this TU exercised `redo()`
// returning false before this case, so `CommandStack::redo`'s own "consume the step even on failure"
// arm (D20) had zero coverage. Proven dead before this case existed: seeding `if (ok) { ++applied; }`
// into `redo()` left the whole suite green.
TEST_CASE("command_stack: a failed redo still consumes its step (C19/AC-5/D20)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandStack stack;
    auto a = std::make_unique<FakeCommand>(logA, "A");
    FakeCommand* const aPtr = a.get();
    REQUIRE(stack.push(ctx, std::move(a)));
    REQUIRE(stack.undo(ctx));
    aPtr->redoResult = false;
    scope.sink()->take(records);
    records.clear();  // A6: LogSink::take requires `out` empty on entry

    CHECK(stack.redo(ctx));  // D20: "did the history move", never "did the command work"
    CHECK(stack.appliedCount() == 1);
    CHECK_FALSE(stack.canRedo());
    CHECK(logA.redoCalls == 2);  // once from the original push, once from this redo

    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);
}

// Code-review round (Gap 5/INV-1): a defaulted move copies the scalars (`applied`, `cleanPosition`,
// `mergeOpen`) while std::vector's own move leaves `history` empty behind -- a moved-from stack would
// then have `applied > 0` over an empty `history`: `canUndo()` lies true, and `undoLabel()`/`undo()`
// index off the end of an empty vector (ASan/UBSan would abort). CommandStack's move ctor/assignment
// are now hand-written to reset the source to clear()'s state.
TEST_CASE("command_stack: a moved-from stack is empty, clean and cannot undo (Gap 5/INV-1)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};

    SUBCASE("move construction") {
        CommandLog logA;
        // Wrapped in std::optional (the shell_test.cpp PanelRegistry precedent): moving *source rather
        // than a bare local, and reading the moved-from state back through source-> afterward, is what
        // keeps bugprone-use-after-move from flagging a deliberate moved-from-state assertion.
        std::optional<CommandStack> source;
        source.emplace();
        REQUIRE(source->push(ctx, std::make_unique<FakeCommand>(logA, "A")));
        REQUIRE(source->canUndo());

        const CommandStack moved{std::move(*source)};
        CHECK(moved.canUndo());
        CHECK(moved.count() == 1);

        CHECK_FALSE(source->canUndo());
        CHECK_FALSE(source->canRedo());
        CHECK(source->count() == 0);
        CHECK(source->appliedCount() == 0);
        CHECK(source->isClean());
        CHECK(source->undoLabel().empty());
        CHECK_FALSE(source->undo(ctx));  // must not index history[applied - 1] on an empty vector
    }

    SUBCASE("move assignment") {
        CommandLog logA;
        CommandLog logB;
        std::optional<CommandStack> source;
        source.emplace();
        REQUIRE(source->push(ctx, std::make_unique<FakeCommand>(logA, "A")));

        CommandStack target;
        REQUIRE(target.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
        target = std::move(*source);
        CHECK(target.count() == 1);

        CHECK_FALSE(source->canUndo());
        CHECK_FALSE(source->canRedo());
        CHECK(source->count() == 0);
        CHECK(source->appliedCount() == 0);
        CHECK(source->isClean());
        CHECK_FALSE(source->undo(ctx));
    }
}

// Code-review round (Gap 1): encodes, at the CommandStack call-sequence level, the ordering
// viewport_panel.cpp::updateGizmo must produce around a gizmo drag's RELEASE frame. ImGuizmo reports
// that frame's final delta on the SAME frame its End edge fires (translate ImGuizmo.cpp ~:2244-2249;
// scale/rotate the same shape), so the chain must close AFTER that frame's own push, never before it --
// closing first records the release frame as a second, un-merged entry, failing AC-16. The panel itself
// is unreachable from this test tier (src-private, ImGui-bound); this case is the policy-level
// regression the panel cannot host.
TEST_CASE("command_stack: a release frame's final delta merges into the drag it completes (Gap 1/AC-16)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};

    SUBCASE("correct order -- End closes AFTER the release frame's push: one entry") {
        std::vector<std::unique_ptr<CommandLog>> logs;
        logs.reserve(6);
        for (int i = 0; i < 6; ++i) {
            logs.push_back(std::make_unique<CommandLog>());
        }
        CommandStack stack;

        stack.breakMergeChain();  // Begin
        for (int i = 0; i < 5; ++i) {
            auto cmd = std::make_unique<FakeCommand>(*logs[static_cast<std::size_t>(i)], "drag");
            cmd->mergeResult = true;
            REQUIRE(stack.push(ctx, std::move(cmd)));
        }
        {
            // The release frame's OWN final delta, pushed while the chain is STILL open.
            auto cmd = std::make_unique<FakeCommand>(*logs[5], "drag");
            cmd->mergeResult = true;
            REQUIRE(stack.push(ctx, std::move(cmd)));
        }
        stack.breakMergeChain();  // End, AFTER the release frame's push

        CHECK(stack.count() == 1);
        CHECK(stack.appliedCount() == 1);
    }

    SUBCASE("broken order -- End closes BEFORE the release frame's push: two entries (documents Gap 1)") {
        std::vector<std::unique_ptr<CommandLog>> logs;
        logs.reserve(6);
        for (int i = 0; i < 6; ++i) {
            logs.push_back(std::make_unique<CommandLog>());
        }
        CommandStack stack;

        stack.breakMergeChain();  // Begin
        for (int i = 0; i < 5; ++i) {
            auto cmd = std::make_unique<FakeCommand>(*logs[static_cast<std::size_t>(i)], "drag");
            cmd->mergeResult = true;
            REQUIRE(stack.push(ctx, std::move(cmd)));
        }
        stack.breakMergeChain();  // End, BEFORE the release frame's push -- the ordering this task's
                                  // code-review round found and fixed in viewport_panel.cpp
        {
            auto cmd = std::make_unique<FakeCommand>(*logs[5], "drag");
            cmd->mergeResult = true;
            REQUIRE(stack.push(ctx, std::move(cmd)));
        }

        // The release frame's delta records as a SEPARATE entry: AC-16 fails under this ordering.
        CHECK(stack.count() == 2);
        CHECK(stack.appliedCount() == 2);
    }
}

// C20 (Phase 2 audit): `push()` step 3 ERASES the redo branch, and that erase can destroy the very
// entry the clean position denotes. `trimToCapacity` already reasons about exactly this hazard for
// the EVICTION path (C14/AC-9/E17) -- truncation is the same hazard reached from the other end, and
// nothing handled it. This is not a cosmetic flag: `isClean()` is the SOLE definition of "dirty" for
// the whole editor (D3 keeps no second copy), so a false CLEAN silently disables the window title's
// marker (editor_app.cpp), the Save item (shell_ui.cpp) AND `guardFor`'s unsaved-changes prompt
// (scene_session.cpp) -- File > New/Open, Ctrl+Q and the window [X] then discard the work with no
// prompt at all.
TEST_CASE("command_stack: a truncated redo branch invalidates the clean position (C20/AC-9/E17)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandLog logC;
    CommandLog logD;
    CommandStack stack;

    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));
    stack.setClean();  // the document on disk is "A then B applied"; clean = applied = 2
    REQUIRE(stack.isClean());

    // The user undoes past the save point, then edits again -- an ordinary sequence, not a corner.
    REQUIRE(stack.undo(ctx));
    REQUIRE(stack.undo(ctx));
    CHECK(stack.appliedCount() == 0);
    CHECK_FALSE(stack.isClean());

    // Each push truncates whatever redo branch survives. The first one destroys A and B -- the two
    // commands the clean position was counting -- so that state can never be returned to again.
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logD, "D")));

    // applied is back to 2, but 2 now counts C and D -- a document sharing nothing with the file.
    CHECK(stack.count() == 2);
    CHECK(stack.appliedCount() == 2);
    CHECK_FALSE(stack.isClean());
}

// C21 (Phase 2 audit): the same truncation must NOT invalidate a clean position that is still
// reachable. Undoing back to a save point and re-editing FORWARD of it destroys only entries the
// clean position does not depend on, so the mark must survive -- otherwise the fix for C20 would
// simply mark everything dirty forever and the asterisk would stop meaning anything.
TEST_CASE("command_stack: truncation forward of the clean position keeps it (C21/AC-9)") {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandContext ctx{world, selection, roots};
    CommandLog logA;
    CommandLog logB;
    CommandLog logC;
    CommandStack stack;

    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logA, "A")));
    stack.setClean();  // clean = 1
    stack.breakMergeChain();
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logB, "B")));  // applied = 2
    REQUIRE(stack.undo(ctx));                                           // applied = 1 == clean
    CHECK(stack.isClean());

    // Truncates B only. A -- the entry the clean position counts -- is untouched, so position 1
    // still denotes the saved document and the stack is clean again once C is undone.
    REQUIRE(stack.push(ctx, std::make_unique<FakeCommand>(logC, "C")));
    CHECK_FALSE(stack.isClean());
    REQUIRE(stack.undo(ctx));
    CHECK(stack.appliedCount() == 1);
    CHECK(stack.isClean());
}
