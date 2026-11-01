// Aero Engine -- the generated built-in binding table (task 4.2.1; BN6, and from the later steps BN37-BN52), and the
// script lifecycle end to end over it (task 4.4.1; BN56, BN57, LC26, LC28-LC30, LC39).
//
// Compiled into aero_tests only when AERO_REFLECT_TOOLS is ON: with the tools OFF the table is empty by design and
// there is nothing generated to check.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/animation_player.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/environment.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/script_component.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("script: the generated table binds exactly a fresh World's registrations, in order (BN6)") {
    const engine::World world;
    const auto components = engine::script::internal::builtinComponentBindings().components();

    REQUIRE(world.componentTypeCount() >= 1U);
    REQUIRE(components.size() == world.componentTypeCount());
    for (std::size_t i = 0; i < components.size(); ++i) {
        CAPTURE(i);
        CAPTURE(components[i].scriptName);
        CHECK(components[i].type == world.componentTypeAt(i));
        const std::string expected = "engine::" + std::string{components[i].scriptName};
        CHECK(std::string_view{expected} == world.componentTypeName(components[i].type));
    }
}

// ---- BN37-BN52: the eleven built-in components, end to end through the generated table
// -------------------------------

namespace {

using engine::Entity;
using engine::Guid;
using engine::Quat;
using engine::Vec3;
using engine::World;
using engine::script::BindStatus;
using engine::script::EvalResult;
using engine::script::EvalStatus;
using engine::script::Vm;
using script_test::completion;
using script_test::expectCleanShutdown;
using script_test::LogCapture;

[[nodiscard]] std::string text(float value) { return std::format("{}", value); }
[[nodiscard]] std::string text(bool value) { return value ? "true" : "false"; }
[[nodiscard]] std::string text(std::uint32_t value) { return std::format("{}", value); }
[[nodiscard]] std::string text(const Guid& value) { return engine::formatGuid(value); }
[[nodiscard]] std::string text(const Vec3& value) { return text(value.x) + "|" + text(value.y) + "|" + text(value.z); }
[[nodiscard]] std::string text(const Quat& value) {
    return text(value.x) + "|" + text(value.y) + "|" + text(value.z) + "|" + text(value.w);
}

// The values C++ formats and the script is expected to print, joined as the script's join('|') joins them.
template <class... Values>
[[nodiscard]] std::string joined(const Values&... values) {
    std::string out;
    const auto append = [&out](const std::string& piece) {
        if (!out.empty()) {
            out += '|';
        }
        out += piece;
    };
    (append(text(values)), ...);
    return out;
}

// A World (all eleven built-ins registered by its constructor), a VM over the generated table bound to it, and one
// entity defined as the global `e`. The VM is declared after the World, so it is destroyed first.
struct BuiltinScene {
    World world;
    Vm vm = script_test::makeBuiltinVm();
    Entity entity = world.create();

    BuiltinScene() { REQUIRE(vm.bindWorld(world) == BindStatus::Ok); }
    BuiltinScene(const BuiltinScene&) = delete;
    BuiltinScene& operator=(const BuiltinScene&) = delete;
    BuiltinScene(BuiltinScene&&) = delete;
    BuiltinScene& operator=(BuiltinScene&&) = delete;
    ~BuiltinScene() = default;

    template <class C>
    void seed(const C& value) {
        REQUIRE(world.add<C>(entity, value) != nullptr);
        REQUIRE(vm.defineEntityGlobal("e", entity) == BindStatus::Ok);
    }

    template <class C>
    [[nodiscard]] const C& held() const {
        const C* const component = world.get<C>(entity);
        REQUIRE(component != nullptr);
        return *component;
    }

    // Runs one statement and returns "Name: message" of what it threw ("no throw" when it did not).
    [[nodiscard]] std::string thrown(const std::string& statement) {
        return completion(vm, "(() => { try { " + statement +
                                  "; } catch (err) { return err.name + ': ' + err.message; } return 'no throw'; })()");
    }
};

}  // namespace

