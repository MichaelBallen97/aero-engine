// Exercises the job system (task 0.2.5): parallelFor, job graphs and their dependency order,
// cycle rejection, handle validation, thread-count configuration, and RAII.
//
// Every case creates its OWN JobSystem: D2 makes that cheap, and doctest runs each case on the
// main thread, which becomes enkiTS's thread 0 — exactly the threading contract jobs.hpp documents.
//
// NO SLEEPS ANYWHERE. Ordering is proven with atomic order stamps taken inside job bodies: a
// dependent's body cannot start until its prerequisite's body has returned, so a strict `<` on the
// stamps is a real proof of order rather than a timing guess. Nothing here asserts a doctest CHECK
// from inside a job body — results are accumulated into atomics and checked on the main thread.

#include <aero/core/jobs.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

namespace {

// Burns a bounded, non-elidable amount of work. Used only to make the long branch of the
// multi-root case reliably outlive the short one (see the AC-6 case) — never to create a timing
// dependency: correctness there rests on wait(), not on this taking any particular duration.
//
// `std::ignore = graph.addJob(...)` appears wherever a case genuinely has no use for the returned
// handle: addJob() is [[nodiscard]], and silently dropping the result would warn.
void burnWork(std::atomic<std::uint64_t>& sink) {
    for (int k = 0; k < 20000; ++k) {
        sink.fetch_add(static_cast<std::uint64_t>(k), std::memory_order_relaxed);
    }
}

}  // namespace

// ---- parallelFor ---------------------------------------------------------------------------

TEST_CASE("jobs: parallelFor visits every index exactly once") {
    engine::JobSystem system;
    system.initialize();

    constexpr std::uint32_t N = 4096;
    std::vector<std::atomic<int>> visits(N);
    for (auto& visit : visits) {
        visit.store(0, std::memory_order_relaxed);
    }
    std::atomic<std::uint64_t> sum{0};

    system.parallelFor(N, [&](engine::JobRange range, std::uint32_t) {
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            visits[i].fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(i, std::memory_order_relaxed);
        }
    });

    bool everyIndexOnce = true;
    for (std::uint32_t i = 0; i < N; ++i) {
        if (visits[i].load(std::memory_order_relaxed) != 1) {
            everyIndexOnce = false;
        }
    }
    CHECK(everyIndexOnce);
    // The sum reduction independently proves no index was skipped or double-counted.
    CHECK(sum.load() == static_cast<std::uint64_t>(N) * (N - 1) / 2);
}

TEST_CASE("jobs: parallelFor with count == 0 is a no-op") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<int> calls{0};
    system.parallelFor(0, [&](engine::JobRange, std::uint32_t) { calls.fetch_add(1, std::memory_order_relaxed); });

    CHECK(calls.load() == 0);
}

TEST_CASE("jobs: parallelFor with count == 1 invokes the body once") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<int> calls{0};
    std::atomic<std::uint32_t> seenBegin{~0U};
    std::atomic<std::uint32_t> seenEnd{~0U};
    system.parallelFor(1, [&](engine::JobRange range, std::uint32_t) {
        calls.fetch_add(1, std::memory_order_relaxed);
        seenBegin.store(range.begin, std::memory_order_relaxed);
        seenEnd.store(range.end, std::memory_order_relaxed);
    });

    CHECK(calls.load() == 1);
    CHECK(seenBegin.load() == 0);
    CHECK(seenEnd.load() == 1);
}

TEST_CASE("jobs: parallelFor honours grainSize") {
    engine::JobSystem system;
    system.initialize();

    // grainSize == count forces exactly ONE partition. This is the only partition count that is
    // deterministic: for a default grainSize enkiTS derives partitions from the thread count.
    constexpr std::uint32_t N = 512;
    std::atomic<int> partitions{0};
    std::atomic<std::uint32_t> seenBegin{~0U};
    std::atomic<std::uint32_t> seenEnd{~0U};
    system.parallelFor(
        N,
        [&](engine::JobRange range, std::uint32_t) {
            partitions.fetch_add(1, std::memory_order_relaxed);
            seenBegin.store(range.begin, std::memory_order_relaxed);
            seenEnd.store(range.end, std::memory_order_relaxed);
        },
        N);

    CHECK(partitions.load() == 1);
    CHECK(seenBegin.load() == 0);
    CHECK(seenEnd.load() == N);
}

