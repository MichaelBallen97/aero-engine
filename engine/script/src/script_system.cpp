// Aero Engine -- engine::script::ScriptSystem (task 4.4.1): the script lifecycle over one World. The contract is
// script_system.hpp's. This file is quickjs-free: it drives the Vm through its public API alone.
//
// update() is six phases, in this order, each its own function:
//   1. the snapshot of the World's ScriptComponents, sorted by entity index, taken before any script runs;
//   2. detach: onDestroy (when onInit succeeded) and destroy, for an instance whose entity left or was renamed;
//   3. attach: resolve each Guid once, load each path once, create one instance per new entity;
//   4. init: onInit of every instance that has not run it, in attach order;
//   5. update: onUpdate(dt) of every initialised instance, in attach order;
//   6. flush: every entity engine.destroy queued, with its subtree: onDestroy of each attached instance in it, in run
//      order and with its entity alive, then the Vm destroys the subtree (task 4.4.2).
// The entity key is ONE function (entityKey), used by every lookup; there is no per-entity linear search.
// A failed call is remembered, not retried (script_system.hpp holds the policy); an Interrupted one suspends its script
// and is followed by settle(), one empty evaluation named with the script's path.

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/scene/component.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script_system.hpp>
#include <aero/script/vm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script {

std::string_view scriptEntityStateLabel(ScriptEntityState state) noexcept {
    switch (state) {
        case ScriptEntityState::None:
            return "None";
        case ScriptEntityState::Unresolved:
            return "Unresolved";
        case ScriptEntityState::LoadFailed:
            return "LoadFailed";
        case ScriptEntityState::AttachFailed:
            return "AttachFailed";
        case ScriptEntityState::PendingInit:
            return "PendingInit";
        case ScriptEntityState::Running:
            return "Running";
        case ScriptEntityState::Faulted:
            return "Faulted";
        case ScriptEntityState::Suspended:
            return "Suspended";
    }
    return "Unknown";
}

namespace {

// The one function every entity lookup goes through.
[[nodiscard]] constexpr std::uint64_t entityKey(Entity entity) noexcept {
    return (std::uint64_t{entity.index} << 32U) | entity.generation;
}

}  // namespace

struct ScriptSystem::Impl {
    // One snapshot row: a live entity naming a script.
    struct Snapshot {
        Entity entity;
        Guid script;
    };
    // The resolver's answer for one Guid; queried once.
    struct ScriptMemo {
        std::string path;
        bool unresolved = false;
    };
    // What the system knows of one module path.
    struct PathMemo {
        bool loaded = false;
        bool loadFailed = false;
        bool suspended = false;
    };
    // One attached instance, in attach order.
    struct Attached {
        Entity entity;
        Guid script;
        std::string path;
        ScriptInstance instance;
        bool initDone = false;
        bool faulted = false;
    };

    Impl(Vm& vmRef, World& worldRef, ScriptResolver resolverFn)
        : vm(vmRef), world(worldRef), resolver(std::move(resolverFn)) {}

    Vm& vm;
    World& world;
    ScriptResolver resolver;
    bool bound = false;
    bool stopped = false;
    std::map<Guid, ScriptMemo> scripts;
    std::map<std::string, PathMemo, std::less<>> paths;
    std::vector<Attached> runOrder;
    std::map<std::uint64_t, std::size_t> attachedAt;  // entity key -> index into runOrder
    std::map<std::uint64_t, Guid> attachFailures;     // entity key -> the Guid its attach failed with

    [[nodiscard]] bool pathSuspended(const std::string& path) const {
        const auto found = paths.find(path);
        return found != paths.end() && found->second.suspended;
    }

    // The entity is alive and still names the instance's script.
    [[nodiscard]] bool entityNames(const Attached& attached) const {
        const auto* const component = std::as_const(world).get<ScriptComponent>(attached.entity);
        return component != nullptr && component->script == attached.script;
    }

    // One empty evaluation named with `path`, its status ignored: after an interrupted call, a reaction job the budget
    // cut short is still queued, and the backend's refusal of it would otherwise surface in the next call, whichever
    // instance that is (R34). Running it here makes the record name the script it came from.
    void settle(const std::string& path) { static_cast<void>(vm.evaluateScript("", path)); }

