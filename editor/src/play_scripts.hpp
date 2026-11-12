#pragma once
// Aero Engine -- the scripts of one Play (task 4.7.4). SRC-PRIVATE and IMPURE: it holds a VirtualFileSystem, a script
// Vm and a ScriptSystem, and it logs one WARN per script GUID it cannot answer. EditorApp keeps it behind a
// unique_ptr over this (incomplete) type, so no public editor header names a script type.
//
// One holder per Play: the VFS, the VM and the system are made together and torn down together, in the reverse order.
// A Play that faulted a script, suspended one or kept module-level state leaves nothing behind for the next Play,
// because the next Play makes a new holder over a new VM.
//
// `res://` is the PROJECT ROOT for the whole Play: a script module is `res://Library/Scripts/<guid>.js` (4.3.1's
// published file), `engine.spawn('res://assets/...')` reads the project's assets folder, and a thrown error's record
// names `res://assets/scripts/<name>.ts` through the source map the transpiler publishes beside the module.
//
// Every call runs on the thread that made the holder (the VM's thread), never inside a draw walk.
#include <aero/core/guid.hpp>
#include <aero/core/vfs.hpp>
#include <aero/script/script_system.hpp>
#include <aero/script/vm.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine {
class World;  // borrowed; play_scripts.cpp needs no more than the reference
}  // namespace engine

namespace engine::platform {
class InputState;  // borrowed by Vm::bindInput
}  // namespace engine::platform

namespace engine::editor {

// res://Library/Scripts/<formatGuid(script)>.js, 4.3.1's published file under the project-root mount; "" for nil.
[[nodiscard]] std::string playScriptModulePath(Guid script);

class PlayScripts {
public:
    // Mounts ONE DirectoryBackend over `projectRootUtf8` at res://, creates a Vm with VmConfig{} over it, binds `game`
    // (Vm::bindInput) and builds a ScriptSystem over `world` with the resolver below. A refused Vm::create leaves the
    // holder inert: the Vm's own ERROR is the one record. Borrows `world` and `game`, which outlive it.
    PlayScripts(std::string_view projectRootUtf8, World& world, const platform::InputState& game);
    PlayScripts(std::string_view, World&&, const platform::InputState&) = delete;
    PlayScripts(std::string_view, World&, const platform::InputState&&) = delete;
    ~PlayScripts();                            // stop()
    PlayScripts(const PlayScripts&) = delete;  // the resolver captures `this`
    PlayScripts& operator=(const PlayScripts&) = delete;
    PlayScripts(PlayScripts&&) = delete;
    PlayScripts& operator=(PlayScripts&&) = delete;

    [[nodiscard]] bool live() const noexcept;      // the system exists and is bound
    script::ScriptSystemReport update(double dt);  // inert or stopped: a default report, nothing runs
    script::ScriptStatus reload(Guid script);      // ScriptSystem::reloadScript; inert or stopped: NotLoaded
    // Idempotent. onDestroy per initialised instance, the system destroyed (the World unbound), then the input unbound.
    script::ScriptSystemReport stop();
    [[nodiscard]] const script::ScriptSystem* system() const noexcept;  // null when inert or stopped
    [[nodiscard]] script::Vm& vm() noexcept;                            // the tests' window onto the same VM
    [[nodiscard]] std::size_t unresolvedCount() const noexcept;         // GUIDs the resolver has WARNed about

private:
    [[nodiscard]] std::string resolve(const Guid& script);

    VirtualFileSystem files;                          // declared FIRST: destroyed LAST
    script::Vm machine;                               // borrows `files`
    std::unique_ptr<script::ScriptSystem> lifecycle;  // borrows `machine`, the World and `this`
    std::vector<Guid> warned;                         // sorted; the GUIDs resolve() has WARNed about
};

}  // namespace engine::editor
