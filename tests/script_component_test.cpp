// Aero Engine -- the generated built-in binding table (task 4.2.1; BN6, and from the later steps BN37-BN49).
//
// Compiled into aero_tests only when AERO_REFLECT_TOOLS is ON: with the tools OFF the table is empty by design and
// there is nothing generated to check.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/animation_player.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/environment.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
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
#include <format>
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

// ---- BN37-BN49: the ten built-in components, end to end through the generated table -------------------------------

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

// A World (all ten built-ins registered by its constructor), a VM over the generated table bound to it, and one
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
    // One entry per case BN37-BN46. A new built-in reddens this case until its end-to-end case exists and is listed.
    constexpr std::array<std::string_view, 10> END_TO_END{
        "Transform",       "Camera",      "DirectionalLight", "PointLight",  "MeshRenderer",
        "AnimationPlayer", "AudioSource", "AudioListener",    "Environment", "SpotLight"};

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
