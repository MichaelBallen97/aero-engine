# tests/boundary-probes/probe_links_e2e.cmake — task 3.7.3: a hermetic proof that
# .github/scripts/check-boundary-probes.sh actually goes RED. Same idiom as
# tests/audio-boundary/guard_e2e.cmake and tests/golden-rule/include_scan_e2e.cmake, same reason: the
# guard is green on the tree it ships into by construction, so only a seeded violation can show it
# asserts anything at all.
#
# WHAT IT IS PROTECTING. Every compile-time boundary probe in this repo works by linking exactly one
# aero:: library, so that vcpkg's shared per-triplet include/ root never reaches its compile line and
# the third-party backend behind that library is genuinely unresolvable there (R12,
# docs/08-risks.md). A second library on the line -- doctest, Tracy through aero::profiling, an
# *_internal target, the backend itself -- restores the shared root and reduces the probe to a target
# that compiles forever and asserts nothing, WITHOUT turning CI red. 0.2.3 named that the one
# unmitigated rot mode and 0.4.5 §7.1 routed it here; these stages are what close it.
#
# git init + git add -A ONLY -- deliberately NEVER git commit: no commit means no
# user.name/user.email is needed on a bare runner, and `git rev-parse --show-toplevel`, which the
# script under test runs, resolves to the SCRATCH repo because git stops at the first .git it finds.

cmake_minimum_required(VERSION 3.28)
foreach(required SCRIPT BASH GIT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "probe_links_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# _bp_run(desc expect_rc <command...>) -- EXACT exit code, never a boolean: 0 clean / 1 violation /
# 2 cannot-self-verify are three different verdicts and this driver is what keeps them apart.
function(_bp_run desc expect_rc)   # ARGN = the command
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK_DIR}/src"
                     RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_bp_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "boundary-probes probe_links_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_bp_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "boundary-probes probe_links_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "boundary-probes probe_links_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

# _bp_seed(relpath content) -- write, stage, READ BACK, and assert the path is in the index. Nothing
# here trusts a write it did not read back; three harness seeds failed to land silently during this
# guard's bring-up and this is what caught every one.
function(_bp_seed relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

    file(READ "${WORK_DIR}/src/${relpath}" _bp_readback)
    string(FIND "${_bp_readback}" "${content}" _bp_idx)
    if(_bp_idx EQUAL -1)
        message(FATAL_ERROR "boundary-probes probe_links_e2e: _bp_seed(${relpath}): the written content did not read back -- the seed did not land.")
    endif()

    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _bp_tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_bp_tracked STREQUAL "")
        message(FATAL_ERROR "boundary-probes probe_links_e2e: _bp_seed(${relpath}): the file is not in the scratch index -- git add did not land.")
    endif()
endfunction()

# --- The fake registry -----------------------------------------------------------------------------
# TWO probes, one of them the canary the guard hardcodes, each in the real registry's exact shape --
# plus ONE non-probe OBJECT library whose link line is deliberately filthy. That third target is a
# false-positive proof: the derived set is filtered to names ending in _boundary_probe, so a
# doctest-linking OBJECT library that is not a probe must be invisible to this guard. P0 exiting 0
# with that target present is what proves the filter is not over-broad.
set(_BP_REGISTRY_HEAD [==[# A minimal stand-in for the real tests/CMakeLists.txt.
add_library(aero_not_a_probe OBJECT helper.cpp)
target_link_libraries(aero_not_a_probe PRIVATE doctest::doctest aero::core)

add_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)
target_link_libraries(aero_scene_boundary_probe PRIVATE aero::scene)

add_library(aero_audio_boundary_probe OBJECT audio_boundary_probe.cpp)
]==])
set(_BP_CANARY_TLL [==[target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio)
]==])
set(_BP_REGISTRY "${_BP_REGISTRY_HEAD}${_BP_CANARY_TLL}")

# --- Scratch-tree bootstrap ------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src/tests")
execute_process(COMMAND "${GIT}" init -q . WORKING_DIRECTORY "${WORK_DIR}/src")
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")

# --- P0: clean registry -> exit 0. Also the proof that aero_not_a_probe is correctly ignored. ------
_bp_run("P0 (clean)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P0" "${_bp_out}" "probe targets verified" TRUE)
_bp_expect_substr("P0" "${_bp_out}" "aero_not_a_probe" FALSE)