TEST_CASE("script: Transform end to end (BN37)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::Transform seeded;
    seeded.position = Vec3{1.5F, -2.25F, 3.0F};
    seeded.rotation = Quat{0.25F, 0.5F, -0.75F, 0.125F};
    seeded.scale = Vec3{2.0F, 4.0F, 0.5F};
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.Transform); const p = c.position, q = c.rotation, "
                     "s = c.scale; [p.x, p.y, p.z, q.x, q.y, q.z, q.w, s.x, s.y, s.z].join('|')") ==
          joined(seeded.position, seeded.rotation, seeded.scale));

    completion(scene.vm,
               "c.position = new engine.Vec3(-8, 16, 0.75); const r = c.rotation; r.x = 1; r.y = -1.5; "
               "r.z = 2.5; r.w = -0.5; c.scale.set(0.25, 8, 6); 0");
    engine::Transform expected;
    expected.position = Vec3{-8.0F, 16.0F, 0.75F};
    expected.rotation = Quat{1.0F, -1.5F, 2.5F, -0.5F};
    expected.scale = Vec3{0.25F, 8.0F, 6.0F};
    const auto& got = scene.held<engine::Transform>();
    CHECK(got.position == expected.position);
    CHECK(got.rotation == expected.rotation);
    CHECK(got.scale == expected.scale);
    CHECK(got == expected);

    CHECK(scene.thrown("c.position = 5") ==
          "TypeError: engine.Transform.position: expected an engine.Vec3, got a number");
    CHECK(scene.held<engine::Transform>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: Camera end to end (BN38)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::Camera seeded;
    seeded.fovYRadians = 1.25F;
    seeded.nearPlane = 0.5F;
    seeded.farPlane = 256.0F;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.Camera); [c.fovYRadians, c.nearPlane, c.farPlane]"
                     ".join('|')") == joined(seeded.fovYRadians, seeded.nearPlane, seeded.farPlane));

    completion(scene.vm, "c.fovYRadians = 0.75; c.nearPlane = 0.0625; c.farPlane = 512; 0");
    engine::Camera expected;
    expected.fovYRadians = 0.75F;
    expected.nearPlane = 0.0625F;
    expected.farPlane = 512.0F;
    const auto& got = scene.held<engine::Camera>();
    CHECK(got.fovYRadians == expected.fovYRadians);
    CHECK(got.nearPlane == expected.nearPlane);
    CHECK(got.farPlane == expected.farPlane);
    CHECK(got == expected);

    CHECK(scene.thrown("c.fovYRadians = 'x'") ==
          "TypeError: engine.Camera.fovYRadians: expected a number, got a string");
    CHECK(scene.held<engine::Camera>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: DirectionalLight end to end (BN39)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::DirectionalLight seeded;
    seeded.color = Vec3{0.5F, 0.25F, 2.0F};
    seeded.intensity = 3.5F;
    seeded.castsShadows = false;
    seeded.shadowBias = 0.00390625F;
    seeded.shadowNormalBias = 0.125F;
    seeded.shadowDistance = 128.0F;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.DirectionalLight); const k = c.color; [k.x, k.y, k.z, "
                     "c.intensity, c.castsShadows, c.shadowBias, c.shadowNormalBias, c.shadowDistance]"
                     ".join('|')") == joined(seeded.color, seeded.intensity, seeded.castsShadows, seeded.shadowBias,
                                             seeded.shadowNormalBias, seeded.shadowDistance));

    completion(scene.vm,
               "c.color = new engine.Vec3(4, 1, 0.5); c.intensity = 0.75; c.castsShadows = true; "
               "c.shadowBias = 0.0078125; c.shadowNormalBias = 0.25; c.shadowDistance = 64; 0");
    engine::DirectionalLight expected;
    expected.color = Vec3{4.0F, 1.0F, 0.5F};
    expected.intensity = 0.75F;
    expected.castsShadows = true;
    expected.shadowBias = 0.0078125F;
    expected.shadowNormalBias = 0.25F;
    expected.shadowDistance = 64.0F;
    const auto& got = scene.held<engine::DirectionalLight>();
    CHECK(got.color == expected.color);
    CHECK(got.intensity == expected.intensity);
    CHECK(got.castsShadows == expected.castsShadows);
    CHECK(got.shadowBias == expected.shadowBias);
    CHECK(got.shadowNormalBias == expected.shadowNormalBias);
    CHECK(got.shadowDistance == expected.shadowDistance);
    CHECK(got == expected);

    CHECK(scene.thrown("c.color = [1, 2, 3]") ==
          "TypeError: engine.DirectionalLight.color: expected an engine.Vec3, got an object");
    CHECK(scene.thrown("c.castsShadows = 1") ==
          "TypeError: engine.DirectionalLight.castsShadows: expected a boolean, got a number");
    CHECK(scene.held<engine::DirectionalLight>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: PointLight end to end (BN40)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::PointLight seeded;
    seeded.color = Vec3{0.125F, 0.5F, 1.5F};
    seeded.intensity = 6.0F;
    seeded.range = 24.5F;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.PointLight); const k = c.color; "
                     "[k.x, k.y, k.z, c.intensity, c.range].join('|')") ==
          joined(seeded.color, seeded.intensity, seeded.range));

    completion(scene.vm, "c.color = new engine.Vec3(2, 0.25, 8); c.intensity = 0.5; c.range = 3.75; 0");
    engine::PointLight expected;
    expected.color = Vec3{2.0F, 0.25F, 8.0F};
    expected.intensity = 0.5F;
    expected.range = 3.75F;
    const auto& got = scene.held<engine::PointLight>();
    CHECK(got.color == expected.color);
    CHECK(got.intensity == expected.intensity);
    CHECK(got.range == expected.range);
    CHECK(got == expected);

    CHECK(scene.thrown("c.color = null") == "TypeError: engine.PointLight.color: expected an engine.Vec3, got null");
    CHECK(scene.held<engine::PointLight>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: MeshRenderer end to end, and its AERO_RANGE is metadata only (BN41)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::MeshRenderer seeded;
    seeded.primitive = 2U;
    seeded.color = Vec3{0.75F, 1.5F, 0.25F};
    seeded.mesh = Guid{0x1111222233334444ULL, 0x5555666677778888ULL};
    seeded.meshIndex = 5U;
    seeded.material = Guid{0x9999AAAABBBBCCCCULL, 0xDDDDEEEEFFFF0001ULL};
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.MeshRenderer); const k = c.color; [c.primitive, k.x, "
                     "k.y, k.z, c.mesh, c.meshIndex, c.material].join('|')") ==
          joined(seeded.primitive, seeded.color, seeded.mesh, seeded.meshIndex, seeded.material));

    completion(scene.vm,
               "c.primitive = 1; c.color = new engine.Vec3(0.5, 4, 8); "
               "c.mesh = 'AAAAAAAAbbbbbbbb0000000000000002'; c.meshIndex = 40; "
               "c.material = '00000000000000010000000000000003'; 0");
    engine::MeshRenderer expected;
    expected.primitive = 1U;
    expected.color = Vec3{0.5F, 4.0F, 8.0F};
    expected.mesh = Guid{0xAAAAAAAABBBBBBBBULL, 0x0000000000000002ULL};
    expected.meshIndex = 40U;
    expected.material = Guid{0x0000000000000001ULL, 0x0000000000000003ULL};
    const auto& got = scene.held<engine::MeshRenderer>();
    CHECK(got.primitive == expected.primitive);
    CHECK(got.color == expected.color);
    CHECK(got.mesh == expected.mesh);
    CHECK(got.meshIndex == expected.meshIndex);
    CHECK(got.material == expected.material);
    CHECK(got == expected);

    // AERO_RANGE(0, 2) is inspector metadata: 3 is stored, and only a non-integer is refused.
    completion(scene.vm, "c.primitive = 3; 0");
    CHECK(scene.held<engine::MeshRenderer>().primitive == 3U);
    expected.primitive = 3U;
    CHECK(scene.thrown("c.primitive = 1.5") ==
          "RangeError: engine.MeshRenderer.primitive: expected an integer in [0, 4294967295], got 1.5");
    CHECK(scene.thrown("c.primitive = 'x'") ==
          "TypeError: engine.MeshRenderer.primitive: expected a number, got a string");
    CHECK(scene.held<engine::MeshRenderer>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: AnimationPlayer end to end (BN42)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::AnimationPlayer seeded;
    seeded.time = 1.75F;
    seeded.speed = -0.5F;
    seeded.loop = false;
    seeded.playing = true;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.AnimationPlayer); [c.time, c.speed, c.loop, c.playing]"
                     ".join('|')") == joined(seeded.time, seeded.speed, seeded.loop, seeded.playing));

    // The two bools take opposite values, so a swapped pair is a different World.
    completion(scene.vm, "c.time = 0.25; c.speed = 2; c.loop = true; c.playing = false; 0");
    engine::AnimationPlayer expected;
    expected.time = 0.25F;
    expected.speed = 2.0F;
    expected.loop = true;
    expected.playing = false;
    const auto& got = scene.held<engine::AnimationPlayer>();
    CHECK(got.time == expected.time);
    CHECK(got.speed == expected.speed);
    CHECK(got.loop == expected.loop);
    CHECK(got.playing == expected.playing);
    CHECK(got == expected);

    CHECK(scene.thrown("c.time = true") == "TypeError: engine.AnimationPlayer.time: expected a number, got a boolean");
    CHECK(scene.held<engine::AnimationPlayer>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: AudioSource end to end (BN43)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::AudioSource seeded;
    seeded.clip = Guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    seeded.volume = 0.5F;
    seeded.pitch = 1.5F;
    seeded.minDistance = 2.0F;
    seeded.maxDistance = 75.0F;
    seeded.loop = true;
    seeded.playing = false;
    seeded.spatialize = false;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.AudioSource); [c.clip, c.volume, c.pitch, "
                     "c.minDistance, c.maxDistance, c.loop, c.playing, c.spatialize].join('|')") ==
          joined(seeded.clip, seeded.volume, seeded.pitch, seeded.minDistance, seeded.maxDistance, seeded.loop,
                 seeded.playing, seeded.spatialize));

    completion(scene.vm,
               "c.clip = '00000000000000FF00000000000000EE'; c.volume = 0.25; c.pitch = 3; "
               "c.minDistance = 0.5; c.maxDistance = 20; c.loop = false; c.playing = true; "
               "c.spatialize = true; 0");
    engine::AudioSource expected;
    expected.clip = Guid{0x00000000000000FFULL, 0x00000000000000EEULL};
    expected.volume = 0.25F;
    expected.pitch = 3.0F;
    expected.minDistance = 0.5F;
    expected.maxDistance = 20.0F;
    expected.loop = false;
    expected.playing = true;
    expected.spatialize = true;
    const auto& got = scene.held<engine::AudioSource>();
    CHECK(got.clip == expected.clip);
    CHECK(got.volume == expected.volume);
    CHECK(got.pitch == expected.pitch);
    CHECK(got.minDistance == expected.minDistance);
    CHECK(got.maxDistance == expected.maxDistance);
    CHECK(got.loop == expected.loop);
    CHECK(got.playing == expected.playing);
    CHECK(got.spatialize == expected.spatialize);
    CHECK(got == expected);

    CHECK(scene.thrown("c.clip = 5") == "TypeError: engine.AudioSource.clip: expected a string, got a number");
    CHECK(scene.thrown("c.clip = 'nope'") == "RangeError: engine.AudioSource.clip: expected 32 hex digits");
    CHECK(scene.held<engine::AudioSource>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: AudioListener end to end (BN44)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::AudioListener seeded;
    seeded.volume = 0.375F;
    scene.seed(seeded);

    CHECK(completion(scene.vm, "globalThis.c = e.get(engine.AudioListener); [c.volume].join('|')") ==
          joined(seeded.volume));

    completion(scene.vm, "c.volume = 0.875; 0");
    engine::AudioListener expected;
    expected.volume = 0.875F;
    CHECK(scene.held<engine::AudioListener>().volume == expected.volume);
    CHECK(scene.held<engine::AudioListener>() == expected);

    CHECK(scene.thrown("c.volume = 'x'") == "TypeError: engine.AudioListener.volume: expected a number, got a string");
    CHECK(scene.held<engine::AudioListener>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: Environment end to end (BN45)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::Environment seeded;
    seeded.backgroundMode = 1U;
    seeded.skyColor = Vec3{0.125F, 0.25F, 0.5F};
    seeded.horizonColor = Vec3{0.75F, 0.625F, 0.375F};
    seeded.groundColor = Vec3{0.0625F, 0.03125F, 0.1875F};
    seeded.solidColor = Vec3{1.5F, 2.5F, 3.5F};
    seeded.ambientMode = 1U;
    seeded.ambientColor = Vec3{0.4375F, 0.5625F, 0.6875F};
    seeded.ambientIntensity = 2.25F;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.Environment); const v = (k) => [k.x, k.y, k.z]; "
                     "[c.backgroundMode, ...v(c.skyColor), ...v(c.horizonColor), ...v(c.groundColor), "
                     "...v(c.solidColor), c.ambientMode, ...v(c.ambientColor), c.ambientIntensity]"
                     ".join('|')") == joined(seeded.backgroundMode, seeded.skyColor, seeded.horizonColor,
                                             seeded.groundColor, seeded.solidColor, seeded.ambientMode,
                                             seeded.ambientColor, seeded.ambientIntensity));

    // Five vectors, three ways to write one: assignment, a view's axes, a view's set.
    completion(scene.vm,
               "c.backgroundMode = 0; c.skyColor = new engine.Vec3(8, 4, 2); "
               "const h = c.horizonColor; h.x = 0.5; h.y = 1.5; h.z = 2.5; c.groundColor.set(6, 7, 9); "
               "c.solidColor = new engine.Vec3(0.25, 0.125, 0.0625); c.ambientMode = 0; "
               "const a = c.ambientColor; a.x = 10; a.y = 11; a.z = 12; c.ambientIntensity = 0.5; 0");
    engine::Environment expected;
    expected.backgroundMode = 0U;
    expected.skyColor = Vec3{8.0F, 4.0F, 2.0F};
    expected.horizonColor = Vec3{0.5F, 1.5F, 2.5F};
    expected.groundColor = Vec3{6.0F, 7.0F, 9.0F};
    expected.solidColor = Vec3{0.25F, 0.125F, 0.0625F};
    expected.ambientMode = 0U;
    expected.ambientColor = Vec3{10.0F, 11.0F, 12.0F};
    expected.ambientIntensity = 0.5F;
    const auto& got = scene.held<engine::Environment>();
    CHECK(got.backgroundMode == expected.backgroundMode);
    CHECK(got.skyColor == expected.skyColor);
    CHECK(got.horizonColor == expected.horizonColor);
    CHECK(got.groundColor == expected.groundColor);
    CHECK(got.solidColor == expected.solidColor);
    CHECK(got.ambientMode == expected.ambientMode);
    CHECK(got.ambientColor == expected.ambientColor);
    CHECK(got.ambientIntensity == expected.ambientIntensity);
    CHECK(got == expected);

    CHECK(scene.thrown("c.backgroundMode = 'x'") ==
          "TypeError: engine.Environment.backgroundMode: expected a number, got a string");
    CHECK(scene.held<engine::Environment>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: SpotLight end to end (BN46)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::SpotLight seeded;
    seeded.color = Vec3{1.25F, 0.5F, 0.75F};
    seeded.intensity = 12.0F;
    seeded.range = 33.0F;
    seeded.innerConeRadians = 0.25F;
    seeded.outerConeRadians = 0.5F;
    scene.seed(seeded);

    CHECK(completion(scene.vm,
                     "globalThis.c = e.get(engine.SpotLight); const k = c.color; [k.x, k.y, k.z, "
                     "c.intensity, c.range, c.innerConeRadians, c.outerConeRadians].join('|')") ==
          joined(seeded.color, seeded.intensity, seeded.range, seeded.innerConeRadians, seeded.outerConeRadians));

    completion(scene.vm,
               "c.color = new engine.Vec3(3, 6, 9); c.intensity = 1.5; c.range = 5.5; "
               "c.innerConeRadians = 0.125; c.outerConeRadians = 1; 0");
    engine::SpotLight expected;
    expected.color = Vec3{3.0F, 6.0F, 9.0F};
    expected.intensity = 1.5F;
    expected.range = 5.5F;
    expected.innerConeRadians = 0.125F;
    expected.outerConeRadians = 1.0F;
    const auto& got = scene.held<engine::SpotLight>();
    CHECK(got.color == expected.color);
    CHECK(got.intensity == expected.intensity);
    CHECK(got.range == expected.range);
    CHECK(got.innerConeRadians == expected.innerConeRadians);
    CHECK(got.outerConeRadians == expected.outerConeRadians);
    CHECK(got == expected);

    CHECK(scene.thrown("c.color = {x: 1, y: 2, z: 3}") ==
          "TypeError: engine.SpotLight.color: expected an engine.Vec3, got an object");
    CHECK(scene.held<engine::SpotLight>() == expected);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a held Transform and position refuse a destroyed entity, and spare its recycled index (BN47)") {
    const LogCapture log;
    BuiltinScene scene;
    engine::Transform seeded;
    seeded.position = Vec3{1.0F, 2.0F, 3.0F};
    scene.seed(seeded);
    completion(scene.vm, "globalThis.t = e.get(engine.Transform); globalThis.p = t.position; 0");
    CHECK(scene.world.destroy(scene.entity));

    const Entity fresh = scene.world.create();
    CHECK(fresh.index == scene.entity.index);
    CHECK(fresh != scene.entity);
    engine::Transform other;
    other.position = Vec3{-4.0F, -5.0F, -6.0F};
    other.scale = Vec3{3.0F, 3.0F, 3.0F};
    REQUIRE(scene.world.add<engine::Transform>(fresh, other) != nullptr);

    CHECK(scene.thrown("t.position = new engine.Vec3(9, 9, 9)") ==
          "TypeError: engine.Transform.position: the entity is not alive");
    CHECK(scene.thrown("p.x = 9") == "TypeError: engine.Transform.position.x: the entity is not alive");
    CHECK(scene.thrown("t.scale") == "TypeError: engine.Transform.scale: the entity is not alive");
    CHECK(scene.thrown("e.get(engine.Transform)") == "TypeError: engine.Entity.get: the entity is not alive");
    CHECK(completion(scene.vm, "e.alive") == "false");

    const engine::Transform* const kept = scene.world.get<engine::Transform>(fresh);
    REQUIRE(kept != nullptr);
    CHECK(*kept == other);
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: the end-to-end list names every built-in a fresh World registers (BN48)") {
    // One entry per case BN37-BN46 and BN52. A new built-in reddens this case until its end-to-end case exists and is
    // listed.
    constexpr std::array<std::string_view, 11> END_TO_END{
        "Transform",   "Camera",        "DirectionalLight", "PointLight", "MeshRenderer",   "AnimationPlayer",
        "AudioSource", "AudioListener", "Environment",      "SpotLight",  "ScriptComponent"};

    const World world;
    std::set<std::string> registered;
    for (std::size_t i = 0; i < world.componentTypeCount(); ++i) {
        std::string name{world.componentTypeName(world.componentTypeAt(i))};
        if (name.starts_with("engine::")) {
            name.erase(0, std::string_view{"engine::"}.size());
        }
        registered.insert(name);
    }
    const std::set<std::string> listed(END_TO_END.begin(), END_TO_END.end());
    CHECK(listed.size() == END_TO_END.size());
    CHECK(registered.size() == world.componentTypeCount());
    for (const std::string& name : registered) {
        INFO("registered, not listed: " << name);
        CHECK(listed.contains(name));
    }
    for (const std::string& name : listed) {
        INFO("listed, not registered: " << name);
        CHECK(registered.contains(name));
    }
}

