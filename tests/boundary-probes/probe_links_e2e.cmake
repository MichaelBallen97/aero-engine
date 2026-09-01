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
# THE LINK COMMAND'S NAME IS COMPOSED FROM A VARIABLE, exactly as in
# tests/audio-boundary/guard_e2e.cmake and for the same reason one layer over. This file is a tracked
# *.cmake, so it sits inside the set check-boundary-probes.sh's own cross-file sweep walks -- and that
# sweep looks for a target_link_libraries naming a DERIVED PROBE in any file other than the registry.
# Spelled literally, the fixtures below made the guard exit 1 on the tree that ships it, naming seven
# lines of this file: textually true, since the text really is there, even though it is fixture data
# that only ever reaches a throwaway scratch tree. Composing the name keeps every scratch file
# byte-identical to the shape under test while leaving no matching literal here. The rejected
# alternative -- excluding this file from the sweep -- would put a permanent hole in a universal
# sweep, in the one file most likely to acquire a real CMake snippet later.
set(_BP_TLL "target_link_libraries")
# Same treatment for the PROPERTY command names: the sweep now also refuses a property write on a
# derived probe from any file, and this file names probes in its fixtures.
set(_BP_STP "set_target_properties")
set(_BP_SP "set_property")

set(_BP_REGISTRY_HEAD "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_not_a_probe OBJECT helper.cpp)\n${_BP_TLL}(aero_not_a_probe PRIVATE doctest::doctest aero::core)\n\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\n${_BP_TLL}(aero_scene_boundary_probe PRIVATE aero::scene)\n\nadd_library(aero_audio_boundary_probe OBJECT audio_boundary_probe.cpp)\n")
set(_BP_CANARY_TLL "${_BP_TLL}(aero_audio_boundary_probe PRIVATE aero::audio)\n")
set(_BP_REGISTRY "${_BP_REGISTRY_HEAD}${_BP_CANARY_TLL}")

# A second CMake file, present in EVERY stage: the cross-file sweep must have something to walk, or
# its anti-vacuity canary (swept == 0 -> exit 2) fires everywhere and hides the real verdicts. It is
# also what P12/P13/P15 seed their cross-directory writes into.
set(_BP_OTHER "add_subdirectory(audio)\n")

# --- Scratch-tree bootstrap ------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src/tests")
execute_process(COMMAND "${GIT}" init -q . WORKING_DIRECTORY "${WORK_DIR}/src")
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}")
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")

# --- P0: clean registry -> exit 0. Also the proof that aero_not_a_probe is correctly ignored. ------
_bp_run("P0 (clean)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P0" "${_bp_out}" "probe targets verified" TRUE)
_bp_expect_substr("P0" "${_bp_out}" "aero_not_a_probe" FALSE)

# --- P1: a vcpkg package joins a probe's line -> exit 1. THE named rot mode: doctest drags the whole
# shared vcpkg include root back onto the compile line and the probe asserts nothing thereafter. ----
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe PRIVATE aero::audio doctest::doctest)\n")
_bp_run("P1 (doctest::doctest on a probe)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P1" "${_bp_out}" "aero_audio_boundary_probe links non-engine token 'doctest::doctest'" TRUE)

# --- P2: a SECOND target_link_libraries call for the same probe -> exit 1. The likeliest real-world
# shape by far, and the one a naive "read the first call" check would miss entirely: CMake TLL calls
# ACCUMULATE, so the second one appends rather than replacing. -------------------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}${_BP_TLL}(aero_audio_boundary_probe PRIVATE aero::profiling)\n")
_bp_run("P2 (a second, appending TLL call)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P2" "${_bp_out}" "target_link_libraries calls -- a second call APPENDS" TRUE)

# --- P3: PRIVATE -> PUBLIC -> exit 1. An OBJECT probe propagates nothing, so PUBLIC buys nothing and
# invites the reuse that contaminates it. ----------------------------------------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe PUBLIC aero::audio)\n")
_bp_run("P3 (PUBLIC visibility)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P3" "${_bp_out}" "must be PRIVATE" TRUE)

# --- P4: an *_internal target -> exit 1 with its own message. It matches the aero:: shape and is
# refused by name: aero::scene_internal carries EnTT::EnTT INTERFACE BY DESIGN. --------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe PRIVATE aero::scene_internal)\n")
_bp_run("P4 (aero::scene_internal on a probe)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P4" "${_bp_out}" "carries its backend INTERFACE by design" TRUE)

# --- P5: TWO aero:: libraries on one probe -> exit 1. Both tokens are legal in isolation; it is the
# COUNT that is the invariant, and a per-token check alone would pass this. ------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe PRIVATE aero::audio aero::core)\n")
_bp_run("P5 (two aero:: libraries)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P5" "${_bp_out}" "aero:: libraries (must be exactly 1)" TRUE)