    // The script's instances: it stops running any hook until it reloads. One notice; the settle call follows.
    void suspend(const std::string& path) {
        PathMemo& memo = paths[path];
        if (memo.suspended) {
            settle(path);
            return;
        }
        memo.suspended = true;
        std::size_t held = 0;
        for (const Attached& attached : runOrder) {
            held += attached.path == path ? 1U : 0U;
        }
        AERO_LOG_WARN(
            "script: {}: suspended: a hook ran past its {} ms time budget; its {} instances run no hook until the "
            "script reloads",
            path, vm.callTimeBudget().count(), held);
        settle(path);
    }

    // One hook call. True when it ended Ok and did not reject. An Interrupted call suspends the script (it is not a
    // failure of the instance: the instance resumes when the script reloads).
    [[nodiscard]] bool callHook(const Attached& attached, ScriptHook hook, double dt) {
        const ScriptHookResult result = vm.callHook(attached.instance, hook, dt);
        if (result.status == ScriptStatus::Interrupted) {
            suspend(attached.path);
            return false;
        }
        return result.status == ScriptStatus::Ok && !result.rejected;
    }

    void reindex() {
        attachedAt.clear();
        for (std::size_t i = 0; i < runOrder.size(); ++i) {
            attachedAt[entityKey(runOrder[i].entity)] = i;
        }
    }

    [[nodiscard]] std::vector<Snapshot> takeSnapshot() {
        std::vector<Snapshot> rows;
        world.each<ScriptComponent>([&rows](Entity entity, ScriptComponent& component) {
            if (component.script.valid()) {
                rows.push_back(Snapshot{entity, component.script});
            }
        });
        std::sort(rows.begin(), rows.end(),
                  [](const Snapshot& a, const Snapshot& b) { return a.entity.index < b.entity.index; });
        return rows;
    }

    // onDestroy when onInit succeeded and the path is not suspended, then the instance is destroyed whatever the hook
    // did.
    void release(Attached& attached, ScriptSystemReport& report) {
        if (attached.initDone && !pathSuspended(attached.path)) {
            static_cast<void>(callHook(attached, ScriptHook::Destroy, 0.0));
        }
        static_cast<void>(vm.destroyInstance(attached.instance));
        ++report.detached;
    }

