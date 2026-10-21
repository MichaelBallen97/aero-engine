# tests/script-boundary/guard_e2e.cmake -- task 4.1.1: a hermetic proof that
# .github/scripts/check-script-boundary.sh actually goes RED, not just that it is green on `main`.
# In tests/audio-boundary/guard_e2e.cmake's idiom exactly, and for its stated reason: this guard is green
# on the tree it ships into BY CONSTRUCTION -- no first-party file outside engine/script named a quickjs
# token before this task -- so "it passes" proves nothing about whether it can fail. The seed matrix is
# what proves it can.
#
# WHY A CTEST ENTRY AND NOT A LINE IN A PLAN. docs/plans/ and docs/specs/ are gitignored, so a sabotage
# proof that lives only there ceases to exist at merge. A ctest entry survives.
#
# git init + git add -A ONLY -- deliberately NEVER git commit: no commit means no user.name/user.email
# configuration is needed, which is what would otherwise break this on a bare CI runner. `git rev-parse
# --show-toplevel`, which the script under test runs from inside this tree, resolves to the SCRATCH repo,
# not the enclosing aero-engine repo, because git stops at the first .git it finds -- even though WORK_DIR
# is itself inside this repo's own (gitignored) build/ tree.
#
# THE GUARD SCANS TRACKED FILES ONLY, so every seed is `git add`-ed, READ BACK, and asserted present in the
# index before any verdict is trusted. An unstaged seed is invisible to the guard and the stage would
# report a false clean for entirely the wrong reason.

cmake_minimum_required(VERSION 3.28)
foreach(required SCRIPT BASH GIT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "guard_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# _sb_run(desc expect_rc <command...>) -- asserts an EXACT expected exit code, never a boolean, because
# the whole point of this driver is distinguishing 0 / 1 / 2 (the three-way exit contract every sibling
# guard carries).
function(_sb_run desc expect_rc)   # ARGN = the command
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK_DIR}/src"
                     RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_sb_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "script-boundary guard_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_sb_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "script-boundary guard_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "script-boundary guard_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

