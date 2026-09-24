# tests/project-no-delete/no_delete_e2e.cmake -- code-review round, task 2.6.1: a hermetic proof that
# .github/scripts/check-project-no-delete.sh actually goes RED, not just that it is green on `main`.
# Mirrors tests/golden-rule/include_scan_e2e.cmake's shape (2.1.2's recipe): a scratch tree, git init +
# git add -A ONLY -- deliberately NEVER git commit, so no user.name/user.email configuration is needed
# on a bare CI runner. `git rev-parse --show-toplevel`, which the script under test runs from inside
# this tree, resolves to the SCRATCH repo, not the enclosing aero-engine repo, because git stops at the
# first .git it finds.

cmake_minimum_required(VERSION 3.28)
foreach(required SCRIPT BASH GIT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "no_delete_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

function(_nd_run desc expect_rc)   # ARGN = the command
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK_DIR}/src"
                     RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_nd_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "no_delete_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_nd_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "no_delete_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "no_delete_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

# _nd_seed(relpath content) -- the seed-landed verification helper (repo memory
# verify-the-seed-landed-in-sabotage-proofs): read the write back and confirm it is tracked before
# trusting the next stage's exit code.
function(_nd_seed relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

    file(READ "${WORK_DIR}/src/${relpath}" _nd_readback)
    string(FIND "${_nd_readback}" "${content}" _nd_idx)
    if(_nd_idx EQUAL -1)
        message(FATAL_ERROR "no_delete_e2e: _nd_seed(${relpath}): the written content did not read back -- the seed did not land.")
    endif()

    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _nd_tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nd_tracked STREQUAL "")
        message(FATAL_ERROR "no_delete_e2e: _nd_seed(${relpath}): the file is not in the scratch index -- git add did not land.")
    endif()
endfunction()

# --- Scratch-tree bootstrap ------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
execute_process(COMMAND "${GIT}" init -q . WORKING_DIRECTORY "${WORK_DIR}/src")

set(_nd_clean_body "// harmless, no delete/rename/copy call here\nvoid noop() {}\n")
_nd_seed("editor/src/project.cpp"      "${_nd_clean_body}")
_nd_seed("editor/src/project_file.cpp" "${_nd_clean_body}")
_nd_seed("editor/src/project_ui.cpp"   "${_nd_clean_body}")
_nd_seed("editor/src/asset_meta.cpp"     "${_nd_clean_body}")
_nd_seed("editor/src/asset_database.cpp" "${_nd_clean_body}")
_nd_seed("editor/src/asset_cache.cpp"    "${_nd_clean_body}")
# task E.4.1: the SEVENTH Check-A file. Self-test 1 refuses to pass when a named file is absent from
# the tree it is scanning, so the scratch tree has to carry every entry of FORBIDDEN_FILES -- adding
# one to the script without adding it here turns stage 1 into an exit 2.
_nd_seed("editor/src/project_state.cpp"  "${_nd_clean_body}")
# task 3.1.3 (D13): Check B's own PERMITTED_DELETERS entries, plus a THIRD, ordinary file that must
# stay clean throughout -- the hole Check B exists to close.
_nd_seed("editor/src/text_file.cpp"          "${_nd_clean_body}")
_nd_seed("editor/src/asset_actions.cpp"      "${_nd_clean_body}")
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")

# --- Stage 1: clean tree -> exit 0. Silently proves the three self-tests pass too. -----------------
_nd_run("stage 1 (clean)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 1" "${_nd_out}" "project-no-delete guard: OK" TRUE)

