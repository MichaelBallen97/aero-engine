# tests/golden-rule/include_scan_e2e.cmake — task 2.1.2, Part C1: a hermetic proof that
# .github/scripts/check-golden-rule.sh actually goes RED, not just that it is green on `main`. In the
# tests/reflect-gen/incremental_e2e.cmake idiom (F13): a scratch tree, no add_subdirectory'd probe
# project needed here (the script is what is under test, not a build graph — that is Part C2's job).
#
# git init + git add -A ONLY -- deliberately NEVER git commit (F11): no commit means no
# user.name/user.email configuration is needed, which is what would otherwise break this on a bare CI
# runner. `git rev-parse --show-toplevel`, which the script under test runs from inside this tree,
# resolves to the SCRATCH repo, not the enclosing aero-engine repo, because git stops at the first
# .git it finds -- even though WORK_DIR is itself inside this repo's own (gitignored) build/ tree.

cmake_minimum_required(VERSION 3.28)
foreach(required SCRIPT BASH GIT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "include_scan_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# _gr_run(desc expect_rc <command...>) -- unlike tests/reflect-gen's _e2e_run (a boolean expect_zero),
# this asserts an EXACT expected exit code, because the whole point of this driver is distinguishing
# 0 / 1 / 2 (D7's three-way contract).
function(_gr_run desc expect_rc)   # ARGN = the command
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK_DIR}/src"
                     RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_gr_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "include_scan_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_gr_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "include_scan_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "include_scan_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

