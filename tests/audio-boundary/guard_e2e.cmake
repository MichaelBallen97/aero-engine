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
# The PROPERTY command names need the same treatment, for the same reason one arm over: Part 1d
# now also sweeps set_target_properties / set_property(TARGET ...) calls naming a guarded target,
# so a literal in a fixture below reddens the tree that ships it.
set(_AB_STP "set_target_properties")
set(_AB_SP "set_property")

# THE GUARDED TARGET NAMES ARE COMPOSED TOO, and this is the durable form of a collision that has
# now appeared three times. Part 1d no longer asks "which commands name a guarded target" -- it asks
# whether the NAME appears at all outside its own CMakeLists, because zero legitimate references
# exist. So any literal aero_assets / aero_audio / aero_scene_audio in this file's fixture code
# reddens the tree that ships it, whatever command it sits in. Composing the COMMAND names (as
# _AB_TLL/_AB_STP/_AB_SP above still do, for the arms that are command-scoped) fixes one round's
# collision; composing the TARGET names fixes the class, because it survives any future widening of
# what the sweep looks for.
# ...and even these definitions are assembled from parts, because the sweep's predicate is now the
# bare NAME. Spelled whole here, the three lines below would be the only thing in the file still
# reddening it -- which is a small enough tail to be tempting to carve out, and carving out is what
# the exclusion-list argument has lost three times.
set(_AB_N_ASSETS "assets")
set(_AB_N_AUDIO "audio")
set(_AB_N_SA "scene_audio")
set(_AB_T_ASSETS "aero_${_AB_N_ASSETS}")
set(_AB_T_AUDIO "aero_${_AB_N_AUDIO}")
set(_AB_T_SA "aero_${_AB_N_SA}")

set(_AB_ENGINE_CMAKE "add_subdirectory(assets)\nadd_subdirectory(audio)\nadd_subdirectory(scene_audio)\n")

set(_AB_ASSETS_CMAKE_HEAD "# engine/assets/ -- the cooked-asset formats.\n#\n# NO find_package. NOT ONE, EVER. ${_AB_T_ASSETS} links no vcpkg package at all, which is what makes\n# its PRIVATE links a REAL compile-time boundary rather than convention-plus-grep (R12).\nadd_library(${_AB_T_ASSETS} STATIC\n    src/cooked_audio.cpp\n)\nadd_library(aero::assets ALIAS ${_AB_T_ASSETS})\n\ntarget_include_directories(${_AB_T_ASSETS} PUBLIC \${CMAKE_CURRENT_SOURCE_DIR}/include)\n\n")
set(_AB_ASSETS_TLL "${_AB_TLL}(${_AB_T_ASSETS}\n    PUBLIC aero::core\n    PRIVATE aero::profiling\n)\n")
set(_AB_ASSETS_CMAKE "${_AB_ASSETS_CMAKE_HEAD}${_AB_ASSETS_TLL}")

set(_AB_AUDIO_CMAKE_HEAD "# engine/audio/ -- the audio layer (ADR-006).\n#\n# NO find_package. NOT ONE, EVER. ${_AB_T_AUDIO} links no vcpkg package at all.\n# ADDING A find_package TO THIS FILE VOIDS THAT SILENTLY WHILE CI STAYS GREEN.\nadd_library(${_AB_T_AUDIO} STATIC\n    src/clip.cpp\n)\nadd_library(aero::audio ALIAS ${_AB_T_AUDIO})\n\ntarget_include_directories(${_AB_T_AUDIO} PUBLIC \${CMAKE_CURRENT_SOURCE_DIR}/include)\n\n")
set(_AB_AUDIO_TLL "${_AB_TLL}(${_AB_T_AUDIO}\n    PUBLIC aero::core aero::assets\n    PRIVATE aero::profiling\n)\n")
set(_AB_AUDIO_CMAKE "${_AB_AUDIO_CMAKE_HEAD}${_AB_AUDIO_TLL}")

set(_AB_SCENE_AUDIO_CMAKE_HEAD "# engine/scene_audio/ -- the World -> audio bridge.\n#\n# NO find_package: this target links no vcpkg package directly either. NEVER aero::scene_internal --\n# that target carries EnTT::EnTT INTERFACE by design.\nadd_library(${_AB_T_SA} STATIC\n    src/scene_audio.cpp\n)\nadd_library(aero::scene_audio ALIAS ${_AB_T_SA})\n\ntarget_include_directories(${_AB_T_SA} PUBLIC \${CMAKE_CURRENT_SOURCE_DIR}/include)\n\n")
set(_AB_SCENE_AUDIO_TLL "${_AB_TLL}(${_AB_T_SA}\n    PUBLIC aero::scene aero::audio\n    PRIVATE aero::profiling\n)\n")
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
    # tools/ is not part of the base tree; S6i creates it and X5's vacuity check depends on the
    # sweep having nothing left to walk once engine/CMakeLists.txt goes.
    file(REMOVE_RECURSE "${WORK_DIR}/src/tools")
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

