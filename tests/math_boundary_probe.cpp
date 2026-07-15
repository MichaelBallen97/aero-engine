// Aero Engine — the math-boundary COMPILE-TIME guard (task 0.2.3; docs/02-adrs.md:211).
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is
// that it COMPILES. Its target (aero_math_boundary_probe, tests/CMakeLists.txt) links ONLY
// aero::core, so vcpkg's shared per-triplet include/ directory never reaches its compile line and
// GLM is genuinely unreachable here. If any public math header ever starts including <glm/...>,
// this TU fails to compile and the build goes red at the moment the leak is written.
//
// This is what aero_tests CANNOT do: aero_tests links doctest::doctest and therefore inherits the
// whole shared vcpkg include root, so GLM resolves there regardless of what aero_core links
// (risk R12, docs/08-risks.md — re-verified 2026-07-15). 0.2.2's AC-9(i) ("aero_tests links
// aero::core but not glm::glm") is style, not a guarantee; THIS target is the guarantee.
//
// KEEP THIS TARGET DEPENDENCY-FREE. Adding doctest, aero::profiling (which links Tracy in Release,
// a vcpkg package), or any other vcpkg package silently destroys the guarantee while leaving the
// build green — the single way this guard can rot. See tests/CMakeLists.txt.

#include <aero/core/math.hpp>

// Force the header's inline/constexpr surface to be instantiated, not merely parsed: a bare
// #include would not evaluate function bodies. No NAMED entity is declared anywhere in this TU —
// docs/04's naming law (readability-identifier-naming, ConstexprVariableCase: UPPER_CASE) has
// nothing to bind to, so this file cannot drift out of lint compliance.
static_assert(engine::Vec2::zero().x == 0.0f);
static_assert(engine::Vec3::unitY().y == 1.0f);
static_assert(engine::Vec4::zero().w == 0.0f);
static_assert(engine::Quat::identity().w == 1.0f);
static_assert(engine::Mat3::identity().columns[0].x == 1.0f);
static_assert(engine::Mat4::identity().columns[0].x == 1.0f);