TEST_CASE("script: ScriptComponent end to end, the Guid shape of AudioSource::clip (BN52)") {
    const LogCapture log;
    BuiltinScene scene;
    REQUIRE(scene.vm.defineEntityGlobal("e", scene.entity) == BindStatus::Ok);

    CHECK(completion(scene.vm, "e.has(engine.ScriptComponent)") == "false");
    CHECK(completion(scene.vm, "globalThis.c = e.add(engine.ScriptComponent); e.has(engine.ScriptComponent)") ==
          "true");
    CHECK(completion(scene.vm, "c.script") == "00000000000000000000000000000000");
    CHECK(scene.held<engine::ScriptComponent>().script == Guid{});

    const Guid stored{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    completion(scene.vm, "c.script = '0123456789abcdeffedcba9876543210'; 0");
    CHECK(scene.held<engine::ScriptComponent>().script == stored);
    CHECK(completion(scene.vm, "c.script") == "0123456789abcdeffedcba9876543210");
    completion(scene.vm,
               "c.script = '00000000000000000000000000000000'; c.script = '0123456789ABCDEFFEDCBA9876543210'; 0");
    CHECK(scene.held<engine::ScriptComponent>().script == stored);
    CHECK(completion(scene.vm, "c.script") == "0123456789abcdeffedcba9876543210");

    CHECK(scene.thrown("c.script = 5") == "TypeError: engine.ScriptComponent.script: expected a string, got a number");
    CHECK(scene.thrown("c.script = 'xyz'") == "RangeError: engine.ScriptComponent.script: expected 32 hex digits");
    CHECK(scene.thrown("c.script = '0123456789abcdeffedcba987654321'") ==
          "RangeError: engine.ScriptComponent.script: expected 32 hex digits");
    CHECK(scene.held<engine::ScriptComponent>().script == stored);

    CHECK(completion(scene.vm, "e.get(engine.ScriptComponent).script") == "0123456789abcdeffedcba9876543210");
    CHECK(completion(scene.vm, "e.remove(engine.ScriptComponent)") == "true");
    CHECK(completion(scene.vm, "e.has(engine.ScriptComponent)") == "false");
    CHECK_FALSE(scene.world.has<engine::ScriptComponent>(scene.entity));
    expectCleanShutdown(scene.vm);
}

TEST_CASE("script: a thousand cached Transforms take fifty frames of position writes (BN49)") {
    const LogCapture log;
    constexpr int ENTITY_COUNT = 1000;
    constexpr int FRAME_COUNT = 50;
    BuiltinScene scene;
    Entity seventh{};
    for (int i = 0; i < ENTITY_COUNT; ++i) {
        const Entity entity = scene.world.create();
        REQUIRE(scene.world.add<engine::Transform>(entity) != nullptr);
        REQUIRE(scene.vm.defineEntityGlobal("e" + std::to_string(i), entity) == BindStatus::Ok);
        if (i == 7) {
            seventh = entity;
        }
    }
    const float before = scene.world.get<engine::Transform>(seventh)->position.x;

    completion(scene.vm,
               "globalThis.ts = []; for (let i = 0; i < 1000; ++i) { "
               "ts.push(globalThis['e' + i].get(engine.Transform)); } ts.length");
    std::vector<double> millis;
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        const EvalResult result = scene.vm.evaluateScript("for (const t of ts) { t.position.x += 0.5; }", "bn49.js");
        const auto stop = std::chrono::steady_clock::now();
        REQUIRE(result.status == EvalStatus::Ok);
        millis.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(millis.begin(), millis.end());
    MESSAGE("BN49: median call over ", ENTITY_COUNT, " entities = ", millis[millis.size() / 2], " ms");

    const engine::Transform* const moved = scene.world.get<engine::Transform>(seventh);
    REQUIRE(moved != nullptr);
    CHECK(moved->position.x == before + 25.0F);
    expectCleanShutdown(scene.vm);
}

// ---- BN56, BN57, LC26, LC28-LC30: the script lifecycle over a World with the built-ins
// -------------------------------

namespace {

using engine::ScriptComponent;
using engine::Transform;
using engine::script::ScriptEntityState;
using engine::script::ScriptInstance;
using engine::script::ScriptSystem;
using engine::script::ScriptSystemReport;
using engine::script::internal::VmAccess;

constexpr std::string_view MOVER_PATH = "res://lifecycle/mover.js";

[[nodiscard]] Guid scriptGuid(std::uint64_t id) { return Guid{0, id}; }

// A VM over a VFS holding one backend, a World with the built-ins, and the ScriptSystem over both. Non-movable: the Vm
// borrows the VFS and the system borrows the Vm and the World; the system is declared last, so it is destroyed first.
class LifecycleScene {
public:
    explicit LifecycleScene(std::unique_ptr<engine::FileSystemBackend> backend) {
        vfs.mount(std::move(backend));
        engine::script::VmCreateResult created = Vm::create(script_test::patient(), vfs);
        REQUIRE(created.vm.valid());
        vm = std::move(created.vm);
        const EvalResult primed = vm.evaluateScript("globalThis.log = [];", "prime.js");
        REQUIRE(primed.status == EvalStatus::Ok);
        system = std::make_unique<ScriptSystem>(vm, world, [this](const Guid& script) { return resolve(script); });
        REQUIRE(system->bound());
    }
    LifecycleScene(const LifecycleScene&) = delete;
    LifecycleScene& operator=(const LifecycleScene&) = delete;
    LifecycleScene(LifecycleScene&&) = delete;
    LifecycleScene& operator=(LifecycleScene&&) = delete;
    ~LifecycleScene() = default;

    void publish(const Guid& guid, std::string_view path) { paths[guid] = std::string{path}; }

    // A live entity with a Transform at the origin, naming `script` when it is not nil.
    [[nodiscard]] Entity spawn(const Guid& script) {
        const Entity entity = world.create();
        REQUIRE(world.add<Transform>(entity) != nullptr);
        if (script != Guid{}) {
            REQUIRE(world.add<ScriptComponent>(entity, ScriptComponent{script}) != nullptr);
        }
        return entity;
    }

    [[nodiscard]] float x(Entity entity) const {
        const auto* const held = world.get<Transform>(entity);
        REQUIRE(held != nullptr);
        return held->position.x;
    }

    [[nodiscard]] std::string log() { return completion(vm, "log.join('|')"); }

    // The system goes first, then the VM is shut down and checked.
    void finish() {
        system.reset();
        expectCleanShutdown(vm);
    }

    engine::VirtualFileSystem vfs;
    World world;
    Vm vm;
    std::map<Guid, std::string> paths;
    std::unique_ptr<ScriptSystem> system;

private:
    [[nodiscard]] std::string resolve(const Guid& script) const {
        const auto found = paths.find(script);
        return found == paths.end() ? std::string{} : found->second;
    }
};

[[nodiscard]] std::unique_ptr<engine::FileSystemBackend> fixtureDirectory() {
    return std::make_unique<engine::DirectoryBackend>(AERO_SCRIPT_FIXTURES_DIR);
}

// A MemoryBackend holding `sources` at res://s/<name>.
[[nodiscard]] std::unique_ptr<engine::FileSystemBackend> memoryOf(
    const std::vector<std::pair<std::string, std::string>>& sources) {
    auto backend = std::make_unique<script_test::MemoryBackend>();
    for (const auto& [name, text] : sources) {
        backend->set("s/" + name, text);
    }
    return backend;
}

// The records at WARN and above whose message contains `needle`.
[[nodiscard]] std::size_t loudRecordsNaming(const LogCapture& capture, std::string_view needle) {
    std::size_t found = 0;
    for (const script_test::CapturedRecord& record : capture.atLeast(engine::LogLevel::Warn)) {
        found += record.message.find(needle) != std::string::npos ? 1U : 0U;
    }
    return found;
}

[[nodiscard]] std::string stateName(ScriptEntityState state) {
    return std::string{engine::script::scriptEntityStateLabel(state)};
}

}  // namespace

TEST_CASE(
    "script: the committed mover bundle moves two Transforms by their own speeds, and stop() keeps the writes "
    "(BN56)") {
    namespace fs = std::filesystem;
    REQUIRE(fs::is_regular_file(fs::path{AERO_SCRIPT_FIXTURES_DIR} / "lifecycle" / "mover.js"));  // never a skip
    const LogCapture log;
    LifecycleScene scene{fixtureDirectory()};
    scene.publish(scriptGuid(1), MOVER_PATH);
    const Entity first = scene.spawn(scriptGuid(1));
    const Entity second = scene.spawn(scriptGuid(1));

    const Transform firstBefore = *scene.world.get<Transform>(first);
    const Transform secondBefore = *scene.world.get<Transform>(second);
    const ScriptComponent firstNames = *scene.world.get<ScriptComponent>(first);
    const ScriptComponent secondNames = *scene.world.get<ScriptComponent>(second);
    REQUIRE(scene.x(first) == 0.0F);
    REQUIRE(scene.x(second) == 0.0F);

    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        const ScriptSystemReport report = scene.system->update(0.5);
        CHECK(report.attached == (frame == 0 ? 2U : 0U));
        CHECK(report.updated == 2U);
        CHECK(report.faulted == 0U);
    }
    CHECK(scene.x(first) == 3.0F);  // 2 * 0.5 three times, exact in binary
    CHECK(scene.x(second) == 3.0F);

    const ScriptInstance handle = scene.system->instanceOf(second);
    REQUIRE(handle.valid());
    REQUIRE(VmAccess::defineInstanceGlobal(scene.vm, handle, "i"));
    CHECK(completion(scene.vm, "i.speed = 4; i.speed") == "4");
    CHECK(completion(scene.vm, "i.entity.alive") == "true");

    const ScriptSystemReport fourth = scene.system->update(0.5);
    CHECK(fourth.updated == 2U);
    CHECK(scene.x(first) == 4.0F);   // still its own speed 2
    CHECK(scene.x(second) == 5.0F);  // 3 + 4 * 0.5

    const ScriptSystemReport stopped = scene.system->stop();
    CHECK(stopped.detached == 2U);
    Transform expectedFirst = firstBefore;
    expectedFirst.position.x = 4.0F;
    Transform expectedSecond = secondBefore;
    expectedSecond.position.x = 5.0F;
    CHECK(*scene.world.get<Transform>(first) == expectedFirst);  // the writes stay, nothing else moved
    CHECK(*scene.world.get<Transform>(second) == expectedSecond);
    CHECK(*scene.world.get<ScriptComponent>(first) == firstNames);
    CHECK(*scene.world.get<ScriptComponent>(second) == secondNames);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    scene.finish();
}

