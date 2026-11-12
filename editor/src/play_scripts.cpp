// Aero Engine -- the scripts of one Play (task 4.7.4); see play_scripts.hpp.
#include "play_scripts.hpp"

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/input.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script_system.hpp>
#include <aero/script/vm.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {
constexpr std::string_view SCRIPT_MODULE_PREFIX = "res://Library/Scripts/";
constexpr std::string_view SCRIPT_MODULE_SUFFIX = ".js";
}  // namespace

std::string playScriptModulePath(Guid script) {
    if (!script.valid()) {
        return {};
    }
    std::string path{SCRIPT_MODULE_PREFIX};
    path += formatGuid(script);
    path += SCRIPT_MODULE_SUFFIX;
    return path;
}

PlayScripts::PlayScripts(std::string_view projectRootUtf8, World& world, const platform::InputState& game) {
    files.mount(std::make_unique<DirectoryBackend>(projectRootUtf8));
    script::VmCreateResult created = script::Vm::create(script::VmConfig{}, files);
    if (created.status != script::VmCreateStatus::Ok) {
        return;  // inert: the Vm logged its one record
    }
    machine = std::move(created.vm);
    // Not Ok only for an invalid VM or another thread, neither reachable here; the VM logs it.
    static_cast<void>(machine.bindInput(game));
    lifecycle =
        std::make_unique<script::ScriptSystem>(machine, world, [this](const Guid& script) { return resolve(script); });
}

script::ScriptSystemReport PlayScripts::stop() {
    script::ScriptSystemReport report;
    if (lifecycle != nullptr) {
        report = lifecycle->stop();  // onDestroy per initialised instance, instances destroyed, the World unbound
        lifecycle.reset();
    }
    if (machine.valid()) {
        machine.unbindInput();
    }
    return report;
}

PlayScripts::~PlayScripts() {
    static_cast<void>(stop());  // then `machine`, then `files` (reverse declaration order)
}

bool PlayScripts::live() const noexcept { return lifecycle != nullptr && lifecycle->bound(); }

script::ScriptSystemReport PlayScripts::update(double dt) {
    return lifecycle != nullptr ? lifecycle->update(dt) : script::ScriptSystemReport{};
}

script::ScriptStatus PlayScripts::reload(Guid script) {
    return lifecycle != nullptr ? lifecycle->reloadScript(script) : script::ScriptStatus::NotLoaded;
}

const script::ScriptSystem* PlayScripts::system() const noexcept { return lifecycle.get(); }

script::Vm& PlayScripts::vm() noexcept { return machine; }

std::size_t PlayScripts::unresolvedCount() const noexcept { return warned.size(); }

std::string PlayScripts::resolve(const Guid& script) {
    std::string path = playScriptModulePath(script);
    if (!path.empty() && files.exists(path)) {
        return path;
    }
    const auto at = std::lower_bound(warned.begin(), warned.end(), script);
    if (at == warned.end() || !(*at == script)) {
        warned.insert(at, script);
        AERO_LOG_WARN(
            "editor: play: script {} has no published JavaScript at '{}'; its entities run no script until it is "
            "published",
            formatGuid(script), path);
    }
    return {};
}

}  // namespace engine::editor
