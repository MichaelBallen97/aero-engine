// The C++ mini-game of the Phase 4 gate demo (task 4.5.1): a player cube on an arena, three target cubes and a
// bullet that destroys one. It is written against the installed SDK alone: every include below is an <aero/...>
// header or a C++ standard one, and every file it reads is found beside the executable, through platform::basePath().
//
// The rules are the constants below, and the README states them as the reference the TypeScript half is written
// against. Rendering is SceneRenderer over primitives; no cooked asset is needed.
//
//   mini_game               interactive: arrows or WASD move, Space fires, Escape quits
//   mini_game --scripted    a fixed 600-frame run driven by a key table, then one summary line on stdout
//
// Frame order, the same in both modes: keys for this frame -> player move, clamp, cooldown, fire -> bullets fly and
// age -> hit test -> expire -> draw. Destruction runs after each walk, as World::each's contract asks.
#include <aero/core/math.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/prefab.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/scene_render/scene_renderer.hpp>
#include <aero/scene_serialize/scene_serialize.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace engine;  // a sample's .cpp, not a header

constexpr float FIXED_DT = 1.0F / 60.0F;
constexpr float ARENA_HALF_WIDTH = 8.0F;  // X is clamped to +-8
constexpr float ARENA_HALF_DEPTH = 6.0F;  // Z is clamped to +-6
constexpr float PLAYER_START_Z = 5.0F;
constexpr float PLAYER_Y = 0.5F;
constexpr float PLAYER_SPEED = 6.0F;    // units per second, 0.1 a frame at 60 Hz
constexpr float FIRE_COOLDOWN = 0.21F;  // never a multiple of FIXED_DT: see the README
constexpr float BULLET_SPEED = 14.0F;   // units per second along -Z
constexpr float BULLET_LIFE = 1.0F;     // seconds
constexpr float TARGET_Z = -5.0F;
constexpr std::array<float, 3> TARGET_X{-4.0F, 0.0F, 4.0F};
constexpr float HIT_RADIUS = 0.6F;  // in the XZ plane

constexpr int SCRIPT_FRAMES = 600;
constexpr int EXIT_NO_GPU = 3;

// What this game adds to the engine's components. Each is registered in the World before any use.
struct Player {
    float cooldown = 0.0F;
};
struct Velocity {
    Vec3 value;
};
struct Bullet {
    float age = 0.0F;
};
struct Target {
    float radius = HIT_RADIUS;
};

struct ScriptedKey {
    int frame;
    bool down;
    platform::Key key;
};

// The scripted run, derived by hand in the README: fired 8, hits 3, no bullet left, four entities left.
constexpr std::array<ScriptedKey, 14> SCRIPT{{
    {10, true, platform::Key::Space},
    {11, false, platform::Key::Space},
    {60, true, platform::Key::Left},
    {100, false, platform::Key::Left},
    {110, true, platform::Key::Space},
    {171, false, platform::Key::Space},
    {200, true, platform::Key::Right},
    {400, false, platform::Key::Right},
    {410, true, platform::Key::Space},
    {411, false, platform::Key::Space},
    {420, true, platform::Key::Left},
    {460, false, platform::Key::Left},
    {470, true, platform::Key::Space},
    {471, false, platform::Key::Space},
}};

struct Game {
    World world;
    Prefab bulletPrefab;
    int fired = 0;
    int hits = 0;
    std::vector<Entity> expired;     // scratch: bullets to destroy after a walk
    std::vector<Entity> hitTargets;  // scratch: targets to destroy after a walk
};

bool registerGameComponents(World& world) {
    return world.registerComponent<Player>("game::Player").valid() &&
           world.registerComponent<Velocity>("game::Velocity").valid() &&
           world.registerComponent<Bullet>("game::Bullet").valid() &&
           world.registerComponent<Target>("game::Target").valid();
}

Entity addCube(World& world, std::string_view name, Vec3 position, Vec3 scale, Vec3 color) {
    const Entity entity = world.create();
    static_cast<void>(world.setName(entity, name));
    world.add<Transform>(entity, Transform{.position = position, .scale = scale});
    world.add<MeshRenderer>(entity, MeshRenderer{.primitive = 0, .color = color});
    return entity;
}

