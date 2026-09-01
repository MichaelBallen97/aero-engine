# tests/boundary-probes/probe_compile_line.cmake — task 3.7.3, fourth code-review round.
#
# THIS IS THE ONE CHECK THAT TESTS THE PROPERTY RATHER THAN A PROXY FOR IT. Every boundary probe's
# guarantee is exactly one sentence: **vcpkg's shared per-triplet include/ root is not on its compile
# line**, so the third-party backend behind its `aero::` library is genuinely unresolvable there
# (R12, docs/08-risks.md). Everything else in this task -- the naming allowlist, the directory-scope
# arms, the link-line shape checks -- is a TEXTUAL PREDICTION of that property from the CMake
# sources. This case reads the compile line itself.
#
# WHY IT EXISTS, stated plainly because it is a scope addition. Four review rounds ran the same way:
# a textual guard enumerated the ways a target could be reached, a round found one it had missed, the
# fix enumerated one more. The naming direction was closed by inverting to an allowlist -- a probe may
# be named by exactly two calls, so a spelling nobody has thought of is a violation on arrival. The
# OTHER direction, reaching a probe WITHOUT naming it, has no such inversion available: CMake pushes
# state into a directory scope through include_directories, link_libraries, link_directories,
# add_compile_options, add_definitions, add_compile_definitions, set_property(DIRECTORY ...),
# set_directory_properties, a CMAKE_<LANG>_FLAGS mutation, a toolchain file and a preset's cache
# variables, and the last two are not in any CMakeLists at all. `add_compile_options(-I/opt/vcpkg/...)`
# at the root was measured escaping every textual arm. Enumerating that list is the losing game; this
# case closes all of it at once, including the routes no textual guard can ever read.
#
# It is the light cousin of the `try_compile` negative harness (plan §L.3 handoff 1), which stays
# deferred: that one would prove <miniaudio.h> is unresolvable under each probe's exact flags. This
# one asserts the flags themselves are clean, which is the same property one step earlier and costs a
# JSON read instead of a nested configure per probe per configuration.
#
# WHAT THIS CASE DOES NOT DO, stated because the first version of the surrounding docs credited it
# with more. It detects ONE symptom: vcpkg's shared include root on a probe's compile line. It says
# nothing about EXCLUDE_FROM_ALL (an excluded target is still configured and still has an entry), and
# nothing at all about the audio guard's three vcpkg-free targets -- it filters to
# *_boundary_probe.cpp, and could not read those anyway, since their own compile lines legitimately
# carry the vcpkg root in Release through aero::profiling. Those halves are held by
# check-boundary-probes.sh and check-audio-boundary.sh respectively.
#
# WHEN compile_commands.json IS ABSENT THIS CASE SKIPS -- AND CTEST IS TOLD SO, via the SKIPPED line
# below and SKIP_REGULAR_EXPRESSION in tests/CMakeLists.txt. The first version simply returned 0, so
# ctest printed "Passed" and the SKIPPED line went unread. That matters more here than almost
# anywhere: CMAKE_EXPORT_COMPILE_COMMANDS is set by the PRESETS, and the two reduced gate
# configurations are raw `cmake -S . -B ...` invocations that do not set it -- so this case,
# described as the thing that closes the class, was passing vacuously in two of the three
# configurations the gate reads. "Skipped" is a reading; "Passed" there was a claim.
#
# It is NOT the same shape as the NOT WIN32 e2e cases, and the earlier comment saying so was wrong:
# those are gated at CONFIGURE time, so they never register at all and `ctest -N` shows the
# divergence. This one always registers and reports its own status at run time.

cmake_minimum_required(VERSION 3.28)
foreach(required DB)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "probe_compile_line.cmake: -D${required}=... is required")
    endif()
endforeach()

if(NOT EXISTS "${DB}")
    message(STATUS "boundary-probes.probe_compile_line: SKIPPED -- no compile_commands.json at "
                   "${DB}. Either the generator does not write one, or this configure did not set "
                   "CMAKE_EXPORT_COMPILE_COMMANDS. The textual guards still run in CI's lint job.")
    # Returning 0 and letting ctest classify the run from the SKIPPED line above, via
    # SKIP_REGULAR_EXPRESSION in tests/CMakeLists.txt. The previous version exited 77 through
    # cmake_language(EXIT), which needs CMake 3.29 -- above this project's declared floor of 3.28,
    # and above the 3.28.3 that Ubuntu 24.04 LTS ships. On the floor version the fallback branch
    # FATAL_ERROR'd, which made the red-proof compile_line_e2e fail UNCONDITIONALLY (its absent-DB
    # stage always takes this path) and made this case fail rather than skip in any configuration
    # that writes no database. CI never sees it, because the runners carry a newer CMake and nothing
    # pins one. SKIP_REGULAR_EXPRESSION has been available since 3.16 and needs no version gate.
    return()
