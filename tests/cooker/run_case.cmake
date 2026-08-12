# tests/cooker/run_case.cmake — task 3.3.1's process-boundary harness for aero_cooker.
#
# Usage: cmake -DTOOL=<path to aero_cooker> -DCASE=<name> -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch
#              dir, unique per case> -P run_case.cmake
#
# WHY THIS EXISTS (rather than a plain `add_test(COMMAND aero_cooker ...)` per case): most cases here
# are NEGATIVE -- they expect aero_cooker itself to exit non-zero (1 for every grammar violation, 2
# for an import or cook error, 3 for an I/O error). A raw add_test treats ANY non-zero exit as a ctest
# FAILURE regardless of whether that was the expected behaviour. This driver runs the tool through
# execute_process, captures RESULT_VARIABLE, and only FATAL_ERRORs -- which is what ctest checks --
# when the actual exit code does not match the expected one for that case.
#
# CLONED from tests/shaderc/run_case.cmake rather than shared with it: the two tools have different
# artifact shapes, and one driver serving both would couple them for no gain.
cmake_minimum_required(VERSION 3.28)

foreach(required TOOL CASE SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_case.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Runs aero_cooker once. ASAN_OPTIONS is scoped to this one process, exactly as
# tests/shaderc/run_case.cmake scopes it and for the same reason: this is a one-shot process whose
# exit-time leaks are not what any case here is about, and aero_editor_core drags in four third-party
# parsers that leak on their own error paths (the two assimp frames tests/lsan.supp already names).
# UBSan and ASan memory-error detection stay live.
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

function(aero_expect_no_files)
    foreach(f IN LISTS ARGN)
        if(EXISTS "${f}")
            message(FATAL_ERROR "case '${CASE}': expected NO file at '${f}' but it exists")
        endif()
    endforeach()
endfunction()

function(aero_expect_contains text needle what)
    string(FIND "${text}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': ${what} does not contain '${needle}'")
    endif()
endfunction()

function(aero_expect_non_empty text what)
    if(text STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': expected non-empty ${what}, got nothing")
    endif()
endfunction()

function(aero_verify_no_files_in dir)
    file(GLOB leftover "${dir}/*")
    if(leftover)
        message(FATAL_ERROR "case '${CASE}': expected NO files under '${dir}', found: ${leftover}")
    endif()
endfunction()

# The paths every grammar case names. NOTHING is read or written by a case that fails the grammar --
# the parse runs to completion before the first byte is touched -- so these need not exist.
set(ANY_INPUT "${SOURCE_DIR}/tests/fixtures/assets/triangle.gltf")
set(OUT "${WORK_DIR}/out.aeromesh")

# --- the case table -------------------------------------------------------------------------------

if(CASE STREQUAL "help")
    aero_run_tool(ARGS --help OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    aero_expect_non_empty("${out}" "stdout")
    aero_expect_contains("${out}" "aero_cooker mesh --input" "the usage text")
    # stdout is reserved for --help/--version; diagnostics go to stderr only, so this one is silent.
    if(NOT err STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': --help wrote to stderr: ${err}")
    endif()

elseif(CASE STREQUAL "version")
    aero_run_tool(ARGS --version OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    aero_expect_contains("${out}" "aero_cooker" "stdout")

elseif(CASE STREQUAL "no_subcommand")
    aero_run_tool(ARGS --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_non_empty("${err}" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "unknown_subcommand")
    # 3.3.2 will make this one pass, deliberately: the grammar is subcommand-shaped from day one.
    aero_run_tool(ARGS texture --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "texture" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "unknown_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --frobnicate OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--frobnicate" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_input_flag")
    aero_run_tool(ARGS mesh --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--input" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_output_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--output" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_flag_value")
    # A value-taking flag at the very end of argv names itself rather than reading past the end.
    aero_run_tool(ARGS mesh --input OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "requires a value" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "repeated_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --input "${ANY_INPUT}" --output "${OUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "at most once" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "bad_guid")
    # Too short, and the dashed 36-character form: parseGuid takes EXACTLY 32 hex digits and nothing
    # else, so a dashed value is a usage error rather than a silently normalized success.
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --guid 0123 OUT_RESULT shortResult)
    aero_expect_exit("${shortResult}" 1)
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}"
        --guid 01234567-89ab-cdef-fedc-ba9876543210 OUT_RESULT dashedResult)
    aero_expect_exit("${dashedResult}" 1)
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "bad_scale")
    # Not a number; a trailing character the parse does not consume; and a non-finite value, which a
    # locale-classic istringstream reads happily and which would NaN every cooked position.
    foreach(bad abc 1.0x nan)
        aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --scale "${bad}" OUT_RESULT result)
        aero_expect_exit("${result}" 1)
    endforeach()
    aero_verify_no_files_in("${WORK_DIR}")

else()
    message(FATAL_ERROR "run_case.cmake: unknown CASE '${CASE}'")
endif()

message(STATUS "cooker ctest case '${CASE}': OK")