TEST_CASE("script: a bind made around the system faults the hooks that touch a stale entity (LC26)") {
    const LogCapture log;
    const std::string toucher = script_test::classSource(
        "  onUpdate() { const t = this.entity.get(engine.Transform); log.push('touch:' + (t ? 1 : 0)); }");
    const std::string plain = script_test::classSource("  onUpdate() { log.push('plain'); }");
    LifecycleScene scene{memoryOf({{"touch.js", toucher}, {"plain.js", plain}})};
    scene.publish(scriptGuid(1), "res://s/touch.js");
    scene.publish(scriptGuid(2), "res://s/plain.js");
    const Entity touchA = scene.spawn(scriptGuid(1));
    const Entity touchB = scene.spawn(scriptGuid(1));
    const Entity quiet = scene.spawn(scriptGuid(2));

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.updated == 3U);
    CHECK(first.faulted == 0U);
    CHECK(scene.log() == "touch:1|touch:1|plain");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);

    REQUIRE(scene.vm.bindWorld(scene.world) == BindStatus::Ok);  // a bind around the system: a new binding
    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.faulted == 2U);
    CHECK(second.running == 1U);
    CHECK(loudRecordsNaming(log, "the entity belongs to an earlier World binding") == 2U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 2U);
    CHECK(stateName(scene.system->stateOf(touchA)) == "Faulted");
    CHECK(stateName(scene.system->stateOf(touchB)) == "Faulted");
    CHECK(stateName(scene.system->stateOf(quiet)) == "Running");
    CHECK(scene.log() == "touch:1|touch:1|plain|plain");  // the hook that touches no entity kept running

    const ScriptSystemReport third = scene.system->update(0.5);
    CHECK(third.faulted == 0U);
    CHECK(third.running == 1U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 2U);  // a faulted instance is not called again
    scene.finish();
}