function(_sb_write relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
endfunction()

# _sb_seed(relpath content) -- the seed-landed verification helper. Nothing in this driver trusts a write
# it did not read back: BSD sed/perl in-place edits have produced silent false PASSes in this project.
function(_sb_seed relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

    file(READ "${WORK_DIR}/src/${relpath}" _sb_readback)
    string(FIND "${_sb_readback}" "${content}" _sb_idx)
    if(_sb_idx EQUAL -1)
        message(FATAL_ERROR "script-boundary guard_e2e: _sb_seed(${relpath}): the written content did not read back -- the seed did not land.")
    endif()

    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _sb_tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_sb_tracked STREQUAL "")
        message(FATAL_ERROR "script-boundary guard_e2e: _sb_seed(${relpath}): the file is not in the scratch index -- git add did not land.")
    endif()
endfunction()

# _sb_unseed(relpath) -- the removal counterpart, with the same never-trust-a-write discipline: the path
# must be GONE from the index afterwards, or the stage that depends on its absence would pass for the
# wrong reason.
function(_sb_unseed relpath)
    file(REMOVE_RECURSE "${WORK_DIR}/src/${relpath}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _sb_still OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _sb_still STREQUAL "")
        message(FATAL_ERROR "script-boundary guard_e2e: _sb_unseed(${relpath}): still in the scratch index -- the removal did not land:\n${_sb_still}")
    endif()
endfunction()

# --- The clean tree ---------------------------------------------------------------------------------------
# A close mirror of the real layout: a core public header citing a backend function in `//` prose (the
# comment-strip canary and the false-positive surface), a core source holding the one first-party JS-prefixed
# word that is not a backend name (JSON), the script layer's own public header, its private side holding the
# include the canary needs, and one runtime source (the real runtime/ is empty today, and nothing had ever
# proven the scan WALKS it rather than merely naming it).
set(_SB_CORE_HPP "#pragma once\n// cites JS_NewRuntime2 in prose\n")
set(_SB_CORE_CPP "const char* kind = \"JSON\";\n")
set(_SB_VM_HPP "#pragma once\nnamespace engine::script { class Vm; }\n")
set(_SB_IMPL_HPP "#include <quickjs.h>\nJSValue held;\n")
set(_SB_RUNTIME_CPP "int main() { return 0; }\n")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
_sb_write("engine/core/include/aero/core/clean.hpp" "${_SB_CORE_HPP}")
_sb_write("engine/core/src/clean.cpp" "${_SB_CORE_CPP}")
_sb_write("engine/script/include/aero/script/vm.hpp" "${_SB_VM_HPP}")
_sb_write("engine/script/src/vm_impl.hpp" "${_SB_IMPL_HPP}")
_sb_write("runtime/entry/clean.cpp" "${_SB_RUNTIME_CPP}")

execute_process(COMMAND "${GIT}" init -q . WORKING_DIRECTORY "${WORK_DIR}/src")
execute_process(COMMAND "${GIT}" add -A WORKING_DIRECTORY "${WORK_DIR}/src")

# --- Stage 1: clean tree -> exit 0. Silently proves self-tests 1, 1b, 2 and 3 pass too. -------------------
_sb_run("stage 1 (clean)" 0 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 1" "${_sb_out}" "script-boundary guard: OK" TRUE)

# --- Stage 2: a forward declaration in another layer's public header -> exit 1. The hole no include scan or
# compiler can see, and the reason this guard exists beside the probe. ------------------------------------
_sb_seed("engine/core/include/aero/core/leak.hpp" "struct JSContext;\n")
_sb_run("stage 2 (forward declaration in a public header)" 1 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 2" "${_sb_out}" "engine/core/include/aero/core/leak.hpp:1" TRUE)
_sb_expect_substr("stage 2" "${_sb_out}" "in a public engine header" TRUE)
_sb_unseed("engine/core/include/aero/core/leak.hpp")

# --- Stage 3: the layer's OWN public header is not exempt -> exit 1, that file and line. -----------------
_sb_seed("engine/script/include/aero/script/vm.hpp" "${_SB_VM_HPP}#include <quickjs.h>\n")
_sb_run("stage 3 (include in the layer's own public header)" 1 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 3" "${_sb_out}" "engine/script/include/aero/script/vm.hpp:3" TRUE)
_sb_expect_substr("stage 3" "${_sb_out}" "quickjs header included in a public engine header" TRUE)
_sb_seed("engine/script/include/aero/script/vm.hpp" "${_SB_VM_HPP}")

# --- Stage 4: a backend call in another layer's source -> exit 1, both tokens reported. -------------------
_sb_seed("engine/scene/src/leak.cpp" "void f(JSRuntime* rt) { JS_RunGC(rt); }\n")
_sb_run("stage 4 (backend names in another layer's source)" 1 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 4" "${_sb_out}" "engine/scene/src/leak.cpp:1" TRUE)
_sb_expect_substr("stage 4" "${_sb_out}" "outside engine/script's private side" TRUE)
_sb_expect_substr("stage 4" "${_sb_out}" "'JSRuntime'" TRUE)
_sb_expect_substr("stage 4" "${_sb_out}" "'JS_RunGC'" TRUE)
_sb_unseed("engine/scene/src/leak.cpp")

# --- Stage 5: the identical class of violation under runtime/ -> exit 1. The hermetic proof SCAN_ROOTS
# covers runtime/ rather than merely naming it (golden-rule's Gap 4). ----------------------------------------
_sb_seed("runtime/entry/leak.cpp" "#include \"quickjs.h\"\n")
_sb_run("stage 5 (include under runtime/)" 1 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 5" "${_sb_out}" "runtime/entry/leak.cpp:1" TRUE)
_sb_unseed("runtime/entry/leak.cpp")

# --- Stage 6: the inverted rule catches a JS<Upper><Upper> family no JS[A-Z][a-z] pattern would. -----------
_sb_seed("engine/core/src/leak.cpp" "JSCFunction* fn = nullptr;\n")
_sb_run("stage 6 (a JS<Upper><Upper> type)" 1 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 6" "${_sb_out}" "engine/core/src/leak.cpp:1" TRUE)
_sb_expect_substr("stage 6" "${_sb_out}" "'JSCFunction'" TRUE)
_sb_unseed("engine/core/src/leak.cpp")

# --- Stage 7: comment-only citation and the JSON exception -> exit 0. The false-positive proof, and on
# macOS the only automated proof that the nl | sed | awk pipeline behaves under BSD userland. -----------------
_sb_seed("engine/core/src/leak.cpp" "// JSValue in prose\nconst char* j = \"JSON\";\n")
_sb_run("stage 7 (comment and JSON, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 7" "${_sb_out}" "script-boundary guard: OK" TRUE)
_sb_unseed("engine/core/src/leak.cpp")

# --- Stage 8: the canary's include removed -> exit 2. Vacuity refusal, distinct from both 0 and 1. ----------
_sb_seed("engine/script/src/vm_impl.hpp" "int held;\n")
_sb_run("stage 8 (canary removed)" 2 "${BASH}" "${SCRIPT}")
_sb_expect_substr("stage 8" "${_sb_out}" "cannot self-verify" TRUE)
_sb_seed("engine/script/src/vm_impl.hpp" "${_SB_IMPL_HPP}")

# --- Stage 9: a narrowed scan -> exit 2 from the coverage self-test. The mutated copy is written into
# WORK_DIR (never over the real script), and the replacement is asserted to have landed before it runs. ------
file(READ "${SCRIPT}" _sb_text)
set(_sb_roots "SCAN_ROOTS=('engine' 'runtime')")
set(_sb_narrow "SCAN_ROOTS=('engine/core' 'runtime')")
string(FIND "${_sb_text}" "${_sb_roots}" _sb_roots_idx)
if(_sb_roots_idx EQUAL -1)
    message(FATAL_ERROR "script-boundary guard_e2e: stage 9: the script no longer declares ${_sb_roots} -- update this stage.")
endif()
string(REPLACE "${_sb_roots}" "${_sb_narrow}" _sb_mutated "${_sb_text}")
string(FIND "${_sb_mutated}" "${_sb_narrow}" _sb_narrow_idx)
if(_sb_narrow_idx EQUAL -1)
    message(FATAL_ERROR "script-boundary guard_e2e: stage 9: the mutation did not land.")
endif()
file(WRITE "${WORK_DIR}/narrowed-check.sh" "${_sb_mutated}")
_sb_run("stage 9 (narrowed SCAN_ROOTS)" 2 "${BASH}" "${WORK_DIR}/narrowed-check.sh")
_sb_expect_substr("stage 9" "${_sb_out}" "scan set does not cover every engine subsystem" TRUE)
_sb_expect_substr("stage 9" "${_sb_out}" "script" TRUE)

message(STATUS "script-boundary.guard_e2e: OK")