function(_gr_write relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
endfunction()

# _gr_seed(relpath content) -- the seed-landed verification helper. Direct mitigation of repo memory
# verify-the-seed-landed-in-sabotage-proofs, where BSD sed/perl once made a MANUAL sabotage report a
# false PASS because the seed never landed. Nothing in this driver trusts a write it did not read
# back.
function(_gr_seed relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

    # Read the file back and assert the content is actually there.
    file(READ "${WORK_DIR}/src/${relpath}" _gr_readback)
    string(FIND "${_gr_readback}" "${content}" _gr_idx)
    if(_gr_idx EQUAL -1)
        message(FATAL_ERROR "include_scan_e2e: _gr_seed(${relpath}): the written content did not read back -- the seed did not land.")
    endif()

    # And that it is actually tracked -- the guard scans TRACKED files only, so a file that exists on
    # disk but is absent from the index is invisible to it, and the next stage would report a false
    # "clean" for the wrong reason.
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _gr_tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_gr_tracked STREQUAL "")
        message(FATAL_ERROR "include_scan_e2e: _gr_seed(${relpath}): the file is not in the scratch index -- git add did not land.")
    endif()
endfunction()

# --- Scratch-tree bootstrap ------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")

# A minimal tree mirroring the real layout: one engine source AND one runtime source (fix, post-review
# Gap 4 -- see stage 3 below: the real tree's runtime/ is legitimately EMPTY today (F7), so nothing had
# ever hermetically proven the scan actually WALKS runtime/, as opposed to SCAN_ROOTS merely NAMING it)
# and the editor's public-header dir (the D8 inverted canary, self-tests 2-3).
_gr_write("engine/core/src/clean.cpp"                "#include <aero/core/log.hpp>\n")
_gr_write("runtime/entry/clean.cpp"                  "#include <cstdio>\n")
_gr_write("editor/include/aero/editor/editor_ui.hpp" "#pragma once\n")

execute_process(COMMAND "${GIT}" init -q .   WORKING_DIRECTORY "${WORK_DIR}/src")
execute_process(COMMAND "${GIT}" add -A      WORKING_DIRECTORY "${WORK_DIR}/src")

# --- Stage 1: clean tree -> exit 0. Silently proves self-tests 1-3 pass too. -----------------------
_gr_run("stage 1 (clean)" 0 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 1" "${_gr_out}" "golden-rule guard: OK" TRUE)

# --- Stage 2: the relative escape under engine/ (row 2 -- the form that actually compiles today) ---
# -> exit 1.
_gr_seed("engine/core/src/leak.cpp" "#include \"../../../editor/include/aero/editor/editor_ui.hpp\"\n")
_gr_run("stage 2 (relative escape, engine/)" 1 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 2" "${_gr_out}" "engine/core/src/leak.cpp:1" TRUE)
_gr_expect_substr("stage 2" "${_gr_out}" ": #include reaches into /editor" TRUE)

# --- Stage 3 (fix, post-review Gap 4): the IDENTICAL violation under runtime/ -> exit 1. This is the
# hermetic proof SCAN_ROOTS actually covers runtime/, not just names it: replacing
# SCAN_ROOTS=('engine' 'runtime') with ('engine') left every stage in this file fully green before this
# fix (runtime/ contributed nothing to seed a violation against), while the real guard's own
# "OK -- N tracked engine/runtime sources scanned" message would have silently become a lie the moment
# 5.2.1 populates runtime/ with a real source. Cleaned up immediately after asserting so stages 4-7 see
# exactly the tree shape they were written against (only the engine/ leak file present).
_gr_seed("runtime/entry/leak.cpp" "#include \"../../editor/include/aero/editor/editor_ui.hpp\"\n")
_gr_run("stage 3 (relative escape, runtime/)" 1 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 3" "${_gr_out}" "runtime/entry/leak.cpp:1" TRUE)
_gr_expect_substr("stage 3" "${_gr_out}" ": #include reaches into /editor" TRUE)
file(REMOVE "${WORK_DIR}/src/runtime/entry/leak.cpp")
execute_process(COMMAND "${GIT}" add -A WORKING_DIRECTORY "${WORK_DIR}/src")

# --- Stage 4 (was stage 3): the forward-declaration hole (row 3 -- invisible to any include scan) ---
# -> exit 1.
_gr_seed("engine/core/src/leak.cpp" "namespace engine::editor { class ImGuiLayer; }\n")
_gr_run("stage 4 (forward declaration)" 1 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 4" "${_gr_out}" ": editor namespace named in engine code" TRUE)

# --- Stage 5 (was stage 4): comment-only citation -> exit 0. The false-positive proof, and (on macOS)
# the only automated proof that the nl | sed comment-stripping pipeline behaves under BSD userland. ---
_gr_seed("engine/core/src/leak.cpp" "// see engine::editor::ImGuiLayer\n// #include <aero/editor/editor_ui.hpp>\n")
_gr_run("stage 5 (comment-only, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 5" "${_gr_out}" "golden-rule guard: OK" TRUE)

# --- Stage 6 (was stage 5): seed removed -> exit 0. Proves removal restores green ("then cleaned"). -
file(REMOVE "${WORK_DIR}/src/engine/core/src/leak.cpp")
execute_process(COMMAND "${GIT}" add -A WORKING_DIRECTORY "${WORK_DIR}/src")
_gr_run("stage 6 (cleaned)" 0 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 6" "${_gr_out}" "golden-rule guard: OK" TRUE)

# --- Stage 7 (was stage 6): canary deleted -> exit 2. Vacuity refusal, distinct from both 0 and 1.
# `clean.cpp` (both engine/ and runtime/) stays tracked, so the scan set is still non-empty -- it is
# genuinely self-test 2 (canary present) that fires here, not self-test 1. --------------------------
file(REMOVE_RECURSE "${WORK_DIR}/src/editor")
execute_process(COMMAND "${GIT}" add -A WORKING_DIRECTORY "${WORK_DIR}/src")
_gr_run("stage 7 (canary deleted)" 2 "${BASH}" "${SCRIPT}")
_gr_expect_substr("stage 7" "${_gr_out}" "cannot self-verify" TRUE)

message(STATUS "golden-rule.include_scan_e2e: OK")
