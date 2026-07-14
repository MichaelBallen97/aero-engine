#pragma once
// Aero Engine — the math umbrella (task 0.2.2). ADR-005's "the ONLY public surface", in practice.
//
// Consumers include THIS and nothing else:  #include <aero/core/math.hpp>
//
// ADR-005 in one line: the engine owns its math types; the backend is swappable. GLM is a PRIVATE
// implementation detail of aero::core — it lives behind engine/core/src/math/glm_backend.cpp and is
// linked PRIVATE, so no GLM header is reachable from here or from any consumer that links no vcpkg
// package directly (see engine/core/CMakeLists.txt) — `#include <glm/...>` is a COMPILE ERROR there.
// This holds for every ENGINE-LAYER target (verified with a throwaway probe target linking only
// aero::core), which is the boundary this comment is about. It does NOT hold inside tests/: vcpkg
// installs every port into one shared per-triplet include/ directory, so aero_tests inherits GLM's
// headers via doctest::doctest regardless of aero_core's PRIVATE link (see docs/02-adrs.md's
// ADR-005 implementation note and risk R12 in docs/08-risks.md). Task 0.2.3's grep-based CI guard
// is the enforcement for tests/ and any other target that links a vcpkg package directly.
//
// ENGINE-WIDE CONVENTIONS PINNED BY THESE TYPES (docs/02 ADR-005 implementation note). Every
// shader, camera, importer, and physics wrapper inherits them — do not change one locally:
//   * Right-handed, Y-up, -Z forward world/view space (glTF 2.0's convention, D3).
//   * Clip-space depth Z in [0,1], 0 = near (SDL_GPU's documented NDC, D4). Never flip Y.
//   * Column-major storage, column-vector math: M * v, Model = T * R * S (D8).
//   * Radians everywhere; the editor converts at the UI boundary (D6).
//   * Quat is {x, y, z, w}, identity (0,0,0,1) — glTF's accessor order (D7).

#include <aero/core/math/constants.hpp>
#include <aero/core/math/mat3.hpp>
#include <aero/core/math/mat4.hpp>
#include <aero/core/math/quat.hpp>
#include <aero/core/math/transform.hpp>
#include <aero/core/math/vec2.hpp>
#include <aero/core/math/vec3.hpp>
#include <aero/core/math/vec4.hpp>