endif()

file(READ "${DB}" _db)
string(JSON _n LENGTH "${_db}")
if(_n LESS 1)
    message(FATAL_ERROR "probe_compile_line: ${DB} holds no entries at all -- the database is "
                        "unusable and this case cannot self-verify.")
endif()

# The shared per-triplet include root vcpkg puts on the compile line of any target linking any vcpkg
# package. Matching the directory NAME rather than an absolute path keeps this working wherever the
# build tree lives -- and BOTH of vcpkg's layouts are matched: manifest mode installs into
# `<build>/vcpkg_installed/<triplet>/`, classic mode into `<vcpkg>/installed/<triplet>/`. Keying on
# only the first spelling let a seeded `-I/opt/vcpkg/installed/arm64-osx/include` through during this
# case's own bring-up, which is exactly the kind of near-miss it exists to catch.
set(_forbidden "vcpkg_installed|vcpkg/installed")

set(_checked 0)
set(_bad "")
math(EXPR _last "${_n} - 1")
foreach(_i RANGE ${_last})
    string(JSON _file GET "${_db}" ${_i} file)
    if(NOT _file MATCHES "_boundary_probe\\.cpp$")
        continue()
    endif()
    math(EXPR _checked "${_checked} + 1")

    # Either spelling of the entry: `command` (a single string) or `arguments` (an array). Both are
    # legal in the format and different generators emit different ones.
    set(_line "")
    string(JSON _cmd ERROR_VARIABLE _e GET "${_db}" ${_i} command)
    if(_cmd AND NOT _e)
        set(_line "${_cmd}")
    else()
        string(JSON _args ERROR_VARIABLE _e2 GET "${_db}" ${_i} arguments)
        if(_args AND NOT _e2)
            set(_line "${_args}")
        endif()
    endif()
    if(_line STREQUAL "")
        message(FATAL_ERROR "probe_compile_line: entry ${_i} (${_file}) has neither a `command` nor "
                            "an `arguments` field -- this case cannot read it, so it is refusing to "
                            "pass rather than skipping it silently.")
    endif()

    if(_line MATCHES "${_forbidden}")
        get_filename_component(_base "${_file}" NAME)
        set(_bad "${_bad}  ${_base}\n")
    endif()
endforeach()

# Anti-vacuity: finding no probe at all would pass silently and prove nothing. The count is a FLOOR,
# not an equality -- a future probe must not have to edit this file.
if(_checked LESS 6)
    message(FATAL_ERROR "probe_compile_line: found only ${_checked} *_boundary_probe.cpp entries in "
                        "${DB}, expected at least 6. Either a probe stopped being CONFIGURED -- its "
                        "add_library deleted or renamed -- or the database is stale. Re-run the "
                        "configure, then re-run this case.\n"
                        "NOTE, because the earlier wording here overstated it: this floor does NOT "
                        "detect EXCLUDE_FROM_ALL. A target excluded from `all` is still CONFIGURED, "
                        "so it still has an entry in compile_commands.json and the count is "
                        "unchanged -- measured, not assumed. The EXCLUDE_FROM_ALL half of the "
                        "invariant is held by check-boundary-probes.sh's own arms and by stages "
                        "P11/P15/P22/P23, not by this case.")
endif()

if(NOT _bad STREQUAL "")
    message(FATAL_ERROR
        "a boundary probe's COMPILE LINE carries vcpkg's shared include root -- task 3.7.3 / R12:\n"
        "${_bad}"
        "\n"
        "Each of those probes now compiles with vcpkg_installed/<triplet>/include on its command "
        "line, so the third-party backend behind its aero:: library resolves there and the probe "
        "asserts nothing. Something put it back: a second link library, a target property, or -- "
        "the reason this case exists rather than a grep -- a DIRECTORY-scoped command "
        "(include_directories, add_compile_options, add_definitions, set_property(DIRECTORY ...)) "
        "in an ancestor CMakeLists, a toolchain file, or a preset's flags. Read "
        "${DB} for the offending entry's full command line.")
endif()

message(STATUS "boundary-probes.probe_compile_line: OK -- ${_checked} probe compile lines read from "
               "${DB}, none carrying vcpkg's shared include root")
