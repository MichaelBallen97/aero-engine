// tests/script_transform_test.cpp -- task 4.4.2: the helpers of engine.Transform (XF1-XF9). A TU of aero_tests,
// compiled only with the reflect tools ON, because the helpers live on the generated engine.Transform. Each case
// builds a World, binds a VM over the built-in table, defines entities as globals, runs a script and reads the
// result back in C++, where the expectation is the engine's own core math (worldMatrix, transformPoint, the
// quaternion product). Where the operands are exact the assertion is exact; elsewhere the epsilon is in the
// assertion.
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Entity;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::script::BindStatus;
using engine::script::Vm;
using script_test::completion;
using script_test::LogCapture;

constexpr float HALF_SQRT2_F = static_cast<float>(0.7071067811865476);
constexpr Quat TURN_Y{0.0F, HALF_SQRT2_F, 0.0F, HALF_SQRT2_F};  // a quarter turn about +Y
constexpr Quat TURN_X{HALF_SQRT2_F, 0.0F, 0.0F, HALF_SQRT2_F};  // a quarter turn about +X
constexpr Quat HALF_TURN_Y{0.0F, 1.0F, 0.0F, 0.0F};

// A World (every built-in registered by its constructor) and a VM over the generated table bound to it. The VM is
// declared after the World, so it is destroyed first.
struct TransformScene {
    World world;
    Vm vm = script_test::makeBuiltinVm();

    TransformScene() { REQUIRE(vm.bindWorld(world) == BindStatus::Ok); }
    TransformScene(const TransformScene&) = delete;
    TransformScene& operator=(const TransformScene&) = delete;
    TransformScene(TransformScene&&) = delete;
    TransformScene& operator=(TransformScene&&) = delete;
    ~TransformScene() = default;

    // An entity holding `transform`, under `parent` when it is valid, and defined as the global `name`.
    Entity make(std::string_view name, const Transform& transform, Entity parent = {}) {
        const Entity entity = world.create();
        REQUIRE(world.add<Transform>(entity, transform) != nullptr);
        if (parent.valid()) {
            REQUIRE(world.setParent(entity, parent));
        }
        REQUIRE(vm.defineEntityGlobal(name, entity) == BindStatus::Ok);
        return entity;
    }

    [[nodiscard]] Transform& transformOf(Entity entity) {
        auto* const found = world.get<Transform>(entity);
        REQUIRE(found != nullptr);
        return *found;
    }

    // A script expression of type Vec3, as the numbers it prints.
    [[nodiscard]] Vec3 vec3(const std::string& expression) {
        const std::string text =
            completion(vm, "(() => { const v = " + expression + "; return [v.x, v.y, v.z].join(); })()");
        std::vector<float> parts;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t comma = text.find(',', start);
            const std::size_t stop = comma == std::string::npos ? text.size() : comma;
            parts.push_back(std::stof(text.substr(start, stop - start)));
            start = stop + 1;
        }
        REQUIRE(parts.size() == 3U);
        return Vec3{parts[0], parts[1], parts[2]};
    }

    // Runs one statement and returns "Name: message" of what it threw ("no throw" when it did not).
    [[nodiscard]] std::string thrown(const std::string& statement) {
        return completion(vm, "(() => { try { " + statement +
                                  "; } catch (err) { return err.name + ': ' + err.message; } return 'no throw'; })()");
    }
};

[[nodiscard]] bool near(Vec3 a, Vec3 b, float epsilon) {
    return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon && std::abs(a.z - b.z) <= epsilon;
}

[[nodiscard]] bool nearQuat(Quat a, Quat b, float epsilon) {
    return std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon && std::abs(a.z - b.z) <= epsilon &&
           std::abs(a.w - b.w) <= epsilon;
}

[[nodiscard]] Vec3 worldPositionOf(const World& world, Entity entity) {
    return engine::transformPoint(engine::worldMatrix(world, entity), Vec3{});
}