    // Phase 2. The releases run over the run order as it stood when the phase began, so an interrupted onDestroy counts
    // every instance of its path; the compaction follows.
    void detachGone(const std::map<std::uint64_t, Guid>& present, ScriptSystemReport& report) {
        const auto isKept = [&present](const Attached& attached) {
            const auto found = present.find(entityKey(attached.entity));
            return found != present.end() && found->second == attached.script;
        };
        for (Attached& attached : runOrder) {
            if (!isKept(attached)) {
                release(attached, report);
            }
        }
        const std::size_t before = runOrder.size();
        runOrder.erase(std::remove_if(runOrder.begin(), runOrder.end(),
                                      [&isKept](const Attached& attached) { return !isKept(attached); }),
                       runOrder.end());
        if (runOrder.size() != before) {
            reindex();
        }
        for (auto it = attachFailures.begin(); it != attachFailures.end();) {
            const auto found = present.find(it->first);
            if (found == present.end() || !(found->second == it->second)) {
                it = attachFailures.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The path the resolver names for `script`, asking once; null when it does not know the script.
    [[nodiscard]] const std::string* resolvePath(const Guid& script) {
        auto found = scripts.find(script);
        if (found == scripts.end()) {
            std::string path = resolver ? resolver(script) : std::string{};
            const bool unknown = path.empty();
            found = scripts.emplace(script, ScriptMemo{std::move(path), unknown}).first;
        }
        return found->second.unresolved ? nullptr : &found->second.path;
    }

    // Loads a path once; a refusal is remembered and never retried.
    [[nodiscard]] bool loadPath(const std::string& path, PathMemo& memo) {
        const ScriptLoadResult loaded = vm.loadScript(path);
        if (loaded.status == ScriptStatus::Ok || loaded.status == ScriptStatus::Unchanged) {
            memo.loaded = true;
            return true;
        }
        memo.loadFailed = true;
        if (loaded.status == ScriptStatus::Interrupted) {
            settle(path);
        }
        return false;
    }

    void attachOne(const Snapshot& row, ScriptSystemReport& report) {
        const std::string* const resolved = resolvePath(row.script);
        if (resolved == nullptr) {
            return;
        }
        PathMemo& memo = paths[*resolved];
        if (memo.suspended || memo.loadFailed || (!memo.loaded && !loadPath(*resolved, memo))) {
            return;
        }
        const ScriptInstanceResult made = vm.createInstance(*resolved, row.entity);
        if (made.status == ScriptStatus::Interrupted) {
            suspend(*resolved);
            return;
        }
        if (made.status != ScriptStatus::Ok) {
            attachFailures[entityKey(row.entity)] = row.script;
            return;
        }
        attachedAt[entityKey(row.entity)] = runOrder.size();
        runOrder.push_back(Attached{row.entity, row.script, *resolved, made.instance, false, false});
        ++report.attached;
    }

    // Phase 3.
    void attachNew(const std::vector<Snapshot>& rows, ScriptSystemReport& report) {
        for (const Snapshot& row : rows) {
            const std::uint64_t key = entityKey(row.entity);
            if (attachedAt.contains(key)) {
                continue;
            }
            const auto failed = attachFailures.find(key);
            if (failed != attachFailures.end() && failed->second == row.script) {
                continue;
            }
            attachOne(row, report);
        }
    }

    // Phase 4.
    void runInits(ScriptSystemReport& report) {
        for (Attached& attached : runOrder) {
            if (attached.initDone || attached.faulted || pathSuspended(attached.path) || !entityNames(attached)) {
                continue;
            }
            if (callHook(attached, ScriptHook::Init, 0.0)) {
                attached.initDone = true;
                ++report.initialised;
            } else if (!pathSuspended(attached.path)) {
                attached.faulted = true;
                ++report.faulted;
            }
        }
    }

    // Phase 5.
    void runUpdates(double dt, ScriptSystemReport& report) {
        for (Attached& attached : runOrder) {
            if (!attached.initDone || attached.faulted || pathSuspended(attached.path) || !entityNames(attached)) {
                continue;
            }
            ++report.updated;
            if (!callHook(attached, ScriptHook::Update, dt) && !pathSuspended(attached.path)) {
                attached.faulted = true;
                ++report.faulted;
            }
        }
    }

    // Every entity key of the subtree below `root`, `root` included. An explicit stack: the walk is not recursive.
    [[nodiscard]] std::set<std::uint64_t> subtreeKeys(Entity root) const {
        std::set<std::uint64_t> keys;
        std::vector<Entity> pending{root};
        while (!pending.empty()) {
            const Entity next = pending.back();
            pending.pop_back();
            keys.insert(entityKey(next));
            world.eachChild(next, [&pending](Entity child) { pending.push_back(child); });
        }
        return keys;
    }

    // True when `root` is `entity` or one of its ancestors, read from the World as it stands now.
    [[nodiscard]] bool isUnder(Entity entity, Entity root) const {
        for (Entity at = entity; at.valid(); at = world.parent(at)) {
            if (at == root) {
                return true;
            }
        }
        return false;
    }

    // Erases the entries `released` marks, in run order, then rebuilds the index when any went.
    void compactReleased(const std::vector<bool>& released) {
        const std::size_t before = runOrder.size();
        std::size_t kept = 0;
        for (std::size_t i = 0; i < runOrder.size(); ++i) {
            if (!released[i]) {
                if (kept != i) {
                    runOrder[kept] = std::move(runOrder[i]);
                }
                ++kept;
            }
        }
        runOrder.erase(runOrder.begin() + static_cast<std::ptrdiff_t>(kept), runOrder.end());
        if (runOrder.size() != before) {
            reindex();
        }
    }

    // Phase 6. Each pass takes the Vm's queue. For each queued entity still alive, every attached instance in its
    // subtree is released (onDestroy under the release rule, then destroyed) in run order, with its entity alive; the
    // walk repeats until it releases nothing, because an onDestroy may move an entity into or out of the subtree (an
    // entity that left is spared, so membership is read again from the World before each release). Then the Vm destroys
    // the subtree. A hook that queues more makes another pass. It ends: each instance is released at most once and
    // nothing attaches in this phase. A subtree the Vm destroys also loses its attach failures here: tally runs in this
    // same update, and a report's failedAttaches counts only entities that still exist (detachGone, which drops the
    // entries of entities that died between updates, has already run).
    void flushDestroys(ScriptSystemReport& report) {
        for (std::vector<Entity> queue = vm.takeDestroyQueue(); !queue.empty(); queue = vm.takeDestroyQueue()) {
            std::vector<bool> released(runOrder.size(), false);
            for (const Entity root : queue) {
                std::set<std::uint64_t> doomed;
                if (world.alive(root)) {
                    for (bool releasedAny = true; releasedAny;) {
                        releasedAny = false;
                        const std::set<std::uint64_t> members = subtreeKeys(root);
                        for (std::size_t i = 0; i < runOrder.size(); ++i) {
                            if (!released[i] && members.contains(entityKey(runOrder[i].entity)) &&
                                isUnder(runOrder[i].entity, root)) {
                                released[i] = true;
                                release(runOrder[i], report);
                                releasedAny = true;
                            }
                        }
                    }
                    doomed = subtreeKeys(root);  // read after the releases: an onDestroy may have moved entities
                }
                if (vm.destroyQueued(root)) {  // a dead root only loses its mark
                    ++report.destroyed;
                    for (const std::uint64_t key : doomed) {
                        attachFailures.erase(key);
                    }
                }
            }
            compactReleased(released);
        }
    }

    // The "after it" figures of a report.
    void tally(const std::vector<Snapshot>& rows, ScriptSystemReport& report) const {
        for (const Attached& attached : runOrder) {
            if (attached.initDone && !attached.faulted && !pathSuspended(attached.path)) {
                ++report.running;
            }
        }
        for (const Snapshot& row : rows) {
            const auto found = scripts.find(row.script);
            if (found != scripts.end() && found->second.unresolved) {
                ++report.unresolved;
            }
        }
        for (const auto& entry : paths) {
            report.failedLoads += entry.second.loadFailed ? 1U : 0U;
            report.suspendedScripts += entry.second.suspended ? 1U : 0U;
        }
        report.failedAttaches = attachFailures.size();
    }
};

ScriptSystem::ScriptSystem(Vm& vm, World& world, ScriptResolver resolver)
    : impl(std::make_unique<Impl>(vm, world, std::move(resolver))) {
    if (!world.registered(componentTypeId<ScriptComponent>())) {
        AERO_LOG_ERROR("script: ScriptSystem refused: the World does not register engine::ScriptComponent");
        return;
    }
    impl->bound = vm.bindWorld(world) == BindStatus::Ok;
}

ScriptSystem::~ScriptSystem() { static_cast<void>(stop()); }

bool ScriptSystem::bound() const noexcept { return impl->bound && !impl->stopped; }

ScriptSystemReport ScriptSystem::update(double dt) {
    if (!bound()) {
        return ScriptSystemReport{};
    }
    const double step = std::isfinite(dt) && dt >= 0.0 ? dt : 0.0;
    ScriptSystemReport report;
    report.bound = true;

    const std::vector<Impl::Snapshot> rows = impl->takeSnapshot();
    std::map<std::uint64_t, Guid> present;
    for (const Impl::Snapshot& row : rows) {
        present.emplace(entityKey(row.entity), row.script);
    }
    impl->detachGone(present, report);
    impl->attachNew(rows, report);
    impl->runInits(report);
    impl->runUpdates(step, report);
    impl->flushDestroys(report);
    impl->tally(rows, report);
    return report;
}

ScriptSystemReport ScriptSystem::stop() {
    if (!bound()) {
        return ScriptSystemReport{};
    }
    ScriptSystemReport report;
    report.bound = true;
    for (Impl::Attached& attached : impl->runOrder) {
        impl->release(attached, report);
    }
    // Every instance is released, and destroyQueued runs no script code, so one take is the whole queue.
    for (const Entity root : impl->vm.takeDestroyQueue()) {
        report.destroyed += impl->vm.destroyQueued(root) ? 1U : 0U;
    }
    impl->runOrder.clear();
    impl->attachedAt.clear();
    impl->scripts.clear();
    impl->paths.clear();
    impl->attachFailures.clear();
    impl->vm.unbindWorld();
    impl->stopped = true;
    return report;
}

ScriptStatus ScriptSystem::reloadScript(const Guid& script) {
    if (!bound()) {
        return ScriptStatus::NotLoaded;
    }
    Impl& state = *impl;
    const auto known = state.scripts.find(script);
    if (known == state.scripts.end()) {
        return ScriptStatus::NotLoaded;
    }
    if (known->second.unresolved) {
        state.scripts.erase(known);  // the next update() asks the resolver again
        return ScriptStatus::NotLoaded;
    }
    const std::string path = known->second.path;
    const std::string now = state.resolver ? state.resolver(script) : std::string{};
    if (now != path) {
        AERO_LOG_ERROR(
            "script: {}: reloadScript refused: the resolver now names '{}' for this script; a new ScriptSystem "
            "resolves it again",
            path, now);
        return ScriptStatus::LoadError;
    }
    const auto memo = state.paths.find(path);
    if (memo == state.paths.end() || (!memo->second.loaded && !memo->second.loadFailed)) {
        return ScriptStatus::NotLoaded;
    }
    const ScriptLoadResult loaded = state.vm.loadScript(path);
    if (loaded.status == ScriptStatus::Interrupted) {
        state.settle(path);
    }
    if (loaded.status != ScriptStatus::Ok) {
        return loaded.status;
    }
    memo->second = Impl::PathMemo{.loaded = true, .loadFailed = false, .suspended = false};
    for (Impl::Attached& attached : state.runOrder) {
        if (attached.path == path) {
            attached.faulted = false;
        }
    }
    for (auto it = state.attachFailures.begin(); it != state.attachFailures.end();) {
        const auto named = state.scripts.find(it->second);
        if (named != state.scripts.end() && !named->second.unresolved && named->second.path == path) {
            it = state.attachFailures.erase(it);
        } else {
            ++it;
        }
    }
    return ScriptStatus::Ok;
}

ScriptEntityState ScriptSystem::stateOf(Entity entity) const {
    const std::uint64_t key = entityKey(entity);
    const auto attached = impl->attachedAt.find(key);
    if (attached != impl->attachedAt.end()) {
        const Impl::Attached& held = impl->runOrder[attached->second];
        if (impl->pathSuspended(held.path)) {
            return ScriptEntityState::Suspended;
        }
        if (held.faulted) {
            return ScriptEntityState::Faulted;
        }
        return held.initDone ? ScriptEntityState::Running : ScriptEntityState::PendingInit;
    }
    const auto* const component = std::as_const(impl->world).get<ScriptComponent>(entity);
    if (component == nullptr || !component->script.valid()) {
        return ScriptEntityState::None;
    }
    const auto failed = impl->attachFailures.find(key);
    if (failed != impl->attachFailures.end() && failed->second == component->script) {
        return ScriptEntityState::AttachFailed;
    }
    const auto script = impl->scripts.find(component->script);
    if (script == impl->scripts.end()) {
        return ScriptEntityState::None;
    }
    if (script->second.unresolved) {
        return ScriptEntityState::Unresolved;
    }
    const auto path = impl->paths.find(script->second.path);
    if (path != impl->paths.end()) {
        if (path->second.loadFailed) {
            return ScriptEntityState::LoadFailed;
        }
        if (path->second.suspended) {
            return ScriptEntityState::Suspended;
        }
    }
    return ScriptEntityState::None;
}

ScriptInstance ScriptSystem::instanceOf(Entity entity) const {
    const auto attached = impl->attachedAt.find(entityKey(entity));
    return attached == impl->attachedAt.end() ? ScriptInstance{} : impl->runOrder[attached->second].instance;
}

}  // namespace engine::script