# --- Stage 2: remove_all seeded in project.cpp -> exit 1. -------------------------------------------
_nd_seed("editor/src/project.cpp" "void bad() { std::error_code ec; std::filesystem::remove_all(\"x\", ec); }\n")
_nd_run("stage 2 (remove_all, project.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 2" "${_nd_out}" "editor/src/project.cpp:1" TRUE)
_nd_seed("editor/src/project.cpp" "${_nd_clean_body}")
_nd_run("stage 2 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 3: std::filesystem::rename seeded in project_file.cpp -> exit 1. ------------------------
_nd_seed("editor/src/project_file.cpp" "void bad() { std::error_code ec; std::filesystem::rename(\"x\", \"y\", ec); }\n")
_nd_run("stage 3 (rename, project_file.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 3" "${_nd_out}" "editor/src/project_file.cpp:1" TRUE)
_nd_seed("editor/src/project_file.cpp" "${_nd_clean_body}")
_nd_run("stage 3 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 4: std::filesystem::copy (the bare `::copy` alternative) seeded in project_ui.cpp -> 1. --
_nd_seed("editor/src/project_ui.cpp" "void bad() { std::error_code ec; std::filesystem::copy(\"x\", \"y\", ec); }\n")
_nd_run("stage 4 (copy, project_ui.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 4" "${_nd_out}" "editor/src/project_ui.cpp:1" TRUE)
_nd_seed("editor/src/project_ui.cpp" "${_nd_clean_body}")
_nd_run("stage 4 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 5: comment-only mention -> exit 0. The false-positive proof, and (on macOS) the only
# automated proof that the nl | sed comment-stripping pipeline behaves under BSD userland. ---------
_nd_seed("editor/src/project.cpp" "// never call std::filesystem::remove_all here (D7/INV-P4)\nvoid noop() {}\n")
_nd_run("stage 5 (comment-only, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 5" "${_nd_out}" "project-no-delete guard: OK" TRUE)
_nd_seed("editor/src/project.cpp" "${_nd_clean_body}")

# --- Stage 6: canary -- one of the SIX named files goes missing -> exit 2. Vacuity refusal, distinct
# from both 0 and 1. Retargeted (task 3.1.2) to asset_cache.cpp -- the file THIS task's widening
# added -- so the widening itself, not just task 3.1.1's, is what the canary proves. -----------------
file(REMOVE "${WORK_DIR}/src/editor/src/asset_cache.cpp")
execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
_nd_run("stage 6 (file missing)" 2 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 6" "${_nd_out}" "cannot self-verify" TRUE)
_nd_seed("editor/src/asset_cache.cpp" "${_nd_clean_body}")
_nd_run("stage 6 (restored)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 7 (task 3.1.1): std::filesystem::remove seeded in asset_database.cpp -> exit 1. -----------
_nd_seed("editor/src/asset_database.cpp" "void bad() { std::error_code ec; std::filesystem::remove(\"x\", ec); }\n")
_nd_run("stage 7 (remove, asset_database.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 7" "${_nd_out}" "editor/src/asset_database.cpp:1" TRUE)
_nd_seed("editor/src/asset_database.cpp" "${_nd_clean_body}")
_nd_run("stage 7 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 8 (task 3.1.1): remove_all seeded in asset_meta.cpp -> exit 1. -----------------------------
_nd_seed("editor/src/asset_meta.cpp" "void bad() { std::error_code ec; std::filesystem::remove_all(\"x\", ec); }\n")
_nd_run("stage 8 (remove_all, asset_meta.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 8" "${_nd_out}" "editor/src/asset_meta.cpp:1" TRUE)
_nd_seed("editor/src/asset_meta.cpp" "${_nd_clean_body}")
_nd_run("stage 8 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 9 (task 3.1.2): std::filesystem::rename seeded in asset_cache.cpp -> exit 1. Rename, not
# remove, so the four forbidden forms stay spread across the stages rather than clustering. -----------
_nd_seed("editor/src/asset_cache.cpp" "void bad() { std::error_code ec; std::filesystem::rename(\"x\", \"y\", ec); }\n")
_nd_run("stage 9 (rename, asset_cache.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 9" "${_nd_out}" "editor/src/asset_cache.cpp:1" TRUE)
_nd_seed("editor/src/asset_cache.cpp" "${_nd_clean_body}")
_nd_run("stage 9 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 10 (task 3.1.3, D13): std::filesystem::remove seeded in a SEVENTH file
# (asset_browser_panel.cpp) -> exit 1. This is the hole Check B exists to close -- Check A alone
# would stay green on it, since asset_browser_panel.cpp is not one of its six named files. -----------
_nd_seed("editor/src/asset_browser_panel.cpp"
         "void bad() { std::error_code ec; std::filesystem::remove(\"x\", ec); }\n")
_nd_run("stage 10 (remove, asset_browser_panel.cpp -- Check B's own hole)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 10" "${_nd_out}" "editor/src/asset_browser_panel.cpp:1" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")
_nd_run("stage 10 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 11 (A9): std::copy seeded in the same seventh file -> exit 0. The false-positive proof. ---
_nd_seed("editor/src/asset_browser_panel.cpp" "void ok() { int a[1]; int b[1]; std::copy(a, a + 1, b); }\n")
_nd_run("stage 11 (std::copy, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 11" "${_nd_out}" "project-no-delete guard: OK" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")

# --- Stage 12 (seed S24): asset_actions.cpp -- Check B's own PERMITTED_DELETERS entry -- goes
# missing -> exit 2, "cannot self-verify". The allowlist's own vacuity refusal. ------------------------
file(REMOVE "${WORK_DIR}/src/editor/src/asset_actions.cpp")
execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
_nd_run("stage 12 (asset_actions.cpp missing -- Check B cannot self-verify)" 2 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 12" "${_nd_out}" "cannot self-verify" TRUE)
_nd_seed("editor/src/asset_actions.cpp" "${_nd_clean_body}")
_nd_run("stage 12 (restored)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 13 (task E.4.1): std::filesystem::copy seeded in project_state.cpp -> exit 1. The seventh
# Check-A file's own proof: listing a file in FORBIDDEN_FILES that no stage ever seeds would be an
# entry nothing shows to be live, which is the same species of hole stage 10 closes for Check B.
#
# THE SEED IS `::copy`, AND THAT IS WHAT MAKES THE STAGE DISCRIMINATE (code review). It is in Check
# A's FORBIDDEN_RE and DELIBERATELY NOT in Check B's DELETE_RE (`::copy` would match `std::copy` under
# a glob over every editor/src TU), so Check A is the ONLY check that can fail this stage. A delete or
# rename seed proves nothing here: Check B's glob catches it in ANY editor/src/*.cpp and emits the
# same file:line prefix, so the stage would stay green with the FORBIDDEN_FILES entry deleted -- green
# against the very mutation it exists to catch. Stage 4 seeds the identical form into project_ui.cpp
# for the same reason; this is that stage's wording, one file over. Verified in both directions: it
# exits 1 with the entry present, and exits 0 (failing this stage, loudly) with the entry removed.
_nd_seed("editor/src/project_state.cpp" "void bad() { std::error_code ec; std::filesystem::copy(\"x\", \"y\", ec); }\n")
_nd_run("stage 13 (copy, project_state.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 13" "${_nd_out}" "editor/src/project_state.cpp:1" TRUE)
_nd_seed("editor/src/project_state.cpp" "${_nd_clean_body}")
_nd_run("stage 13 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stages 14-19 (task E.4.3) -------------------------------------------------------------------
# E.4.3 puts five std::filesystem::rename call sites into asset_actions.cpp. The stages below prove
# Check B covers that spelling in a third file, that it still PERMITS it in asset_actions.cpp, and
# that Check B's new prong 2 (the std::filesystem alias ban) fires and does not over-match.
#
# TWO OF THE PLAN'S PROPOSED STAGES WERE ALREADY HERE AND ARE NOT DUPLICATED: stage 10 already seeds
# std::filesystem::remove into asset_browser_panel.cpp expecting exit 1, and stage 12 already covers
# a renamed asset_actions.cpp ("cannot self-verify", seed S24). What was genuinely missing is the
# RENAME spelling in a third file, the remove_all alternative, and -- above all -- the two
# ANTI-VACUITY arms inside asset_actions.cpp: without them stage 14 proves only that the guard fires
# SOMEWHERE, and a guard that refused everything, permitted files included, would pass it while
# breaking task E.4.3's own executor.
#
# A guard's own .cmake e2e driver is inside the set some guards sweep, and that bit this tree twice
# at 3.7.3. It does NOT bite here: check-project-no-delete.sh sweeps editor/src/*.cpp and the seven
# named files, never *.cmake, so the seed strings below are invisible to it in the real repository.
# Stated rather than rediscovered -- if a future prong ever widens the scan set to *.cmake, THIS FILE
# is the first thing that breaks.

# --- Stage 14 (B-new-1): the RENAME spelling in a third editor/src TU -> exit 1. ---------------------
_nd_seed("editor/src/asset_browser_panel.cpp"
         "void bad() { std::error_code ec; std::filesystem::rename(pathFromUtf8(a), pathFromUtf8(b), ec); }\n")
_nd_run("stage 14 (rename, asset_browser_panel.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 14" "${_nd_out}" "editor/src/asset_browser_panel.cpp:1" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")
_nd_run("stage 14 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 15 (B-new-2): the SAME call in asset_actions.cpp -> exit 0. THE ANTI-VACUITY ARM. ---------
# This is the stage that says the allowlist still means something. Without it, a guard that refused
# every file would pass stage 14 and break task E.4.3's executor on the very next commit.
_nd_seed("editor/src/asset_actions.cpp"
         "void permitted() { std::error_code ec; std::filesystem::rename(pathFromUtf8(a), pathFromUtf8(b), ec); }\n")
_nd_run("stage 15 (rename, asset_actions.cpp -- PERMITTED)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 15" "${_nd_out}" "project-no-delete guard: OK" TRUE)
_nd_seed("editor/src/asset_actions.cpp" "${_nd_clean_body}")

# --- Stage 16 (B-new-3): the remove_all alternative in a third TU -> exit 1. -------------------------
# Distinct from stage 10, which seeds std::filesystem::remove: remove_all is its OWN alternative in
# DELETE_RE and was exercised only by the script's internal self-test, never hermetically.
_nd_seed("editor/src/asset_browser_panel.cpp" "void bad() { std::error_code ec; std::filesystem::remove_all(\"x\", ec); }\n")
_nd_run("stage 16 (remove_all, asset_browser_panel.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 16" "${_nd_out}" "editor/src/asset_browser_panel.cpp:1" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")
_nd_run("stage 16 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 17 (B-new-4): the SAME remove_all in asset_actions.cpp -> exit 0. The SECOND anti-vacuity
# arm, and it is the one that keeps D1 honest: Check B PERMITS remove_all here, which is exactly why
# AA61's source-text pin is the only thing asserting the call is not actually present.
_nd_seed("editor/src/asset_actions.cpp" "void permitted() { std::error_code ec; std::filesystem::remove_all(\"x\", ec); }\n")
_nd_run("stage 17 (remove_all, asset_actions.cpp -- PERMITTED)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 17" "${_nd_out}" "project-no-delete guard: OK" TRUE)
_nd_seed("editor/src/asset_actions.cpp" "${_nd_clean_body}")
_nd_run("stage 17 (restored)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 18 (B-new-5): prong 2 fires on a namespace alias -> exit 1. --------------------------------
# The call itself is `fs::rename`, which DELETE_RE cannot see AT ALL. Only prong 2 can fail this
# stage, which is what makes it discriminate the new prong rather than the old pattern.
_nd_seed("editor/src/asset_browser_panel.cpp"
         "namespace fs = std::filesystem;\nvoid bad() { std::error_code ec; fs::rename(\"x\", \"y\", ec); }\n")
_nd_run("stage 18 (namespace alias, asset_browser_panel.cpp)" 1 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 18" "${_nd_out}" "alias" TRUE)
_nd_expect_substr("stage 18" "${_nd_out}" "editor/src/asset_browser_panel.cpp:1" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")
_nd_run("stage 18 (cleaned)" 0 "${BASH}" "${SCRIPT}")

# --- Stage 19 (B-new-6): prong 2's false-positive proof -> exit 0. -----------------------------------
# An ordinary `#include <filesystem>` must not trip it. Stage 18 alone proves the prong fires
# somewhere; this is what stops a pattern matching everything from passing.
_nd_seed("editor/src/asset_browser_panel.cpp" "#include <filesystem>\nvoid ok() {}\n")
_nd_run("stage 19 (ordinary include, prong 2 false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_nd_expect_substr("stage 19" "${_nd_out}" "project-no-delete guard: OK" TRUE)
_nd_seed("editor/src/asset_browser_panel.cpp" "${_nd_clean_body}")
_nd_run("stage 19 (cleaned)" 0 "${BASH}" "${SCRIPT}")

message(STATUS "project-no-delete.no_delete_e2e: OK")
