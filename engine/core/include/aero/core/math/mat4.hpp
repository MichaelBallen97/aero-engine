#pragma once
// Aero Engine — Mat4 (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/mat3.hpp>
#include <aero/core/math/vec3.hpp>
#include <aero/core/math/vec4.hpp>

#include <array>
#include <type_traits>

namespace engine {

struct Mat4 {
    // D8: COLUMN-MAJOR storage, COLUMN-VECTOR math. columns[c] is the c-th basis vector;
    // columns[3] is the translation. M * v; composition right-to-left (Model = T * R * S).
    // Matches GLM/GLSL/HLSL, so GPU upload is a straight memcpy with NO transpose. The member
    // name STATES the convention where a bare `float m[16]` would be silent.
    //
    // D14: defaults to IDENTITY, not zero — a zero matrix silently collapses all geometry to a
    // point, which is a nasty, hard-to-spot bug. Costs a dead store the optimizer removes.
    //
    // std::array, not Vec4[4]: clang-tidy's modernize-avoid-c-arrays (enabled via modernize-*)
    // rejects C arrays and CI runs --warnings-as-errors='*'. std::array is std, so the boundary
    // rule is untouched, and the static_asserts below prove the layout is byte-identical.
    std::array<Vec4, 4> columns = {Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                                   Vec4{0.0f, 0.0f, 1.0f, 0.0f}, Vec4{0.0f, 0.0f, 0.0f, 1.0f}};

    static constexpr Mat4 identity() { return {}; }
    static constexpr Mat4 zero() { return Mat4{std::array<Vec4, 4>{Vec4{}, Vec4{}, Vec4{}, Vec4{}}}; }

    // 16 contiguous floats, column-major — GPU-upload-ready and glm::make_mat4-ready.
    // E4: reading 16 floats from &columns[0].x is universally sound in practice but rests on the
    // standard-layout + no-padding static_asserts below, which make a hypothetical layout change
    // break the BUILD rather than corrupt a GPU upload. Order is unit-tested in AC-11.
    const float* data() const { return &columns[0].x; }
    float* data() { return &columns[0].x; }

    bool operator==(const Mat4&) const = default;
};

static_assert(std::is_trivially_copyable_v<Mat4>);
static_assert(std::is_standard_layout_v<Mat4>);  // what data() rests on
static_assert(std::is_aggregate_v<Mat4>);
static_assert(sizeof(Mat4) == 16 * sizeof(float));  // no padding — what data() depends on

// Column-vector math: M * v = c0*v.x + c1*v.y + c2*v.z + c3*v.w.
constexpr Vec4 operator*(const Mat4& m, Vec4 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}

// Composition is RIGHT-TO-LEFT: (a * b) applies b first, then a.
constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    return Mat4{std::array<Vec4, 4>{a * b.columns[0], a * b.columns[1], a * b.columns[2], a * b.columns[3]}};
}

constexpr Mat4 transpose(const Mat4& m) {
    return Mat4{std::array<Vec4, 4>{Vec4{m.columns[0].x, m.columns[1].x, m.columns[2].x, m.columns[3].x},
                                    Vec4{m.columns[0].y, m.columns[1].y, m.columns[2].y, m.columns[3].y},
                                    Vec4{m.columns[0].z, m.columns[1].z, m.columns[2].z, m.columns[3].z},
                                    Vec4{m.columns[0].w, m.columns[1].w, m.columns[2].w, m.columns[3].w}}};
}

// w = 1: the translation APPLIES. Affine only — no perspective divide (see the projection tests
// for the divide, which is the caller's job in clip space).
constexpr Vec3 transformPoint(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

// w = 0: the translation does NOT apply — directions are never translated.
constexpr Vec3 transformDirection(const Mat4& m, Vec3 v) {
    const Vec4 r = m * Vec4{v.x, v.y, v.z, 0.0f};
    return {r.x, r.y, r.z};
}

// The upper-left 3x3 — the rotation+scale block; drops translation.
constexpr Mat3 toMat3(const Mat4& m) {
    return Mat3{std::array<Vec3, 3>{Vec3{m.columns[0].x, m.columns[0].y, m.columns[0].z},
                                    Vec3{m.columns[1].x, m.columns[1].y, m.columns[1].z},
                                    Vec3{m.columns[2].x, m.columns[2].y, m.columns[2].z}}};
}

constexpr bool approxEquals(const Mat4& a, const Mat4& b, float epsilon = EPSILON) {
    return approxEquals(a.columns[0], b.columns[0], epsilon) && approxEquals(a.columns[1], b.columns[1], epsilon) &&
           approxEquals(a.columns[2], b.columns[2], epsilon) && approxEquals(a.columns[3], b.columns[3], epsilon);
}

// Declared here, DEFINED IN THE GLM BACKEND (D1) — this is where GLM earns its keep. The RTM swap
// (ADR-005 / docs/08) replaces those definitions and nothing else; this declaration is stable.
Mat4 inverse(const Mat4& m);
float determinant(const Mat4& m);

}  // namespace engine
