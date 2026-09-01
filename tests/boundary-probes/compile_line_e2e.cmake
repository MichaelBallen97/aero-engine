# tests/boundary-probes/compile_line_e2e.cmake — task 3.7.3, fifth code-review round.
#
# A hermetic red-proof for tests/boundary-probes/probe_compile_line.cmake, which had shipped with
# none. Both textual guards carry one; the case described as the thing that closes the
# directory-scope class did not, and §H of this task's plan is explicit that a proof living only in
# prose ceases to exist at merge.
#
# It feeds the driver SYNTHETIC compile_commands.json files rather than building anything: the
# property under test is "given this database, does the driver reach the right verdict", and a real
# build would make the case slow, order-dependent, and unable to exercise the shapes that matter
# (a malformed entry, an empty array, a database from a generator that emits `arguments` instead of
# `command`). Exit codes are asserted EXACTLY, including 77 for skip -- the status whose absence made
# the driver report Passed on a configuration it had not read.

cmake_minimum_required(VERSION 3.28)
foreach(required DRIVER WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "compile_line_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

function(_cl_run desc expect_rc db)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DDB=${db}" -P "${DRIVER}"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_cl_out "${_out}${_err}" PARENT_SCOPE)
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR "compile_line_e2e: ${desc}: expected exit ${expect_rc}, got ${_rc}:\nSTDOUT:\n${_out}\nSTDERR:\n${_err}")
    endif()
endfunction()

