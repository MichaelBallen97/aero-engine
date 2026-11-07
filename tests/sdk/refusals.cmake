# tests/sdk/refusals.cmake -- task 4.5.1: the AERO_SDK configuration refuses what an SDK cannot ship from.
# Runs cmake/sdk_check.cmake alone under `cmake -P`, once per combination, and asserts the exit code and the name of
# the variable the message blames (invoked: -DCHECK=<path to sdk_check.cmake> -P refusals.cmake).
#
# Ids: SDK1 is the good combination, SDK2 the six bad ones. The check always receives all five variables, because under
# -P an option never passed is unset and reads as OFF.
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED CHECK)
    message(FATAL_ERROR "refusals.cmake: -DCHECK=... is required")
endif()

# _sdk_run(<result-var> <output-var> <build-type> <profiling> <sanitizers> <shader-tools> <reflect-tools>)
function(_sdk_run result output buildType profiling sanitizers shaderTools reflectTools)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            "-DCMAKE_BUILD_TYPE=${buildType}"
            -DAERO_ENABLE_PROFILING=${profiling}
            -DAERO_ENABLE_SANITIZERS=${sanitizers}
            -DAERO_SHADER_TOOLS=${shaderTools}
            -DAERO_REFLECT_TOOLS=${reflectTools}
            -P ${CHECK}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(${result} "${_rc}" PARENT_SCOPE)
    set(${output} "${_out}${_err}" PARENT_SCOPE)
endfunction()

# SDK1: the good combination exits 0. Without it, a check that refuses everything would pass SDK2 alone.
_sdk_run(rc out Release OFF OFF ON ON)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "SDK1: the good combination was refused (exit ${rc}):\n${out}")
endif()

# SDK2: each bad combination exits non-zero AND names its own variable, so an arm that fails for an unrelated reason
# cannot pass for the one it is named after.
function(_sdk_expect_refused label needle buildType profiling sanitizers shaderTools reflectTools)
    _sdk_run(rc out "${buildType}" ${profiling} ${sanitizers} ${shaderTools} ${reflectTools})
    if(rc EQUAL 0)
        message(FATAL_ERROR "SDK2: ${label} was accepted")
    endif()
    string(FIND "${out}" "${needle}" idx)
    if(idx EQUAL -1)
        message(FATAL_ERROR "SDK2: ${label} was refused but the output does not name ${needle}:\n${out}")
    endif()
endfunction()

_sdk_expect_refused("Debug"                CMAKE_BUILD_TYPE       Debug   OFF OFF ON  ON)
_sdk_expect_refused("an empty build type"  CMAKE_BUILD_TYPE       ""      OFF OFF ON  ON)
_sdk_expect_refused("profiling ON"         AERO_ENABLE_PROFILING  Release ON  OFF ON  ON)
_sdk_expect_refused("sanitizers ON"        AERO_ENABLE_SANITIZERS Release OFF ON  ON  ON)
_sdk_expect_refused("shader tools OFF"     AERO_SHADER_TOOLS      Release OFF OFF OFF ON)
_sdk_expect_refused("reflect tools OFF"    AERO_REFLECT_TOOLS     Release OFF OFF ON  OFF)

message(STATUS "sdk.refusals: SDK1 and SDK2 passed")

# The package config's MSVC block sits above its find_dependency, so `-P` with MSVC=1 reaches it with no project and no
# prefix. _sdk_config_run runs it, tracing when asked: the trace goes to stderr, and is read together with stdout.
# CMake exits 1 on every run, accepted or not (the config stops at find_dependency(Threads) under -P), so the exit code
# decides nothing. An empty argument is an argument not passed.
get_filename_component(_sdk_cmake_dir "${CHECK}" DIRECTORY)
set(_sdk_config "${_sdk_cmake_dir}/AeroEngineConfig.cmake")

# _sdk_config_run(<out> <traced> <msvc> <build-type> <configuration-types> <runtime>)
function(_sdk_config_run out traced msvc buildType configurationTypes runtime)
    set(flags "")
    if(traced)
        set(flags --trace-expand --trace-format=json-v1)
    endif()
    set(defines "")
    if(msvc)
        list(APPEND defines -DMSVC=1)
    endif()
    if(NOT "${buildType}" STREQUAL "")
        list(APPEND defines "-DCMAKE_BUILD_TYPE=${buildType}")
    endif()
    if(NOT "${configurationTypes}" STREQUAL "")
        string(REPLACE ";" "\\;" escaped "${configurationTypes}")
        list(APPEND defines "-DCMAKE_CONFIGURATION_TYPES=${escaped}")
    endif()
    if(NOT "${runtime}" STREQUAL "")
        list(APPEND defines "-DCMAKE_MSVC_RUNTIME_LIBRARY=${runtime}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} ${flags} ${defines} -P ${_sdk_config}
        OUTPUT_VARIABLE _o
        ERROR_VARIABLE _e)
    set(${out} "${_o}${_e}" PARENT_SCOPE)
