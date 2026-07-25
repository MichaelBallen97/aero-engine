# tests/golden-rule/link_probe/ — task 2.1.2, Part C2: a hermetic probe project proving
# cmake/golden_rule.cmake's aero_assert_golden_rule() actually goes RED. Tracked, but — like
# tests/reflect-gen/incremental/ — NEVER add_subdirectory()'d by the real build; it is only ever
# configured standalone, hermetically, by tests/golden-rule/link_graph_e2e.cmake.
cmake_minimum_required(VERSION 3.28)
project(AeroGoldenRuleProbe LANGUAGES NONE)          # D13 -- no compiler detection, sub-second
include("${AERO_REPO_ROOT}/cmake/golden_rule.cmake") # the REAL module under test

# Five mutually-exclusive seams. The driver sets exactly one per configure; each is a distinct
# failure mode of aero_assert_golden_rule (2 violation arms, 3 self-verify arms).
option(PROBE_SEED_LINK          "Seed an engine -> editor LINK edge, at the DEEPEST target"  OFF)
option(PROBE_SEED_INCLUDE_DIR   "Seed editor/include onto an engine target"                  OFF)
option(PROBE_SEED_BREAK_CANARY  "Break the reverse canary (probe_editor_core -/-> probe_core)" OFF)
option(PROBE_SEED_NO_CONSUMERS  "Define NO engine targets -> empty consumer set"             OFF)
option(PROBE_SEED_NO_FORBIDDEN  "Define NO editor targets -> empty forbidden set"            OFF)

add_subdirectory(engine)
add_subdirectory(runtime)     # deliberately target-less, mirroring the real tree (F7)
add_subdirectory(editor)

aero_assert_golden_rule(
    CONSUMER_DIRS   "${CMAKE_SOURCE_DIR}/engine" "${CMAKE_SOURCE_DIR}/runtime"
    FORBIDDEN_DIRS  "${CMAKE_SOURCE_DIR}/editor"
    CANARY_CONSUMER probe_editor_core
    CANARY_DEP      probe_core)
