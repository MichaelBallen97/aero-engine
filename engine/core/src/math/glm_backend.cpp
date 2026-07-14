// engine/core/src/math/glm_backend.cpp
// ============================================================================================
// THE ONLY FILE IN THIS REPOSITORY PERMITTED TO INCLUDE GLM.
//
// ADR-005 in practice: the public math headers (engine/core/include/aero/core/math/*.hpp) are
// GLM-free and self-contained; this TU implements only the ops where GLM earns its keep. CMake
// links glm::glm PRIVATE, so GLM's include directory never propagates to any consumer —
// `#include <glm/...>` anywhere else in the repo is a COMPILE ERROR, not a convention someone has
// to remember (see engine/core/CMakeLists.txt). Task 0.2.3's guard is defence in depth on top.
//
// THE RTM SWAP (ADR-005 / docs/08) REPLACES THIS FILE AND NOTHING ELSE. Every public header and
// every consumer is untouched by construction. ADR-005's exit door is exactly one file wide —
// keep it that way: never add a GLM type or header to a public header, never widen this TU.
//
// No GLM_FORCE_* macro is defined anywhere in this build (D5) — the explicitly suffixed entry
// points (perspectiveRH_ZO, orthoRH_ZO, lookAtRH) state the convention AT THE CALL SITE instead
// of depending on an invisible global that a build-system edit could silently drop. No
// GLM_ENABLE_EXPERIMENTAL either (D9): every entry point below is stable, non-GTX GLM (verified
// against the GLM 1.0.3 source — none of the six headers below mentions it).
// ============================================================================================

#include <aero/core/math/mat3.hpp>
#include <aero/core/math/mat4.hpp>
#include <aero/core/math/quat.hpp>
#include <aero/core/math/transform.hpp>
#include <aero/core/math/vec3.hpp>

#include <glm/ext/matrix_clip_space.hpp>  // perspectiveRH_ZO, orthoRH_ZO
#include <glm/ext/matrix_transform.hpp>   // lookAtRH
#include <glm/ext/quaternion_common.hpp>  // slerp
#include <glm/gtc/quaternion.hpp>         // mat3_cast, mat4_cast, quat_cast
#include <glm/gtc/type_ptr.hpp>           // make_mat3, make_mat4, value_ptr
#include <glm/matrix.hpp>                 // inverse, determinant

#include <cassert>
#include <cstring>

