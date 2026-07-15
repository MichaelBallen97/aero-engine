# tests/ — doctest unit tests (docs/04). Harness created by task 0.1.4.

find_package(doctest CONFIG REQUIRED)

# One runner for the whole project: test TUs register TEST_CASEs, test_main.cpp
# only generates main(). This is the repo's first compiled+linked target.
add_executable(aero_tests
    test_main.cpp
    sanity_test.cpp
    profiler_test.cpp
    handle_test.cpp
    slot_map_test.cpp
    log_test.cpp
    math_test.cpp
    jobs_test.cpp
)

# AC-9(i) (task 0.2.2): aero::core is linked; glm::glm deliberately IS NOT — that is intentional
# style, not a compile-time guarantee. Do not "fix" it by adding glm::glm here. (glm's ARCHIVE
# still reaches the link line via $<LINK_ONLY:> from aero_core's PRIVATE link — that is correct
# and carries no headers.)
#
# This target ALSO links doctest::doctest, which — via vcpkg's shared per-triplet include/
# directory — exports GLM's headers to every TU here regardless of what aero_core links (verified:
# a probe TU with this target's exact flags plus `#include <glm/vec3.hpp>` compiles clean). So a
# public math header pulling GLM in would NOT necessarily fail to compile here — this target does
# NOT prove the compile-time boundary the way an engine-layer target linking only aero::core does
# (verified with exactly that probe). See docs/02-adrs.md's ADR-005 implementation note and risk
# R12 in docs/08-risks.md; .github/scripts/check-math-boundary.sh (task 0.2.3, run by the `lint`
# job) is the real enforcement for tests/. The sibling aero_math_boundary_probe target below (also
# 0.2.3) covers the public math headers at compile time instead.
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core)

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)

# ---------------------------------------------------------------------------------------------
# The math-boundary COMPILE-TIME guard (task 0.2.3; docs/02-adrs.md:211).
#
# OBJECT, not an executable: compiling is the whole assertion, so there is nothing to link or run
# and no ctest entry to register. It builds as part of `all`, so every preset on every CI lane
# enforces it for free.
#
# THE LINK LINE IS THE TEST. aero::core links glm::glm PRIVATE, so a target that links ONLY
# aero::core never sees vcpkg's shared per-triplet include/ directory and cannot resolve
# <glm/...>. That is exactly what makes a leaked GLM include in a PUBLIC math header a compile
# error here (verified 2026-07-15: with GLM installed in the shared root, this flag set still
# reports `fatal error: 'glm/vec3.hpp' file not found`, while aero_tests' flag set compiles it
# clean — risk R12, docs/08-risks.md).
#
# DO NOT ADD ANY OTHER LINK LIBRARY. Not doctest::doctest, not aero::profiling (it links Tracy —
# a vcpkg package — in Release), not glm::glm. Any one of them puts the shared vcpkg include root
# back on this compile line and silently reduces this guard to a no-op WITHOUT turning CI red.
# That is the only way this guard can rot, so it is the one thing to never "simplify".
add_library(aero_math_boundary_probe OBJECT math_boundary_probe.cpp)
target_link_libraries(aero_math_boundary_probe PRIVATE aero::core)

# ---------------------------------------------------------------------------------------------
# The jobs-boundary COMPILE-TIME guard (task 0.2.5; docs/04 boundary rule).
#
# Same mechanism as aero_math_boundary_probe above, for a different backend: aero::core links
# enkiTS::enkiTS PRIVATE, so a target linking ONLY aero::core never sees vcpkg's shared per-triplet
# include/ directory and cannot resolve <TaskScheduler.h>. A leaked enkiTS include in a public header
# is therefore a compile error the moment it is written.
#
# A separate target from the math probe on purpose: each guard names its own backend, so a failure
# says which boundary broke. A future task may consolidate both into one aero_core_boundary_probe as
# 0.2.6 adds time/VFS headers — do NOT do that here; it is churn on a guard that just landed.
#
# DO NOT ADD ANY OTHER LINK LIBRARY — not doctest, not aero::profiling, not enkiTS::enkiTS. Any of
# them restores the shared vcpkg include root and silently reduces this guard to a no-op while CI
# stays green.
add_library(aero_jobs_boundary_probe OBJECT jobs_boundary_probe.cpp)
target_link_libraries(aero_jobs_boundary_probe PRIVATE aero::core)
