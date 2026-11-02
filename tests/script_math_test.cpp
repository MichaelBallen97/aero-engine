// tests/script_math_test.cpp -- task 4.4.2: the in-place helpers of engine.Vec3 and engine.Quat (VX1-VX15). A TU of
// aero_tests, which supplies main() from test_main.cpp. Every case runs over ProbeScene, whose ScriptProbe holds a
// Vec3 `v` and a Quat `q`: a result is copied into the probe's field and read in C++, so the C++ side of the
// comparison is the engine's own core math. Where the operands make the float arithmetic exact (small integers,
// halves) the assertion is exact; elsewhere it is within 1e-6, because the C++ side is another translation unit and
// the default contraction of GCC may fuse differently.
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <format>
#include <ostream>
#include <string>
#include <string_view>

namespace {

using engine::Quat;
using engine::Vec3;
using script_test::completion;
using script_test::ProbeScene;

// The square root of one half, as the script spells it and as the C++ side narrows it.
constexpr double HALF_SQRT2 = 0.7071067811865476;
constexpr float HALF_SQRT2_F = static_cast<float>(HALF_SQRT2);
constexpr float TOLERANCE = 1e-6F;

// `t(f)` reads "Name: message" of what f throws, or "no throw".
constexpr std::string_view THROWS =
    "const t = (f) => { try { f(); return 'no throw'; } catch (x) { return x.name + ': ' + x.message; } };";

[[nodiscard]] std::string iife(std::string_view body) {
    return "(() => { " + std::string{THROWS} + " " + std::string{body} + " })()";
}

// Runs a script expression of type Vec3 and returns it as the probe's `v` holds it after a copy.
[[nodiscard]] Vec3 vecOf(ProbeScene& scene, std::string_view expression) {
    completion(scene.vm, std::format("e.get(engine.ScriptProbe).v.copy({}); 0", expression));
    return scene.probe().v;
}

[[nodiscard]] Quat quatOf(ProbeScene& scene, std::string_view expression) {
    completion(scene.vm, std::format("e.get(engine.ScriptProbe).q.copy({}); 0", expression));
    return scene.probe().q;
}

// A script expression of type number, narrowed through the probe's float field.
[[nodiscard]] float numberOf(ProbeScene& scene, std::string_view expression) {
    completion(scene.vm, std::format("e.get(engine.ScriptProbe).f = {}; 0", expression));
    return scene.probe().f;
}

[[nodiscard]] bool close(Vec3 a, Vec3 b) {
    return std::abs(a.x - b.x) <= TOLERANCE && std::abs(a.y - b.y) <= TOLERANCE && std::abs(a.z - b.z) <= TOLERANCE;
}

// Componentwise, not up to sign: the helpers keep the sign the core's own function gives.
[[nodiscard]] bool close(Quat a, Quat b) {
    return std::abs(a.x - b.x) <= TOLERANCE && std::abs(a.y - b.y) <= TOLERANCE && std::abs(a.z - b.z) <= TOLERANCE &&
           std::abs(a.w - b.w) <= TOLERANCE;
}

[[nodiscard]] bool allNaN(Quat q) { return std::isnan(q.x) && std::isnan(q.y) && std::isnan(q.z) && std::isnan(q.w); }

// A quarter turn about +Y and about +X, as a script spells them.
constexpr std::string_view TURN_Y = "new engine.Quat(0, Math.SQRT1_2, 0, Math.SQRT1_2)";
constexpr std::string_view TURN_X = "new engine.Quat(Math.SQRT1_2, 0, 0, Math.SQRT1_2)";
constexpr Quat TURN_Y_C{0.0F, HALF_SQRT2_F, 0.0F, HALF_SQRT2_F};
constexpr Quat TURN_X_C{HALF_SQRT2_F, 0.0F, 0.0F, HALF_SQRT2_F};

}  // namespace

