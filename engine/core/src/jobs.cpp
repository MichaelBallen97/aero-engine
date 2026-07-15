// Aero Engine — the job system's enkiTS backend (task 0.2.5).
// THE ONLY translation unit in the engine that may name enkiTS. The public surface is
// <aero/core/jobs.hpp>; the `lint` job's boundary step and tests/jobs_boundary_probe.cpp enforce
// that mechanically (project boundary rule, docs/04).

#include <aero/core/jobs.hpp>
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/slot_map.hpp>

#include <TaskScheduler.h>  // enkiTS — the ONLY third-party backend include in this TU (task 0.2.5)
#include <array>
#include <cassert>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace engine {
namespace {

// Worker-thread entry hook. enkiTS's ProfilerCallbackFunc is a plain `void(*)(uint32_t)`, so a
// capture-less function converts. Registered unconditionally: when profiling is off the macro is
// ((void)0) and this costs one snprintf per thread, once, at startup.
//
// 16 bytes is not arbitrary: pthread_setname_np truncates at 15 characters on Linux (and on macOS
// where Tracy takes that path), and "aero worker " + 3 digits + NUL is exactly 16. The stack buffer
// is safe because Tracy COPIES the name (tracy_malloc + memcpy — verified in
// tracy/public/common/TracySystem.cpp @ 0.13.1).
void onWorkerThreadStart(std::uint32_t threadNum) {
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "aero worker %u", threadNum);
    AERO_PROFILE_SET_THREAD_NAME(name.data());
}

// The no-throw contract (spec D11), enforced rather than documented.
// noexcept + an explicit std::terminate() — NOT a rethrow. A rethrow inside a noexcept function also
// terminates, but trips bugprone-exception-escape, which the 0.1.6 clang-tidy gate has enabled.
template <typename Fn>
void invokeJobBody(const char* name, Fn&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("job '{}' escaped an exception: {} — terminating (a job body must not throw)", name,
                          e.what());
        std::terminate();
    } catch (...) {
        AERO_LOG_CRITICAL("job '{}' escaped a non-std exception — terminating (a job body must not throw)", name);
        std::terminate();
    }
}

// One graph node.
//
// ADDRESS STABILITY IS THE WHOLE POINT of holding these by unique_ptr (spec D6): enkiTS's Dependency
// stores a raw ICompletable* and links itself into an intrusive list by address, while SlotMap stores
// T by value in a std::vector that reallocates on growth. SlotMap<JobNode> would hand enkiTS
// addresses that move — a use-after-free that only shows up once a graph outgrows capacity.
struct JobNode final : enki::ITaskSet {
    std::string name;
    JobFunction fn;

    // Edges, recorded by addDependency() as plain handles. NOT Dependency objects — see below.
    std::vector<Handle<Job>> prerequisites;

    // The real enkiTS wiring. Sized EXACTLY ONCE in submit(), then wired in place, then never
    // touched again.
    //
    // NEVER push_back into this after wiring. enkiTS's Dependency move constructor repoints the
    // intrusive list at the new object but leaves the moved-FROM object holding live pointers, so
    // that object's destructor runs ClearDependency() and DECREMENTS m_DependenciesCount on a task
    // it no longer belongs to. The dependent then fires early. A vector reallocation mid-wiring does
    // exactly this — silently, in Release only. enkiTS's own SetDependenciesVec avoids it the same
    // way: resize() first, wire second.
    std::vector<enki::Dependency> dependencies;

    JobNode(std::string_view jobName, JobFunction jobFn) : name(jobName), fn(std::move(jobFn)) {}

    void ExecuteRange(enki::TaskSetPartition, std::uint32_t) override {
        AERO_PROFILE_ZONE;
        AERO_PROFILE_ZONE_NAME(name.c_str(), name.size());
        invokeJobBody(name.c_str(), fn);
    }
};

// The implicit start node (spec D8). Its body is empty ON PURPOSE — queueing it is the entire job.
//
// Every user root depends on this, so submit() is ONE AddTaskSetToPipe call. That matters because
// AddTaskSetToPipe runs InitDependencies SYNCHRONOUSLY on the calling thread, walking the whole
// downstream graph and marking every transitive dependent (including `finish`) not-complete BEFORE
// it dispatches any work. By the time submit() returns, wait() cannot return early. Without this
// node, submit() would queue each root in turn — and root A's entire subtree could complete and fire
// the finish sentinel before root B was even queued.
//
// (enkiTS's own example/Dependencies.cpp launches its graph from a task instead, and consequently
// has to add a second dependency onto the launcher to plug precisely this race — its comment says
// so. Queueing the root directly from the submitting thread avoids needing that.)
struct JobGraphStart final : enki::ITaskSet {
    void ExecuteRange(enki::TaskSetPartition, std::uint32_t) override {}
};

