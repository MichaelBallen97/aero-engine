# tests/golden-rule/link_graph_e2e.cmake — task 2.1.2, Part C2: a hermetic proof that
# cmake/golden_rule.cmake's aero_assert_golden_rule() actually goes RED. Six NESTED configures into
# six fresh build directories (clean + 5 PROBE_SEED_* seams) against the tracked-but-never-
# add_subdirectory'd probe project at tests/golden-rule/link_probe/ (the tests/reflect-gen/incremental/
# precedent). This driver needs no _gr_seed helper (unlike Part C1's include_scan_e2e.cmake): the
# seeds are option()s, so there is nothing to write and nothing that can silently fail to land.
#
# Registered from a WORK_DIR whose path deliberately contains a literal `editor` path segment
# (tests/CMakeLists.txt) — the free AC-9 second-half proof that step 5's include-directory check is a
# resolved-path comparison, never a substring match on the word "editor" (D10). Do not "tidy" that
# segment away.

cmake_minimum_required(VERSION 3.28)
foreach(required FIXTURE AERO_REPO_ROOT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "link_graph_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# _gr_run(desc expect_zero <command...>) -- expect_zero is a BOOLEAN (the tests/reflect-gen/
# incremental_e2e.cmake _e2e_run precedent, F13), not an exact code: every seeded case here only needs
# to be proven NON-zero (a FATAL_ERROR configure always exits non-zero; the exact value is not part of
# this module's contract, unlike Part C1's script, which distinguishes an exact 0/1/2).
function(_gr_run desc expect_zero)   # ARGN = the command
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_gr_out "${_out}${_err}" PARENT_SCOPE)
    if(expect_zero AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "link_graph_e2e: ${desc}: expected exit 0, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    elseif(NOT expect_zero AND _rc EQUAL 0)
        message(FATAL_ERROR "link_graph_e2e: ${desc}: expected a NON-ZERO exit, got 0:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_gr_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "link_graph_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "link_graph_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${FIXTURE}/" DESTINATION "${WORK_DIR}/src")

# Case 1 — clean. No PROBE_SEED_* set: proves the combined-set anti-vacuity check does NOT fire when
# only runtime/ is legitimately empty (the F7 false-positive direction).
_gr_run("case 1 (clean)" TRUE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-clean" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}")
_gr_expect_substr("case 1" "${_gr_out}" "GOLDEN RULE" FALSE)
_gr_expect_substr("case 1" "${_gr_out}" "cannot self-verify" FALSE)

# Case 2 — a link edge, seeded at the DEEPEST target (spec correction C2). Assert on probe_rhi
# specifically: a finding only reachable via a REAL traversal (probe_rhi is 3 hops from probe_core).
_gr_run("case 2 (link edge)" FALSE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-link" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}" -DPROBE_SEED_LINK=ON)
_gr_expect_substr("case 2" "${_gr_out}" "GOLDEN RULE VIOLATION" TRUE)
_gr_expect_substr("case 2" "${_gr_out}" "probe_rhi" TRUE)
_gr_expect_substr("case 2" "${_gr_out}" "cannot self-verify" FALSE)

# Case 3 — an editor include directory on an engine target.
_gr_run("case 3 (include dir)" FALSE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-incdir" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}" -DPROBE_SEED_INCLUDE_DIR=ON)
_gr_expect_substr("case 3" "${_gr_out}" "GOLDEN RULE VIOLATION" TRUE)
_gr_expect_substr("case 3" "${_gr_out}" "on its include path" TRUE)
_gr_expect_substr("case 3" "${_gr_out}" "cannot self-verify" FALSE)

# Case 4 — a broken reverse canary.
_gr_run("case 4 (broken canary)" FALSE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-canary" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}" -DPROBE_SEED_BREAK_CANARY=ON)
_gr_expect_substr("case 4" "${_gr_out}" "golden-rule assertion cannot self-verify" TRUE)
_gr_expect_substr("case 4" "${_gr_out}" "reverse canary" TRUE)
_gr_expect_substr("case 4" "${_gr_out}" "GOLDEN RULE VIOLATION" FALSE)

# Case 5 — no consumers (empty consumer set). Proves the combined-set check DOES fire when the
# combined set is genuinely empty (the false-negative direction) -- together with case 1, this pins
# the exact combined-set semantics from both sides. Also asserts the ABSENCE of "non-existent target":
# the mechanical proof that step 2 fires before step 3 and the fixture's dangling probe::core link
# (editor/CMakeLists.txt still links against a now-absent probe::core) never reaches generate (§C2.1.1).
_gr_run("case 5 (no consumers)" FALSE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-noconsumers" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}" -DPROBE_SEED_NO_CONSUMERS=ON)
_gr_expect_substr("case 5" "${_gr_out}" "golden-rule assertion cannot self-verify" TRUE)
_gr_expect_substr("case 5" "${_gr_out}" "no targets under CONSUMER_DIRS" TRUE)
_gr_expect_substr("case 5" "${_gr_out}" "GOLDEN RULE VIOLATION" FALSE)
_gr_expect_substr("case 5" "${_gr_out}" "non-existent target" FALSE)

# Case 6 — no forbidden targets (empty forbidden set).
_gr_run("case 6 (no forbidden)" FALSE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/b-noforbidden" -G Ninja
    "-DAERO_REPO_ROOT=${AERO_REPO_ROOT}" -DPROBE_SEED_NO_FORBIDDEN=ON)
_gr_expect_substr("case 6" "${_gr_out}" "golden-rule assertion cannot self-verify" TRUE)
_gr_expect_substr("case 6" "${_gr_out}" "no targets under FORBIDDEN_DIRS" TRUE)
_gr_expect_substr("case 6" "${_gr_out}" "GOLDEN RULE VIOLATION" FALSE)
_gr_expect_substr("case 6" "${_gr_out}" "non-existent target" FALSE)

message(STATUS "golden-rule.link_graph_e2e: OK")