void buildScene(World& world) {
    const Entity camera = world.create();
    static_cast<void>(world.setName(camera, "camera"));
    world.add<Transform>(
        camera, Transform{.position = {0.0F, 12.0F, 11.0F}, .rotation = fromAxisAngle(Vec3::unitX(), radians(-47.5F))});
    world.add<Camera>(camera);

    const Entity sun = world.create();
    static_cast<void>(world.setName(sun, "sun"));
    world.add<Transform>(sun, Transform{.rotation = fromAxisAngle(Vec3::unitX(), radians(-55.0F))});
    world.add<DirectionalLight>(sun);

    const Entity ground = world.create();
    static_cast<void>(world.setName(ground, "ground"));
    world.add<Transform>(ground, Transform{.scale = {10.0F, 1.0F, 10.0F}});
    world.add<MeshRenderer>(ground, MeshRenderer{.primitive = 2, .color = {0.35F, 0.4F, 0.35F}});

    const Entity player = addCube(world, "player", {0.0F, PLAYER_Y, PLAYER_START_Z}, Vec3::one(), {0.2F, 0.5F, 0.9F});
    world.add<Player>(player);

    for (const float x : TARGET_X) {
        const Entity target = addCube(world, "target", {x, PLAYER_Y, TARGET_Z}, Vec3::one(), {0.9F, 0.3F, 0.25F});
        world.add<Target>(target);
    }
}

void spawnBullet(Game& game, float x, float z) {
    const Entity bullet = game.bulletPrefab.instantiate(game.world, Transform{.position = {x, PLAYER_Y, z}});
    if (!bullet.valid()) {
        return;
    }
    game.world.add<Velocity>(bullet, Velocity{{0.0F, 0.0F, -BULLET_SPEED}});
    game.world.add<Bullet>(bullet);
    ++game.fired;
}

void movePlayerAndFire(Game& game, const platform::InputState& input, float dt) {
    using platform::Key;
    float moveX = 0.0F;
    float moveZ = 0.0F;
    moveX -= (input.keyDown(Key::Left) || input.keyDown(Key::A)) ? 1.0F : 0.0F;
    moveX += (input.keyDown(Key::Right) || input.keyDown(Key::D)) ? 1.0F : 0.0F;
    moveZ -= (input.keyDown(Key::Up) || input.keyDown(Key::W)) ? 1.0F : 0.0F;
    moveZ += (input.keyDown(Key::Down) || input.keyDown(Key::S)) ? 1.0F : 0.0F;
    const bool wantsFire = input.keyDown(Key::Space);

    bool fires = false;
    Vec3 muzzle;
    game.world.each<Transform, Player>([&](Entity /*entity*/, Transform& transform, Player& player) {
        transform.position.x =
            std::clamp(transform.position.x + (moveX * PLAYER_SPEED * dt), -ARENA_HALF_WIDTH, ARENA_HALF_WIDTH);
        transform.position.z =
            std::clamp(transform.position.z + (moveZ * PLAYER_SPEED * dt), -ARENA_HALF_DEPTH, ARENA_HALF_DEPTH);
        player.cooldown = std::max(0.0F, player.cooldown - dt);
        if (wantsFire && player.cooldown <= 0.0F) {
            player.cooldown = FIRE_COOLDOWN;
            fires = true;
            muzzle = transform.position;
        }
    });
    if (fires) {
        spawnBullet(game, muzzle.x, muzzle.z);  // a spawn adds entities: after the walk, never inside it
    }
}

void flyBullets(Game& game, float dt) {
    game.expired.clear();
    game.world.each<Transform, Velocity, Bullet>(
        [&](Entity entity, Transform& transform, const Velocity& velocity, Bullet& bullet) {
            transform.position += velocity.value * dt;
            bullet.age += dt;
            if (bullet.age >= BULLET_LIFE) {
                game.expired.push_back(entity);
            }
        });
}

void hitTest(Game& game) {
    game.hitTargets.clear();
    game.world.each<Transform, Bullet>([&](Entity bulletEntity, const Transform& bulletAt, const Bullet& /*bullet*/) {
        game.world.each<Transform, Target>([&](Entity targetEntity, const Transform& targetAt, const Target& target) {
            const float dx = bulletAt.position.x - targetAt.position.x;
            const float dz = bulletAt.position.z - targetAt.position.z;
            if ((dx * dx) + (dz * dz) <= target.radius * target.radius) {
                game.hitTargets.push_back(targetEntity);
                game.expired.push_back(bulletEntity);  // a bullet that hits is spent
            }
        });
    });
}

void destroyMarked(Game& game) {
    for (const Entity target : game.hitTargets) {
        if (game.world.destroy(target)) {
            ++game.hits;
        }
    }
    for (const Entity bullet : game.expired) {
        static_cast<void>(game.world.destroy(bullet));  // false for a bullet already spent: harmless
    }
}

void stepGame(Game& game, const platform::InputState& input, float dt) {
    movePlayerAndFire(game, input, dt);
    flyBullets(game, dt);
    hitTest(game);
    destroyMarked(game);
}

struct Data {
    VirtualFileSystem shaders;
    VirtualFileSystem assets;
};