// The implicit finish sentinel (spec D8). A bare ICompletable with dependencies on every leaf.
// enkiTS's own header blesses this use (TaskScheduler.h): "ICompletable is a base class used to
// check for completion. Can be used with dependencies to wait for their completion. Derive from
// ITaskSet or IPinnedTask for running parallel tasks."
// It really completes: ICompletable::OnDependenciesComplete does m_RunningCount.fetch_sub(1)
// (gc_TaskStartCount 2 -> gc_TaskAlmostCompleteCount 1) then TaskComplete(), which stores 0.
struct JobGraphFinish final : enki::ICompletable {
    std::vector<enki::Dependency> dependencies;  // sized once in submit(); see JobNode::dependencies
};

}  // namespace

struct JobGraph::Impl {
    SlotMap<std::unique_ptr<JobNode>, Job> jobs;
    std::vector<Handle<Job>> order;  // insertion order — deterministic iteration, and handle.index == position
    JobGraphStart start;
    JobGraphFinish finish;
    JobSystem* submittedTo = nullptr;
};

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;
    bool isInitialized = false;
    std::thread::id ownerThread;
};

JobGraph::JobGraph() : impl(std::make_unique<Impl>()) {}

JobGraph::~JobGraph() {
    // Destroying an in-flight graph is UB, not merely untidy: ~ICompletable asserts GetIsComplete()
    // and ~Dependency (via ClearDependency) asserts it on both endpoints. Waiting here is a safety
    // net for a caller who forgot; the assert says it IS a caller bug.
    if (impl->submittedTo != nullptr && !impl->finish.GetIsComplete()) {
        assert(false && "~JobGraph() while still in flight — call JobSystem::wait(graph) first");
        impl->submittedTo->wait(*this);
    }
}

Handle<Job> JobGraph::addJob(std::string_view name, JobFunction fn) {
    assert(impl->submittedTo == nullptr && "addJob() after submit(): the graph is frozen once submitted");
    if (impl->submittedTo != nullptr) {
        return {};  // null handle
    }
    const Handle<Job> handle = impl->jobs.insert(std::make_unique<JobNode>(name, std::move(fn)));
    impl->order.push_back(handle);
    return handle;
}

bool JobGraph::addDependency(Handle<Job> dependent, Handle<Job> prerequisite) {
    if (impl->submittedTo != nullptr) {
        return false;
    }
    if (dependent == prerequisite) {
        return false;  // a self-dependency is a 1-cycle; enkiTS asserts on it too
    }
    auto* dependentSlot = impl->jobs.get(dependent);
    auto* prerequisiteSlot = impl->jobs.get(prerequisite);
    if (dependentSlot == nullptr || prerequisiteSlot == nullptr) {
        return false;  // null, stale, or out of range
    }
    (*dependentSlot)->prerequisites.push_back(prerequisite);
    return true;
}

std::size_t JobGraph::jobCount() const noexcept { return impl->order.size(); }

bool JobGraph::submitted() const noexcept { return impl->submittedTo != nullptr; }

JobSystem::JobSystem() : impl(std::make_unique<Impl>()) {}

JobSystem::~JobSystem() { shutdown(); }

void JobSystem::initialize(const JobConfig& config) {
    assert(!impl->isInitialized && "initialize() called twice");
    if (impl->isInitialized) {
        return;
    }
    enki::TaskSchedulerConfig cfg;
    if (config.threadCount > 0) {
        // enkiTS counts the initializing thread as thread 0, so it creates threadCount-1. Zero IS a
        // legal value here despite the field's "Must be > 0" comment — that comment is a doc error:
        // enkiTS's own Initialize(1) sets this field to 0. Verified in TaskScheduler.cpp.
        cfg.numTaskThreadsToCreate = config.threadCount - 1;
    }
    cfg.profilerCallbacks.threadStart = &onWorkerThreadStart;
    impl->scheduler.Initialize(cfg);
    impl->isInitialized = true;
    impl->ownerThread = std::this_thread::get_id();
    AERO_LOG_INFO("job system initialized: {} threads (including the owning thread)",
                  impl->scheduler.GetNumTaskThreads());
}

void JobSystem::shutdown() {
    if (!impl->isInitialized) {
        return;  // idempotent
    }
    impl->scheduler.WaitforAllAndShutdown();
    impl->isInitialized = false;
    AERO_LOG_INFO("job system shut down");
}

bool JobSystem::initialized() const noexcept { return impl->isInitialized; }

// Gated on isInitialized rather than forwarded blind: enkiTS's TaskScheduler ctor does
// m_NumThreads(0), so "0 before initialize()" would hold anyway — but WaitforAllAndShutdown()
// does NOT reset m_NumThreads, so after shutdown() GetNumTaskThreads() still reports the old
// count. This gate makes jobs.hpp's documented contract true by construction. (Verified against
// enkiTS v1.12's TaskScheduler.cpp.)
std::uint32_t JobSystem::threadCount() const noexcept {
    return impl->isInitialized ? impl->scheduler.GetNumTaskThreads() : 0;
}

