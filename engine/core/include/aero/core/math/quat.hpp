#pragma once
// Aero Engine — Quat (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/mat3.hpp>
#include <aero/core/math/mat4.hpp>
#include <aero/core/math/vec3.hpp>

#include <cassert>
#include <cmath>
#include <type_traits>

namespace engine {

struct Quat {
    // D7: {x, y, z, w} — glTF's accessor order (glTF is the engine's canonical asset format, so
    // this is the order that costs zero conversions on import).
    //
    // ⚠ GLM's quaternion does NOT match: its ctor is w-FIRST and BOTH its member order and its
    // ctor argument order are #ifdef-dependent (two separate GLM_FORCE_QUAT_DATA_* macros).
    // Conversion therefore lives ONLY in glm_backend.cpp and goes through glm::quat::wxyz() —
    // never a bit_cast, memcpy, or raw 4-arg ctor. See the comment there.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;  // D14: default IS identity (0,0,0,1)

    static constexpr Quat identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    bool operator==(const Quat&) const = default;  // exact; use approxEquals for tolerance
};

static_assert(std::is_trivially_copyable_v<Quat>);
static_assert(std::is_standard_layout_v<Quat>);
static_assert(std::is_aggregate_v<Quat>);
static_assert(sizeof(Quat) == 4 * sizeof(float));

// Hamilton product. Composition is right-to-left, matching D8's matrix convention:
// (a * b) applies b first, then a.
constexpr Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Rotate a vector: t = 2*cross(q.xyz, v); v + q.w*t + cross(q.xyz, t). No matrix build, no sqrt.
constexpr Vec3 operator*(Quat q, Vec3 v) {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = 2.0f * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

constexpr float dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr float lengthSquared(Quat q) { return dot(q, q); }
inline float length(Quat q) { return std::sqrt(lengthSquared(q)); }  // E6: sqrt -> inline

constexpr Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// For a UNIT quaternion this equals conjugate(); the division makes it correct for any non-zero
// quaternion. Prefer conjugate() on the hot path when you know the input is normalized.
constexpr Quat inverse(Quat q) {
    const float lenSq = lengthSquared(q);
    assert(lenSq > 0.0f && "inverse() of a zero quaternion");
    const Quat c = conjugate(q);
    return {c.x / lenSq, c.y / lenSq, c.z / lenSq, c.w / lenSq};
}

// D15's split, applied to Quat: normalize() asserts and does not branch; normalizeOrIdentity()
// branches and returns identity.
inline Quat normalize(Quat q) {
    assert(lengthSquared(q) > 0.0f && "normalize() of a zero quaternion; use normalizeOrIdentity()");
    const float invLen = 1.0f / length(q);
    return {q.x * invLen, q.y * invLen, q.z * invLen, q.w * invLen};
}

inline Quat normalizeOrIdentity(Quat q, float epsilon = EPSILON) {
    const float lenSq = lengthSquared(q);
    if (lenSq <= epsilon * epsilon) return Quat::identity();
    const float invLen = 1.0f / std::sqrt(lenSq);
    return {q.x * invLen, q.y * invLen, q.z * invLen, q.w * invLen};
}

// D6: RADIANS. The axis MUST be normalized — asserted in debug (D15's discipline, D16's facility).
// The parameter is angleRadians, not `radians`, so it does not shadow engine::radians().
inline Quat fromAxisAngle(Vec3 axis, float angleRadians) {
    assert(approxEquals(lengthSquared(axis), 1.0f) && "fromAxisAngle() requires a normalized axis");
    const float half = angleRadians * 0.5f;
    const float s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// Normalized LERP: cheap, but NOT constant angular velocity. Use slerp when that matters.
inline Quat lerp(Quat a, Quat b, float t) {
    // Take the SHORTER arc — q and -q are the same rotation (E8, double cover).
    const Quat bb = dot(a, b) < 0.0f ? Quat{-b.x, -b.y, -b.z, -b.w} : b;
    return normalize(
        Quat{a.x + (bb.x - a.x) * t, a.y + (bb.y - a.y) * t, a.z + (bb.z - a.z) * t, a.w + (bb.w - a.w) * t});
}

// E8 — DOUBLE COVER: q and -q encode the SAME rotation, so BOTH signs must compare equal.
// toQuat() of a matrix may legitimately hand back -q; a naive componentwise compare here would
// produce a flaky test that "fails" on correct code.
constexpr bool approxEquals(Quat a, Quat b, float epsilon = EPSILON) {
    const bool same = approxEquals(a.x, b.x, epsilon) && approxEquals(a.y, b.y, epsilon) &&
                      approxEquals(a.z, b.z, epsilon) && approxEquals(a.w, b.w, epsilon);
    const bool negated = approxEquals(a.x, -b.x, epsilon) && approxEquals(a.y, -b.y, epsilon) &&
                         approxEquals(a.z, -b.z, epsilon) && approxEquals(a.w, -b.w, epsilon);
    return same || negated;
}

// Declared here, DEFINED IN THE GLM BACKEND (D1).
Quat slerp(Quat a, Quat b, float t);
Mat3 toMat3(Quat q);
Mat4 toMat4(Quat q);
Quat toQuat(const Mat3& m);

// ---- Tait-Bryan euler angles (task 2.2.2, for the editor's rotation UI) -----------------------
// RADIANS, always: degrees are a UI concern -- call degrees()/radians() at the call site (D17).
// The triplet is (X = pitch, Y = yaw, Z = roll), and composition is
//     fromEulerAngles({ax, ay, az})  ==  qZ(az) * qY(ay) * qX(ax)
// i.e. rotate about X first, then Y, then Z (extrinsic; equivalently intrinsic Z-Y'-X''). This is
// GLM 1.0.3's convention, verified NUMERICALLY against all three candidate orderings rather than
// assumed -- do not replace the backend with a hand-rolled formula without re-checking it.
//
// fromEulerAngles(eulerAngles(q)) represents the SAME ROTATION as q -- possibly as the negated
// quaternion (E8's double cover). It is NOT componentwise-equal, and eulerAngles is not injective:
// the returned triplet is one valid representative.
//
// The YAW (Y) component comes from asin() and is confined to [-pi/2, pi/2]. At the gimbal poles
// (|yaw| == pi/2) the pitch/roll split is not unique, AND float conditioning of asin near +/-1 costs
// real precision: a pure 90-degree Y rotation round-trips as 89.98 degrees -- ~3.5e-4 rad, 34x
// EPSILON. The ROTATION stays exact (|dot| == 1); only the triplet drifts. Anything comparing euler
// triplets near a pole must use a loose tolerance or compare rotations. X and Z go through atan2 and
// are exact.
//
// fromEulerAngles does NOT normalize: GLM's euler ctor is unit only up to float error (measured
// length 0.99999994). Normalize at the call site when the result is stored.
[[nodiscard]] Vec3 eulerAngles(Quat q);
[[nodiscard]] Quat fromEulerAngles(Vec3 anglesRadians);

}  // namespace engine
