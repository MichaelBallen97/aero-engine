// Aero Engine -- PlayScripts, one VM and ScriptSystem per Play over the project root (task 4.7.4, SX1-SX8). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, and UNGATED: every case but SX7 runs in every configuration
// (an instance observes through this.entity.name, which needs no bound component), and SX7 asserts that the build has
// no scene I/O and returns when it has none. Do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; shell_test.cpp
// supplies main().
//
// SX9, SX10 and SX11 read and run the committed TypeScript demo project, samples/phase-4-scripting/ts, through
// AERO_PHASE4_TS_SAMPLE_DIR (a path, not a flag: a missing directory is a REQUIRE failure). SX9 only reads it in place;
// SX10 and SX11 copy it to a temporary directory first, so no case writes into the tree.
#include "../../editor/src/play_scripts.hpp"

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/script_types.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/prefab.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script_system.hpp>
#include <aero/script/vm.hpp>

#include "../../editor/src/script_transpiler.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
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
#include <thread>
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

namespace {

// The GUID in samples/phase-4-scripting/ts/assets/scripts/mover.ts.meta.
constexpr std::string_view SAMPLE_MOVER_GUID = "0364eb133a9bb87c48e6ba1b38862722";

[[nodiscard]] std::string sampleRoot() { return std::string{AERO_PHASE4_TS_SAMPLE_DIR}; }

// The whole file at `absolutePathUtf8`; the REQUIRE fails the case when it cannot be read.
[[nodiscard]] std::string readSampleFile(std::string_view absolutePathUtf8) {
    const engine::editor::FileReadResult read = engine::editor::readTextFile(absolutePathUtf8);
    REQUIRE(read.text.has_value());
    return *read.text;
}

// The scene and its two stacks, the shape every scene-opening case in this binary builds.
struct SampleScene {
    engine::World world;
    engine::editor::Selection selection;
    engine::editor::RootOrder roots;
    engine::editor::CommandStack commands;
    engine::editor::CommandContext context{world, selection, roots};
};

// The Transform x of every entity whose ScriptComponent names `script`, in ascending entity index.
[[nodiscard]] std::vector<float> scriptedX(engine::World& world, Guid script) {
    std::vector<float> positions;
    world.each<ScriptComponent, engine::Transform>(
        [&](Entity /*entity*/, ScriptComponent& component, engine::Transform& transform) {
            if (component.script == script) {
                positions.push_back(transform.position.x);
            }
        });
    return positions;
}

}  // namespace

TEST_CASE(
    "play scripts: the TypeScript sample opens as a project, and its scene is the file the editor would write "
    "(SX9)") {
    const std::string root = sampleRoot();
    const engine::editor::ProjectLoadOutcome loaded = engine::editor::loadProjectFrom(root);
    REQUIRE(loaded.ok);
    CHECK(loaded.manifest.language == engine::editor::ProjectLanguage::Ts);
    CHECK(loaded.manifest.name == "phase-4-scripting");
    CHECK(loaded.unknownKeys.empty());

    CHECK(readSampleFile(root + "/tsconfig.json") == engine::editor::renderTsconfig("assets"));

    const engine::editor::MetaParseResult meta =
        engine::editor::parseMeta(readSampleFile(root + "/assets/scripts/mover.ts.meta"));
    REQUIRE(meta.guid.has_value());
    CHECK(*meta.guid != Guid{});
    CHECK(engine::formatGuid(*meta.guid) == SAMPLE_MOVER_GUID);

    if (!engine::editor::sceneIoAvailable()) {
        CHECK_FALSE(engine::editor::sceneIoAvailable());
        return;
    }
    const std::string text = readSampleFile(root + "/scenes/main.scene.json");
    SampleScene scene;
    const engine::editor::SceneOpenOutcome opened = engine::editor::openSceneText(scene.context, scene.commands, text);
    REQUIRE(opened.ok);
    CHECK(opened.skipped == 0);
    CHECK(opened.failed == 0);
    CHECK(scene.world.entityCount() == 4);
    CHECK(scene.world.componentCount<ScriptComponent>() == 2);
    CHECK(scene.world.componentCount<engine::Camera>() == 1);
    CHECK(scene.world.componentCount<engine::DirectionalLight>() == 1);
    CHECK(scriptedX(scene.world, *meta.guid).size() == 2);

    const std::optional<std::string> again = engine::editor::sceneToText(scene.world);
    REQUIRE(again.has_value());
    CHECK(*again == text);
}