TEST_CASE(
    "script: a hook that clears another entity's ScriptComponent keeps that entity's onUpdate from running "
    "(LC28)") {
    const LogCapture log;
    const std::string clearer = script_test::classSource(
        "  onUpdate() { log.push('a:update'); globalThis.b.get(engine.ScriptComponent).script = "
        "'00000000000000000000000000000000'; }");
    const std::string victim = script_test::classSource(
        "  onInit() { log.push('b:init'); }\n  onUpdate() { log.push('b:update'); }\n"
        "  onDestroy() { log.push('b:destroy'); }");
    LifecycleScene scene{memoryOf({{"a.js", clearer}, {"b.js", victim}})};
    scene.publish(scriptGuid(1), "res://s/a.js");
    scene.publish(scriptGuid(2), "res://s/b.js");
    const Entity a = scene.spawn(scriptGuid(1));
    const Entity b = scene.spawn(scriptGuid(2));
    REQUIRE(a.index < b.index);  // A runs before B
    REQUIRE(scene.vm.defineEntityGlobal("b", b) == BindStatus::Ok);

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.attached == 2U);
    CHECK(first.updated == 1U);  // B did not update: it no longer names its script
    CHECK(scene.log() == "b:init|a:update");
    CHECK(scene.world.get<ScriptComponent>(b)->script == Guid{});

    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.detached == 1U);
    CHECK(scene.log() == "b:init|a:update|b:destroy|a:update");
    CHECK_FALSE(scene.system->instanceOf(b).valid());
    CHECK(loudRecordsNaming(log, "") == 0U);
    scene.finish();
}