# --- P1: a vcpkg package joins a probe's line -> exit 1. THE named rot mode: doctest drags the whole
# shared vcpkg include root back onto the compile line and the probe asserts nothing thereafter. ----
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio doctest::doctest)\n")
_bp_run("P1 (doctest::doctest on a probe)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P1" "${_bp_out}" "aero_audio_boundary_probe links non-engine token 'doctest::doctest'" TRUE)

# --- P2: a SECOND target_link_libraries call for the same probe -> exit 1. The likeliest real-world
# shape by far, and the one a naive "read the first call" check would miss entirely: CMake TLL calls
# ACCUMULATE, so the second one appends rather than replacing. -------------------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}target_link_libraries(aero_audio_boundary_probe PRIVATE aero::profiling)\n")
_bp_run("P2 (a second, appending TLL call)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P2" "${_bp_out}" "target_link_libraries calls -- a second call APPENDS" TRUE)

# --- P3: PRIVATE -> PUBLIC -> exit 1. An OBJECT probe propagates nothing, so PUBLIC buys nothing and
# invites the reuse that contaminates it. ----------------------------------------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}target_link_libraries(aero_audio_boundary_probe PUBLIC aero::audio)\n")
_bp_run("P3 (PUBLIC visibility)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P3" "${_bp_out}" "must be PRIVATE" TRUE)

# --- P4: an *_internal target -> exit 1 with its own message. It matches the aero:: shape and is
# refused by name: aero::scene_internal carries EnTT::EnTT INTERFACE BY DESIGN. --------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}target_link_libraries(aero_audio_boundary_probe PRIVATE aero::scene_internal)\n")
_bp_run("P4 (aero::scene_internal on a probe)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P4" "${_bp_out}" "carries its backend INTERFACE by design" TRUE)

# --- P5: TWO aero:: libraries on one probe -> exit 1. Both tokens are legal in isolation; it is the
# COUNT that is the invariant, and a per-token check alone would pass this. ------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio aero::core)\n")
_bp_run("P5 (two aero:: libraries)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P5" "${_bp_out}" "aero:: libraries (must be exactly 1)" TRUE)

# --- P6: a probe with NO target_link_libraries call at all -> exit 1. It compiles nothing aero, so
# it cannot fail on a leaked backend header either. ------------------------------------------------
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\n\nadd_library(aero_audio_boundary_probe OBJECT audio_boundary_probe.cpp)\n${_BP_CANARY_TLL}")
_bp_run("P6 (a probe with zero TLL calls)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P6" "${_bp_out}" "aero_scene_boundary_probe has NO target_link_libraries call" TRUE)

# --- P7: the canary probe deleted -> exit 2, NOT a quiet pass over the one probe that remains. -----
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\ntarget_link_libraries(aero_scene_boundary_probe PRIVATE aero::scene)\n")
_bp_run("P7 (canary probe deleted)" 2 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P7" "${_bp_out}" "is not in the derived probe set" TRUE)

# --- P8: every probe renamed, so DERIVATION itself yields nothing -> exit 2. Vacuity refusal: an
# empty derived set is the shape in which a rotted extractor and a probe-less tree look identical. --
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_check OBJECT scene_boundary_probe.cpp)\ntarget_link_libraries(aero_scene_check PRIVATE aero::scene)\n\nadd_library(aero_audio_check OBJECT audio_boundary_probe.cpp)\ntarget_link_libraries(aero_audio_check PRIVATE aero::audio)\n")
_bp_run("P8 (derivation empty)" 2 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P8" "${_bp_out}" "derived an EMPTY probe set" TRUE)

# --- P9: contamination inside a MULTI-LINE call -> exit 1. The registry does not use this shape
# today, which is exactly why it is worth pinning: the flatten is what makes the check independent of
# how the call happens to be wrapped, and nothing else in the suite exercises it. ------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}target_link_libraries(aero_audio_boundary_probe\n    PRIVATE aero::audio\n    SDL3::SDL3\n)\n")
_bp_run("P9 (contamination in a multi-line TLL)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P9" "${_bp_out}" "non-engine token 'SDL3::SDL3'" TRUE)

# --- Restored -> exit 0. Proves every stage above was the seed talking. ---------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")
_bp_run("P0' (registry restored)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P0'" "${_bp_out}" "probe targets verified" TRUE)

message(STATUS "boundary-probes.probe_links_e2e: OK -- P0-P9 + a restored-registry stage, 11 in all")