TEST_CASE("math: the Vec3 mutators are exactly the core's arithmetic and return their receiver (VX1)") {
    const script_test::LogCapture log;
    ProbeScene scene;
    const Vec3 a{1.0F, 2.0F, 3.0F};
    const Vec3 v{4.0F, -5.0F, 6.0F};
    const std::string lhs = "new engine.Vec3(1, 2, 3)";
    const std::string rhs = "new engine.Vec3(4, -5, 6)";

    CHECK(vecOf(scene, lhs + ".add(" + rhs + ")") == a + v);
    CHECK(vecOf(scene, lhs + ".sub(" + rhs + ")") == a - v);
    CHECK(vecOf(scene, lhs + ".scale(2.5)") == a * 2.5F);
    CHECK(vecOf(scene, lhs + ".addScaled(" + rhs + ", 0.5)") == a + v * 0.5F);
    CHECK(vecOf(scene, lhs + ".negate()") == -a);
    CHECK(vecOf(scene, lhs + ".lerp(" + rhs + ", 0.5)") == engine::lerp(a, v, 0.5F));
    CHECK(vecOf(scene, lhs + ".lerp(" + rhs + ", 2)") == engine::lerp(a, v, 2.0F));  // t is not clamped
    CHECK(vecOf(scene, lhs + ".cross(" + rhs + ")") == engine::cross(a, v));

    // Each returns its receiver, so a chain works.
    CHECK(completion(scene.vm, iife("const a = " + lhs + ", v = " + rhs +
                                    ";"
                                    "return [a.add(v) === a, a.sub(v) === a, a.scale(2) === a, a.addScaled(v, 2) === a,"
                                    "  a.negate() === a, a.lerp(v, 0.5) === a, a.cross(v) === a].join();")) ==
          "true,true,true,true,true,true,true");
    CHECK(vecOf(scene, lhs + ".add(" + rhs + ").scale(2).negate()") == -((a + v) * 2.0F));
    CHECK(log.countAtLeast(engine::LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: Vec3 normalize is total, a zero stays zero and NaN stays NaN without a Debug abort (VX2)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(vecOf(scene, "new engine.Vec3(3, 4, 0).normalize()") == engine::normalizeOrZero(Vec3{3.0F, 4.0F, 0.0F}));
    CHECK(vecOf(scene, "new engine.Vec3(0, 0, 0).normalize()") == Vec3{});
    const Vec3 nan = vecOf(scene, "new engine.Vec3(NaN, 1, 0).normalize()");
    CHECK(std::isnan(nan.x));
    CHECK(std::isnan(nan.y));
    CHECK(std::isnan(nan.z));
    CHECK(completion(scene.vm, "new engine.Vec3(1, 2, 3).normalize() instanceof engine.Vec3") == "true");
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: dot, length, lengthSquared and distanceTo return the core's floats and write nothing (VX3)") {
    const script_test::LogCapture log;
    ProbeScene scene;
    const Vec3 a{3.0F, 4.0F, 0.0F};
    const Vec3 v{0.0F, 4.0F, 3.0F};
    const std::string lhs = "new engine.Vec3(3, 4, 0)";
    const std::string rhs = "new engine.Vec3(0, 4, 3)";

    CHECK(numberOf(scene, lhs + ".dot(" + rhs + ")") == 16.0F);
    CHECK(numberOf(scene, lhs + ".length()") == 5.0F);
    CHECK(numberOf(scene, lhs + ".lengthSquared()") == 25.0F);
    CHECK(numberOf(scene, lhs + ".distanceTo(" + rhs + ")") == engine::distance(a, v));
    CHECK(completion(scene.vm, iife("const a = " + lhs + ", v = " + rhs +
                                    ";"
                                    "a.dot(v); a.length(); a.lengthSquared(); a.distanceTo(v);"
                                    "return [a.x, a.y, a.z, v.x, v.y, v.z].join();")) == "3,4,0,0,4,3");
    CHECK(completion(scene.vm, "typeof new engine.Vec3(1, 2, 3).length()") == "number");
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: applyQuat rotates by the normalized quaternion and a zero quaternion changes nothing (VX4)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    const Vec3 turned = vecOf(scene, "new engine.Vec3(1, 0, 0).applyQuat(" + std::string{TURN_Y} + ")");
    CHECK(close(turned, Vec3{0.0F, 0.0F, -1.0F}));
    // The same rotation scaled by 2 gives the same vector: the quaternion is normalized first.
    const Vec3 scaled =
        vecOf(scene, "new engine.Vec3(1, 0, 0).applyQuat(new engine.Quat(0, 2 * Math.SQRT1_2, 0, 2 * Math.SQRT1_2))");
    CHECK(close(scaled, Vec3{0.0F, 0.0F, -1.0F}));
    CHECK(vecOf(scene, "new engine.Vec3(1, 2, 3).applyQuat(new engine.Quat(0, 0, 0, 0))") == Vec3{1.0F, 2.0F, 3.0F});
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: a helper on a view writes the component's field and returns the same view (VX5)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(completion(scene.vm, iife("const p = e.get(engine.ScriptProbe), v = p.v;"
                                    "return v.addScaled(new engine.Vec3(1, 0, 0), 2) === v;")) == "true");
    CHECK(scene.probe().v == Vec3{3.0F, 2.0F, 3.0F});
    CHECK(completion(scene.vm, iife("const p = e.get(engine.ScriptProbe), q = p.q;"
                                    "return q.multiply(" +
                                    std::string{TURN_Y} + ") === q;")) == "true");
    CHECK(close(scene.probe().q, TURN_Y_C));
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: an argument is read out whole before the receiver is written, so aliasing is safe (VX6)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(
        completion(scene.vm, iife("const v = new engine.Vec3(1, 2, 3); v.cross(v); return [v.x, v.y, v.z].join();")) ==
        "0,0,0");
    completion(scene.vm, "e.get(engine.ScriptProbe).v.add(e.get(engine.ScriptProbe).v); 0");
    CHECK(scene.probe().v == Vec3{2.0F, 4.0F, 6.0F});
    completion(scene.vm, "e.get(engine.ScriptProbe).v.lerp(e.get(engine.ScriptProbe).v, 3); 0");
    CHECK(scene.probe().v == Vec3{2.0F, 4.0F, 6.0F});
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: clone and toEuler return detached vectors, never a view (VX7)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(completion(scene.vm, iife("const p = e.get(engine.ScriptProbe), c = p.v.clone();"
                                    "const same = c !== p.v, kind = c instanceof engine.Vec3;"
                                    "c.x = 99; c.add(new engine.Vec3(1, 1, 1));"
                                    "return [same, kind, c.x, p.v.x].join();")) == "true,true,100,1");
    CHECK(scene.probe().v == Vec3{1.0F, 2.0F, 3.0F});
    CHECK(completion(scene.vm, iife("const p = e.get(engine.ScriptProbe), c = p.q.clone();"
                                    "const kind = c instanceof engine.Quat; c.x = 5;"
                                    "return [c !== p.q, kind, c.x, p.q.x].join();")) == "true,true,5,0");
    CHECK(scene.probe().q == Quat::identity());
    CHECK(completion(scene.vm, iife("const p = e.get(engine.ScriptProbe), r = p.q.toEuler();"
                                    "const kind = r instanceof engine.Vec3; r.x = 7;"
                                    "return [kind, p.q.x].join();")) == "true,0");
    CHECK(scene.probe().q == Quat::identity());
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: an argument of the wrong kind is a TypeError that writes nothing and converts nothing (VX8)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    const std::string rows = completion(scene.vm, iife(R"js(
        const p = e.get(engine.ScriptProbe);
        globalThis.flag = false;
        const o = { valueOf() { globalThis.flag = true; return 2; } };
        return [
            t(() => p.v.add(1)),
            t(() => p.v.add(new engine.Quat())),
            t(() => p.v.add({ x: 1, y: 2, z: 3 })),
            t(() => p.v.scale("2")),
            t(() => p.v.scale(o)),
            t(() => p.v.addScaled(new engine.Vec3(1, 1, 1))),
            t(() => p.q.multiply(new engine.Vec3())),
            t(() => p.q.setFromAxisAngle(new engine.Quat(), 1)),
            t(() => p.q.setFromEuler(1, 2, "3")),
        ].join("|");
    )js"));
    CHECK(rows ==
          "TypeError: engine.Vec3.add: expected an engine.Vec3, got a number|"
          "TypeError: engine.Vec3.add: expected an engine.Vec3, got an engine.Quat|"
          "TypeError: engine.Vec3.add: expected an engine.Vec3, got an object|"
          "TypeError: engine.Vec3.scale: expected a number, got a string|"
          "TypeError: engine.Vec3.scale: expected a number, got an object|"
          "TypeError: engine.Vec3.addScaled: expected a number, got undefined|"
          "TypeError: engine.Quat.multiply: expected an engine.Quat, got an engine.Vec3|"
          "TypeError: engine.Quat.setFromAxisAngle: expected an engine.Vec3, got an engine.Quat|"
          "TypeError: engine.Quat.setFromEuler: expected a number, got a string");
    CHECK(completion(scene.vm, "String(flag)") == "false");
    CHECK(scene.probe().v == Vec3{1.0F, 2.0F, 3.0F});
    CHECK(scene.probe().q == Quat::identity());
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: a helper lifted onto the other class refuses its receiver and names the class (VX9)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(completion(scene.vm, iife(R"js(
        return [
            t(() => engine.Quat.prototype.invert.call(new engine.Vec3())),
            t(() => engine.Vec3.prototype.add.call({}, new engine.Vec3())),
            t(() => engine.Vec3.prototype.normalize.call(new engine.Quat())),
            t(() => engine.Quat.prototype.dot.call(new engine.Vec3(), new engine.Quat())),
            t(() => engine.Vec3.prototype.clone.call(engine.Vec3.prototype)),
        ].join("|");
    )js")) ==
          "TypeError: engine.Quat.invert: the receiver is not an engine.Quat|"
          "TypeError: engine.Vec3.add: the receiver is not an engine.Vec3|"
          "TypeError: engine.Vec3.normalize: the receiver is not an engine.Vec3|"
          "TypeError: engine.Quat.dot: the receiver is not an engine.Quat|"
          "TypeError: engine.Vec3.clone: the receiver is not an engine.Vec3");
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: multiply applies the argument first and premultiply last, and the two differ (VX10)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    const Quat product = quatOf(scene, std::string{TURN_Y} + ".multiply(" + std::string{TURN_X} + ")");
    const Quat swapped = quatOf(scene, std::string{TURN_Y} + ".premultiply(" + std::string{TURN_X} + ")");
    CHECK(close(product, TURN_Y_C * TURN_X_C));
    CHECK(close(swapped, TURN_X_C * TURN_Y_C));
    CHECK_FALSE(close(product, swapped));
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: invert and Quat normalize are total, a zero gives the identity and NaN gives NaN (VX11)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    CHECK(close(quatOf(scene, std::string{TURN_Y} + ".invert()"), engine::inverse(TURN_Y_C)));
    CHECK(quatOf(scene, "new engine.Quat(0, 0, 0, 0).invert()") == Quat::identity());
    CHECK(allNaN(quatOf(scene, "new engine.Quat(NaN, 0, 0, 1).invert()")));
    CHECK(quatOf(scene, "new engine.Quat(0, 0, 0, 0).normalize()") == Quat::identity());
    CHECK(close(quatOf(scene, "new engine.Quat(0, 2, 0, 0).normalize()"), Quat{0.0F, 1.0F, 0.0F, 0.0F}));
    CHECK(allNaN(quatOf(scene, "new engine.Quat(NaN, 0, 0, 1).normalize()")));
    CHECK(completion(scene.vm, iife("const q = new engine.Quat(0, 0, 0, 0);"
                                    "return [q.invert() === q, q.normalize() === q].join();")) == "true,true");
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: setFromAxisAngle normalizes its axis and is total for a zero or non-finite axis (VX12)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    const Quat expected = engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, 0.75F);
    CHECK(close(quatOf(scene, "new engine.Quat().setFromAxisAngle(new engine.Vec3(0, 1, 0), 0.75)"), expected));
    CHECK(close(quatOf(scene, "new engine.Quat().setFromAxisAngle(new engine.Vec3(0, 2, 0), 0.75)"), expected));
    CHECK(quatOf(scene, "new engine.Quat(1, 0, 0, 0).setFromAxisAngle(new engine.Vec3(0, 0, 0), 0.75)") ==
          Quat::identity());
    CHECK(allNaN(quatOf(scene, "new engine.Quat().setFromAxisAngle(new engine.Vec3(NaN, 0, 0), 0.75)")));
    CHECK(allNaN(quatOf(scene, "new engine.Quat().setFromAxisAngle(new engine.Vec3(Infinity, 0, 0), 0.75)")));
    CHECK(allNaN(quatOf(scene, "new engine.Quat().setFromAxisAngle(new engine.Vec3(0, 1, 0), NaN)")));
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: setFromEuler and toEuler are the core's, in radians (VX13)") {
    const script_test::LogCapture log;
    ProbeScene scene;
    const Vec3 angles{0.3F, 0.5F, 0.7F};

    const Quat set = quatOf(scene, "new engine.Quat().setFromEuler(0.3, 0.5, 0.7)");
    CHECK(close(set, engine::normalizeOrIdentity(engine::fromEulerAngles(angles))));
    const Vec3 back = vecOf(scene, "e.get(engine.ScriptProbe).q.toEuler()");
    CHECK(close(back, engine::eulerAngles(set)));
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: slerp is the core's, t is not clamped and dot is the core's (VX14)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    const std::string expression = std::string{TURN_X} + ".slerp(" + std::string{TURN_Y} + ", 0.25)";
    CHECK(close(quatOf(scene, expression), engine::slerp(TURN_X_C, TURN_Y_C, 0.25F)));
    CHECK(completion(scene.vm, iife("return t(() => " + std::string{TURN_X} + ".slerp(" + std::string{TURN_Y} +
                                    ", 1.5));")) == "no throw");
    CHECK(close(quatOf(scene, std::string{TURN_X} + ".slerp(" + std::string{TURN_Y} + ", 1.5)"),
                engine::slerp(TURN_X_C, TURN_Y_C, 1.5F)));
    CHECK(std::abs(numberOf(scene, std::string{TURN_X} + ".dot(" + std::string{TURN_Y} + ")") -
                   engine::dot(TURN_X_C, TURN_Y_C)) <= TOLERANCE);
    script_test::expectCleanShutdown(scene.vm);
}

TEST_CASE("math: a held view of a destroyed entity throws as receiver and as argument and writes nothing (VX15)") {
    const script_test::LogCapture log;
    ProbeScene scene;

    completion(scene.vm, "globalThis.pv = e.get(engine.ScriptProbe).v; globalThis.w = new engine.Vec3(1, 2, 3); 0");
    REQUIRE(scene.world.destroy(scene.entity));
    const std::string rows = completion(scene.vm, iife(R"js(
        const rows = [t(() => pv.add(w)), t(() => w.add(pv)), t(() => w.cross(pv)), t(() => w.dot(pv))];
        return rows.map((row) => row.endsWith("the entity is not alive") ? "dead" : row).join("|") +
            "|" + [w.x, w.y, w.z].join();
    )js"));
    CHECK(rows == "dead|dead|dead|dead|1,2,3");
    script_test::expectCleanShutdown(scene.vm);
}