TEST_CASE("play scripts: the sample's two cubes move under Play and stop cleanly, from a copy of the project (SX10)") {
    if (!engine::editor::sceneIoAvailable()) {
        CHECK_FALSE(engine::editor::sceneIoAvailable());
        return;
    }
    const TempDir copy;
    const std::string root = copy.utf8();
    const std::filesystem::path source = std::filesystem::path{sampleRoot()};
    std::error_code ec;
    for (const char* const leaf : {"project.json", "tsconfig.json"}) {
        std::filesystem::copy_file(source / leaf, copy.path() / leaf, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        REQUIRE_FALSE(ec);
    }
    for (const char* const leaf : {"assets", "scenes"}) {
        std::filesystem::copy(source / leaf, copy.path() / leaf, std::filesystem::copy_options::recursive, ec);
        REQUIRE_FALSE(ec);
    }

    engine::editor::AssetDatabase database;
    engine::GuidGenerator generator{0x4311ULL};
    static_cast<void>(database.rescan(root, root + "/assets", generator));
    const engine::editor::AssetRecord* const record = database.findByPath("scripts/mover.ts");
    REQUIRE(record != nullptr);
    CHECK(engine::formatGuid(record->guid) == SAMPLE_MOVER_GUID);
    CHECK(record->state == engine::editor::AssetMetaState::Ok);
    const Guid script = record->guid;

    engine::editor::ScriptTranspilerConfig config;
    config.esbuildPath = std::string{AERO_TEST_ESBUILD};
    bool published = false;
    {
        engine::editor::ScriptTranspiler transpiler{config};
        transpiler.noteScan(database,
                            engine::editor::ScriptProjectView{root, "assets", engine::editor::ProjectLanguage::Ts});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (!transpiler.idle() && std::chrono::steady_clock::now() < deadline) {
            transpiler.service(database, 0.0F);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(transpiler.idle());
        for (const Guid& guid : transpiler.takePublished()) {
            published = published || guid == script;
        }
    }
    CHECK(published);

    SampleScene scene;
    const std::string text = readSampleFile(root + "/scenes/main.scene.json");
    REQUIRE(engine::editor::openSceneText(scene.context, scene.commands, text).ok);
    const std::vector<float> start = scriptedX(scene.world, script);
    REQUIRE(start.size() == 2);

    const engine::platform::InputState game;
    const LogCapture log;
    PlayScripts play{root, scene.world, game};
    REQUIRE(play.live());
    for (int frame = 0; frame < 30; ++frame) {
        static_cast<void>(play.update(1.0 / 60.0));
    }
    const std::vector<float> moved = scriptedX(scene.world, script);
    REQUIRE(moved.size() == 2);
    CHECK(std::abs(moved[0] - start[0]) > 0.1F);
    CHECK(std::abs(moved[1] - start[1]) > 0.1F);
    CHECK(std::abs(moved[1] - moved[0]) > 1.0F);
    CHECK(play.stop().detached == 2);
    CHECK(log.loud() == 0);
}

TEST_CASE("play scripts: a saved SPEED reaches the sample's running cubes at once, with no jump (SX11)") {
    if (!engine::editor::sceneIoAvailable()) {
        CHECK_FALSE(engine::editor::sceneIoAvailable());
        return;
    }
    const TempDir copy;
    const std::string root = copy.utf8();
    const std::filesystem::path source = std::filesystem::path{sampleRoot()};
    std::error_code ec;
    for (const char* const leaf : {"project.json", "tsconfig.json"}) {
        std::filesystem::copy_file(source / leaf, copy.path() / leaf, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        REQUIRE_FALSE(ec);
    }
    for (const char* const leaf : {"assets", "scenes"}) {
        std::filesystem::copy(source / leaf, copy.path() / leaf, std::filesystem::copy_options::recursive, ec);
        REQUIRE_FALSE(ec);
    }

    // 1. The copy's script holds the speed once, and the case rewrites it.
    const std::string moverPath = root + "/assets/scripts/mover.ts";
    std::string moverText = readSampleFile(moverPath);
    constexpr std::string_view OLD_SPEED = "SPEED = 1.5";
    constexpr std::string_view NEW_SPEED = "SPEED = 4.5";
    const std::size_t speedAt = moverText.find(OLD_SPEED);
    REQUIRE(speedAt != std::string::npos);
    REQUIRE(moverText.find(OLD_SPEED, speedAt + 1) == std::string::npos);

    engine::editor::AssetDatabase database;
    engine::GuidGenerator generator{0x4311ULL};
    static_cast<void>(database.rescan(root, root + "/assets", generator));
    const engine::editor::AssetRecord* const record = database.findByPath("scripts/mover.ts");
    REQUIRE(record != nullptr);
    const Guid script = record->guid;

    engine::editor::ScriptTranspilerConfig config;
    config.esbuildPath = std::string{AERO_TEST_ESBUILD};
    const engine::editor::ScriptProjectView view{root, "assets", engine::editor::ProjectLanguage::Ts};
    const auto settle = [&](engine::editor::ScriptTranspiler& transpiler) {
        transpiler.noteScan(database, view);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (!transpiler.idle() && std::chrono::steady_clock::now() < deadline) {
            transpiler.service(database, 0.0F);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(transpiler.idle());
    };
    const auto publishedMover = [&](engine::editor::ScriptTranspiler& transpiler) {
        bool found = false;
        for (const Guid& guid : transpiler.takePublished()) {
            found = found || guid == script;
        }
        return found;
    };

    engine::editor::ScriptTranspiler transpiler{config};
    settle(transpiler);
    REQUIRE(publishedMover(transpiler));

    SampleScene scene;
    const std::string sceneText = readSampleFile(root + "/scenes/main.scene.json");
    REQUIRE(engine::editor::openSceneText(scene.context, scene.commands, sceneText).ok);
    const std::vector<float> origin = scriptedX(scene.world, script);
    REQUIRE(origin.size() == 2);

    constexpr double DT = 1.0 / 60.0;
    constexpr int BEFORE_FRAMES = 30;
    constexpr int AFTER_FRAMES = 30;
    constexpr double OLD_RATE = 1.5;
    constexpr double NEW_RATE = 4.5;
    constexpr double AMPLITUDE = 2.0;
    constexpr double TOLERANCE = 2.0e-3;

    const engine::platform::InputState game;
    const LogCapture log;
    {
        PlayScripts play{root, scene.world, game};
        REQUIRE(play.live());

        // 2. Thirty frames at the old speed.
        for (int frame = 0; frame < BEFORE_FRAMES; ++frame) {
            static_cast<void>(play.update(DT));
        }
        const std::vector<float> before = scriptedX(scene.world, script);
        REQUIRE(before.size() == 2);

        // 3. Save the new speed: the publish names the mover and the reload is Ok.
        moverText.replace(speedAt, NEW_SPEED.size(), NEW_SPEED);
        REQUIRE(engine::editor::writeTextFileAtomic(moverPath, moverText).empty());
        static_cast<void>(database.rescan(root, root + "/assets", generator));
        settle(transpiler);
        CHECK(publishedMover(transpiler));
        CHECK(play.reload(script) == ScriptStatus::Ok);

        // 4. No jump: the first frame moves no further than one frame's travel at the new speed.
        static_cast<void>(play.update(DT));
        const std::vector<float> first = scriptedX(scene.world, script);
        REQUIRE(first.size() == 2);
        const double oneFrame = AMPLITUDE * NEW_RATE * DT + TOLERANCE;
        CHECK(std::abs(static_cast<double>(first[0] - before[0])) <= oneFrame);
        CHECK(std::abs(static_cast<double>(first[1] - before[1])) <= oneFrame);

        // 5. The new rate: the rest of the frames follow the phase the first frames reached, at the new speed.
        for (int frame = 1; frame < AFTER_FRAMES; ++frame) {
            static_cast<void>(play.update(DT));
        }
        const std::vector<float> after = scriptedX(scene.world, script);
        REQUIRE(after.size() == 2);
        const double phase0 = BEFORE_FRAMES * DT * OLD_RATE;
        const double expectedPhase = phase0 + AFTER_FRAMES * DT * NEW_RATE;
        const double oldRatePhase = phase0 + AFTER_FRAMES * DT * OLD_RATE;
        for (std::size_t cube = 0; cube < 2; ++cube) {
            const double expected = static_cast<double>(origin[cube]) + std::sin(expectedPhase) * AMPLITUDE;
            const double stale = static_cast<double>(origin[cube]) + std::sin(oldRatePhase) * AMPLITUDE;
            CHECK(std::abs(expected - stale) > 100.0 * TOLERANCE);  // the old rate would land somewhere else
            CHECK(std::abs(static_cast<double>(after[cube]) - expected) <= TOLERANCE);
        }

        // 6. Stop is clean.
        CHECK(play.stop().detached == 2);
    }
    CHECK(log.loud() == 0);
    static_cast<void>(transpiler.takePublished());
}
