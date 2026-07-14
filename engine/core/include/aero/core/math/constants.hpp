#pragma once
// Aero Engine — scalar math constants and helpers (task 0.2.2, ADR-005).
// Public surface: engine:: and the standard library only. GLM appears NOWHERE under
// engine/core/include — it is a private implementation detail of aero::core, confined to
// engine/core/src/math/glm_backend.cpp and linked PRIVATE (engine/core/CMakeLists.txt).

#include <numbers>

namespace engine {

// D6: RADIANS everywhere in the public API. The editor converts at the UI boundary (Phase 2);
// the engine never does. GLM agrees — perspectiveRH_ZO's implementation is tan(fovy / 2), with
// no degree conversion, despite its doc comment saying otherwise.
//
// PI is DERIVED from <numbers> rather than spelled as a literal: clang-tidy's
// modernize-use-std-numbers (enabled via modernize-*, not subtracted) rejects the literal form
// outright, and CI runs --warnings-as-errors='*'. <numbers> is std, so the boundary rule is
// untouched. Everything else is derived from PI so there is exactly one source of truth.
inline constexpr float PI = std::numbers::pi_v<float>;
inline constexpr float TWO_PI = 2.0f * PI;
inline constexpr float HALF_PI = 0.5f * PI;
inline constexpr float DEG_TO_RAD = PI / 180.0f;
inline constexpr float RAD_TO_DEG = 180.0f / PI;

// float32 tolerance for a value accumulated over a handful of ops. Matrix inversion and
// projection accumulate more error and earn looser explicit tolerances at their call sites.
inline constexpr float EPSILON = 1e-5f;

// Params are deg/rad, NOT degrees/radians — those would shadow the functions themselves.
constexpr float radians(float deg) { return deg * DEG_TO_RAD; }
constexpr float degrees(float rad) { return rad * RAD_TO_DEG; }

// Hand-rolled |a-b| rather than std::abs: std::abs is not constexpr in C++20 (std::fabs only
// becomes constexpr in C++26), and this must work at compile time.
constexpr bool approxEquals(float a, float b, float epsilon = EPSILON) {
    const float diff = a > b ? a - b : b - a;
    return diff <= epsilon;
}

}  // namespace engine
