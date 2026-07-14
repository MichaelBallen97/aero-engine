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

# AC-9(i) (task 0.2.2): aero::core is linked; glm::glm deliberately IS NOT. That omission is a
# PERMANENT, free regression test — if any public math header ever pulled GLM in, this target
# would fail to compile. Do not "fix" it by adding glm::glm here. (glm's ARCHIVE still reaches
# the link line via $<LINK_ONLY:> from aero_core's PRIVATE link — that is correct and carries no
# headers.) NOTE: this target also links doctest::doctest, which — via vcpkg's shared per-triplet
# include/ directory — means it does NOT prove the compile-time boundary the way an engine-layer
# target linking only aero::core would; see docs/02-adrs.md's ADR-005 implementation note.
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core)

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)