TEST_CASE("jobs: parallelFor worker index is in range") {
    engine::JobSystem system;
    system.initialize();

    // NOTE: worker 0 is legal and expected — enkiTS's thread 0 IS the calling thread, which runs
    // partitions while it waits. Never assert worker != 0.
    std::atomic<std::uint32_t> maxWorker{0};
    system.parallelFor(4096, [&](engine::JobRange, std::uint32_t worker) {
        std::uint32_t previous = maxWorker.load(std::memory_order_relaxed);
        while (worker > previous && !maxWorker.compare_exchange_weak(previous, worker, std::memory_order_relaxed)) {
        }
    });

    CHECK(maxWorker.load() < system.threadCount());
}

// ---- job graphs: ordering ------------------------------------------------------------------

TEST_CASE("jobs: a diamond graph runs in dependency order") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<int> clock{0};
    std::atomic<int> stampA{-1};
    std::atomic<int> stampB{-1};
    std::atomic<int> stampC{-1};
    std::atomic<int> stampD{-1};

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [&] { stampA.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    const auto b = graph.addJob("B", [&] { stampB.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    const auto c = graph.addJob("C", [&] { stampC.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    const auto d = graph.addJob("D", [&] { stampD.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    CHECK(graph.addDependency(b, a));
    CHECK(graph.addDependency(c, a));
    CHECK(graph.addDependency(d, b));
    CHECK(graph.addDependency(d, c));

    REQUIRE(system.submit(graph));
    system.wait(graph);

    CHECK(stampA.load() < stampB.load());
    CHECK(stampA.load() < stampC.load());
    CHECK(stampB.load() < stampD.load());
    CHECK(stampC.load() < stampD.load());
}

TEST_CASE("jobs: wait() waits for BOTH roots of a multi-root graph") {
    engine::JobSystem system;
    system.initialize();

    // AC-6 — THE case that fails without D8's implicit start node. Two INDEPENDENT roots with
    // deliberately UNEQUAL chain lengths (1 vs 8, the long one doing real work): a submit() that
    // queued roots one at a time could let the short chain complete and fire the finish sentinel
    // before the long chain was even queued, so wait() would return with longTailRan still false.
    // Equal-length chains would let a broken wait() pass by luck.
    std::atomic<bool> shortTailRan{false};
    std::atomic<bool> longTailRan{false};
    std::atomic<std::uint64_t> sink{0};

    engine::JobGraph graph;
    std::ignore = graph.addJob("short-root", [&] { shortTailRan.store(true, std::memory_order_relaxed); });

    constexpr int LONG_CHAIN = 8;
    engine::Handle<engine::Job> previous{};
    for (int i = 0; i < LONG_CHAIN; ++i) {
        const bool isTail = (i == LONG_CHAIN - 1);
        const auto handle = graph.addJob("long-chain", [&, isTail] {
            burnWork(sink);
            if (isTail) {
                longTailRan.store(true, std::memory_order_relaxed);
            }
        });
        if (previous.valid()) {
            CHECK(graph.addDependency(handle, previous));
        }
        previous = handle;
    }

    REQUIRE(system.submit(graph));
    system.wait(graph);

    CHECK(shortTailRan.load());
    CHECK(longTailRan.load());
}

TEST_CASE("jobs: a chain of 64 jobs runs in order") {
    engine::JobSystem system;
    system.initialize();

    constexpr int CHAIN = 64;
    std::atomic<int> clock{0};
    std::vector<std::atomic<int>> stamps(CHAIN);
    for (auto& stamp : stamps) {
        stamp.store(-1, std::memory_order_relaxed);
    }

    engine::JobGraph graph;
    engine::Handle<engine::Job> previous{};
    for (int i = 0; i < CHAIN; ++i) {
        const auto handle = graph.addJob("chain", [&stamps, &clock, i] {
            stamps[static_cast<std::size_t>(i)].store(clock.fetch_add(1, std::memory_order_relaxed));
        });
        if (previous.valid()) {
            CHECK(graph.addDependency(handle, previous));
        }
        previous = handle;
    }

    REQUIRE(system.submit(graph));
    system.wait(graph);

    // A strict chain forces a total order: job i must carry stamp i exactly.
    bool strictSequence = true;
    for (int i = 0; i < CHAIN; ++i) {
        if (stamps[static_cast<std::size_t>(i)].load() != i) {
            strictSequence = false;
        }
    }
    CHECK(strictSequence);
}

// ---- job graphs: edges of the shape space --------------------------------------------------

TEST_CASE("jobs: an empty graph submits and waits") {
    engine::JobSystem system;
    system.initialize();

    // D8's uniform rule: with no leaves, finish depends on start, so both still terminate.
    engine::JobGraph graph;
    CHECK(graph.jobCount() == 0);
    REQUIRE(system.submit(graph));
    system.wait(graph);  // must return rather than hang
    CHECK(graph.submitted());
}

TEST_CASE("jobs: a graph with one isolated job runs it") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<bool> ran{false};
    engine::JobGraph graph;
    std::ignore = graph.addJob("solo", [&] { ran.store(true, std::memory_order_relaxed); });

    REQUIRE(system.submit(graph));
    system.wait(graph);

    CHECK(ran.load());
}

TEST_CASE("jobs: threadCount == 1 completes a graph") {
    engine::JobSystem system;
    system.initialize(engine::JobConfig{.threadCount = 1});
    REQUIRE(system.threadCount() == 1);

    // E5: zero extra threads. Everything runs inline on the owning thread during wait() — the
    // configuration most likely to deadlock if the wiring were wrong.
    std::atomic<int> clock{0};
    std::atomic<int> stampA{-1};
    std::atomic<int> stampB{-1};

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [&] { stampA.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    const auto b = graph.addJob("B", [&] { stampB.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    CHECK(graph.addDependency(b, a));

    REQUIRE(system.submit(graph));
    system.wait(graph);

    CHECK(stampA.load() == 0);
    CHECK(stampB.load() == 1);
}

TEST_CASE("jobs: threadCount == 1 completes a parallelFor") {
    engine::JobSystem system;
    system.initialize(engine::JobConfig{.threadCount = 1});
    REQUIRE(system.threadCount() == 1);

    constexpr std::uint32_t N = 256;
    std::vector<std::atomic<int>> visits(N);
    for (auto& visit : visits) {
        visit.store(0, std::memory_order_relaxed);
    }

    system.parallelFor(N, [&](engine::JobRange range, std::uint32_t) {
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            visits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });

    bool everyIndexOnce = true;
    for (std::uint32_t i = 0; i < N; ++i) {
        if (visits[i].load(std::memory_order_relaxed) != 1) {
            everyIndexOnce = false;
        }
    }
    CHECK(everyIndexOnce);
}

TEST_CASE("jobs: threadCount() reports the configured total") {
    engine::JobSystem system;

    // D14: the count INCLUDES the owning thread. V7: gated on isInitialized, because enkiTS's
    // WaitforAllAndShutdown() does not reset its own thread count.
    CHECK_FALSE(system.initialized());
    CHECK(system.threadCount() == 0);

    system.initialize(engine::JobConfig{.threadCount = 4});
    CHECK(system.initialized());
    CHECK(system.threadCount() == 4);

    system.shutdown();
    CHECK_FALSE(system.initialized());
    CHECK(system.threadCount() == 0);
}

// ---- rejection: cycles ---------------------------------------------------------------------

TEST_CASE("jobs: submit() rejects a 2-cycle") {
    engine::JobSystem system;
    system.initialize();

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});
    const auto b = graph.addJob("B", [] {});
    CHECK(graph.addDependency(b, a));  // A -> B
    CHECK(graph.addDependency(a, b));  // B -> A: closes the cycle

    CHECK_FALSE(system.submit(graph));
    CHECK_FALSE(graph.submitted());
}

TEST_CASE("jobs: submit() rejects a 3-cycle") {
    engine::JobSystem system;
    system.initialize();

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});
    const auto b = graph.addJob("B", [] {});
    const auto c = graph.addJob("C", [] {});
    CHECK(graph.addDependency(b, a));  // A -> B
    CHECK(graph.addDependency(c, b));  // B -> C
    CHECK(graph.addDependency(a, c));  // C -> A: closes the cycle

    CHECK_FALSE(system.submit(graph));
    CHECK_FALSE(graph.submitted());
}

TEST_CASE("jobs: submit() rejects a cycle with a valid root attached") {
    engine::JobSystem system;
    system.initialize();

    // Kahn must count ALL n nodes, not only those reachable from a root. R is a perfectly good
    // root, but A and B form a cycle hanging off it. This is not a hang if it slipped through:
    // InitDependencies recurses over dependents marking nodes only on the way back up, so a cycle
    // reachable from a root is unbounded mutual recursion — a stack-overflow CRASH (V6).
    engine::JobGraph graph;
    const auto r = graph.addJob("R", [] {});
    const auto a = graph.addJob("A", [] {});
    const auto b = graph.addJob("B", [] {});
    CHECK(graph.addDependency(a, r));  // R -> A
    CHECK(graph.addDependency(b, a));  // A -> B
    CHECK(graph.addDependency(a, b));  // B -> A: closes the cycle

    CHECK_FALSE(system.submit(graph));
    CHECK_FALSE(graph.submitted());
}

// ---- rejection: handles and the frozen graph -----------------------------------------------

TEST_CASE("jobs: submit() twice returns false") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<int> ran{0};
    engine::JobGraph graph;
    std::ignore = graph.addJob("once", [&] { ran.fetch_add(1, std::memory_order_relaxed); });

    REQUIRE(system.submit(graph));
    CHECK_FALSE(system.submit(graph));  // single-shot (D15)
    system.wait(graph);

    CHECK(ran.load() == 1);
}

TEST_CASE("jobs: addDependency rejects a self-dependency") {
    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});

    CHECK_FALSE(graph.addDependency(a, a));
}

TEST_CASE("jobs: addDependency rejects a null handle") {
    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});
    const engine::Handle<engine::Job> null{};

    CHECK_FALSE(null.valid());
    CHECK_FALSE(graph.addDependency(a, null));
    CHECK_FALSE(graph.addDependency(null, a));
}

