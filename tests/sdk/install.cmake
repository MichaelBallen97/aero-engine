# tests/sdk/install.cmake -- task 4.5.1: the fixture that owns the prefix. Every run removes it and installs the build
# tree into it afresh, so a header dropped from the install set cannot survive in a stale prefix.
# Invoked: -DSOURCE_DIR -DBUILD_DIR -DWORK_ROOT ... -P install.cmake (tests/CMakeLists.txt, `_aero_sdk_args`).
#
# SDK4: the old prefix is gone, `cmake --install` exits 0, and the package config exists.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/sdk_common.cmake")

set(prefix "${WORK_ROOT}/prefix")
file(REMOVE_RECURSE "${prefix}")
if(EXISTS "${prefix}")
    sdk_fail(SDK4 "the previous prefix could not be removed: ${prefix}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${prefix}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    sdk_fail(SDK4 "cmake --install exited ${rc}:\n${out}${err}")
endif()
if(NOT EXISTS "${prefix}/lib/cmake/AeroEngine/AeroEngineConfig.cmake")
    sdk_fail(SDK4 "the install left no AeroEngineConfig.cmake under ${prefix}/lib/cmake/AeroEngine")
endif()

message(STATUS "sdk.install: SDK4 passed")
