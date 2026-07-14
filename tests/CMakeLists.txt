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
# R12 in docs/08-risks.md; the textual grep guard (0.2.3) is the real enforcement for tests/.
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core)

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)
