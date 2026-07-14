#pragma once
// Aero Engine — Vec4 (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/constants.hpp>
#include <aero/core/math/vec3.hpp>

#include <cassert>
#include <cmath>
#include <type_traits>

namespace engine {

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    // D13: static constexpr member FUNCTIONS, not data members — see vec3.hpp for the rationale.
    static constexpr Vec4 zero() { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr Vec4 one() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    static constexpr Vec4 unitX() { return {1.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr Vec4 unitY() { return {0.0f, 1.0f, 0.0f, 0.0f}; }
    static constexpr Vec4 unitZ() { return {0.0f, 0.0f, 1.0f, 0.0f}; }
    static constexpr Vec4 unitW() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    // EXACT by design (E11); tolerance is opt-in, via approxEquals.
    bool operator==(const Vec4&) const = default;
};

// AC-11 — see vec3.hpp for what these rest on.
static_assert(std::is_trivially_copyable_v<Vec4>);
static_assert(std::is_standard_layout_v<Vec4>);
static_assert(std::is_aggregate_v<Vec4>);
static_assert(sizeof(Vec4) == 4 * sizeof(float));

constexpr Vec4 operator-(Vec4 v) { return {-v.x, -v.y, -v.z, -v.w}; }
constexpr Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
constexpr Vec4 operator*(Vec4 a, Vec4 b) {  // componentwise
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
constexpr Vec4 operator/(Vec4 a, Vec4 b) { return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}; }
constexpr Vec4 operator*(Vec4 v, float s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }
constexpr Vec4 operator*(float s, Vec4 v) { return v * s; }
constexpr Vec4 operator/(Vec4 v, float s) { return {v.x / s, v.y / s, v.z / s, v.w / s}; }

constexpr Vec4& operator+=(Vec4& a, Vec4 b) { return a = a + b; }
constexpr Vec4& operator-=(Vec4& a, Vec4 b) { return a = a - b; }
constexpr Vec4& operator*=(Vec4& v, float s) { return v = v * s; }
constexpr Vec4& operator/=(Vec4& v, float s) { return v = v / s; }

constexpr float dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

constexpr float lengthSquared(Vec4 v) { return dot(v, v); }

// E6: std::sqrt is not constexpr until C++26 -> these are `inline`, not `constexpr`.
inline float length(Vec4 v) { return std::sqrt(lengthSquared(v)); }
inline float distance(Vec4 a, Vec4 b) { return length(b - a); }

// D15: precondition NON-ZERO, assert()ed in debug, no runtime branch in release.
inline Vec4 normalize(Vec4 v) {
    assert(lengthSquared(v) > 0.0f && "normalize() of a zero-length vector; use normalizeOrZero()");
    return v / length(v);
}

inline Vec4 normalizeOrZero(Vec4 v, float epsilon = EPSILON) {
    const float lenSq = lengthSquared(v);
    if (lenSq <= epsilon * epsilon) return Vec4::zero();
    return v / std::sqrt(lenSq);
}

constexpr Vec4 lerp(Vec4 a, Vec4 b, float t) { return a + (b - a) * t; }

constexpr bool approxEquals(Vec4 a, Vec4 b, float epsilon = EPSILON) {
    return approxEquals(a.x, b.x, epsilon) && approxEquals(a.y, b.y, epsilon) && approxEquals(a.z, b.z, epsilon) &&
           approxEquals(a.w, b.w, epsilon);
}

// Size-changing conversions are EXPLICIT ONLY — never implicit constructors or conversion
// operators. A silent Vec3 -> Vec4 would fabricate a w the caller never chose.
constexpr Vec3 xyz(Vec4 v) { return {v.x, v.y, v.z}; }
constexpr Vec4 toVec4(Vec3 v, float w) { return {v.x, v.y, v.z, w}; }

}  // namespace engine