TEST_CASE(
    "script: an onInit that removes its own ScriptComponent gets no onUpdate, and one onDestroy next update "
    "(LC29)") {
    const LogCapture log;
    const std::string leaver = script_test::classSource(
        "  onInit() { log.push('init'); this.entity.remove(engine.ScriptComponent); }\n"
        "  onUpdate() { log.push('update'); }\n  onDestroy() { log.push('destroy'); }");
    LifecycleScene scene{memoryOf({{"leaver.js", leaver}})};
    scene.publish(scriptGuid(1), "res://s/leaver.js");
    const Entity entity = scene.spawn(scriptGuid(1));

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.attached == 1U);
    CHECK(first.initialised == 1U);
    CHECK(first.updated == 0U);
    CHECK_FALSE(scene.world.has<ScriptComponent>(entity));
    CHECK(scene.log() == "init");

    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.detached == 1U);
    CHECK(second.updated == 0U);
    CHECK(scene.log() == "init|destroy");  // its onInit succeeded, so its onDestroy runs, once
    CHECK_FALSE(scene.system->instanceOf(entity).valid());

    const ScriptSystemReport third = scene.system->update(0.5);
    CHECK(third.detached == 0U);
    CHECK(scene.log() == "init|destroy");
    CHECK(loudRecordsNaming(log, "") == 0U);
    scene.finish();
}

