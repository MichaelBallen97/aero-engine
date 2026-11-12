// Aero Engine -- PlayScripts, one VM and ScriptSystem per Play over the project root (task 4.7.4, SX1-SX8). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, and UNGATED: every case but SX7 runs in every configuration
// (an instance observes through this.entity.name, which needs no bound component), and SX7 asserts that the build has
// no scene I/O and returns when it has none. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp
// supplies main().
#include "../../editor/src/play_scripts.hpp"

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/prefab.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script_system.hpp>
#include <aero/script/vm.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using engine::Entity;
using engine::Guid;
using engine::LogLevel;
using engine::ScriptComponent;
using engine::editor::playScriptModulePath;
using engine::editor::PlayScripts;
using engine::script::ScriptEntityState;
using engine::script::ScriptStatus;

// A unique temp directory that removes itself (the play_recovery_test posture).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_play_scripts_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath / "Library" / "Scripts", ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dirPath; }

private:
    std::filesystem::path dirPath;
};

// Every record, from any thread, copied under a lock.
class LogCapture {
public:
    LogCapture() {
        engine::setLogCallback([this](const engine::LogRecord& record) {
            const std::lock_guard<std::mutex> lock{mutex};
            lines.push_back(Line{record.level, std::string{record.message}});
        });
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;
    ~LogCapture() { engine::setLogCallback({}); }

    // How many records at `level` contain `needle`.
    [[nodiscard]] std::size_t count(LogLevel level, std::string_view needle = {}) const {
        const std::lock_guard<std::mutex> lock{mutex};
        std::size_t found = 0;
        for (const Line& line : lines) {
            found += (line.level == level && line.text.find(needle) != std::string::npos) ? 1U : 0U;
        }
        return found;
    }
    // Every record at WARN or above.
    [[nodiscard]] std::size_t loud() const { return count(LogLevel::Warn) + count(LogLevel::Error); }

private:
    struct Line {
        LogLevel level = LogLevel::Info;
        std::string text;
    };
    mutable std::mutex mutex;
    std::vector<Line> lines;
};

[[nodiscard]] Guid guidOf(std::uint64_t id) { return Guid{0, id}; }

// Writes `text` at <root>/<relative>, parents made.
void writeProjectFile(const TempDir& root, std::string_view relative, std::string_view text) {
    const std::filesystem::path target = root.path() / std::filesystem::path{std::string{relative}};
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const std::u8string bytes = target.u8string();
    const std::string absolute{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    REQUIRE(engine::editor::writeTextFileAtomic(absolute, text).empty());
}

// Publishes `source` as <root>/Library/Scripts/<guid>.js, the file 4.3.1's transpiler writes.
void publish(const TempDir& root, Guid guid, std::string_view source) {
    writeProjectFile(root, std::format("Library/Scripts/{}.js", engine::formatGuid(guid)), source);
}

// A script in esbuild 0.28.2's measured shape: an optional module prelude, then the class, then the default export.
[[nodiscard]] std::string scriptSource(std::string_view members, std::string_view prelude = {}) {
    return std::format("{}var S = class extends engine.Script {{\n{}\n}};\nexport {{\n  S as default\n}};\n", prelude,
                       members);
}

// A live entity named `name` that runs `script`.
[[nodiscard]] Entity scripted(engine::World& world, Guid script, std::string_view name = {}) {
    const Entity entity = world.create();
    REQUIRE(world.add<ScriptComponent>(entity, ScriptComponent{script}) != nullptr);
    if (!name.empty()) {
        REQUIRE(world.setName(entity, name));
    }
    return entity;
}

[[nodiscard]] std::string nameOf(const engine::World& world, Entity entity) { return std::string{world.name(entity)}; }

// A script that records its own life in the entity's name: tag/i<inits>/f<frames>/t<total>.
constexpr std::string_view COUNTING_MEMBERS =
    "  tag = this.entity.name; inits = 0; frames = 0; total = 0;\n"
    "  note() { this.entity.name = this.tag + '/i' + this.inits + '/f' + this.frames + '/t' + this.total; }\n"
    "  onInit() { this.inits += 1; this.note(); }\n"
    "  onUpdate(dt) { this.frames += 1; this.total += dt; this.note(); }\n";

}  // namespace

TEST_CASE("play scripts: the module path is res://Library/Scripts/<guid>.js and nil has none (task 4.7.4, SX1)") {
    CHECK(playScriptModulePath(Guid{}).empty());
    const Guid guid{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    CHECK(playScriptModulePath(guid) == "res://Library/Scripts/0123456789abcdeffedcba9876543210.js");
    CHECK(playScriptModulePath(guidOf(1)) == std::format("res://Library/Scripts/{}.js", engine::formatGuid(guidOf(1))));
}

TEST_CASE("play scripts: two entities of one script each run onInit once and onUpdate with the clock's dt (SX2)") {
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;
    const Guid script = guidOf(2);
    publish(root, script, scriptSource(COUNTING_MEMBERS));
    const Entity a = scripted(world, script, "A");
    const Entity b = scripted(world, script, "B");

    PlayScripts play{root.utf8(), world, game};
    CHECK(play.live());
    CHECK(play.system() != nullptr);
    engine::script::ScriptSystemReport report;
    for (int frame = 0; frame < 3; ++frame) {
        report = play.update(0.25);
    }
    CHECK(nameOf(world, a) == "A/i1/f3/t0.75");
    CHECK(nameOf(world, b) == "B/i1/f3/t0.75");
    CHECK(report.running == 2);
    CHECK(play.live());
}

TEST_CASE("play scripts: stop runs onDestroy for the initialised instance only, once, and tears down quietly (SX3)") {
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;
    const Guid good = guidOf(3);
    const Guid bad = guidOf(4);
    publish(root, good,
            scriptSource("  onInit() { this.entity.name = 'init'; }\n"
                         "  onDestroy() { this.entity.name = 'destroyed'; }\n"));
    publish(root, bad,
            scriptSource("  onInit() { this.entity.name = 'threw'; throw new Error('onInit'); }\n"
                         "  onDestroy() { this.entity.name = 'bad-destroyed'; }\n"));
    const Entity first = scripted(world, good);
    const Entity second = scripted(world, bad);

    const LogCapture log;
    auto play = std::make_unique<PlayScripts>(root.utf8(), world, game);
    static_cast<void>(play->update(0.5));
    CHECK(nameOf(world, first) == "init");
    CHECK(nameOf(world, second) == "threw");
    CHECK(log.count(LogLevel::Error) == 1);

    const engine::script::ScriptSystemReport report = play->stop();
    CHECK(nameOf(world, first) == "destroyed");
    CHECK(nameOf(world, second) == "threw");
    CHECK(report.detached == 2);
    CHECK_FALSE(play->live());
    CHECK(play->system() == nullptr);

    const std::size_t loudBefore = log.loud();
    const engine::script::ScriptSystemReport again = play->stop();
    CHECK(again.detached == 0);
    CHECK_FALSE(again.bound);
    CHECK(nameOf(world, first) == "destroyed");
    CHECK(nameOf(world, second) == "threw");
    play.reset();
    CHECK(nameOf(world, first) == "destroyed");
    CHECK(log.loud() == loudBefore);
}

TEST_CASE("play scripts: engine.input reads the game's own state, and nothing is bound after stop (SX4)") {
    const TempDir root;
    engine::World world;
    engine::platform::InputState game;
    const Guid script = guidOf(5);
    publish(root, script, scriptSource("  onUpdate() { this.entity.name = engine.input.keyDown('W') ? 'W' : '-'; }\n"));
    const Entity entity = scripted(world, script);

    PlayScripts play{root.utf8(), world, game};
    engine::platform::Event event;
    event.type = engine::platform::EventType::KeyDown;
    event.key.code = engine::platform::Key::W;
    game.process(event);
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "W");

    event.type = engine::platform::EventType::KeyUp;
    game.process(event);
    game.newFrame();
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "-");

