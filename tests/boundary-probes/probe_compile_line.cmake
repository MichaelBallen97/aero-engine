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
# GATED ON compile_commands.json EXISTING. CMAKE_EXPORT_COMPILE_COMMANDS is ON in every preset
# (CMakePresets.json), but multi-config generators (Visual Studio, Xcode) do not write the file at
# all, so on those lanes this case SKIPS rather than fails -- the same shape the NOT WIN32 e2e cases
# use, and for the same reason: no coverage of the invariant is lost, since the textual guards run in
# the lint job on every push regardless.

cmake_minimum_required(VERSION 3.28)
foreach(required DB)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "probe_compile_line.cmake: -D${required}=... is required")
    endif()
endforeach()

if(NOT EXISTS "${DB}")
    message(STATUS "boundary-probes.probe_compile_line: SKIPPED -- no compile_commands.json at "
                   "${DB} (a multi-config generator, or a configure that did not export it). The "
                   "textual guards still run in CI's lint job.")
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
                        "${DB}, expected at least 6. Either a probe stopped being built -- which is "
                        "itself the rot this case exists to catch -- or the database is stale. "
                        "Re-run the build, then re-run this case.")
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
