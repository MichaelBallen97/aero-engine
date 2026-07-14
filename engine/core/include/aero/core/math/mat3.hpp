#pragma once
// Aero Engine — Mat3 (task 0.2.2, ADR-005). Public engine:: type; ZERO GLM.

#include <aero/core/math/vec3.hpp>

#include <array>
#include <type_traits>

namespace engine {

struct Mat3 {
    // D8: COLUMN-MAJOR storage, COLUMN-VECTOR math — see mat4.hpp for the full rationale. Mat3
    // is the rotation+scale block: no translation to carry.
    //
    // std::array, not Vec3[3]: clang-tidy's modernize-avoid-c-arrays (enabled via modernize-*)
    // rejects C arrays and CI runs --warnings-as-errors='*'. std::array is std, so the boundary
    // rule is untouched, and the static_asserts below prove the layout is byte-identical.
    std::array<Vec3, 3> columns = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};

    static constexpr Mat3 identity() { return {}; }
    static constexpr Mat3 zero() { return Mat3{std::array<Vec3, 3>{Vec3{}, Vec3{}, Vec3{}}}; }

    // 9 contiguous floats, column-major — GPU-upload-ready and glm::make_mat3-ready.
    // E4: reading 9 floats from &columns[0].x rests on the standard-layout + no-padding
    // static_asserts below. Order is unit-tested in AC-11.
    const float* data() const { return &columns[0].x; }
    float* data() { return &columns[0].x; }

    bool operator==(const Mat3&) const = default;
};

static_assert(std::is_trivially_copyable_v<Mat3>);
static_assert(std::is_standard_layout_v<Mat3>);  // what data() rests on
static_assert(std::is_aggregate_v<Mat3>);
static_assert(sizeof(Mat3) == 9 * sizeof(float));  // no padding — what data() depends on

// Column-vector math: M * v = c0*v.x + c1*v.y + c2*v.z.
constexpr Vec3 operator*(const Mat3& m, Vec3 v) { return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z; }

// Composition is RIGHT-TO-LEFT: (a * b) applies b first, then a.
constexpr Mat3 operator*(const Mat3& a, const Mat3& b) {
    return Mat3{std::array<Vec3, 3>{a * b.columns[0], a * b.columns[1], a * b.columns[2]}};
}

constexpr Mat3 transpose(const Mat3& m) {
    return Mat3{std::array<Vec3, 3>{Vec3{m.columns[0].x, m.columns[1].x, m.columns[2].x},
                                    Vec3{m.columns[0].y, m.columns[1].y, m.columns[2].y},
                                    Vec3{m.columns[0].z, m.columns[1].z, m.columns[2].z}}};
}

constexpr bool approxEquals(const Mat3& a, const Mat3& b, float epsilon = EPSILON) {
    return approxEquals(a.columns[0], b.columns[0], epsilon) && approxEquals(a.columns[1], b.columns[1], epsilon) &&
           approxEquals(a.columns[2], b.columns[2], epsilon);
}

// Declared here, DEFINED IN THE GLM BACKEND (D1) — this is where GLM earns its keep. The RTM swap
// (ADR-005 / docs/08) replaces those definitions and nothing else; this declaration is stable.
Mat3 inverse(const Mat3& m);
float determinant(const Mat3& m);

}  // namespace engine