TEST_CASE(
    "script: an onInit that clears another entity's ScriptComponent keeps that entity's onInit from running (LC39)") {
    const LogCapture log;
    const std::string clearer = script_test::classSource(
        "  onInit() { log.push('a:init'); globalThis.b.get(engine.ScriptComponent).script = "
        "'00000000000000000000000000000000'; }\n  onUpdate() { log.push('a:update'); }");
    const std::string victim = script_test::classSource(
        "  onInit() { log.push('b:init'); }\n  onUpdate() { log.push('b:update'); }\n"
        "  onDestroy() { log.push('b:destroy'); }");
    LifecycleScene scene{memoryOf({{"a.js", clearer}, {"b.js", victim}})};
    scene.publish(scriptGuid(1), "res://s/a.js");
    scene.publish(scriptGuid(2), "res://s/b.js");
    const Entity a = scene.spawn(scriptGuid(1));
    const Entity b = scene.spawn(scriptGuid(2));
    REQUIRE(a.index < b.index);  // A's onInit runs before B's
    REQUIRE(scene.vm.defineEntityGlobal("b", b) == BindStatus::Ok);

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.attached == 2U);
    CHECK(first.initialised == 1U);  // B attached, but no longer names its script when its turn comes
    CHECK(first.updated == 1U);
    CHECK(scene.log() == "a:init|a:update");
    CHECK(scene.world.get<ScriptComponent>(b)->script == Guid{});

    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.detached == 1U);
    CHECK(scene.log() == "a:init|a:update|a:update");  // B never ran onInit, so it gets no onDestroy either
    CHECK_FALSE(scene.system->instanceOf(b).valid());
    CHECK(loudRecordsNaming(log, "") == 0U);
    scene.finish();
}