TEST_CASE("jobs: addDependency rejects a stale handle") {
    // E9 documents the limit precisely: two graphs independently mint {index:0, generation:1}, so
    // an IN-RANGE foreign handle is indistinguishable and WILL be accepted. Only an out-of-range
    // (or null/stale) handle is detectable — that is what this asserts, and nothing stronger.
    engine::JobGraph other;
    engine::Handle<engine::Job> foreign{};
    for (int i = 0; i < 5; ++i) {
        foreign = other.addJob("foreign", [] {});
    }
    REQUIRE(foreign.valid());
    REQUIRE(foreign.index == 4);

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});  // graph holds exactly one job: index 0

    CHECK_FALSE(graph.addDependency(a, foreign));
    CHECK_FALSE(graph.addDependency(foreign, a));
}

TEST_CASE("jobs: addDependency after submit returns false") {
    engine::JobSystem system;
    system.initialize();

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [] {});
    const auto b = graph.addJob("B", [] {});

    REQUIRE(system.submit(graph));
    CHECK_FALSE(graph.addDependency(b, a));  // the graph is frozen once submitted
    system.wait(graph);
}

TEST_CASE("jobs: duplicate edges behave as one") {
    engine::JobSystem system;
    system.initialize();

    // E15: two Dependency objects, m_DependenciesCount == 2, and both fire — correct, just
    // wasteful. The observable behaviour must be identical to a single edge.
    std::atomic<int> clock{0};
    std::atomic<int> stampA{-1};
    std::atomic<int> stampB{-1};
    std::atomic<int> ranA{0};
    std::atomic<int> ranB{0};

    engine::JobGraph graph;
    const auto a = graph.addJob("A", [&] {
        ranA.fetch_add(1, std::memory_order_relaxed);
        stampA.store(clock.fetch_add(1, std::memory_order_relaxed));
    });
    const auto b = graph.addJob("B", [&] {
        ranB.fetch_add(1, std::memory_order_relaxed);
        stampB.store(clock.fetch_add(1, std::memory_order_relaxed));
    });
    CHECK(graph.addDependency(b, a));
    CHECK(graph.addDependency(b, a));  // the same edge again

    REQUIRE(system.submit(graph));
    system.wait(graph);

    CHECK(ranA.load() == 1);
    CHECK(ranB.load() == 1);
    CHECK(stampA.load() < stampB.load());
}

