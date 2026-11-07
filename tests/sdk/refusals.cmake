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