# --- P6: a probe with NO target_link_libraries call at all -> exit 1. It compiles nothing aero, so
# it cannot fail on a leaked backend header either. ------------------------------------------------
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\n\nadd_library(aero_audio_boundary_probe OBJECT audio_boundary_probe.cpp)\n${_BP_CANARY_TLL}")
_bp_run("P6 (a probe with zero TLL calls)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P6" "${_bp_out}" "aero_scene_boundary_probe has NO target_link_libraries call" TRUE)

# --- P7: the canary probe deleted -> exit 2, NOT a quiet pass over the one probe that remains. -----
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\n${_BP_TLL}(aero_scene_boundary_probe PRIVATE aero::scene)\n")
_bp_run("P7 (canary probe deleted)" 2 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P7" "${_bp_out}" "is not in the derived probe set" TRUE)

# --- P8: every probe renamed, so DERIVATION itself yields nothing -> exit 2. Vacuity refusal: an
# empty derived set is the shape in which a rotted extractor and a probe-less tree look identical. --
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_check OBJECT scene_boundary_probe.cpp)\n${_BP_TLL}(aero_scene_check PRIVATE aero::scene)\n\nadd_library(aero_audio_check OBJECT audio_boundary_probe.cpp)\n${_BP_TLL}(aero_audio_check PRIVATE aero::audio)\n")
_bp_run("P8 (derivation empty)" 2 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P8" "${_bp_out}" "derived an EMPTY probe set" TRUE)

# --- P9: contamination inside a MULTI-LINE call -> exit 1. The registry does not use this shape
# today, which is exactly why it is worth pinning: the flatten is what makes the check independent of
# how the call happens to be wrapped, and nothing else in the suite exercises it. ------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe\n    PRIVATE aero::audio\n    SDL3::SDL3\n)\n")
_bp_run("P9 (contamination in a multi-line TLL)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P9" "${_bp_out}" "non-engine token 'SDL3::SDL3'" TRUE)

# --- P10: the PLAIN signature, with no visibility keyword at all -> exit 1. Rejecting PUBLIC and
# INTERFACE is NOT the same check as requiring PRIVATE: target_link_libraries(<target> <lib>) is
# CMake's transitive all-keyword form, and it passed while this guard's own banner said "each linking
# exactly one aero:: library PRIVATE" -- enforcement claimed and not delivered, which
# .claude/rules/boundary-guards.md names as the failure to avoid by name. ---------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY_HEAD}${_BP_TLL}(aero_audio_boundary_probe aero::audio)\n")
_bp_run("P10 (plain signature, no PRIVATE keyword)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P10" "${_bp_out}" "0 PRIVATE keywords (must be exactly 1" TRUE)

# --- P11: EXCLUDE_FROM_ALL on a probe -> exit 1. A link line that never runs. The target is not built
# by `all`, so it compiles nothing and asserts nothing in ANY configuration, while every other check
# in this guard still passes and the probe's link line stays perfectly canonical. Plan §B.3's P-a
# names this rot mode -- "the 0.2.3 silent-green lesson" -- and it had been verified exactly once, by
# hand, at implementation time; nothing noticed afterwards. -----------------------------------------
_bp_seed("tests/CMakeLists.txt" "# A minimal stand-in for the real tests/CMakeLists.txt.\nadd_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)\n${_BP_TLL}(aero_scene_boundary_probe PRIVATE aero::scene)\n\nadd_library(aero_audio_boundary_probe OBJECT EXCLUDE_FROM_ALL audio_boundary_probe.cpp)\n${_BP_CANARY_TLL}")
_bp_run("P11 (EXCLUDE_FROM_ALL on a probe)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P11" "${_bp_out}" "aero_audio_boundary_probe is declared EXCLUDE_FROM_ALL" TRUE)

# --- P12: a cross-directory append from ANOTHER CMake file -> exit 1. CMake >= 3.13 lets any
# CMakeLists add to a target defined elsewhere, and target_link_libraries calls ACCUMULATE -- so this
# puts doctest on the probe's compile line with the registry unchanged by a single byte. Reading only
# the registry made R-b a claim about a FILE rather than about the PROBE; the sibling guard's Part 1d
# exists for precisely this capability. -------------------------------------------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}${_BP_TLL}(aero_audio_boundary_probe PRIVATE doctest::doctest)\n")
_bp_run("P12 (cross-directory append from engine/CMakeLists.txt)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P12" "${_bp_out}" "engine/CMakeLists.txt:" TRUE)
_bp_expect_substr("P12" "${_bp_out}" "appends to aero_audio_boundary_probe's link line from outside" TRUE)
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}")

