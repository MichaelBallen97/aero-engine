# tests/sdk/sdk_common.cmake -- task 4.5.1: the helpers every sdk.* driver shares. include()d, never run alone.
#
# A driver is a `cmake -P` script that receives the tree's own generator, compiler and build type as CONSUMER_*
# variables (tests/CMakeLists.txt, `_aero_sdk_args`), so a consumer is configured exactly the way the tree was and with
# no toolchain file: a consumer that needed vcpkg would prove nothing about a prefix that ships without it.

foreach(_sdk_required IN ITEMS SOURCE_DIR BUILD_DIR WORK_ROOT CONSUMER_GENERATOR CONSUMER_MAKE_PROGRAM
                               CONSUMER_CXX_COMPILER CONSUMER_BUILD_TYPE)
    if(NOT DEFINED ${_sdk_required})
        message(FATAL_ERROR "sdk driver: -D${_sdk_required}=... is required")
    endif()
endforeach()
unset(_sdk_required)

# sdk_fail(<id> <text>...): the failure names the claim that fell.
function(sdk_fail id)
    message(FATAL_ERROR "${id}: ${ARGN}")
endfunction()

# sdk_normalise_separators(<out> <text>): the text with `\` read as `/`, a run of `/` as one, and a `/` before a space
# dropped. A tool writes a path its own way -- CMake on Windows writes backslashes into compile_commands.json, doubled
# by JSON, and a shell-quoted space as `\ ` -- so a search for a path in tool output normalises the text and the path
# with this one function and only then looks. A path goes through file(TO_CMAKE_PATH) first.
function(sdk_normalise_separators out text)
    string(REPLACE "\\" "/" text "${text}")
    string(REGEX REPLACE "/+" "/" text "${text}")
    string(REPLACE "/ " " " text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# sdk_executable_path(<out> <binary dir> <target>)
function(sdk_executable_path out binary target)
    set(suffix "")
    if(CMAKE_HOST_WIN32)
        set(suffix ".exe")
    endif()
    set(${out} "${binary}/${target}${suffix}" PARENT_SCOPE)
endfunction()

# sdk_configure_consumer(SOURCE <dir> BINARY <dir> PREFIX <dir> [BUILD_TYPE <type>] [OUTPUT <var>] RESULT <var>)
# Removes BINARY, then configures SOURCE against PREFIX with the tree's generator and compiler. VCPKG_ROOT and
# CMAKE_TOOLCHAIN_FILE are unset in the child's environment and never passed.
function(sdk_configure_consumer)
    cmake_parse_arguments(ARG "" "SOURCE;BINARY;PREFIX;BUILD_TYPE;OUTPUT;RESULT" "" ${ARGN})
    if(NOT ARG_BUILD_TYPE)
        set(ARG_BUILD_TYPE "${CONSUMER_BUILD_TYPE}")
    endif()
    file(REMOVE_RECURSE "${ARG_BINARY}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env --unset=VCPKG_ROOT --unset=CMAKE_TOOLCHAIN_FILE
            ${CMAKE_COMMAND} -S "${ARG_SOURCE}" -B "${ARG_BINARY}" -G "${CONSUMER_GENERATOR}"
            "-DCMAKE_MAKE_PROGRAM=${CONSUMER_MAKE_PROGRAM}"
            "-DCMAKE_CXX_COMPILER=${CONSUMER_CXX_COMPILER}"
            "-DCMAKE_BUILD_TYPE=${ARG_BUILD_TYPE}"
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            "-DCMAKE_PREFIX_PATH=${ARG_PREFIX}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(${ARG_RESULT} "${rc}" PARENT_SCOPE)
    if(ARG_OUTPUT)
        set(${ARG_OUTPUT} "${out}${err}" PARENT_SCOPE)
    endif()
endfunction()

# sdk_build_consumer(BINARY <dir> [TARGET <target>] OUTPUT <var> RESULT <var>): always three jobs, never the machine's.
function(sdk_build_consumer)
    cmake_parse_arguments(ARG "" "BINARY;TARGET;OUTPUT;RESULT" "" ${ARGN})
    set(targetArgs "")
    if(ARG_TARGET)
        set(targetArgs --target "${ARG_TARGET}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${ARG_BINARY}" --parallel 3 ${targetArgs}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(${ARG_RESULT} "${rc}" PARENT_SCOPE)
    set(${ARG_OUTPUT} "${out}${err}" PARENT_SCOPE)
endfunction()

# sdk_run_game(EXE <path> WORKDIR <dir> [ARGS <arg>...] OUTPUT <var> RESULT <var>)
function(sdk_run_game)
    cmake_parse_arguments(ARG "" "EXE;WORKDIR;OUTPUT;RESULT" "ARGS" ${ARGN})
    file(MAKE_DIRECTORY "${ARG_WORKDIR}")
    execute_process(
        COMMAND "${ARG_EXE}" ${ARG_ARGS}
        WORKING_DIRECTORY "${ARG_WORKDIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(${ARG_RESULT} "${rc}" PARENT_SCOPE)
    set(${ARG_OUTPUT} "${out}${err}" PARENT_SCOPE)
endfunction()

# sdk_gpu_gate(<result> <output>): a game that exits 3 with `no GPU device` follows the tree's convention. With
# AERO_REQUIRE_GPU set it is a failure; without, a loud skip that leaves SDK_SKIPPED true for the caller.
macro(sdk_gpu_gate result output)
    set(SDK_SKIPPED FALSE)
    if("${${result}}" STREQUAL "3" AND "${${output}}" MATCHES "no GPU device")
        if(DEFINED ENV{AERO_REQUIRE_GPU} AND NOT "$ENV{AERO_REQUIRE_GPU}" STREQUAL "")
            message(FATAL_ERROR "sdk: no GPU device and AERO_REQUIRE_GPU is set")
        endif()
        message(STATUS "sdk: SKIPPED -- no GPU device")
        set(SDK_SKIPPED TRUE)
    endif()
endmacro()
