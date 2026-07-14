#pragma once
// Aero Engine — Vec2 (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/constants.hpp>

#include <cassert>
#include <cmath>
#include <type_traits>

namespace engine {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    // D13: static constexpr member FUNCTIONS, not data members — see vec3.hpp for the rationale.
    static constexpr Vec2 zero() { return {0.0f, 0.0f}; }
    static constexpr Vec2 one() { return {1.0f, 1.0f}; }
    static constexpr Vec2 unitX() { return {1.0f, 0.0f}; }
    static constexpr Vec2 unitY() { return {0.0f, 1.0f}; }

    // EXACT by design (E11); tolerance is opt-in, via approxEquals.
    bool operator==(const Vec2&) const = default;
};

// AC-11 — see vec3.hpp for what these rest on.
static_assert(std::is_trivially_copyable_v<Vec2>);
static_assert(std::is_standard_layout_v<Vec2>);
static_assert(std::is_aggregate_v<Vec2>);
static_assert(sizeof(Vec2) == 2 * sizeof(float));

constexpr Vec2 operator-(Vec2 v) { return {-v.x, -v.y}; }
constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }  // componentwise
constexpr Vec2 operator/(Vec2 a, Vec2 b) { return {a.x / b.x, a.y / b.y}; }
constexpr Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
constexpr Vec2 operator*(float s, Vec2 v) { return v * s; }
constexpr Vec2 operator/(Vec2 v, float s) { return {v.x / s, v.y / s}; }

constexpr Vec2& operator+=(Vec2& a, Vec2 b) { return a = a + b; }
constexpr Vec2& operator-=(Vec2& a, Vec2 b) { return a = a - b; }
constexpr Vec2& operator*=(Vec2& v, float s) { return v = v * s; }
constexpr Vec2& operator/=(Vec2& v, float s) { return v = v / s; }

constexpr float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

// D10: Vec2::cross is deliberately deferred — nothing needs it yet.

constexpr float lengthSquared(Vec2 v) { return dot(v, v); }

// E6: std::sqrt is not constexpr until C++26 -> these are `inline`, not `constexpr`.
inline float length(Vec2 v) { return std::sqrt(lengthSquared(v)); }
inline float distance(Vec2 a, Vec2 b) { return length(b - a); }

// D15: precondition NON-ZERO, assert()ed in debug, no runtime branch in release.
inline Vec2 normalize(Vec2 v) {
    assert(lengthSquared(v) > 0.0f && "normalize() of a zero-length vector; use normalizeOrZero()");
    return v / length(v);
}

inline Vec2 normalizeOrZero(Vec2 v, float epsilon = EPSILON) {
    const float lenSq = lengthSquared(v);
    if (lenSq <= epsilon * epsilon) return Vec2::zero();
    return v / std::sqrt(lenSq);
}

constexpr Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

constexpr bool approxEquals(Vec2 a, Vec2 b, float epsilon = EPSILON) {
    return approxEquals(a.x, b.x, epsilon) && approxEquals(a.y, b.y, epsilon);
}

}  // namespace engine
