#pragma once
// Aero Engine -- the script lifecycle over one World (task 4.4.1). A ScriptSystem owns nothing a script can see: it
// walks the World's ScriptComponents, attaches one instance of the named script's class to each entity through the
// Vm, and drives the instance's hooks (onInit, onUpdate, onDestroy) once per update(). It is written against the Vm's
// public API only, so it is as quickjs-free as vm.hpp is.
//
// The three borrows (the Vm, the World and the resolver's captures) outlive the system. One system per Vm at a time:
// it binds the World at construction and is that Vm's only binder while it lives, so a bind made by anyone else turns
// every attached `entity` stale and each hook that touches it faults its instance. A Vm::loadScript made around the
// system moves its instances and clears none of the failures the system remembers. No call may run while the caller
// iterates the World.
//
// One update(dt), in this order:
//   1. the World's ScriptComponents are read into a snapshot, sorted by entity index, before any script runs;
//   2. detach: an instance whose entity left the snapshot or now names another script gets onDestroy (only when its
//      onInit succeeded), then is destroyed;
//   3. attach, in ascending entity index: each Guid is resolved once and each path loaded once; a refused load or
//      attach is remembered, never retried, and logged once by the Vm;
//   4. init: onInit of every instance that has not run it, in attach order;
//   5. update: onUpdate(dt) of every initialised instance, in attach order.
// Each hook runs only while its entity is alive and still names its script. A failed hook faults its instance after
// the Vm's one record. dt reaches scripts as given when it is finite and non-negative, else as 0.
//
// What a failure costs, and what undoes it:
//   - an unresolved Guid, a refused load, a refused attach, a failed hook (a throw, a heap refusal, an async hook that
//     rejects): one record, then the entity or instance stops until its script reloads; nothing is retried per frame.
//     A failed onInit means neither onUpdate nor onDestroy runs; a failed onDestroy still destroys the instance.
//   - a hook or a constructor that runs past its time budget costs a whole budget per call, so it suspends the whole
//     script in this system: the Vm's one ERROR, then one WARN naming the script, the budget and the instances it
//     holds. No hook of that script runs again, onDestroy included, and nothing new attaches it, until it reloads.
//   - after any call that ended Interrupted the system makes one empty evaluation named with the script's path and
//     ignores its status, so a reaction job the interrupt left behind answers for the script it came from instead of
//     faulting whichever instance runs next.
//   - reloadScript is the undo: its Ok clears every failure remembered for that script, suspension included.

#include <aero/core/guid.hpp>
#include <aero/scene/entity.hpp>
#include <aero/script/vm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace engine {
class World;  // borrowed; src/script_system.cpp includes <aero/scene/world.hpp>
}  // namespace engine

namespace engine::script {

// A script asset's GUID to its module's absolute res:// path; "" when the asset is unknown.
using ScriptResolver = std::function<std::string(const Guid& script)>;

enum class ScriptEntityState : std::uint8_t {
    None = 0,      // no ScriptComponent, or one naming no script
    Unresolved,    // the resolver does not know the script
    LoadFailed,    // the script's load was refused
    AttachFailed,  // the script's constructor was refused for this entity
    PendingInit,   // attached, onInit not yet run
    Running,       // attached and initialised
    Faulted,       // a hook failed; no further hook runs until the script reloads
    Suspended,     // the script ran past its time budget
};
// Never named toString (doctest's ADL trap). The view is over a terminated literal.
[[nodiscard]] std::string_view scriptEntityStateLabel(ScriptEntityState state) noexcept;

struct ScriptSystemReport {
    bool bound = false;                // false: inert or stopped, and nothing ran
    std::size_t attached = 0;          // this call: instances created
    std::size_t detached = 0;          // this call: instances destroyed (detach, or stop)
    std::size_t initialised = 0;       // this call: instances that reached Running (onInit Ok, or absent)
    std::size_t updated = 0;           // this call: onUpdate calls made (the hook present or absent)
    std::size_t faulted = 0;           // this call: instances that became Faulted
    std::size_t running = 0;           // after it: instances Running
    std::size_t unresolved = 0;        // after it: snapshot entities whose script the resolver does not know
    std::size_t failedLoads = 0;       // after it: script paths whose load failed
    std::size_t failedAttaches = 0;    // after it: entities whose attach failed
    std::size_t suspendedScripts = 0;  // after it: script paths suspended
};

class ScriptSystem {
public:
    // Binds `world` to `vm`. The World must register engine::ScriptComponent and the Vm must accept the bind; either
    // refusal is one ERROR and leaves the system inert: bound() is false, update() and stop() do nothing.
    ScriptSystem(Vm& vm, World& world, ScriptResolver resolver);
    ScriptSystem(Vm&& vm, World& world, ScriptResolver resolver) = delete;  // no temporary: the system borrows both
    ScriptSystem(Vm& vm, World&& world, ScriptResolver resolver) = delete;
    ScriptSystem(Vm&& vm, World&& world, ScriptResolver resolver) = delete;
    ScriptSystem(const ScriptSystem&) = delete;  // the system holds references
    ScriptSystem& operator=(const ScriptSystem&) = delete;
    ScriptSystem(ScriptSystem&&) = delete;
    ScriptSystem& operator=(ScriptSystem&&) = delete;
    ~ScriptSystem();  // stop()

    // True while this system holds the World: from a successful construction until stop().
    [[nodiscard]] bool bound() const noexcept;

    // One frame of the lifecycle above. An inert or stopped system does nothing and reports bound == false.
    ScriptSystemReport update(double dt);

    // onDestroy once per initialised instance, in attach order, then every instance destroyed, the memos cleared and
    // the World unbound from the Vm. The system is stopped for good: a later update() or stop() does nothing.
    ScriptSystemReport stop();

    // Reloads the script the Guid names, the call a reload trigger makes. NotLoaded, with no Vm call, for a Guid no
    // attach has met, for an inert or stopped system, and for a path whose load was never attempted; an unresolved
    // Guid is forgotten, so the next update() asks the resolver again. Otherwise the Guid is resolved again: a
    // different path is one ERROR and LoadError, nothing changes (the instances belong to the path they were made
    // from; a new system resolves the new one). Then Vm::loadScript runs: on Ok every failure remembered for the
    // script is cleared (a failed onInit runs again at the next update, a failed onUpdate resumes without onInit,
    // unattached entities attach, a suspension ends); any other status, Unchanged included, is returned and changes
    // nothing.
    ScriptStatus reloadScript(const Guid& script);

    [[nodiscard]] ScriptEntityState stateOf(Entity entity) const;
    // The instance attached to `entity`, or a null handle. Valid exactly while the instance is attached.
    [[nodiscard]] ScriptInstance instanceOf(Entity entity) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace engine::script
