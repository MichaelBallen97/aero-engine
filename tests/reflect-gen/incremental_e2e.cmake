cmake_minimum_required(VERSION 3.28)
foreach(required TOOL SOURCE_DIR WORK_DIR AERO_LLVM_ROOT ENTT_INCLUDE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "incremental_e2e.cmake: -D${required}=... is required")
    endif()
endforeach()

# Helper: run a command, capture combined output, FATAL on unexpected exit (dump output).
function(_e2e_run desc expect_zero)   # ARGN = the command
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_e2e_out "${_out}${_err}" PARENT_SCOPE)
    if(expect_zero AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "incremental_e2e: ${desc} failed (rc=${_rc}):\n${_out}\n${_err}")
    endif()
endfunction()

function(_e2e_expect_substr desc haystack needle present)   # present=TRUE => must contain
    string(FIND "${haystack}" "${needle}" _idx)
    if(present AND _idx EQUAL -1)
        message(FATAL_ERROR "incremental_e2e: ${desc}: expected output to contain '${needle}':\n${haystack}")
    elseif(NOT present AND NOT _idx EQUAL -1)
        message(FATAL_ERROR "incremental_e2e: ${desc}: output must NOT contain '${needle}':\n${haystack}")
    endif()
endfunction()

# 1. Hermetic copy: repo tree is NEVER touched (AC-5).
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
file(COPY "${SOURCE_DIR}/tests/reflect-gen/incremental/" DESTINATION "${WORK_DIR}/src")
file(COPY "${SOURCE_DIR}/tests/reflect-gen/fixtures/aero_reflect.hpp" DESTINATION "${WORK_DIR}/src/fixtures")

# 2. Configure the nested project against the REAL module + prebuilt tool.
_e2e_run("configure" TRUE "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DAERO_REFLECT_TOOLS=ON
    -DAERO_REPO_ROOT=${SOURCE_DIR}
    -DAERO_REFLECT_GEN_EXECUTABLE=${TOOL}
    -DAERO_LLVM_ROOT=${AERO_LLVM_ROOT}
    "-DAERO_ENTT_INCLUDE=${ENTT_INCLUDE}")

set(_gen "${WORK_DIR}/build/reflect-generated/reflect_probe")

# 3. Build 1 — generates.
_e2e_run("build 1" TRUE "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
_e2e_expect_substr("build 1" "${_e2e_out}" "reflect-gen: widget" TRUE)
foreach(f widget.meta.gen.cpp widget.meta.gen.cpp.d reflect_probe.aggregator.gen.cpp)
    if(NOT EXISTS "${_gen}/${f}")
        message(FATAL_ERROR "incremental_e2e: build 1 did not produce ${_gen}/${f}")
    endif()
endforeach()
file(READ "${_gen}/reflect_probe.aggregator.gen.cpp" _agg)
_e2e_expect_substr("aggregator" "${_agg}" "aero_reflect_register_widget" TRUE)         # AC-9
_e2e_expect_substr("aggregator" "${_agg}" "aero_reflect_register_all_reflect_probe" TRUE)

# 4. Build 2 — no-op (V6).
_e2e_run("build 2" TRUE "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
_e2e_expect_substr("build 2" "${_e2e_out}" "no work to do" TRUE)

# 5. Touch the component header -> regen.
execute_process(COMMAND "${CMAKE_COMMAND}" -E touch "${WORK_DIR}/src/fixtures/widget.hpp")
_e2e_run("build 3" TRUE "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
_e2e_expect_substr("build 3" "${_e2e_out}" "reflect-gen: widget" TRUE)

# 6. Touch the TRANSITIVE header -> regen (THE depfile proof).
execute_process(COMMAND "${CMAKE_COMMAND}" -E touch "${WORK_DIR}/src/fixtures/widget_types.hpp")
_e2e_run("build 4" TRUE "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
_e2e_expect_substr("build 4 (depfile)" "${_e2e_out}" "reflect-gen: widget" TRUE)

# 7. Touch an unrelated TU -> compile/link only, NO regen.
execute_process(COMMAND "${CMAKE_COMMAND}" -E touch "${WORK_DIR}/build/probe_main.cpp")
_e2e_run("build 5" TRUE "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
_e2e_expect_substr("build 5 (no over-trigger)" "${_e2e_out}" "reflect-gen: widget" FALSE)

# 8. The probe runs -> aggregator linked, called, returned.
_e2e_run("run probe" TRUE "${WORK_DIR}/build/reflect_probe")

message(STATUS "reflect-gen.incremental_e2e: OK")