endfunction()

# A pin line holds the `set` of the runtime variable to the SDK's runtime. The text is split into lines with its own
# semicolons made into commas first, so no line is split in two.
function(_sdk_has_pin result output)
    set(${result} FALSE PARENT_SCOPE)
    set(_isSet "\"cmd\":\"set\"")
    set(_isPin "\"args\":\\[\"CMAKE_MSVC_RUNTIME_LIBRARY\",\"MultiThreadedDLL\"\\]")
    string(REPLACE ";" "," _text "${output}")
    string(REPLACE "\n" ";" _lines "${_text}")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "${_isSet}" AND _line MATCHES "${_isPin}")
            set(${result} TRUE PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

# A run is refused when its output holds the config's own prefix. A refused run is never traced: the trace repeats the
# message's tokens, and CMake re-wraps a FATAL_ERROR text, so a test looks for single tokens and never for phrases.
function(_sdk_is_refused result output)
    string(FIND "${output}" "AeroEngine:" _at)
    if(_at EQUAL -1)
        set(${result} FALSE PARENT_SCOPE)
    else()
        set(${result} TRUE PARENT_SCOPE)
    endif()
endfunction()

# _sdk_expect_pinned(<id> <label> <msvc> <build-type> <configuration-types>): the run pins the runtime and is not
# refused.
function(_sdk_expect_pinned id label msvc buildType configurationTypes)
    _sdk_config_run(out ON ${msvc} "${buildType}" "${configurationTypes}" "")
    _sdk_has_pin(pinned "${out}")
    _sdk_is_refused(refused "${out}")
    if(refused)
        message(FATAL_ERROR "${id}: ${label} was refused:\n${out}")
    endif()
    if(NOT pinned)
        message(FATAL_ERROR "${id}: ${label} never set CMAKE_MSVC_RUNTIME_LIBRARY to MultiThreadedDLL")
    endif()
endfunction()

# SDK3: a single-config MSVC run pins the runtime under Debug and under Release alike, and refuses neither.
_sdk_expect_pinned(SDK3 "a Debug build type" 1 Debug "")
_sdk_expect_pinned(SDK3 "a Release build type" 1 Release "")

# SDK28: a multi-config MSVC run sets no build type and still pins; a run outside MSVC pins nothing and is not refused.
_sdk_expect_pinned(SDK28 "Debug;Release configuration types with no build type" 1 "" "Debug;Release")
_sdk_config_run(out ON 0 "" "Debug;Release" "")
_sdk_has_pin(pinned "${out}")
_sdk_is_refused(refused "${out}")
if(pinned OR refused)
    message(FATAL_ERROR "SDK28: a run outside MSVC pinned (${pinned}) or was refused (${refused}):\n${out}")
endif()

# SDK29: a runtime the project chose is refused by name, never overwritten; the SDK's own runtime is accepted; outside
# MSVC nothing is read.
function(_sdk_expect_runtime label msvc runtime expectRefused)
    _sdk_config_run(out OFF ${msvc} "" "" "${runtime}")
    _sdk_is_refused(refused "${out}")
    if(NOT refused STREQUAL expectRefused)
        message(FATAL_ERROR "SDK29: ${label}: refused is ${refused}, not ${expectRefused}:\n${out}")
    endif()
    if(expectRefused)
        foreach(token IN ITEMS CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
            string(FIND "${out}" "${token}" at)
            if(at EQUAL -1)
                message(FATAL_ERROR "SDK29: ${label} was refused but the output does not name ${token}:\n${out}")
            endif()
        endforeach()
    endif()
endfunction()

_sdk_expect_runtime("a static runtime" 1 "MultiThreaded" TRUE)
_sdk_expect_runtime("a per-configuration runtime" 1 "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" TRUE)
_sdk_expect_runtime("the SDK's runtime" 1 "MultiThreadedDLL" FALSE)
_sdk_expect_runtime("a static runtime outside MSVC" 0 "MultiThreaded" FALSE)

message(STATUS "sdk.refusals: SDK3, SDK28 and SDK29 passed")