void JobSystem::parallelFor(std::uint32_t count, JobParallelFunction fn, std::uint32_t grainSize) {
    AERO_PROFILE_ZONE_NAMED("jobs::parallelFor");
    assert(impl->isInitialized && "parallelFor() before initialize()");
    assert(std::this_thread::get_id() == impl->ownerThread && "parallelFor() off the owning thread");
    assert(grainSize >= 1 && "grainSize must be >= 1");
    if (count == 0) {
        return;
    }
    // Local task: we block until it completes, so it outlives every worker touching it. (~ICompletable
    // asserts completion — that assert is exactly why this must not be a fire-and-forget local.)
    enki::TaskSet task(count, [&fn](enki::TaskSetPartition range, std::uint32_t worker) {
        AERO_PROFILE_ZONE_NAMED("jobs::parallelFor body");
        invokeJobBody("parallelFor", [&] { fn(JobRange{range.start, range.end}, worker); });
    });
    task.m_MinRange = grainSize;  // read at queue time by AddTaskSetToPipeInt; safe to set post-construction
    impl->scheduler.AddTaskSetToPipe(&task);
    impl->scheduler.WaitforTask(&task);
}

bool JobSystem::submit(JobGraph& graph) {
    assert(impl->isInitialized && "submit() before initialize()");
    assert(std::this_thread::get_id() == impl->ownerThread && "submit() off the owning thread");
    JobGraph::Impl& g = *graph.impl;
    if (g.submittedTo != nullptr) {
        return false;  // single-shot (D15)
    }

    // Build the dependent adjacency once; it answers BOTH "is there a cycle?" and "which nodes are
    // leaves?". Jobs are never removed from a graph, so handle.index is a dense [0, n) id.
    const std::size_t n = g.order.size();
    std::vector<std::vector<std::uint32_t>> dependents(n);
    std::vector<std::uint32_t> indegree(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const JobNode& node = **g.jobs.get(g.order[i]);
        indegree[i] = static_cast<std::uint32_t>(node.prerequisites.size());
        for (const Handle<Job> p : node.prerequisites) {
            dependents[p.index].push_back(static_cast<std::uint32_t>(i));
        }
    }

    // Kahn. A cycle is refused, not queued. enkiTS does NOT defend against one: InitDependencies
    // recurses over dependents and only skips nodes ALREADY marked, while nothing is marked on the
    // way down — so a cycle REACHABLE FROM A ROOT is unbounded mutual recursion (a stack-overflow
    // crash, not a hang), and an unreachable one silently never runs while wait() returns early.
    // Note this counts ALL n nodes, not only those reachable from a root (D9).
    std::vector<std::uint32_t> stack;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            stack.push_back(static_cast<std::uint32_t>(i));
        }
    }
    std::size_t visited = 0;
    while (!stack.empty()) {
        const std::uint32_t i = stack.back();
        stack.pop_back();
        ++visited;
        for (const std::uint32_t d : dependents[i]) {
            if (--indegree[d] == 0) {
                stack.push_back(d);
            }  // Kahn guarantees no underflow here
        }
    }
    if (visited != n) {
        AERO_LOG_ERROR(
            "JobGraph::submit rejected: the graph contains a dependency cycle ({} of {} jobs are unreachable)",
            n - visited, n);
        return false;
    }

    // PHASE 1 — size every dependency vector, wiring NOTHING. After this point no vector grows, so
    // no Dependency is ever moved. (See JobNode::dependencies for why that is mandatory.)
    std::vector<JobNode*> leaves;
    for (std::size_t i = 0; i < n; ++i) {
        JobNode& node = **g.jobs.get(g.order[i]);
        node.dependencies.resize(node.prerequisites.empty() ? 1 : node.prerequisites.size());
        if (dependents[i].empty()) {
            leaves.push_back(&node);
        }
    }
    // An EMPTY graph has no leaves — finish then depends on start, so submit/wait still terminate.
    // Uniform rule: finish depends on every node with no dependents among {start} ∪ jobs; when the
    // graph is non-empty, start has dependents (the roots) and is excluded automatically.
    g.finish.dependencies.resize(leaves.empty() ? 1 : leaves.size());

    // PHASE 2 — wire in place.
    for (std::size_t i = 0; i < n; ++i) {
        JobNode& node = **g.jobs.get(g.order[i]);
        if (node.prerequisites.empty()) {
            node.SetDependency(node.dependencies[0], &g.start);  // a root: depends on start
        } else {
            for (std::size_t d = 0; d < node.prerequisites.size(); ++d) {
                node.SetDependency(node.dependencies[d], g.jobs.get(node.prerequisites[d])->get());
            }
        }
    }
    if (leaves.empty()) {
        g.finish.SetDependency(g.finish.dependencies[0], &g.start);
    } else {
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            g.finish.SetDependency(g.finish.dependencies[i], leaves[i]);
        }
    }

    g.submittedTo = this;
    // ONE call. InitDependencies inside it marks the whole graph — including finish — not-complete
    // synchronously, before any work is dispatched. This is what makes the following wait() safe.
    impl->scheduler.AddTaskSetToPipe(&g.start);
    return true;
}

void JobSystem::wait(JobGraph& graph) {
    AERO_PROFILE_ZONE_NAMED("jobs::wait");
    assert(graph.impl->submittedTo == this && "wait() on a graph this JobSystem did not submit");
    assert(std::this_thread::get_id() == impl->ownerThread && "wait() off the owning thread");
    impl->scheduler.WaitforTask(&graph.impl->finish);
}

}  // namespace engine
