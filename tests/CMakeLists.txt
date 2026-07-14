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
)
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core)

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)
