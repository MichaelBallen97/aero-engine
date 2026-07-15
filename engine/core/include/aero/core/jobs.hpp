#pragma once
// Aero Engine — job system API (task 0.2.5).
// The engine's own task-scheduling surface. The enkiTS backend is an implementation detail of
// src/jobs.cpp and never crosses this boundary (project boundary rule, docs/04); the `lint` job's
// boundary step and tests/jobs_boundary_probe.cpp enforce that mechanically.
//
// THREADING CONTRACT (enkiTS's, surfaced here — read before using):
//   * Create exactly ONE JobSystem per process. Each one spawns its own thread pool; two of them
//     oversubscribe the CPU. Nothing structurally prevents a second — this is a contract (D2).
//   * The thread that calls initialize() is the OWNING thread (enkiTS's "thread 0"). Every other
//     call — parallelFor, submit, wait, and ~JobGraph — must happen on THAT thread. Asserted in
//     debug builds.
//   * Consequently, a job body must NOT call back into the JobSystem: no nested parallelFor, no
//     submit from inside a job. enkiTS itself permits this (its rule is "the owning thread or
//     inside a task"), but nesting also lets you deadlock by waiting on anything that is not your
//     own child, so this API takes the stricter, checkable contract until a consumer needs more.
//   * A JobGraph must NOT outlive the JobSystem it was submitted to.
//   * Job bodies must NOT throw: an escaping exception is logged and then terminates the process
//     (there is no way to unwind a worker thread meaningfully). See src/jobs.cpp.

#include <aero/core/handle.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace engine {

// The half-open index range [begin, end) handed to one invocation of a parallelFor body.
struct JobRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

// Tag for Handle<Job>. DELIBERATELY NEVER DEFINED: Handle<Tag> uses Tag only as a phantom type (it
// stores no Tag), so an incomplete type is legal here — and that is what keeps JobNode, which derives
// from an enkiTS class, out of this header entirely.
struct Job;

// A graph node's body. Runs once, on an unspecified worker thread.
using JobFunction = std::function<void()>;

// A parallelFor body. Called once per partition with a distinct sub-range; `worker` is the calling
// thread's index in [0, threadCount()), intended ONLY for per-thread output buckets — never for
// varying what the computation does.
using JobParallelFunction = std::function<void(JobRange range, std::uint32_t worker)>;

struct JobConfig {
    // Total threads that may run jobs, INCLUDING the calling thread — enkiTS's own model, where the
    // thread that initializes the scheduler is thread 0 and also runs work while waiting.
    //   0 => one per hardware thread (the default).
    //   1 => no extra threads at all; everything runs inline on the owning thread during waits.
    std::uint32_t threadCount = 0;
};

class JobSystem;

// A DAG of jobs, built up front and submitted as one unit.
//
// WHY BUILD-THEN-SUBMIT, and not "add a job and it starts": enkiTS refuses to wire a dependency onto
// a task that is already in flight (it asserts both tasks are complete). Under NDEBUG that assert is
// gone and the dependent SILENTLY NEVER RUNS. Deferring all wiring to submit() makes that class of
// bug unrepresentable rather than merely documented (spec D4/D5).
class JobGraph {
public:
    JobGraph();
    ~JobGraph();

    // Neither copyable nor movable: submit() hands enkiTS raw addresses of this graph's internals.
    JobGraph(const JobGraph&) = delete;
    JobGraph& operator=(const JobGraph&) = delete;
    JobGraph(JobGraph&&) = delete;
    JobGraph& operator=(JobGraph&&) = delete;

    // Add a job. `name` is copied; it names the Tracy zone, and identifies the job in the crash log
    // if the body throws and in the diagnostic if submit() finds a cycle.
    // Returns a handle valid for this graph only. Must be called before submit().
    [[nodiscard]] Handle<Job> addJob(std::string_view name, JobFunction fn);

    // Declare "`dependent` runs only after `prerequisite` completes". Both handles must come from
    // THIS graph. Must be called before submit().
    // Returns false (and changes nothing) if either handle is null/stale, if they are equal (a
    // self-dependency), or if the graph is already submitted. Duplicate edges are permitted and
    // behave identically to a single edge.
    bool addDependency(Handle<Job> dependent, Handle<Job> prerequisite);

    [[nodiscard]] std::size_t jobCount() const noexcept;
    [[nodiscard]] bool submitted() const noexcept;

private:
    friend class JobSystem;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Owns the worker threads. Create exactly one; see the threading contract above.
class JobSystem {
public:
    JobSystem();
    ~JobSystem();  // shuts down if still initialized

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Spawns the pool and makes the calling thread the owning thread. Asserts if already
    // initialized (and then does nothing).
    void initialize(const JobConfig& config = {});

    // Waits for all outstanding work, then joins the pool. Idempotent; called by ~JobSystem().
    void shutdown();

    [[nodiscard]] bool initialized() const noexcept;

    // Threads that may run jobs, INCLUDING the owning thread. 0 before initialize().
    [[nodiscard]] std::uint32_t threadCount() const noexcept;

    // Run `fn` over [0, count), split across the pool. BLOCKS until every index has been visited.
    // `count == 0` returns immediately. `grainSize` is the minimum partition size — enkiTS's "grain
    // size": raise it until one partition is worth at least ~10k clock cycles, or scheduling
    // overhead dominates. The last partition may be smaller than `grainSize`.
    // `grainSize` MUST be >= 1. Asserted in debug; a 0 reaches enkiTS's partitioner as a divisor
    // and faults the process in release.
    void parallelFor(std::uint32_t count, JobParallelFunction fn, std::uint32_t grainSize = 1);

    // Wire the graph's dependencies and queue it. Non-blocking.
    // Returns false (having done nothing) if the graph is already submitted, or if its edges contain
    // a CYCLE. A cycle is refused rather than queued because enkiTS does not defend against one:
    // reachable from a root it is unbounded mutual recursion while dependencies are initialized —
    // a stack-overflow crash, not a hang — and unreachable it silently never runs while wait()
    // returns early. Neither is survivable, so submit() rejects the graph instead.
    bool submit(JobGraph& graph);

    // Block until every job in `graph` has completed. The calling thread runs jobs while it waits.
    // The graph must have been submitted to THIS JobSystem.
    void wait(JobGraph& graph);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace engine
