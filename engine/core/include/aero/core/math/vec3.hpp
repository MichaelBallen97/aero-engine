#pragma once
// Aero Engine — Vec3 (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/constants.hpp>

#include <cassert>
#include <cmath>
#include <type_traits>

namespace engine {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // D13: static constexpr member FUNCTIONS, not data members. `static constexpr Vec3 ZERO;`
    // inside Vec3 is ill-formed — Vec3 is incomplete there. Member function BODIES are parsed in
    // the complete-class context, so these are legal, constexpr, and free after inlining.
    static constexpr Vec3 zero() { return {0.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 one() { return {1.0f, 1.0f, 1.0f}; }
    static constexpr Vec3 unitX() { return {1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 unitY() { return {0.0f, 1.0f, 0.0f}; }
    static constexpr Vec3 unitZ() { return {0.0f, 0.0f, 1.0f}; }

    // EXACT by design (E11): serialization and change-detection want exactness; a cheap identity
    // test is what D14's aggregates need. Tolerance is opt-in, via approxEquals.
    bool operator==(const Vec3&) const = default;
};

// AC-11 — the invariants data(), the GPU upload path, and ADR-004 serialization all rest on.
// A padding change breaks the BUILD rather than corrupting a buffer at runtime.
static_assert(std::is_trivially_copyable_v<Vec3>);
static_assert(std::is_standard_layout_v<Vec3>);
static_assert(std::is_aggregate_v<Vec3>);
static_assert(sizeof(Vec3) == 3 * sizeof(float));

constexpr Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }  // componentwise
constexpr Vec3 operator/(Vec3 a, Vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
constexpr Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }
constexpr Vec3 operator/(Vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }

constexpr Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }
constexpr Vec3& operator-=(Vec3& a, Vec3 b) { return a = a - b; }
constexpr Vec3& operator*=(Vec3& v, float s) { return v = v * s; }
constexpr Vec3& operator/=(Vec3& v, float s) { return v = v / s; }

constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// D3: RIGHT-HANDED. cross(unitX, unitY) == unitZ — asserted in the tests (AC-7). This is the
// engine's handedness statement in code; glTF (the canonical asset format) uses the same one.
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

constexpr float lengthSquared(Vec3 v) { return dot(v, v); }

// E6: std::sqrt is not constexpr until C++26 -> these are `inline`, not `constexpr`. The inline is
// also required by clang-tidy's misc-definitions-in-headers. Do NOT hand-roll a constexpr sqrt
// (precision + maintenance for no gain); revisit only at C++26.
inline float length(Vec3 v) { return std::sqrt(lengthSquared(v)); }
inline float distance(Vec3 a, Vec3 b) { return length(b - a); }

// D15: precondition NON-ZERO, assert()ed in debug, NO runtime branch in release (yields inf/NaN
// exactly as GLM would). This is the Unreal GetUnsafeNormal/GetSafeNormal split: normalize() is
// the hottest op in the engine and must not pay a branch. If the input may be zero, use
// normalizeOrZero(). NDEBUG in the *-release presets compiles the assert out (D16).
inline Vec3 normalize(Vec3 v) {
    assert(lengthSquared(v) > 0.0f && "normalize() of a zero-length vector; use normalizeOrZero()");
    return v / length(v);
}

inline Vec3 normalizeOrZero(Vec3 v, float epsilon = EPSILON) {
    const float lenSq = lengthSquared(v);
    if (lenSq <= epsilon * epsilon) return Vec3::zero();
    return v / std::sqrt(lenSq);
}

constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

constexpr bool approxEquals(Vec3 a, Vec3 b, float epsilon = EPSILON) {
    return approxEquals(a.x, b.x, epsilon) && approxEquals(a.y, b.y, epsilon) && approxEquals(a.z, b.z, epsilon);
}

}  // namespace engine