# --- P13: the same append, WRAPPED so the target is not on the command's physical line -> exit 1.
# The sibling guard's finding 5, one guard over: only a flattened read sees the target here, and
# P12 alone would have passed over a line-scoped rewrite of this sweep. ---------------------------
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}${_BP_TLL}(\n    aero_audio_boundary_probe\n    PRIVATE doctest::doctest\n)\n")
_bp_run("P13 (WRAPPED cross-directory append)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P13" "${_bp_out}" "appends to aero_audio_boundary_probe's link line from outside" TRUE)
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}")

# --- P14: the same append written into a `*.cmake` file -> exit 1. The sweep's pathspec has three
# arms and P12/P13 exercise only 'CMakeLists.txt'-shaped ones; deleting '*.cmake' left all stages
# green while every .cmake file in the tree -- including this driver and its sibling -- silently left
# the swept set. The sibling guard's finding 7, one guard over. -----------------------------------
_bp_seed("cmake/probe_extras.cmake" "${_BP_TLL}(aero_audio_boundary_probe PRIVATE doctest::doctest)\n")
_bp_run("P14 (cross-directory append in a *.cmake file)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P14" "${_bp_out}" "cmake/probe_extras.cmake:" TRUE)
_bp_expect_substr("P14" "${_bp_out}" "appends to aero_audio_boundary_probe's link line from outside" TRUE)
file(REMOVE "${WORK_DIR}/src/cmake/probe_extras.cmake")
execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

# --- P15: EXCLUDE_FROM_ALL in its PROPERTY spelling -> exit 1. The add_library arm (P11) reads one
# spelling of two: set_target_properties / set_property(TARGET …) set the same property at any later
# point, and the registry with a byte-perfect add_library and this line appended passed with the
# banner still claiming "each built by `all`" (3.7.3's second review round, the blocking finding).
# Refusing the property CALLS rather than one property at a time is what closes it generally. -----
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}${_BP_STP}(aero_audio_boundary_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)\n")
_bp_run("P15 (EXCLUDE_FROM_ALL via set_target_properties)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P15" "${_bp_out}" "a target property is set on aero_audio_boundary_probe" TRUE)

# --- P16: the link line reached through its PROPERTY, from another file -> exit 1. LINK_LIBRARIES is
# what target_link_libraries writes, so this is P12's contaminant with none of P12's words in it. --
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}${_BP_SP}(TARGET aero_audio_boundary_probe APPEND PROPERTY LINK_LIBRARIES doctest::doctest)\n")
_bp_run("P16 (LINK_LIBRARIES via set_property from another file)" 1 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P16" "${_bp_out}" "engine/CMakeLists.txt:" TRUE)
_bp_expect_substr("P16" "${_bp_out}" "a target property is set on aero_audio_boundary_probe" TRUE)

# --- P17: a property write on a NON-probe target, and a non-TARGET scope -> exit 0. The
# false-positive half of P15/P16: ordinary CMake must stay silent, or the arm reds the tree on its
# first legitimate use. ---------------------------------------------------------------------------
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}${_BP_SP}(GLOBAL PROPERTY USE_FOLDERS ON)\n${_BP_STP}(aero_not_a_probe PROPERTIES FOLDER tests)\n")
_bp_run("P17 (non-probe property write, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P17" "${_bp_out}" "probe targets verified" TRUE)

# --- P18: the cross-file sweep walks ZERO other CMake files -> exit 2. Anti-vacuity: `swept` was
# printed in the banner but never asserted, so a tree with no other CMake files -- or a rotted
# pathspec -- reported "0 ... swept" and exited 0. ------------------------------------------------
file(REMOVE "${WORK_DIR}/src/engine/CMakeLists.txt")
execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
_bp_run("P18 (sweep walked zero other CMake files)" 2 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P18" "${_bp_out}" "walked ZERO other CMake files" TRUE)
_bp_seed("engine/CMakeLists.txt" "${_BP_OTHER}")

# --- P19: a file that is TRACKED but ABSENT -> exit 0, not a crash. The sibling aborted outright on
# this; both now skip it, so the two guards agree on the ordinary middle of a rename. -------------
_bp_seed("cmake/gone.cmake" "set(AERO_UNUSED 1)\n")
file(REMOVE "${WORK_DIR}/src/cmake/gone.cmake")   # staged, then gone: deliberately NOT re-added
_bp_run("P19 (tracked-but-deleted CMake file)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P19" "${_bp_out}" "No such file or directory" FALSE)
execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

# --- Restored -> exit 0. Proves every stage above was the seed talking. ---------------------------
_bp_seed("tests/CMakeLists.txt" "${_BP_REGISTRY}")
_bp_run("P0' (registry restored)" 0 "${BASH}" "${SCRIPT}")
_bp_expect_substr("P0'" "${_bp_out}" "probe targets verified" TRUE)

message(STATUS "boundary-probes.probe_links_e2e: OK -- P0-P19 + a restored-registry stage, 21 in all")
