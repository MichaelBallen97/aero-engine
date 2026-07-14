# cmake/profiling.cmake — Tracy profiler wiring (docs/01 stack, docs/03 placement; task 0.1.5).
# Provides engine profiling via the aero::profiling INTERFACE library; macro wrappers live in
# engine/core/include/aero/core/profiler.hpp. Tracy is DEV-BUILDS-ONLY (docs/03 invariant): the
# client is linked and TRACY_ENABLE activated ONLY when AERO_ENABLE_PROFILING=ON (the *-release
# presets set it; OFF by default). When OFF, the wrappers compile to nothing and no Tracy is linked.
# Included from the root CMakeLists BEFORE add_subdirectory() so the target is globally visible.

add_library(aero_profiling INTERFACE)
add_library(aero::profiling ALIAS aero_profiling)

# Permanent home of the wrapper header (a core service like log/time; engine/core's compiled
# library lands at 0.2.4 — logging is the first core service that needs a .cpp). Exposed to
# every consumer as <aero/core/profiler.hpp>, in all configs.
target_include_directories(aero_profiling INTERFACE ${CMAKE_SOURCE_DIR}/engine/core/include)

if(AERO_ENABLE_PROFILING)
    # Tracy 0.13.1 (pinned baseline). find_package only when enabled — a default build never needs it.
    # The vcpkg client is built with TRACY_ENABLE=ON (upstream default, PUBLIC), so linking the target
    # activates the Tracy macros in our code automatically.
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(aero_profiling INTERFACE Tracy::TracyClient)
    # Drives the wrapper header; keeps the header backend-agnostic (only this module names Tracy).
    target_compile_definitions(aero_profiling INTERFACE AERO_PROFILING_ENABLED)
endif()