// ---- accessors and RAII --------------------------------------------------------------------

TEST_CASE("jobs: jobCount() and submitted() report correctly") {
    engine::JobSystem system;
    system.initialize();

    engine::JobGraph graph;
    CHECK(graph.jobCount() == 0);
    CHECK_FALSE(graph.submitted());

    std::ignore = graph.addJob("A", [] {});
    std::ignore = graph.addJob("B", [] {});
    CHECK(graph.jobCount() == 2);
    CHECK_FALSE(graph.submitted());

    REQUIRE(system.submit(graph));
    CHECK(graph.submitted());
    CHECK(graph.jobCount() == 2);

    system.wait(graph);
}

TEST_CASE("jobs: shutdown() is idempotent") {
    engine::JobSystem system;
    system.initialize();
    CHECK(system.initialized());

    system.shutdown();
    system.shutdown();  // second call must be a no-op, not a crash

    CHECK_FALSE(system.initialized());
}

TEST_CASE("jobs: ~JobSystem() without shutdown() is clean") {
    // Declaration order matters (E8): the graph is declared AFTER the system, so it is destroyed
    // FIRST — a graph must never outlive the JobSystem it was submitted to.
    std::atomic<int> ran{0};
    {
        engine::JobSystem system;
        system.initialize(engine::JobConfig{.threadCount = 2});

        engine::JobGraph graph;
        std::ignore = graph.addJob("J", [&] { ran.fetch_add(1, std::memory_order_relaxed); });
        REQUIRE(system.submit(graph));
        system.wait(graph);
    }  // ~JobGraph then ~JobSystem (which shuts down): ASan proves no leak

    CHECK(ran.load() == 1);
}