[[nodiscard]] Vec3 worldAxisOf(const World& world, Entity entity, Vec3 axis) {
    return engine::normalizeOrZero(engine::transformDirection(engine::worldMatrix(world, entity), axis));
}

[[nodiscard]] Transform transformAt(Vec3 position, Quat rotation = Quat::identity(), Vec3 scale = Vec3::one()) {
    Transform transform;
    transform.position = position;
    transform.rotation = rotation;
    transform.scale = scale;
    return transform;
}

}  // namespace

TEST_CASE("transform: worldPosition is the world matrix's translation, as a new detached Vec3 (XF1)") {
    const LogCapture log;
    TransformScene scene;
    const Entity parent = scene.make("p", transformAt(Vec3{0.0F, 0.0F, 5.0F}, TURN_Y, Vec3{2.0F, 2.0F, 2.0F}));
    const Entity child = scene.make("c", transformAt(Vec3{1.0F, 0.0F, 0.0F}), parent);

    const Vec3 got = scene.vec3("c.get(engine.Transform).worldPosition()");
    const Vec3 expected = engine::transformPoint(engine::worldMatrix(scene.world, child), Vec3{});
    CHECK(near(got, expected, 1e-5F));
    CHECK_FALSE(near(got, Vec3{1.0F, 0.0F, 0.0F}, 0.5F));  // not the local position

    // The result is detached: writing it leaves the Transform alone.
    const Transform before = scene.transformOf(child);
    completion(scene.vm, "const w = c.get(engine.Transform).worldPosition(); w.x = 99; w.set(7, 8, 9); 0");
    CHECK(scene.transformOf(child) == before);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: forward, right and up are the world axes, unit length, under a scaled parent (XF2)") {
    const LogCapture log;
    TransformScene scene;
    const Entity parent = scene.make("p", transformAt(Vec3{1.0F, 2.0F, 3.0F}, TURN_Y, Vec3{2.0F, 3.0F, 4.0F}));
    const Entity child = scene.make("c", transformAt(Vec3{0.5F, 0.0F, 0.0F}, TURN_X), parent);

    CHECK(
        near(scene.vec3("c.get(engine.Transform).forward()"), worldAxisOf(scene.world, child, Vec3{0, 0, -1}), 1e-6F));
    CHECK(near(scene.vec3("c.get(engine.Transform).right()"), worldAxisOf(scene.world, child, Vec3{1, 0, 0}), 1e-6F));
    CHECK(near(scene.vec3("c.get(engine.Transform).up()"), worldAxisOf(scene.world, child, Vec3{0, 1, 0}), 1e-6F));
    for (const char* helper : {"forward", "right", "up"}) {
        CAPTURE(helper);
        const std::string length =
            completion(scene.vm, std::string{"String(c.get(engine.Transform)."} + helper + "().length().toFixed(6))");
        CHECK(length == "1.000000");
    }

    // A root with no turn: forward is exactly -Z (not +Z, not scaled).
    scene.make("r", Transform{});
    CHECK(scene.vec3("r.get(engine.Transform).forward()") == Vec3{0.0F, 0.0F, -1.0F});
    CHECK(scene.vec3("r.get(engine.Transform).right()") == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(scene.vec3("r.get(engine.Transform).up()") == Vec3{0.0F, 1.0F, 0.0F});

    // A parent that collapses z: forward is the zero vector, right is still a unit vector.
    const Entity flat = scene.make("f", transformAt(Vec3{}, Quat::identity(), Vec3{1.0F, 1.0F, 0.0F}));
    scene.make("fc", Transform{}, flat);
    CHECK(scene.vec3("fc.get(engine.Transform).forward()") == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(near(scene.vec3("fc.get(engine.Transform).right()"), Vec3{1.0F, 0.0F, 0.0F}, 1e-6F));
    CHECK(completion(scene.vm, "fc.get(engine.Transform).right().length().toFixed(6)") == "1.000000");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: translate moves along the entity's own axes, ignoring scale, and returns the receiver (XF3)") {
    const LogCapture log;
    TransformScene scene;
    const Vec3 start{1.0F, 2.0F, 3.0F};
    const Entity e = scene.make("e", transformAt(start, TURN_Y, Vec3{5.0F, 5.0F, 5.0F}));

    CHECK(completion(scene.vm,
                     "(() => { const t = e.get(engine.Transform); return String(t.translate(new engine.Vec3(0, 0, -1)) "
                     "=== t); })()") == "true");
    const Vec3 step = engine::normalizeOrIdentity(TURN_Y) * Vec3{0.0F, 0.0F, -1.0F};
    const Transform& moved = scene.transformOf(e);
    CHECK(near(moved.position, start + step, 1e-6F));
    CHECK_FALSE(near(moved.position, start + Vec3{0.0F, 0.0F, -1.0F}, 0.1F));  // not the parent-space axis
    CHECK_FALSE(near(moved.position, start + step * 5.0F, 0.1F));              // not scaled
    CHECK(moved.rotation == TURN_Y);
    CHECK(moved.scale == Vec3{5.0F, 5.0F, 5.0F});
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: rotate applies q in the entity's own frame and stores a unit quaternion (XF4)") {
    const LogCapture log;
    TransformScene scene;
    const Entity e = scene.make("e", transformAt(Vec3{}, TURN_Y));

    completion(scene.vm, "e.get(engine.Transform).rotate(new engine.Quat(Math.SQRT1_2, 0, 0, Math.SQRT1_2)); 0");
    const Quat expected = engine::normalizeOrIdentity(TURN_Y * TURN_X);
    CHECK(nearQuat(scene.transformOf(e).rotation, expected, 1e-6F));
    CHECK_FALSE(nearQuat(scene.transformOf(e).rotation, engine::normalizeOrIdentity(TURN_X * TURN_Y), 1e-3F));

    // The same turn scaled by two stores the same unit quaternion.
    scene.transformOf(e).rotation = TURN_Y;
    completion(scene.vm,
               "e.get(engine.Transform).rotate(new engine.Quat(2 * Math.SQRT1_2, 0, 0, 2 * Math.SQRT1_2)); 0");
    CHECK(nearQuat(scene.transformOf(e).rotation, expected, 1e-6F));
    CHECK(std::abs(engine::length(scene.transformOf(e).rotation) - 1.0F) <= 1e-6F);
    CHECK(completion(
              scene.vm,
              "(() => { const t = e.get(engine.Transform); return String(t.rotate(new engine.Quat()) === t); })()") ==
          "true");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: lookAt faces -Z at the target, with the given or the world up (XF5)") {
    const LogCapture log;
    TransformScene scene;
    scene.make("e", Transform{});

    CHECK(completion(scene.vm,
                     "(() => { const t = e.get(engine.Transform); return String(t.lookAt(new engine.Vec3(3, 0, -4)) "
                     "=== t); })()") == "true");
    CHECK(near(scene.vec3("e.get(engine.Transform).forward()"), Vec3{0.6F, 0.0F, -0.8F}, 1e-6F));
    const Vec3 up = scene.vec3("e.get(engine.Transform).up()");
    CHECK(up.y > 0.0F);
    CHECK(std::abs(engine::dot(up, scene.vec3("e.get(engine.Transform).forward()"))) <= 1e-6F);

    CHECK(completion(scene.vm,
                     "(() => { const u = e.get(engine.Transform);"
                     "return String(u.lookAt(new engine.Vec3(0, 0, -5), new engine.Vec3(1, 0, 0)) === u); })()") ==
          "true");
    CHECK(near(scene.vec3("e.get(engine.Transform).up()"), Vec3{1.0F, 0.0F, 0.0F}, 1e-6F));
    CHECK(near(scene.vec3("e.get(engine.Transform).forward()"), Vec3{0.0F, 0.0F, -1.0F}, 1e-6F));
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: lookAt under a turned parent stores the local rotation, not the world one (XF6)") {
    const LogCapture log;
    TransformScene scene;
    const Entity parent = scene.make("p", transformAt(Vec3{0.0F, 0.0F, 10.0F}, HALF_TURN_Y));
    const Entity child = scene.make("c", transformAt(Vec3{0.0F, 0.0F, 1.0F}), parent);

    completion(scene.vm, "c.get(engine.Transform).lookAt(new engine.Vec3(3, 0, 4)); 0");
    const Vec3 eye = worldPositionOf(scene.world, child);
    const Vec3 expected = engine::normalize(Vec3{3.0F, 0.0F, 4.0F} - eye);
    CHECK(near(worldAxisOf(scene.world, child, Vec3{0, 0, -1}), expected, 1e-5F));

    engine::Trs world;
    REQUIRE(engine::decompose(engine::worldMatrix(scene.world, child), world));
    CHECK_FALSE(nearQuat(scene.transformOf(child).rotation, world.rotation, 1e-2F));
    CHECK_FALSE(nearQuat(scene.transformOf(child).rotation,
                         Quat{-world.rotation.x, -world.rotation.y, -world.rotation.z, -world.rotation.w}, 1e-2F));
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: every degenerate lookAt leaves the rotation bit-identical, silently (XF7)") {
    const LogCapture log;
    TransformScene scene;
    const Quat turned{0.25F, 0.5F, -0.5F, 0.6614378F};
    const Entity e = scene.make("e", transformAt(Vec3{1.0F, 2.0F, 3.0F}, turned));
    const Entity flat = scene.make("flat", transformAt(Vec3{}, Quat::identity(), Vec3{1.0F, 0.0F, 1.0F}));
    const Entity under = scene.make("under", transformAt(Vec3{}, turned), flat);

    const std::vector<std::string> statements{
        "e.get(engine.Transform).lookAt(new engine.Vec3(1, 2, 3))",                              // own position
        "e.get(engine.Transform).lookAt(new engine.Vec3(1.000001, 2, 3))",                       // 1e-6 away
        "e.get(engine.Transform).lookAt(new engine.Vec3(1, 12, 3))",                             // straight up
        "e.get(engine.Transform).lookAt(new engine.Vec3(5, 2, 3), new engine.Vec3(0, 0, 0))",    // zero up
        "e.get(engine.Transform).lookAt(new engine.Vec3(NaN, 0, 0))",                            // NaN target
        "e.get(engine.Transform).lookAt(new engine.Vec3(0, 0, 0), new engine.Vec3(NaN, 1, 0))",  // NaN up
        "e.get(engine.Transform).lookAt(new engine.Vec3(Infinity, 0, 0))",                       // infinite target
        "under.get(engine.Transform).lookAt(new engine.Vec3(0, 0, -5))",                         // decompose fails
    };
    for (const std::string& statement : statements) {
        CAPTURE(statement);
        CHECK(scene.thrown(statement) == "no throw");
        CHECK(scene.transformOf(e).rotation == turned);
        CHECK(scene.transformOf(under).rotation == turned);
    }
    // A target that is valid again moves it, so the cases above were not no-ops of a broken helper.
    CHECK(scene.thrown("e.get(engine.Transform).lookAt(new engine.Vec3(5, 2, 3))") == "no throw");
    CHECK_FALSE(scene.transformOf(e).rotation == turned);
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: a wrong argument or receiver is a TypeError naming the helper, and writes nothing (XF8)") {
    const LogCapture log;
    TransformScene scene;
    const Transform seeded = transformAt(Vec3{1.0F, 2.0F, 3.0F}, TURN_Y, Vec3{2.0F, 2.0F, 2.0F});
    const Entity e = scene.make("e", seeded);
    const Entity other = scene.world.create();
    REQUIRE(scene.world.add<engine::Camera>(other) != nullptr);
    REQUIRE(scene.vm.defineEntityGlobal("other", other) == BindStatus::Ok);

    CHECK(scene.thrown("e.get(engine.Transform).translate(1)") ==
          "TypeError: engine.Transform.translate: expected an engine.Vec3, got a number");
    CHECK(scene.thrown("e.get(engine.Transform).translate(new engine.Quat())") ==
          "TypeError: engine.Transform.translate: expected an engine.Vec3, got an engine.Quat");
    CHECK(scene.thrown("e.get(engine.Transform).rotate(new engine.Vec3())") ==
          "TypeError: engine.Transform.rotate: expected an engine.Quat, got an engine.Vec3");
    CHECK(scene.thrown("e.get(engine.Transform).lookAt({x: 0, y: 0, z: 0})") ==
          "TypeError: engine.Transform.lookAt: expected an engine.Vec3, got an object");
    CHECK(scene.thrown("e.get(engine.Transform).lookAt(new engine.Vec3(1, 1, 1), 5)") ==
          "TypeError: engine.Transform.lookAt: expected an engine.Vec3, got a number");
    CHECK(scene.transformOf(e) == seeded);

    // The receiver: another component's object, and a plain object, never taken by shape.
    CHECK(scene.thrown("engine.Transform.prototype.translate.call(other.get(engine.Camera), new engine.Vec3())") ==
          "TypeError: engine.Transform.translate: the receiver is not an engine.Transform");
    CHECK(scene.thrown("engine.Transform.prototype.forward.call({position: 1})") ==
          "TypeError: engine.Transform.forward: the receiver is not an engine.Transform");

    // A held object after remove: the value checks come first, then the entity has no Transform.
    completion(scene.vm, "globalThis.held = e.get(engine.Transform); e.remove(engine.Transform); 0");
    CHECK(scene.thrown("held.translate(new engine.Vec3())") ==
          "TypeError: engine.Transform.translate: the entity has no engine.Transform");
    CHECK(scene.thrown("held.worldPosition()") ==
          "TypeError: engine.Transform.worldPosition: the entity has no engine.Transform");
    CHECK(scene.thrown("held.translate(1)") ==
          "TypeError: engine.Transform.translate: expected an engine.Vec3, got a number");
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("transform: the seven helpers sit on the frozen Transform prototype alone (XF9)") {
    const LogCapture log;
    TransformScene scene;
    const auto components = engine::script::internal::builtinComponentBindings().components();
    const auto bound = std::ranges::find_if(components, [](const auto& c) { return c.scriptName == "Transform"; });
    REQUIRE(bound != components.end());

    const std::string helpers = "['worldPosition', 'forward', 'right', 'up', 'translate', 'rotate', 'lookAt']";
    CHECK(completion(scene.vm,
                     "(() => { const d = (n) => Object.getOwnPropertyDescriptor(engine.Transform.prototype, n);"
                     "return " +
                         helpers +
                         ".every((n) => d(n) !== undefined && typeof d(n).value === 'function').toString(); })()") ==
          "true");
    CHECK(completion(scene.vm, "Object.isFrozen(engine.Transform.prototype).toString()") == "true");
    // The lengths are the arities the prelude declares (DT28 compares them with the declarations).
    CHECK(completion(scene.vm, "(() => " + helpers + ".map((n) => engine.Transform.prototype[n].length).join())()") ==
          "0,0,0,0,1,1,2");

    // No other class has any of the seven.
    CHECK(completion(scene.vm,
                     "(() => Object.getOwnPropertyNames(engine).filter((c) => c !== 'Transform' && "
                     "Object.hasOwn(engine[c], 'prototype') && " +
                         helpers + ".some((n) => Object.hasOwn(engine[c].prototype, n))).join())()")
              .empty());

    // The prototype's own names are the bound fields plus the seven: no field shares a helper's name.
    const std::size_t fields = bound->fields.size();
    CHECK(completion(scene.vm,
                     "Object.getOwnPropertyNames(engine.Transform.prototype)"
                     ".filter((n) => n !== 'constructor').length.toString()") == std::to_string(fields + 7U));

    CHECK(scene.vec3("engine.spawn().get(engine.Transform).forward()") == Vec3{0.0F, 0.0F, -1.0F});
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}