// Both data folders sit beside the executable. `error` says what failed when the result is empty.
std::optional<Prefab> loadData(Data& data, std::string& error) {
    const std::string base = platform::basePath();
    if (base.empty()) {
        error = "the OS did not say where the executable is";
        return std::nullopt;
    }
    data.shaders.mount(std::make_unique<DirectoryBackend>(base + "shaders"));
    data.assets.mount(std::make_unique<DirectoryBackend>(base + "assets"));
    const std::optional<std::string> text = data.assets.readText("res://bullet.aeroprefab");
    if (!text) {
        error = "cannot read assets/bullet.aeroprefab beside the executable";
        return std::nullopt;
    }
    scene_serialize::PrefabLoadResult loaded = scene_serialize::loadPrefabText(*text);
    if (!loaded.prefab) {
        error = "assets/bullet.aeroprefab: " + loaded.error;
        return std::nullopt;
    }
    return std::move(loaded.prefab);
}

// Folds the keys of one scripted frame into the game's own input state.
void applyScriptedKeys(platform::InputState& input, int frame) {
    input.newFrame();
    for (const ScriptedKey& scripted : SCRIPT) {
        if (scripted.frame == frame) {
            platform::Event event;
            event.type = scripted.down ? platform::EventType::KeyDown : platform::EventType::KeyUp;
            event.key = platform::KeyData{.code = scripted.key};
            input.process(event);
        }
    }
}

int usage() {
    std::printf("usage: aero_mini_game [--scripted]\n");
    return 2;
}

int run(bool scripted) {
    Data data;
    std::string error;
    std::optional<Prefab> prefab = loadData(data, error);
    if (!prefab) {
        std::printf("mini_game: %s\n", error.c_str());
        return 1;
    }

    platform::Context ctx;
    std::optional<platform::Window> window;
    if (ctx.valid()) {
        window = ctx.createWindow({.title = "Aero mini-game (C++)", .width = 640, .height = 360});
    }
    std::optional<rhi::Device> device;
    if (window) {
        device = rhi::Device::create();
    }
    if (!window || !device) {
        std::printf("mini_game: no GPU device\n");
        return EXIT_NO_GPU;
    }
    std::optional<render::Renderer> renderer = render::Renderer::create(*device, *window, {.depth = true});
    if (!renderer) {
        std::printf("mini_game: cannot create the renderer\n");
        return 1;
    }
    std::optional<scene_render::SceneRenderer> sceneRenderer =
        scene_render::SceneRenderer::create(*device, data.shaders, renderer->colorFormat(), renderer->depthFormat());
    if (!sceneRenderer) {
        std::printf("mini_game: cannot create the scene renderer (are the shaders beside the executable?)\n");
        return 1;
    }

    Game game{.world = World{}, .bulletPrefab = std::move(*prefab)};
    if (!registerGameComponents(game.world)) {
        std::printf("mini_game: cannot register the game's components\n");
        return 1;
    }
    buildScene(game.world);

    platform::InputState scriptedInput;  // --scripted ignores ctx.input() entirely
    FrameClock clock;
    clock.setMaxDeltaSeconds(0.05F);
    int frames = 0;
    int drawn = 0;
    bool running = true;
    while (running && (!scripted || frames < SCRIPT_FRAMES)) {
        ctx.newFrame();
        platform::Event event;
        while (ctx.pollEvent(event)) {
            if (event.type == platform::EventType::Quit || event.type == platform::EventType::WindowClose) {
                running = false;
            }
        }
        if (scripted) {
            applyScriptedKeys(scriptedInput, frames);
            stepGame(game, scriptedInput, FIXED_DT);
        } else {
            clock.tick();
            running = running && !ctx.input().keyDown(platform::Key::Escape);
            stepGame(game, ctx.input(), clock.deltaSeconds());
        }
        ++frames;

        const rhi::Color sky{0.05F, 0.06F, 0.08F, 1.0F};
        std::optional<render::Frame> frame = renderer->beginFrame(sky);
        if (frame) {
            ++drawn;
            sceneRenderer->render(game.world, *frame);
            static_cast<void>(renderer->endFrame(std::move(*frame)));
        }
    }

    if (scripted) {
        std::printf("mini_game: frames=%d fired=%d hits=%d bullets=%zu entities=%zu drawn=%d\n", frames, game.fired,
                    game.hits, game.world.componentCount<Bullet>(), game.world.entityCount(), drawn);
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool scripted = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scripted") != 0) {
            return usage();
        }
        scripted = true;
    }
    try {
        return run(scripted);
    } catch (const std::exception& e) {
        std::printf("mini_game: unexpected exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::printf("mini_game: unexpected exception\n");
        return 1;
    }
}