    static_cast<void>(play.stop());
    const engine::script::EvalResult probe = play.vm().evaluateScript("engine.input.keyDown('W')", "probe.js");
    CHECK(probe.status == engine::script::EvalStatus::Exception);
    CHECK(probe.error.message.find("no input is bound") != std::string::npos);
}

TEST_CASE("play scripts: an unpublished script warns once, and its publish plus a reload attaches it (SX5)") {
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;
    const Guid script = guidOf(6);
    const Entity entity = scripted(world, script);

    const LogCapture log;
    PlayScripts play{root.utf8(), world, game};
    static_cast<void>(play.update(0.1));
    REQUIRE(play.system() != nullptr);
    CHECK(play.system()->stateOf(entity) == ScriptEntityState::Unresolved);
    CHECK(log.count(LogLevel::Warn, engine::formatGuid(script)) == 1);
    CHECK(log.count(LogLevel::Warn, "res://Library/Scripts/") == 1);
    CHECK(log.loud() == 1);

    static_cast<void>(play.update(0.1));
    CHECK(log.loud() == 1);

    publish(root, script, scriptSource("  onUpdate() { this.entity.name = 'ran'; }\n"));
    CHECK(play.reload(script) == ScriptStatus::NotLoaded);
    static_cast<void>(play.update(0.1));
    CHECK(play.system()->stateOf(entity) == ScriptEntityState::Running);
    CHECK(nameOf(world, entity) == "ran");
    CHECK(play.unresolvedCount() == 1);
    CHECK(log.loud() == 1);
}

