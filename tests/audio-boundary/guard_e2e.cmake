# tests/audio-boundary/guard_e2e.cmake — task 3.7.3: a hermetic proof that
# .github/scripts/check-audio-boundary.sh actually goes RED, not just that it is green on `main`.
# In tests/golden-rule/include_scan_e2e.cmake's idiom exactly, and for its stated reason: this guard
# is green on the tree it ships into BY CONSTRUCTION, so "it passes" proves nothing at all about
# whether it can fail. The seed matrix is what proves it can.
#
# WHY A CTEST ENTRY AND NOT A LINE IN A PLAN. docs/plans/ and docs/specs/ are gitignored, so a
# sabotage proof that lives only there CEASES TO EXIST AT MERGE -- the reasoning that produced
# project-no-delete.no_delete_e2e. A ctest entry survives.
#
# git init + git add -A ONLY -- deliberately NEVER git commit: no commit means no
# user.name/user.email configuration is needed, which is what would otherwise break this on a bare CI
# runner. `git rev-parse --show-toplevel`, which the script under test runs from inside this tree,
# resolves to the SCRATCH repo, not the enclosing aero-engine repo, because git stops at the first
# .git it finds -- even though WORK_DIR is itself inside this repo's own (gitignored) build/ tree.
#
# THE GUARD SCANS TRACKED FILES ONLY, so every seed is `git add`-ed, READ BACK, and asserted present
# in the index before any verdict is trusted. An unstaged seed is invisible to the guard and the
# stage would report a false clean for entirely the wrong reason.

cmake_minimum_required(VERSION 3.28)
foreach(required SCRIPT BASH GIT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "guard_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# _ab_run(desc expect_rc <command...>) -- asserts an EXACT expected exit code, never a boolean,
# because the whole point of this driver is distinguishing 0 / 1 / 2 (the three-way exit contract
# every sibling guard carries).
function(_ab_run desc expect_rc)   # ARGN = the command
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK_DIR}/src"
                     RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_ab_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "audio-boundary guard_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_ab_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "audio-boundary guard_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "audio-boundary guard_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

