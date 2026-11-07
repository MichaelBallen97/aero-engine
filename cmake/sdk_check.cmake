# task 4.5.1: the refusals of the AERO_SDK configuration (docs/04). Included by the root CMakeLists when AERO_SDK is
# ON, and run alone by tests/sdk/refusals.cmake under `cmake -P`, so it reads variables and calls nothing that needs a
# project. Under -P an option never passed is unset, hence the two tool checks read `NOT <option>`.
#
# An installable SDK is a Release build with the profiler and the sanitizers off: a Debug archive, an ASan-instrumented
# archive or a Tracy-linked archive does not link into a consumer that did not ask for the same runtime. Both tool
# options stay ON because the SDK ships the compiled shaders and the generated scene serializers.

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "AERO_SDK needs CMAKE_BUILD_TYPE=Release (got '${CMAKE_BUILD_TYPE}'): an SDK built in any "
                        "other configuration does not link into a consumer's Release build.")
endif()

if(AERO_ENABLE_PROFILING)
    message(FATAL_ERROR "AERO_SDK needs AERO_ENABLE_PROFILING=OFF: Tracy is dev-builds-only and must not reach a "
                        "shipped archive.")
endif()

if(AERO_ENABLE_SANITIZERS)
    message(FATAL_ERROR "AERO_SDK needs AERO_ENABLE_SANITIZERS=OFF: an instrumented archive does not link into an "
                        "uninstrumented consumer.")
endif()

if(NOT AERO_SHADER_TOOLS)
    message(FATAL_ERROR "AERO_SDK needs AERO_SHADER_TOOLS=ON: the SDK installs the compiled shaders.")
endif()

if(NOT AERO_REFLECT_TOOLS)
    message(FATAL_ERROR "AERO_SDK needs AERO_REFLECT_TOOLS=ON: the SDK installs aero::scene_serialize, which the "
                        "reflection generator produces.")
endif()