TEST_CASE("play scripts: a new Play makes a new VM, so module state and faults do not carry over (SX6)") {
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;
    const Guid counter = guidOf(7);
    const Guid thrower = guidOf(8);
    publish(root, counter,
            scriptSource("  n = ++count;\n  onInit() { this.entity.name = 'm' + this.n; }\n", "var count = 0;\n"));
    publish(root, thrower, scriptSource("  onUpdate() { this.entity.name = 'ran'; throw new Error('boom'); }\n"));
    const Entity first = scripted(world, counter);
    const Entity second = scripted(world, counter);
    const Entity faulty = scripted(world, thrower);

    {
        PlayScripts play{root.utf8(), world, game};
        static_cast<void>(play.update(0.1));
        CHECK(nameOf(world, first) == "m1");
        CHECK(nameOf(world, second) == "m2");
        CHECK(nameOf(world, faulty) == "ran");
        REQUIRE(play.system() != nullptr);
        CHECK(play.system()->stateOf(faulty) == ScriptEntityState::Faulted);
    }
    for (const Entity entity : {first, second, faulty}) {
        REQUIRE(world.setName(entity, "reset"));
    }

    const LogCapture log;
    PlayScripts again{root.utf8(), world, game};
    static_cast<void>(again.update(0.1));
    CHECK(nameOf(world, first) == "m1");
    CHECK(nameOf(world, second) == "m2");
    CHECK(nameOf(world, faulty) == "ran");  // its first onUpdate ran: the fault did not carry over
    REQUIRE(again.system() != nullptr);
    CHECK(again.system()->stateOf(faulty) == ScriptEntityState::Faulted);
    CHECK(log.count(LogLevel::Error) == 1);
}

TEST_CASE("play scripts: engine.spawn reads res://assets under the project root (SX7)") {
    if (!engine::editor::sceneIoAvailable()) {
        CHECK_FALSE(engine::editor::sceneIoAvailable());
        return;
    }
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;

    engine::World source;
    const Entity prefabRoot = source.create();
    REQUIRE(source.setName(prefabRoot, "PrefabRoot"));
    REQUIRE(source.add<engine::Transform>(prefabRoot, engine::Transform{}) != nullptr);
    std::optional<engine::Prefab> prefab = engine::Prefab::fromWorld(std::move(source));
    REQUIRE(prefab.has_value());
    const std::optional<std::string> text = engine::editor::prefabToText(*prefab);
    REQUIRE(text.has_value());
    writeProjectFile(root, "assets/prefabs/p.aeroprefab", *text);

    const Guid script = guidOf(9);
    publish(
        root, script,
        scriptSource("  onInit() { this.entity.name = engine.spawn('res://assets/prefabs/p.aeroprefab').name; }\n"));
    const Entity entity = scripted(world, script);

    const LogCapture log;
    PlayScripts play{root.utf8(), world, game};
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "PrefabRoot");
    CHECK(world.entityCount() == 2);
    CHECK(log.loud() == 0);
}

TEST_CASE("play scripts: reload keeps values, refuses a broken publish and recovers a fault (SX8)") {
    const TempDir root;
    engine::World world;
    const engine::platform::InputState game;
    const Guid script = guidOf(10);
    const std::string bump = "globalThis.initCalls = (globalThis.initCalls || 0) + 1;";
    const auto version = [&](std::string_view tag, std::string_view extra = {}) {
        return scriptSource(
            std::format("  count = 0;\n  onInit() {{ {} }}\n"
                        "  onUpdate() {{ this.count += 1; this.entity.name = '{}:' + this.count; {} }}\n",
                        bump, tag, extra));
    };
    publish(root, script, version("v1"));
    const Entity entity = scripted(world, script);

    const LogCapture log;
    PlayScripts play{root.utf8(), world, game};
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "v1:1");

    publish(root, script, version("v2"));
    CHECK(play.reload(script) == ScriptStatus::Ok);
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "v2:2");  // the count was kept

    publish(root, script, "export default class { broken;\n");
    CHECK(play.reload(script) == ScriptStatus::CompileError);
    CHECK(log.count(LogLevel::Error) == 1);
    static_cast<void>(play.update(0.1));
    CHECK(nameOf(world, entity) == "v2:3");  // still on v2
    CHECK(play.reload(script) == ScriptStatus::AlreadyRefused);
    CHECK(log.count(LogLevel::Error) == 1);

    publish(root, script, version("v4", "throw new Error('v4');"));
    CHECK(play.reload(script) == ScriptStatus::Ok);
    static_cast<void>(play.update(0.1));
    REQUIRE(play.system() != nullptr);
    CHECK(play.system()->stateOf(entity) == ScriptEntityState::Faulted);
    CHECK(log.count(LogLevel::Error) == 2);

    publish(root, script, version("v5"));
    CHECK(play.reload(script) == ScriptStatus::Ok);
    static_cast<void>(play.update(0.1));
    CHECK(play.system()->stateOf(entity) == ScriptEntityState::Running);
    CHECK(nameOf(world, entity) == "v5:5");
    CHECK(play.vm().evaluateScript("String(globalThis.initCalls)", "probe.js").completion == "1");  // onInit ran once

    CHECK(play.reload(guidOf(11)) == ScriptStatus::NotLoaded);
}