function(_ab_write relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
endfunction()

# _ab_seed(relpath content) -- the seed-landed verification helper. Nothing in this driver trusts a
# write it did not read back: BSD sed/perl in-place edits have produced silent false PASSes in this
# project before, and three harness seeds failed to land during this guard's own bring-up.
function(_ab_seed relpath content)
    file(WRITE "${WORK_DIR}/src/${relpath}" "${content}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)

    file(READ "${WORK_DIR}/src/${relpath}" _ab_readback)
    string(FIND "${_ab_readback}" "${content}" _ab_idx)
    if(_ab_idx EQUAL -1)
        message(FATAL_ERROR "audio-boundary guard_e2e: _ab_seed(${relpath}): the written content did not read back -- the seed did not land.")
    endif()

    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _ab_tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_ab_tracked STREQUAL "")
        message(FATAL_ERROR "audio-boundary guard_e2e: _ab_seed(${relpath}): the file is not in the scratch index -- git add did not land.")
    endif()
endfunction()

# _ab_unseed(relpath) -- the removal counterpart, with the same never-trust-a-write discipline: the
# path must be GONE from the index afterwards, or the X-stage that depends on its absence would pass
# for the wrong reason.
function(_ab_unseed relpath)
    file(REMOVE_RECURSE "${WORK_DIR}/src/${relpath}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" ls-files -- "${relpath}"
                     OUTPUT_VARIABLE _ab_still OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _ab_still STREQUAL "")
        message(FATAL_ERROR "audio-boundary guard_e2e: _ab_unseed(${relpath}): still in the scratch index -- the removal did not land:\n${_ab_still}")
    endif()
endfunction()

# --- The base tree's file contents ----------------------------------------------------------------
# Bracket arguments ([==[ ... ]==]) so ${CMAKE_CURRENT_SOURCE_DIR} survives into the scratch file as
# literal text rather than being expanded by this driver.
#
# A close mirror of the real layout: three CMakeLists each carrying a "NO find_package" prohibition
# comment (the comment-strip canary), each with a REAL MULTI-LINE target_link_libraries call (the
# shape all three actually use, and the shape the extractor must handle), a public audio header
# citing ma_device_uninit in `//` prose (the identifier-regex canary AND the false-positive surface),
# and clean sources under BOTH audio roots.
# THE LINK COMMAND'S NAME IS COMPOSED FROM A VARIABLE, AND THAT IS NOT A STYLE CHOICE. This file is
# a tracked *.cmake, so it sits inside the very set prong A-d sweeps -- and A-d looks for a
# target_link_libraries call naming aero_assets / aero_audio / aero_scene_audio in any CMake file
# other than the three guarded ones. Spelling the fixtures below with the literal command name made
# the guard exit 1 on the tree that ships it, naming six lines of this file: textually a true
# positive, since the text really is there, even though it is fixture data that only ever reaches a
# throwaway scratch tree. Composing the name keeps the scratch file BYTE-IDENTICAL to the shape under
# test while leaving no matching literal here.
#
# The rejected alternative was adding this file (and its sibling) to Part 1d's skip list. That would
# have put a permanent, silent hole in a sweep whose whole value is being universal -- and the hole
# would sit in the one file a future reader is most likely to paste a real CMake snippet into. A
# fixture that has to spell one identifier indirectly is the cheaper price. Do not "simplify" it back.
set(_AB_TLL "target_link_libraries")

set(_AB_ENGINE_CMAKE [==[add_subdirectory(assets)
add_subdirectory(audio)
add_subdirectory(scene_audio)
]==])

set(_AB_ASSETS_CMAKE_HEAD [==[# engine/assets/ -- the cooked-asset formats.
#
# NO find_package. NOT ONE, EVER. aero_assets links no vcpkg package at all, which is what makes its
# PRIVATE links a REAL compile-time boundary rather than convention-plus-grep (R12).
add_library(aero_assets STATIC
    src/cooked_audio.cpp
)
add_library(aero::assets ALIAS aero_assets)

target_include_directories(aero_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

]==])
set(_AB_ASSETS_TLL "${_AB_TLL}(aero_assets\n    PUBLIC aero::core\n    PRIVATE aero::profiling\n)\n")
set(_AB_ASSETS_CMAKE "${_AB_ASSETS_CMAKE_HEAD}${_AB_ASSETS_TLL}")

set(_AB_AUDIO_CMAKE_HEAD [==[# engine/audio/ -- the audio layer (ADR-006).
#
# NO find_package. NOT ONE, EVER. aero_audio links no vcpkg package at all.
# ADDING A find_package TO THIS FILE VOIDS THAT SILENTLY WHILE CI STAYS GREEN.
add_library(aero_audio STATIC
    src/clip.cpp
)
add_library(aero::audio ALIAS aero_audio)

target_include_directories(aero_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

]==])
set(_AB_AUDIO_TLL "${_AB_TLL}(aero_audio\n    PUBLIC aero::core aero::assets\n    PRIVATE aero::profiling\n)\n")
set(_AB_AUDIO_CMAKE "${_AB_AUDIO_CMAKE_HEAD}${_AB_AUDIO_TLL}")

set(_AB_SCENE_AUDIO_CMAKE_HEAD [==[# engine/scene_audio/ -- the World -> audio bridge.
#
# NO find_package: this target links no vcpkg package directly either. NEVER aero::scene_internal --
# that target carries EnTT::EnTT INTERFACE by design.
add_library(aero_scene_audio STATIC
    src/scene_audio.cpp
)
add_library(aero::scene_audio ALIAS aero_scene_audio)

target_include_directories(aero_scene_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

]==])
set(_AB_SCENE_AUDIO_TLL "${_AB_TLL}(aero_scene_audio\n    PUBLIC aero::scene aero::audio\n    PRIVATE aero::profiling\n)\n")
set(_AB_SCENE_AUDIO_CMAKE "${_AB_SCENE_AUDIO_CMAKE_HEAD}${_AB_SCENE_AUDIO_TLL}")

set(_AB_AUDIO_HPP [==[#pragma once
// The audio umbrella header. Lifetime rule: stop the device before the mixer dies, because
// ma_device_uninit joins the audio thread and would otherwise run the callback against a dead mixer.
namespace engine::audio {}
]==])
set(_AB_CLIP_CPP [==[#include <aero/audio/audio.hpp>

namespace engine::audio {}
]==])
set(_AB_SCENE_AUDIO_HPP [==[#pragma once
namespace engine::scene_audio {}
]==])
set(_AB_SCENE_AUDIO_CPP [==[#include <aero/scene_audio/scene_audio.hpp>

namespace engine::scene_audio {}
]==])
set(_AB_COOKED_AUDIO_CPP [==[#include <aero/assets/cooked_audio.hpp>

namespace engine::assets {}
]==])

# _ab_base() -- restore the pristine scratch tree. Called before every stage so no stage can inherit
# a previous stage's seed: the whole engine/ subtree is removed and rewritten, which also undoes the
# DELETIONS the X-stages make. `.git` lives beside engine/, not inside it, so it survives.
function(_ab_base)
    file(REMOVE_RECURSE "${WORK_DIR}/src/engine")
    _ab_write("engine/CMakeLists.txt"                                    "${_AB_ENGINE_CMAKE}")
    _ab_write("engine/assets/CMakeLists.txt"                             "${_AB_ASSETS_CMAKE}")
    _ab_write("engine/assets/src/cooked_audio.cpp"                       "${_AB_COOKED_AUDIO_CPP}")
    _ab_write("engine/audio/CMakeLists.txt"                              "${_AB_AUDIO_CMAKE}")
    _ab_write("engine/audio/include/aero/audio/audio.hpp"                "${_AB_AUDIO_HPP}")
    _ab_write("engine/audio/src/clip.cpp"                                "${_AB_CLIP_CPP}")
    _ab_write("engine/scene_audio/CMakeLists.txt"                        "${_AB_SCENE_AUDIO_CMAKE}")
    _ab_write("engine/scene_audio/include/aero/scene_audio/scene_audio.hpp" "${_AB_SCENE_AUDIO_HPP}")
    _ab_write("engine/scene_audio/src/scene_audio.cpp"                   "${_AB_SCENE_AUDIO_CPP}")
    execute_process(COMMAND "${GIT}" -C "${WORK_DIR}/src" add -A)
endfunction()

# --- Scratch-tree bootstrap ------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
execute_process(COMMAND "${GIT}" init -q . WORKING_DIRECTORY "${WORK_DIR}/src")
_ab_base()

# --- S0: clean scratch tree -> exit 0. Silently proves all three self-test blocks pass too. --------
_ab_run("S0 (clean)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S0" "${_ab_out}" "audio-boundary guard: OK" TRUE)

# --- S1: a find_package hook enters engine/audio/CMakeLists.txt -> exit 1 (Part 1a). ---------------
# The named vector: the one sentence engine/audio/CMakeLists.txt's own header spends a paragraph on.
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}find_package(miniaudio CONFIG REQUIRED)\n")
_ab_run("S1 (find_package in engine/audio)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S1" "${_ab_out}" "engine/audio/CMakeLists.txt:" TRUE)
_ab_expect_substr("S1" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S2: the SAME command UPPER-CASE, in engine/assets -> exit 1. Two things at once: CMake command
# names are case-insensitive, and engine/assets is genuinely inside the roster (D2), not decoration.
_ab_base()
_ab_seed("engine/assets/CMakeLists.txt" "${_AB_ASSETS_CMAKE}FIND_PACKAGE(Tracy CONFIG REQUIRED)\n")
_ab_run("S2 (FIND_PACKAGE upper-case, engine/assets)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2" "${_ab_out}" "engine/assets/CMakeLists.txt:" TRUE)
_ab_expect_substr("S2" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S3: a non-aero:: token INSIDE THE MULTI-LINE TLL call -> exit 1 (Part 1b). The multi-line shape
# is the one all three files actually use, so a line-at-a-time check would miss it entirely. --------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE_HEAD}${_AB_TLL}(aero_audio\n    PUBLIC aero::core aero::assets\n    PRIVATE aero::profiling miniaudio\n)\n")
_ab_run("S3 (miniaudio token in a multi-line TLL)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S3" "${_ab_out}" "link token 'miniaudio' is not an aero:: engine target" TRUE)

# --- S4: aero::scene_internal on scene_audio's link line -> exit 1, with its OWN message. It matches
# the aero:: shape and is refused BY NAME: it carries EnTT::EnTT INTERFACE by design. ---------------
_ab_base()
_ab_seed("engine/scene_audio/CMakeLists.txt" "${_AB_SCENE_AUDIO_CMAKE_HEAD}${_AB_TLL}(aero_scene_audio\n    PUBLIC aero::scene aero::audio aero::scene_internal\n    PRIVATE aero::profiling\n)\n")
_ab_run("S4 (aero::scene_internal on a link line)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S4" "${_ab_out}" "carries EnTT::EnTT INTERFACE by design" TRUE)

# --- S5: an include directory reaching outside the subsystem -> exit 1 (Part 1c). This is the 3.7.1
# editor vector exactly: find_path + a SYSTEM PRIVATE include dir and NO link line at all, which
# reaches an external header with the word find_package never appearing. ---------------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}target_include_directories(aero_audio SYSTEM PRIVATE \${MINIAUDIO_INCLUDE_DIR})\n")
_ab_run("S5 (include dir outside the subsystem)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S5" "${_ab_out}" "reaches outside the subsystem" TRUE)

# --- S6: a CROSS-DIRECTORY target_link_libraries from engine/CMakeLists.txt -> exit 1 (Part 1d).
# CMake >= 3.13 permits this, and it voids the property from OUTSIDE the guarded files while all
# three of them stay byte-identical -- the hole no amount of reading the three files can close. -----
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}${_AB_TLL}(aero_audio PRIVATE miniaudio)\n")
_ab_run("S6 (cross-directory TLL)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6" "${_ab_out}" "cross-directory target_link_libraries on a vcpkg-free target" TRUE)

# --- S7: a ma_ identifier used as CODE in an audio src/ file -> exit 1 (Part 2). This is the half
# check-platform-boundary.sh does not reach: it stops at engine/*/include/*, and the shape
# engine/audio/CMakeLists.txt's header warns about happens in src/. --------------------------------
_ab_base()
_ab_seed("engine/audio/src/clip.cpp" "${_AB_CLIP_CPP}\nvoid leak(ma_device* d);\n")
_ab_run("S7 (ma_ identifier in an audio source)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S7" "${_ab_out}" "engine/audio/src/clip.cpp:" TRUE)
_ab_expect_substr("S7" "${_ab_out}" "miniaudio identifier used as code inside the audio layer" TRUE)

# --- S8: <miniaudio.h> under engine/scene_audio/ -> exit 1. LOAD-BEARING: this is the ONLY hermetic
# proof that the SECOND root is WALKED rather than merely NAMED. The include_scan stage-3 lesson --
# replacing SCAN_ROOTS=('engine' 'runtime') with ('engine') once left every stage green while the
# guard's own scanned count silently became a lie. Narrowing AUDIO_ROOTS to one entry MUST redden
# this stage, and the path assertion below is what makes it do so. --------------------------------
_ab_base()
_ab_seed("engine/scene_audio/src/scene_audio.cpp" "${_AB_SCENE_AUDIO_CPP}\n#include <miniaudio.h>\n")
_ab_run("S8 (miniaudio include under the SECOND root)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S8" "${_ab_out}" "engine/scene_audio/src/scene_audio.cpp:" TRUE)
_ab_expect_substr("S8" "${_ab_out}" "miniaudio header included inside the audio layer" TRUE)

# --- S9: the QUOTED include spelling in a public audio header -> exit 1. Both spellings, since a
# vendored copy beside the source resolves "miniaudio.h" just as well as <miniaudio.h>. The
# ma_device_uninit prose line survives the seed on purpose: without it this stage would exit 2 on
# the canary rather than 1 on the violation, and would prove nothing about the include arm. --------
_ab_base()
_ab_seed("engine/audio/include/aero/audio/audio.hpp" "${_AB_AUDIO_HPP}#include \"miniaudio.h\"\n")
_ab_run("S9 (quoted miniaudio include in a public header)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S9" "${_ab_out}" "engine/audio/include/aero/audio/audio.hpp:" TRUE)
_ab_expect_substr("S9" "${_ab_out}" "miniaudio header included inside the audio layer" TRUE)

# --- S10: ma_ named in `//` PROSE ONLY -> exit 0. The false-positive proof, and on macOS the only
# automated proof that the `nl | sed` comment-stripping pipelines behave under BSD userland. The real
# tree depends on this: three shipped audio headers cite ma_ names in first-party prose. -----------
_ab_base()
_ab_seed("engine/audio/src/clip.cpp" "${_AB_CLIP_CPP}\n// prose only: ma_engine and ma_sound are what this layer deliberately does NOT use.\n")
_ab_run("S10 (comment-only citation, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S10" "${_ab_out}" "audio-boundary guard: OK" TRUE)

# --- X1: a guarded CMakeLists loses its find_package prohibition comment -> exit 2. The canary is
# the comment itself: it is the in-tree proof that comment-stripping is doing real work, so losing it
# makes the guard unable to self-verify rather than free to pass. ----------------------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "# engine/audio/ -- the audio layer (ADR-006).\nadd_library(aero_audio STATIC\n    src/clip.cpp\n)\n\n${_AB_AUDIO_TLL}")
_ab_run("X1 (prohibition comment deleted)" 2 "${BASH}" "${SCRIPT}")
_ab_expect_substr("X1" "${_ab_out}" "the guard's comment-strip canary is gone" TRUE)

# --- X2: a guarded CMakeLists is untracked/renamed -> exit 2, not a silent pass over two files. ----
_ab_base()
_ab_unseed("engine/scene_audio/CMakeLists.txt")
_ab_run("X2 (guarded CMakeLists untracked)" 2 "${BASH}" "${SCRIPT}")
_ab_expect_substr("X2" "${_ab_out}" "is not a tracked file" TRUE)

# --- X3: a root loses every C-family source, KEEPING its CMakeLists -> exit 2. Per-root vacuity,
# counted over C-family files only precisely so the tracked CMakeLists cannot mask an emptied tree. -
_ab_base()
_ab_unseed("engine/scene_audio/include")
_ab_unseed("engine/scene_audio/src")
_ab_run("X3 (root emptied of C-family sources)" 2 "${BASH}" "${SCRIPT}")
_ab_expect_substr("X3" "${_ab_out}" "contains no tracked C-family sources" TRUE)

# --- X4: every raw ma_ citation reworded away -> exit 2. Regex-rot detection: with no real ma_ token
# left anywhere under the roots, a silently broken MA_IDENTIFIER_RE would be indistinguishable from a
# clean tree, so the guard refuses to pass instead. ------------------------------------------------
_ab_base()
_ab_seed("engine/audio/include/aero/audio/audio.hpp" "#pragma once\n// The audio umbrella header. Stop the device before the mixer dies.\nnamespace engine::audio {}\n")
_ab_run("X4 (every raw ma_ citation reworded away)" 2 "${BASH}" "${SCRIPT}")
_ab_expect_substr("X4" "${_ab_out}" "no raw ma_ citation found" TRUE)

# --- Restored -> exit 0. Proves every stage above was the seed talking, not accumulated damage. ----
_ab_base()
_ab_run("S0' (base restored)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S0'" "${_ab_out}" "audio-boundary guard: OK" TRUE)

message(STATUS "audio-boundary.guard_e2e: OK -- S0-S10 + X1-X4 + a restored-tree stage, 16 in all")
