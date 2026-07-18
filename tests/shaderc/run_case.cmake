# tests/shaderc/run_case.cmake — task 0.4.3 net-new CTest harness (spec D9/§3.6, plan §C.3).
#
# Usage: cmake -DTOOL=<path to aero_shaderc> -DCASE=<name> -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch
#              dir, unique per case> -P run_case.cmake
#
# WHY THIS EXISTS (rather than a plain `add_test(COMMAND aero_shaderc ...)` per case): several cases
# are NEGATIVE -- they expect aero_shaderc itself to exit non-zero (bad_syntax=2, missing_input=3,
# bad_stage/unknown_flag/double_include=1). A raw add_test treats ANY non-zero exit as a ctest FAILURE
# regardless of whether that was the expected behavior. This driver runs the tool via execute_process,
# captures RESULT_VARIABLE, and only FATAL_ERRORs (making run_case.cmake ITSELF exit non-zero, which
# IS what ctest checks) when the actual exit code does not match the expected one for that case.
cmake_minimum_required(VERSION 3.28)

foreach(required TOOL CASE SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_case.cmake: -D${required}=... is required")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/verify_artifacts.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(SHADERS_DIR "${SOURCE_DIR}/shaders")
set(FIXTURES_DIR "${SOURCE_DIR}/tests/shaderc/fixtures")

# Runs aero_shaderc once. D8: ASAN_OPTIONS=detect_leaks=0 scoped to this one process, matching the
# build-time custom-command wrapping in cmake/shaders.cmake -- LLVM-family tools leak by design at
# exit; UBSan and ASan memory-error detection stay live in aero_shaderc's own code either way.
function(aero_run_tool)
    cmake_parse_arguments(RT "" "OUT_RESULT;OUT_STDOUT;OUT_STDERR" "ARGS" ${ARGN})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0 "${TOOL}" ${RT_ARGS}
        RESULT_VARIABLE _aero_result
        OUTPUT_VARIABLE _aero_stdout
        ERROR_VARIABLE _aero_stderr
    )
    set(${RT_OUT_RESULT} "${_aero_result}" PARENT_SCOPE)
    if(RT_OUT_STDOUT)
        set(${RT_OUT_STDOUT} "${_aero_stdout}" PARENT_SCOPE)
    endif()
    if(RT_OUT_STDERR)
        set(${RT_OUT_STDERR} "${_aero_stderr}" PARENT_SCOPE)
    endif()
endfunction()

function(aero_expect_exit actual expected)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "case '${CASE}': expected exit ${expected}, got ${actual}")
    endif()
endfunction()

function(aero_expect_files)
    foreach(f IN LISTS ARGN)
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "case '${CASE}': expected file '${f}' to exist")
        endif()
    endforeach()
endfunction()

function(aero_expect_no_files)
    foreach(f IN LISTS ARGN)
        if(EXISTS "${f}")
            message(FATAL_ERROR "case '${CASE}': expected NO file at '${f}' but it exists")
        endif()
    endforeach()
endfunction()

# --- the case table (spec §3.6) -------------------------------------------------------------------

if(CASE STREQUAL "help")
    aero_run_tool(ARGS --help OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    if(out STREQUAL "")
        message(FATAL_ERROR "case 'help': expected usage text on stdout, got nothing")
    endif()

elseif(CASE STREQUAL "vert_happy")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage vertex --output-dir "${WORK_DIR}"
        OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_verify_happy_set("${WORK_DIR}/triangle.vert" vertex 0 0 0 0)

elseif(CASE STREQUAL "frag_happy")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_verify_happy_set("${WORK_DIR}/triangle.frag" fragment 0 0 0 0)

elseif(CASE STREQUAL "reflect_ubo")
    aero_run_tool(ARGS --input "${FIXTURES_DIR}/uniform_color.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_verify_json_counts("${WORK_DIR}/uniform_color.frag.json" 0 0 0 1)

elseif(CASE STREQUAL "subset_msl")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage vertex --output-dir "${WORK_DIR}"
        --formats msl OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_files("${WORK_DIR}/triangle.vert.msl" "${WORK_DIR}/triangle.vert.json")
    aero_expect_no_files("${WORK_DIR}/triangle.vert.spv" "${WORK_DIR}/triangle.vert.dxil")
    aero_verify_contains("${WORK_DIR}/triangle.vert.json" "\"formats\": [\"msl\"]")

elseif(CASE STREQUAL "include_dir")
    aero_run_tool(ARGS --input "${FIXTURES_DIR}/with_include.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        --include "${FIXTURES_DIR}/inc" OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_files("${WORK_DIR}/with_include.frag.spv")

elseif(CASE STREQUAL "defines")
    aero_run_tool(ARGS --input "${FIXTURES_DIR}/define_gated.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        --define AERO_TEST_COLOR=1 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_files("${WORK_DIR}/define_gated.frag.spv")

elseif(CASE STREQUAL "bad_syntax")
    aero_run_tool(ARGS --input "${FIXTURES_DIR}/bad_syntax.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    if(err STREQUAL "")
        message(FATAL_ERROR "case 'bad_syntax': expected non-empty stderr")
    endif()
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_input")
    aero_run_tool(ARGS --input "${WORK_DIR}/does-not-exist.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        OUT_RESULT result)
    aero_expect_exit("${result}" 3)
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "bad_stage")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage geometry --output-dir "${WORK_DIR}"
        OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "unknown_flag")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage vertex --output-dir "${WORK_DIR}"
        --frobnicate OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "double_include")
    aero_run_tool(ARGS --input "${FIXTURES_DIR}/with_include.frag.hlsl" --stage fragment --output-dir "${WORK_DIR}"
        --include "${FIXTURES_DIR}/inc" --include "${FIXTURES_DIR}/inc" OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "determinism")
    set(dir1 "${WORK_DIR}/run1")
    set(dir2 "${WORK_DIR}/run2")
    file(MAKE_DIRECTORY "${dir1}")
    file(MAKE_DIRECTORY "${dir2}")
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage vertex --output-dir "${dir1}"
        OUT_RESULT result1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS --input "${SHADERS_DIR}/triangle.vert.hlsl" --stage vertex --output-dir "${dir2}"
        OUT_RESULT result2)
    aero_expect_exit("${result2}" 0)
    foreach(ext spv msl dxil json)
        aero_verify_files_identical("${dir1}/triangle.vert.${ext}" "${dir2}/triangle.vert.${ext}")
    endforeach()

else()
    message(FATAL_ERROR "run_case.cmake: unknown CASE '${CASE}'")
endif()

message(STATUS "shaderc ctest case '${CASE}': OK")