namespace engine {
namespace {

glm::vec3 toGlm(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 fromGlm(const glm::vec3& v) { return {v.x, v.y, v.z}; }

// D7 — THE SINGLE MOST DANGEROUS GOTCHA IN THIS TASK. Verified against the GLM 1.0.3 source
// (glm/detail/type_quat.hpp:45-88, type_quat.inl:157):
//   * the MEMBER order (x,y,z,w vs w,x,y,z) flips under GLM_FORCE_QUAT_DATA_WXYZ;
//   * the 4-scalar CONSTRUCTOR's ARGUMENT order flips under GLM_FORCE_QUAT_DATA_XYZW — a
//     SECOND, INDEPENDENT macro. So glm::quat(w, x, y, z) is NOT unconditionally w-first.
//   * qua::wxyz() IS declared unconditionally and absorbs that #ifdef internally: it is always
//     w-first, BY NAME. GLM's own operator+/-/*, conjugate, slerp etc. all route through it for
//     exactly this reason.
// Therefore: wxyz() on the way in, NAMED MEMBERS on the way out. Never bit_cast/memcpy a
// quaternion; never call the raw 4-arg ctor. This is what makes D7's promise real — and it is why
// D5's "define no GLM_FORCE_* macro anywhere" is doubly load-bearing.
glm::quat toGlm(Quat q) { return glm::quat::wxyz(q.w, q.x, q.y, q.z); }
Quat fromGlm(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

// Matrices are plain column-major float blocks in both worlds (D8); make_mat3/make_mat4 and
// value_ptr are documented, stable, order-unambiguous GLM APIs. Rests on Mat3/Mat4's
// static_asserts (no padding, standard layout) — if those ever broke, the build breaks first.
glm::mat3 toGlm(const Mat3& m) { return glm::make_mat3(m.data()); }
glm::mat4 toGlm(const Mat4& m) { return glm::make_mat4(m.data()); }

Mat3 fromGlm(const glm::mat3& m) {
    Mat3 r;
    std::memcpy(r.data(), glm::value_ptr(m), sizeof(Mat3));
    return r;
}

Mat4 fromGlm(const glm::mat4& m) {
    Mat4 r;
    std::memcpy(r.data(), glm::value_ptr(m), sizeof(Mat4));
    return r;
}

}  // namespace

// ---- Matrix ops (mat3.hpp / mat4.hpp) ----------------------------------------------------

Mat3 inverse(const Mat3& m) { return fromGlm(glm::inverse(toGlm(m))); }
Mat4 inverse(const Mat4& m) { return fromGlm(glm::inverse(toGlm(m))); }
float determinant(const Mat3& m) { return glm::determinant(toGlm(m)); }
float determinant(const Mat4& m) { return glm::determinant(toGlm(m)); }

// ---- Quaternion ops (quat.hpp) ------------------------------------------------------------

Quat slerp(Quat a, Quat b, float t) { return fromGlm(glm::slerp(toGlm(a), toGlm(b), t)); }
Mat3 toMat3(Quat q) { return fromGlm(glm::mat3_cast(toGlm(q))); }
Mat4 toMat4(Quat q) { return fromGlm(glm::mat4_cast(toGlm(q))); }
Quat toQuat(const Mat3& m) { return fromGlm(glm::quat_cast(toGlm(m))); }

// ---- Transform (transform.hpp) ------------------------------------------------------------

// D8: T * R * S, right-to-left — scale first, then rotate, then translate.
Mat4 compose(const Trs& trs) { return translation(trs.translation) * toMat4(trs.rotation) * scaling(trs.scale); }

// D9: TRS-only, hand-built from STABLE GLM parts. glm::decompose is deliberately unused — it is
// experimental GTX (glm/gtx/matrix_decompose.hpp #errors without GLM_ENABLE_EXPERIMENTAL, which
// this build never defines) and it models skew + perspective this engine does not have.
bool decompose(const Mat4& m, Trs& out) {
    const Vec3 c0{m.columns[0].x, m.columns[0].y, m.columns[0].z};
    const Vec3 c1{m.columns[1].x, m.columns[1].y, m.columns[1].z};
    const Vec3 c2{m.columns[2].x, m.columns[2].y, m.columns[2].z};

    const float sxRaw = length(c0);
    const float sy = length(c1);
    const float sz = length(c2);

    // A degenerate axis makes the rotation unrecoverable — report it, never guess. `out` is left
    // untouched (docs/04: explicit status, no exceptions across the public API).
    if (sxRaw <= EPSILON || sy <= EPSILON || sz <= EPSILON) return false;

    // A negative determinant means an odd number of axes are mirrored. The convention (matching
    // glm::decompose and every engine that models TRS) is to put the flip on X — which makes the
    // normalized 3x3 below a PROPER rotation (det +1), as quat_cast requires.
    const float sx = determinant(m) < 0.0f ? -sxRaw : sxRaw;

    // NB: every local here is const — clang-tidy's misc-const-correctness is enabled and CI runs
    // --warnings-as-errors='*'. The ternary above exists for that reason as much as for clarity.
    const Mat3 rot{std::array<Vec3, 3>{c0 / sx, c1 / sy, c2 / sz}};

    out.translation = Vec3{m.columns[3].x, m.columns[3].y, m.columns[3].z};
    out.rotation = toQuat(rot);
    out.scale = Vec3{sx, sy, sz};
    return true;
}

// D3: right-handed view space. Verified from GLM 1.0.3's implementation: f = normalize(target-eye),
// row 2 = -f, Result[3][2] = dot(f, eye) -> a point at the target lands at NEGATIVE view-space z.
Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) { return fromGlm(glm::lookAtRH(toGlm(eye), toGlm(target), toGlm(up))); }

// D4/D6: RH view space -> Z in [0,1] clip space (SDL_GPU's documented NDC). fovY in RADIANS.
// GLM asserts on aspect ~ 0 internally; assert the full contract here so a bad call fails at OUR
// boundary with our message, not deep inside a GLM header (D16: <cassert>, compiled out by NDEBUG).
Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
    assert(aspect > 0.0f && "perspective() requires aspect > 0");
    assert(zNear > 0.0f && "perspective() requires zNear > 0");
    assert(zFar > zNear && "perspective() requires zFar > zNear");
    return fromGlm(glm::perspectiveRH_ZO(fovY, aspect, zNear, zFar));
}

Mat4 ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
    assert(right > left && "ortho() requires right > left");
    assert(top > bottom && "ortho() requires top > bottom");
    assert(zFar > zNear && "ortho() requires zFar > zNear");
    return fromGlm(glm::orthoRH_ZO(left, right, bottom, top, zNear, zFar));
}

}  // namespace engine