function(_cl_expect desc haystack needle)
    string(FIND "${haystack}" "${needle}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "compile_line_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    endif()
endfunction()

function(_cl_write name content)
    file(WRITE "${WORK_DIR}/${name}" "${content}")
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Six clean probe entries, the shape a real Ninja configure produces.
set(_clean [==[[
{"directory":"/b","file":"/r/tests/math_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/core/include -o m.o -c /r/tests/math_boundary_probe.cpp"},
{"directory":"/b","file":"/r/tests/jobs_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/core/include -o j.o -c /r/tests/jobs_boundary_probe.cpp"},
{"directory":"/b","file":"/r/tests/platform_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/platform/include -o p.o -c /r/tests/platform_boundary_probe.cpp"},
{"directory":"/b","file":"/r/tests/rhi_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/rhi/include -o r.o -c /r/tests/rhi_boundary_probe.cpp"},
{"directory":"/b","file":"/r/tests/scene_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/scene/include -o s.o -c /r/tests/scene_boundary_probe.cpp"},
{"directory":"/b","file":"/r/tests/audio_boundary_probe.cpp","command":"/usr/bin/c++ -I/r/engine/audio/include -o a.o -c /r/tests/audio_boundary_probe.cpp"},
{"directory":"/b","file":"/r/engine/audio/src/mixer.cpp","command":"/usr/bin/c++ -I/b/vcpkg_installed/arm64-osx/include -o x.o -c /r/engine/audio/src/mixer.cpp"}
]]==])

# --- C0: a clean database -> exit 0. The last entry is engine/audio/src/mixer.cpp WITH a vcpkg root
# on it, which is what a Release build legitimately looks like (this task's D4 measurement): the
# driver must filter to *_boundary_probe.cpp and ignore it, or it would red every Release build. ----
_cl_write("clean.json" "${_clean}")
_cl_run("C0 (clean, incl. a legitimately contaminated non-probe entry)" 0 "${WORK_DIR}/clean.json")
_cl_expect("C0" "${_cl_out}" "6 probe compile lines read")

# --- C1: manifest-mode vcpkg root on a probe -> exit 1, naming the probe. --------------------------
string(REPLACE "-I/r/engine/audio/include" "-I/b/vcpkg_installed/arm64-osx/include" _c1 "${_clean}")
_cl_write("c1.json" "${_c1}")
_cl_run("C1 (vcpkg_installed on a probe)" 1 "${WORK_DIR}/c1.json")
_cl_expect("C1" "${_cl_out}" "audio_boundary_probe.cpp")

# --- C2: CLASSIC-mode vcpkg root -> exit 1. Both layouts, because keying on `vcpkg_installed` alone
# let a seeded `-I/opt/vcpkg/installed/...` through during this driver's own bring-up. -------------
string(REPLACE "-I/r/engine/scene/include" "-I/opt/vcpkg/installed/arm64-osx/include" _c2 "${_clean}")
_cl_write("c2.json" "${_c2}")
_cl_run("C2 (classic-layout vcpkg/installed on a probe)" 1 "${WORK_DIR}/c2.json")
_cl_expect("C2" "${_cl_out}" "scene_boundary_probe.cpp")

# --- C3: the `arguments` array form, clean -> exit 0. Different generators emit `command` or
# `arguments`; reading only one would make this driver silently blind on the other. ---------------
_cl_write("c3.json" [==[[
{"directory":"/b","file":"/r/tests/math_boundary_probe.cpp","arguments":["/usr/bin/c++","-I/r/engine/core/include","-c","/r/tests/math_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/jobs_boundary_probe.cpp","arguments":["/usr/bin/c++","-I/r/engine/core/include","-c","/r/tests/jobs_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/platform_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/platform_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/rhi_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/rhi_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/scene_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/scene_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/audio_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/audio_boundary_probe.cpp"]}
]]==])
_cl_run("C3 (arguments form, clean)" 0 "${WORK_DIR}/c3.json")

# --- C4: the `arguments` form, contaminated -> exit 1. C3 alone would pass a driver that read the
# arguments form but never inspected it. ----------------------------------------------------------
_cl_write("c4.json" [==[[
{"directory":"/b","file":"/r/tests/math_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/math_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/jobs_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/jobs_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/platform_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/platform_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/rhi_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/rhi_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/scene_boundary_probe.cpp","arguments":["/usr/bin/c++","-c","/r/tests/scene_boundary_probe.cpp"]},
{"directory":"/b","file":"/r/tests/audio_boundary_probe.cpp","arguments":["/usr/bin/c++","-I/b/vcpkg_installed/arm64-osx/include","-c","/r/tests/audio_boundary_probe.cpp"]}
]]==])
_cl_run("C4 (arguments form, contaminated)" 1 "${WORK_DIR}/c4.json")
_cl_expect("C4" "${_cl_out}" "audio_boundary_probe.cpp")

# --- C5: too few probes -> exit 1. Anti-vacuity: a database with no probe entries would otherwise
# pass while reading nothing at all. The floor is a FLOOR, so a seventh probe needs no edit here. ---
_cl_write("c5.json" [==[[
{"directory":"/b","file":"/r/tests/math_boundary_probe.cpp","command":"/usr/bin/c++ -c /r/tests/math_boundary_probe.cpp"}
]]==])
_cl_run("C5 (fewer probes than the floor)" 1 "${WORK_DIR}/c5.json")
_cl_expect("C5" "${_cl_out}" "expected at least 6")

# --- C6: an entry with neither `command` nor `arguments` -> exit 1, not a silent skip of that entry.
_cl_write("c6.json" [==[[
{"directory":"/b","file":"/r/tests/math_boundary_probe.cpp","command":"/usr/bin/c++ -c a.cpp"},
{"directory":"/b","file":"/r/tests/jobs_boundary_probe.cpp","command":"/usr/bin/c++ -c b.cpp"},
{"directory":"/b","file":"/r/tests/platform_boundary_probe.cpp","command":"/usr/bin/c++ -c c.cpp"},
{"directory":"/b","file":"/r/tests/rhi_boundary_probe.cpp","command":"/usr/bin/c++ -c d.cpp"},
{"directory":"/b","file":"/r/tests/scene_boundary_probe.cpp","command":"/usr/bin/c++ -c e.cpp"},
{"directory":"/b","file":"/r/tests/audio_boundary_probe.cpp"}
]]==])
_cl_run("C6 (entry with no command and no arguments)" 1 "${WORK_DIR}/c6.json")
# A short needle on purpose: CMake wraps FATAL_ERROR text, so a longer one can be split
# mid-phrase -- which is how this stage first failed against a driver that was behaving.
_cl_expect("C6" "${_cl_out}" "entry 5")

# --- C7: an empty array -> exit 1. Distinct from C5: the database is readable but says nothing. ----
_cl_write("c7.json" "[]")
_cl_run("C7 (empty database)" 1 "${WORK_DIR}/c7.json")

# --- C8: no database at all -> exit 77, the SKIP status. THE stage this round exists for: the driver
# returned 0 here, so ctest printed Passed and the SKIPPED line went unread -- in two of the three
# gate configurations, because only the presets set CMAKE_EXPORT_COMPILE_COMMANDS. A skip that
# cannot be told apart from a pass is the vacuity this whole task exists to prevent. --------------
_cl_run("C8 (absent database -> skip, not pass)" 77 "${WORK_DIR}/does-not-exist.json")
_cl_expect("C8" "${_cl_out}" "SKIPPED")

message(STATUS "boundary-probes.compile_line_e2e: OK -- C0-C8, 9 stages")