TEST_CASE("jobs: ~JobGraph() after wait() is clean") {
    engine::JobSystem system;
    system.initialize();

    std::atomic<int> ran{0};
    {
        engine::JobGraph graph;
        std::ignore = graph.addJob("J", [&] { ran.fetch_add(1, std::memory_order_relaxed); });
        REQUIRE(system.submit(graph));
        system.wait(graph);
    }  // D10's happy path: finish is complete, so ~JobGraph neither asserts nor waits

    CHECK(ran.load() == 1);
}

// ---- integration ---------------------------------------------------------------------------

TEST_CASE("jobs: a 64-node fan-out/fan-in graph runs every body exactly once") {
    engine::JobSystem system;
    system.initialize();

    constexpr int FANOUT = 64;
    std::atomic<int> clock{0};
    std::atomic<int> stampRoot{-1};
    std::atomic<int> stampSink{-1};
    std::vector<std::atomic<int>> ran(FANOUT);
    for (auto& counter : ran) {
        counter.store(0, std::memory_order_relaxed);
    }

    engine::JobGraph graph;
    const auto root = graph.addJob("root", [&] { stampRoot.store(clock.fetch_add(1, std::memory_order_relaxed)); });
    const auto sink = graph.addJob("sink", [&] { stampSink.store(clock.fetch_add(1, std::memory_order_relaxed)); });

    for (int i = 0; i < FANOUT; ++i) {
        const auto middle = graph.addJob(
            "middle", [&ran, i] { ran[static_cast<std::size_t>(i)].fetch_add(1, std::memory_order_relaxed); });
        CHECK(graph.addDependency(middle, root));
        CHECK(graph.addDependency(sink, middle));
    }

    REQUIRE(system.submit(graph));
    system.wait(graph);

    bool everyBodyOnce = true;
    for (int i = 0; i < FANOUT; ++i) {
        if (ran[static_cast<std::size_t>(i)].load() != 1) {
            everyBodyOnce = false;
        }
    }
    CHECK(everyBodyOnce);
    // The root must precede the sink transitively, through all 64 middles.
    CHECK(stampRoot.load() < stampSink.load());
}