TEST_CASE("script: a ScriptComponent a hook adds attaches at the next update, never inside the current one (LC30)") {
    const LogCapture log;
    const std::string adder = script_test::classSource(
        "  onUpdate() { if (globalThis.go) { globalThis.go = false; log.push('a:add'); "
        "globalThis.c.add(engine.ScriptComponent).script = '" +
        engine::formatGuid(scriptGuid(2)) + "'; } }");
    const std::string added =
        script_test::classSource("  onInit() { log.push('c:init'); }\n  onUpdate() { log.push('c:update'); }");
    LifecycleScene scene{memoryOf({{"adder.js", adder}, {"added.js", added}})};
    scene.publish(scriptGuid(1), "res://s/adder.js");
    scene.publish(scriptGuid(2), "res://s/added.js");
    const Entity a = scene.spawn(scriptGuid(1));
    const Entity c = scene.spawn(Guid{});
    REQUIRE(scene.vm.defineEntityGlobal("c", c) == BindStatus::Ok);
    CHECK(a.index < c.index);

    const ScriptSystemReport first = scene.system->update(0.5);
    CHECK(first.attached == 1U);
    REQUIRE(scene.vm.evaluateScript("globalThis.go = true;", "go.js").status == EvalStatus::Ok);

    const ScriptSystemReport second = scene.system->update(0.5);
    CHECK(second.attached == 0U);  // the add landed during the walk; the snapshot was taken before it
    CHECK(scene.world.has<ScriptComponent>(c));
    CHECK_FALSE(scene.system->instanceOf(c).valid());
    CHECK(scene.log() == "a:add");

    const ScriptSystemReport third = scene.system->update(0.5);
    CHECK(third.attached == 1U);
    CHECK(scene.system->instanceOf(c).valid());
    CHECK(scene.log() == "a:add|c:init|c:update");
    CHECK(loudRecordsNaming(log, "") == 0U);
    scene.finish();
}

TEST_CASE("script: a thousand entities on the committed bundle take two hundred updates with a flat heap (BN57)") {
    const LogCapture log;
    constexpr int ENTITY_COUNT = 1000;
    constexpr int UPDATE_COUNT = 200;
    LifecycleScene scene{fixtureDirectory()};
    scene.publish(scriptGuid(1), MOVER_PATH);
    std::vector<Entity> entities;
    entities.reserve(ENTITY_COUNT);
    for (int i = 0; i < ENTITY_COUNT; ++i) {
        entities.push_back(scene.spawn(scriptGuid(1)));
    }

    std::vector<double> millis;
    millis.reserve(UPDATE_COUNT);
    std::size_t firstHeap = 0;
    std::size_t firstAllocations = 0;
    std::size_t heapDrift = 0;
    std::size_t allocationDrift = 0;
    std::size_t shortUpdates = 0;
    for (int update = 0; update < UPDATE_COUNT; ++update) {
        const auto start = std::chrono::steady_clock::now();
        const ScriptSystemReport report = scene.system->update(0.5);
        const auto stop = std::chrono::steady_clock::now();
        millis.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        shortUpdates += report.updated == static_cast<std::size_t>(ENTITY_COUNT) ? 0U : 1U;

        const engine::script::VmStats stats = scene.vm.stats();
        if (update == 0) {
            firstHeap = stats.heapBytesInUse;
            firstAllocations = stats.liveAllocations;
        } else {
            heapDrift += stats.heapBytesInUse == firstHeap ? 0U : 1U;
            allocationDrift += stats.liveAllocations == firstAllocations ? 0U : 1U;
        }
    }
    std::sort(millis.begin(), millis.end());
    MESSAGE("BN57: update over ", ENTITY_COUNT, " entities, median = ", millis[millis.size() / 2],
            " ms, p95 = ", millis[(millis.size() * 95U) / 100U], " ms");

    CHECK(shortUpdates == 0U);
    CHECK(firstHeap > 0U);  // anti-vacuity: the figure is a measurement
    CHECK(firstAllocations > 0U);
    CHECK(heapDrift == 0U);
    CHECK(allocationDrift == 0U);
    std::size_t wrongX = 0;
    for (const Entity entity : entities) {
        wrongX += scene.x(entity) == 200.0F ? 0U : 1U;
    }
    CHECK(wrongX == 0U);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    scene.finish();
}