# --- S2b: find_path, the OTHER named vector -> exit 1. §B.1 singles this member out because the tree
# itself proves it: 3.7.1 wired the editor's decode headers with exactly find_path + a SYSTEM PRIVATE
# include dir and NO link line, so the identical edit here would void the property with the word
# find_package never appearing. Until this stage existed, deleting find_path|find_library|find_file|
# find_program|pkg_check_modules|pkg_search_module from the alternation left every stage green --
# S1/S2 pin find_package and nothing pinned the rest of the list. ---------------------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)\n")
_ab_run("S2b (find_path in engine/audio)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2b" "${_ab_out}" "engine/audio/CMakeLists.txt:" TRUE)
_ab_expect_substr("S2b" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S2c: add_subdirectory -> exit 1. The directory-scoped half of the banned list, and the member
# whose boundary character the leading (^|[^a-zA-Z0-9_]) protects. ---------------------------------
_ab_base()
_ab_seed("engine/scene_audio/CMakeLists.txt" "${_AB_SCENE_AUDIO_CMAKE}add_subdirectory(vendor)\n")
_ab_run("S2c (add_subdirectory in engine/scene_audio)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2c" "${_ab_out}" "engine/scene_audio/CMakeLists.txt:" TRUE)
_ab_expect_substr("S2c" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S2d: include() -> exit 1. THE FLAGSHIP BYPASS, and the one that made every other arm of prong A
# optional: Parts 1a/1b/1c read exactly the three rostered files and Part 1d looks only for
# target_link_libraries, so NOTHING followed an include(). Two lines -- this one, plus a find_package
# and a foreign include dir in the included file -- voided the entire prong while the guard printed
# its OK banner. include(cmake/*.cmake) is a live idiom in this tree, so the vector has in-tree
# precedent, which is the bar every other member of the banned list is held to. --------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}include(\${CMAKE_CURRENT_SOURCE_DIR}/audio_deps.cmake)\n")
_ab_run("S2d (include() in engine/audio)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2d" "${_ab_out}" "engine/audio/CMakeLists.txt:" TRUE)
_ab_expect_substr("S2d" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S2e: the PROPERTY spelling of the link line, inside a guarded file -> exit 1. LINK_LIBRARIES is
# what target_link_libraries writes, so set_property reaches it with none of the words Part 1b looks
# for; measured passing before this arm existed. Both property commands are banned outright rather
# than one property at a time -- the general form of "match the predicate, not the spelling". -------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}${_AB_SP}(TARGET ${_AB_T_AUDIO} APPEND PROPERTY LINK_LIBRARIES miniaudio)\n")
_ab_run("S2e (set_property LINK_LIBRARIES in engine/audio)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2e" "${_ab_out}" "engine/audio/CMakeLists.txt:" TRUE)
_ab_expect_substr("S2e" "${_ab_out}" "a vcpkg/dependency hook command entered a vcpkg-free CMakeLists" TRUE)

# --- S3: a non-aero:: token INSIDE THE MULTI-LINE TLL call -> exit 1 (Part 1b). The multi-line shape
# is the one all three files actually use, so a line-at-a-time check would miss it entirely. --------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE_HEAD}${_AB_TLL}(${_AB_T_AUDIO}\n    PUBLIC aero::core aero::assets\n    PRIVATE aero::profiling miniaudio\n)\n")
_ab_run("S3 (miniaudio token in a multi-line TLL)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S3" "${_ab_out}" "link token 'miniaudio' is not an aero:: engine target" TRUE)

# --- S4: an *_internal target on scene_audio's link line -> exit 1, with its OWN message. It matches
# the aero:: shape and is refused BY NAME: it carries its backend INTERFACE by design. --------------
_ab_base()
_ab_seed("engine/scene_audio/CMakeLists.txt" "${_AB_SCENE_AUDIO_CMAKE_HEAD}${_AB_TLL}(${_AB_T_SA}\n    PUBLIC aero::scene aero::audio aero::scene_internal\n    PRIVATE aero::profiling\n)\n")
_ab_run("S4 (aero::scene_internal on a link line)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S4" "${_ab_out}" "'aero::scene_internal' on a link line" TRUE)
_ab_expect_substr("S4" "${_ab_out}" "an *_internal target carries its backend INTERFACE by design" TRUE)

# --- S4b: the SAME target by its RAW CMake name -> exit 1. The alias is not the only spelling, and
# for a while it was the only one refused: aero_scene_internal / aero_platform_internal /
# aero_rhi_internal all exist in this tree and all match LINK_TOKEN_RE's aero_[a-z_]+ branch, so they
# passed with the OK banner. The raw name is what anyone copying from engine/scene/CMakeLists.txt
# would write, which makes it the LIKELIER spelling, not the exotic one. --------------------------
_ab_base()
_ab_seed("engine/scene_audio/CMakeLists.txt" "${_AB_SCENE_AUDIO_CMAKE_HEAD}${_AB_TLL}(${_AB_T_SA}\n    PUBLIC aero::scene aero::audio aero_scene_internal\n    PRIVATE aero::profiling\n)\n")
_ab_run("S4b (RAW aero_scene_internal on a link line)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S4b" "${_ab_out}" "'aero_scene_internal' on a link line" TRUE)
_ab_expect_substr("S4b" "${_ab_out}" "an *_internal target carries its backend INTERFACE by design" TRUE)

# --- S5: an include directory reaching outside the subsystem -> exit 1 (Part 1c). This is the 3.7.1
# editor vector exactly: find_path + a SYSTEM PRIVATE include dir and NO link line at all, which
# reaches an external header with the word find_package never appearing.
#
# THE ASSERTION NAMES THE VARIABLE, NOT JUST THE ARM, AND THAT IS THE WHOLE POINT OF THIS STAGE. The
# seed also contains the keyword SYSTEM; while SYSTEM was missing from Part 1c's allowlist it
# produced a violation of its own, so a generic "reaches outside the subsystem" assertion was
# satisfied by the KEYWORD and said nothing at all about the PATH. Measured: replacing the allowlist's
# ${CMAKE_CURRENT_SOURCE_DIR} arm with a bare ${ -- i.e. admitting any variable-rooted include dir,
# the exact rot this arm exists to prevent -- left all sixteen stages green. Pin the token.
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}target_include_directories(${_AB_T_AUDIO} SYSTEM PRIVATE \${MINIAUDIO_INCLUDE_DIR})\n")
_ab_run("S5 (include dir outside the subsystem)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S5" "${_ab_out}" "include dir '\${MINIAUDIO_INCLUDE_DIR}' reaches outside the subsystem" TRUE)
# ...and SYSTEM, a legal keyword, must NOT be reported as a path. Both halves in one stage: the arm
# has to fire on the real offender and stay silent on the keyword beside it.
_ab_expect_substr("S5" "${_ab_out}" "include dir 'SYSTEM'" FALSE)

# --- S5b: a wholly LEGAL target_include_directories carrying SYSTEM -> exit 0. The false-positive
# half of S5: SYSTEM/BEFORE/AFTER are keywords, and reporting one as an include path is a guard
# telling a true story about the wrong token. -----------------------------------------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE_HEAD}${_AB_TLL}(${_AB_T_AUDIO}\n    PUBLIC aero::core aero::assets\n    PRIVATE aero::profiling\n)\ntarget_include_directories(${_AB_T_AUDIO} SYSTEM PUBLIC \${CMAKE_CURRENT_SOURCE_DIR}/include)\n")
_ab_run("S5b (legal SYSTEM keyword, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S5b" "${_ab_out}" "audio-boundary guard: OK" TRUE)

# --- S6: a CROSS-DIRECTORY target_link_libraries from engine/CMakeLists.txt -> exit 1 (Part 1d).
# CMake >= 3.13 permits this, and it voids the property from OUTSIDE the guarded files while all
# three of them stay byte-identical -- the hole no amount of reading the three files can close. -----
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}${_AB_TLL}(${_AB_T_AUDIO} PRIVATE miniaudio)\n")
_ab_run("S6 (cross-directory TLL)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6b: the same link, WRAPPED so the target is not on the same physical line as the command ->
# exit 1. A line-scoped grep sees `target_link_libraries(` with nothing after the paren and finds no
# target at all, which is exactly how this form passed before the sweep was made to flatten like
# Parts 1b and 1c already did. No self-test can pin this: extract_calls flattens its own input, so
# the helper looks correct in isolation -- only a stage proves the SWEEP uses it. -------------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}${_AB_TLL}(\n    ${_AB_T_AUDIO}\n    PRIVATE miniaudio\n)\n")
_ab_run("S6b (WRAPPED cross-directory TLL)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6b" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6b" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6c: the same link written into a scratch `*.cmake` -> exit 1. Part 1d's pathspec has THREE
# arms ('CMakeLists.txt', '*/CMakeLists.txt', '*.cmake') and S6/S6b exercise only the second. This is
# prong B's own "a root that contributes no seed is a root nothing proves is walked" applied to the
# sweep: dropping '*.cmake' silently removed every .cmake file from the swept set -- including both of
# this task's own e2e drivers -- and left every stage green. -----------------------------------------
_ab_base()
_ab_seed("engine/audio_deps.cmake" "${_AB_TLL}(${_AB_T_AUDIO} PRIVATE miniaudio)\n")
_ab_run("S6c (cross-directory TLL in a *.cmake file)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6c" "${_ab_out}" "engine/audio_deps.cmake:" TRUE)
_ab_expect_substr("S6c" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6d: the PROPERTY spelling of the cross-directory mutation, WRAPPED -> exit 1. Part 1d's other
# half: set_target_properties / set_property(TARGET …) reach LINK_LIBRARIES and INCLUDE_DIRECTORIES
# from any file in the tree, so a sweep that reads only target_link_libraries watches one of two
# doors. Wrapped, because the extractor's alternation has to be parenthesised for that to work --
# unparenthesised it bound as `(^|[^..])set_target_properties` OR `set_property\(…\)` and dropped
# every wrapped call, which is how this arm first shipped broken. ----------------------------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}${_AB_STP}(\n    ${_AB_T_SA}\n    PROPERTIES LINK_LIBRARIES miniaudio\n)\n")
_ab_run("S6d (WRAPPED cross-directory property write)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6d" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6d" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6f: a cross-directory target_include_directories -> exit 1. THE third round's blocking
# finding for this guard, and the reason Part 1d is now "the name may not appear at all" rather than
# a list of commands: the property spelling of this write was refused while the PLAIN one was not,
# in the same file, on the same target. It is the 3.7.1 editor find_path vector relocated one
# directory up -- two lines in a file the sweep already walked. -----------------------------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)\ntarget_include_directories(${_AB_T_AUDIO} SYSTEM PRIVATE \${MINIAUDIO_INCLUDE_DIR})\n")
_ab_run("S6f (cross-directory target_include_directories)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6f" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6f" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6g: a command NOBODY has enumerated, naming a guarded target from another file -> exit 1.
# This is the stage that distinguishes an allowlist from a longer denylist: target_compile_options
# was never on any banned list, in any round, and needs no arm of its own. If this stage ever has to
# be written a second time for a different command, the inversion has been undone. ----------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}target_compile_options(${_AB_T_SA} PRIVATE -I/opt/vcpkg/include)\n")
_ab_run("S6g (an un-enumerated command naming a guarded target)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6g" "${_ab_out}" "is named outside its own CMakeLists" TRUE)

# --- S6h: a directory-scoped include_directories in an ANCESTOR -> exit 1. The one class the
# name-based sweep structurally cannot see: CMake's directory properties are inherited, so this puts
# vcpkg's root on every target defined below it while naming none of them. Closed by an ancestor
# check derived from the roster rather than left as documented residual, because no ancestor of the
# three guarded directories uses these commands today. -------------------------------------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "include_directories(/opt/vcpkg/installed/arm64-osx/include)\n${_AB_ENGINE_CMAKE}")
_ab_run("S6h (directory-scoped include_directories in an ancestor)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6h" "${_ab_out}" "engine/CMakeLists.txt:" TRUE)
_ab_expect_substr("S6h" "${_ab_out}" "reaches it without naming it" TRUE)

# --- S6i: the SAME command in a file that is NOT an ancestor -> exit 0. The ancestor check is
# scoped, not a repo-wide ban: include_directories elsewhere in the tree is ordinary CMake and
# reaches nothing guarded. ------------------------------------------------------------------------
_ab_base()
_ab_seed("tools/CMakeLists.txt" "include_directories(/opt/vcpkg/installed/arm64-osx/include)\n")
_ab_run("S6i (the same command in a NON-ancestor, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6i" "${_ab_out}" "audio-boundary guard: OK" TRUE)

# --- S2f: a command outside the guarded-file allowlist, INSIDE a guarded file -> exit 1. The
# in-file half of the same inversion: set_source_files_properties names no target at all, so no
# target-scoped arm could ever have seen it, and it reaches an external header through the source. -
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "${_AB_AUDIO_CMAKE}set_source_files_properties(src/clip.cpp PROPERTIES INCLUDE_DIRECTORIES /opt/vcpkg/include)\n")
_ab_run("S2f (a command outside the guarded-file allowlist)" 1 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S2f" "${_ab_out}" "'set_source_files_properties' is not one of the three commands" TRUE)

# --- S12: a tracked-then-deleted .cpp UNDER AN AUDIO ROOT -> exit 0 with no shell noise. S11 seeds a
# .cmake, which Part 2 never walks, so the [ -f ] guard on the SOURCE loops was covered by no stage
# at all -- deleting it left every stage green. Two loops read sources by name (self-test 2's raw-ma_
# canary and Part 2 itself) and this is what pins both. ------------------------------------------
_ab_base()
_ab_seed("engine/audio/src/gone.cpp" "namespace engine::audio {}\n")
file(REMOVE "${WORK_DIR}/src/engine/audio/src/gone.cpp")   # staged, then gone: NOT re-added
_ab_run("S12 (tracked-but-deleted source under an audio root)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S12" "${_ab_out}" "audio-boundary guard: OK" TRUE)
_ab_expect_substr("S12" "${_ab_out}" "No such file or directory" FALSE)

# --- S6e: a set_property on a NON-TARGET scope, elsewhere in the tree -> exit 0. The false-positive
# half of S6d: `set_property(GLOBAL …)` and a property write on some other target are ordinary CMake
# and must stay silent, or the arm would red the tree on its first legitimate use. -----------------
_ab_base()
_ab_seed("engine/CMakeLists.txt" "${_AB_ENGINE_CMAKE}${_AB_SP}(GLOBAL PROPERTY USE_FOLDERS ON)\n${_AB_STP}(aero_render PROPERTIES FOLDER engine)\n")
_ab_run("S6e (non-target scope + another target, false-positive proof)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S6e" "${_ab_out}" "audio-boundary guard: OK" TRUE)

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

# --- S11: a file that is TRACKED but ABSENT -> exit 0, not a crash. `git add` then `rm` is the
# ordinary middle of a rename, and the sweep reads every tracked CMake path by name: the redirect
# failed, `set -e` fired, and the guard exited 1 printing a shell error and NO verdict at all --
# outside the 0/1/2 contract entirely, on a routine working state (3.7.3's second review round). --
_ab_base()
_ab_seed("engine/extra.cmake" "set(AERO_UNUSED 1)\n")
file(REMOVE "${WORK_DIR}/src/engine/extra.cmake")   # staged, then gone: deliberately NOT re-added
_ab_run("S11 (tracked-but-deleted CMake file)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S11" "${_ab_out}" "audio-boundary guard: OK" TRUE)
_ab_expect_substr("S11" "${_ab_out}" "No such file or directory" FALSE)

# --- X1: a guarded CMakeLists loses its find_package prohibition comment -> exit 2. The canary is
# the comment itself: it is the in-tree proof that comment-stripping is doing real work, so losing it
# makes the guard unable to self-verify rather than free to pass. ----------------------------------
_ab_base()
_ab_seed("engine/audio/CMakeLists.txt" "# engine/audio/ -- the audio layer (ADR-006).\nadd_library(${_AB_T_AUDIO} STATIC\n    src/clip.cpp\n)\n\n${_AB_AUDIO_TLL}")
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

# --- X5: the cross-directory sweep walks ZERO other CMake files -> exit 2. Anti-vacuity, per
# .claude/rules/boundary-guards.md: a sweep that traversed nothing proves nothing, and would print
# the OK banner all the same. Removing engine/CMakeLists.txt leaves only the three guarded files,
# which the sweep skips by construction -- so `swept` is 0 and the guard must refuse rather than
# pass. This is the same species as X3's per-root vacuity check, one prong over. ------------------
_ab_base()
_ab_unseed("engine/CMakeLists.txt")
_ab_run("X5 (sweep walked zero other CMake files)" 2 "${BASH}" "${SCRIPT}")
_ab_expect_substr("X5" "${_ab_out}" "walked ZERO other CMake files" TRUE)

# --- Restored -> exit 0. Proves every stage above was the seed talking, not accumulated damage. ----
_ab_base()
_ab_run("S0' (base restored)" 0 "${BASH}" "${SCRIPT}")
_ab_expect_substr("S0'" "${_ab_out}" "audio-boundary guard: OK" TRUE)

message(STATUS "audio-boundary.guard_e2e: OK -- S0-S11 (with S2b-S2e, S4b, S5b, S6b-S6e) + X1-X5 "
               "+ a restored-tree stage, 34 in all")
