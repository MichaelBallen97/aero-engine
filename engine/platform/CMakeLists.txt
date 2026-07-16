# engine/platform/ — the SDL3 wrapper layer (docs/03): window, event pump, input, audio device.
# The FIRST engine layer above core, and the FIRST engine target to link a vcpkg package (SDL3)
# directly. Depends on core; may depend on NO layer above it, and never on editor/ (golden rule).
#
# BOUNDARY RULE (docs/04, project rule #3): no SDL type crosses a public engine header. SDL3 is
# linked PRIVATE and used ONLY in src/platform.cpp. Two guards enforce it, not convention: the
# `lint` job's SDL grep over engine/*/include, and the compile-time probe
# tests/platform_boundary_probe.cpp (which links only aero::platform).

find_package(SDL3 CONFIG REQUIRED)   # package SDL3, target SDL3::SDL3

add_library(aero_platform STATIC
    src/platform.cpp
    src/input.cpp   # task 0.3.2 — InputState fold (SDL-free)
)
add_library(aero::platform ALIAS aero_platform)

# PUBLIC: consumers see <aero/platform/context.hpp>, window.hpp, event.hpp, platform.hpp.
target_include_directories(aero_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# aero::core is PUBLIC — public platform headers surface engine types and the impl logs through
# <aero/core/log.hpp>; consumers (and the boundary probe) get core's include root transitively.
# core's own third-party backends are PRIVATE-to-core, so this propagates NO vcpkg headers.
#
# SDL3::SDL3 and aero::profiling are PRIVATE. For a STATIC library CMake propagates a PRIVATE link
# as $<LINK_ONLY:...>: the archive reaches the final binary, but the usage requirements — SDL's
# headers AND vcpkg's shared per-triplet include/ root; Tracy's headers — do NOT reach consumers.
# That is exactly what keeps SDL out of every aero::platform consumer, and what makes
# tests/platform_boundary_probe.cpp a real compile-time guard. Flipping SDL3 to PUBLIC would reopen
# the boundary engine-wide — do not "simplify" it. (Same reasoning as engine/core/CMakeLists.txt.)
target_link_libraries(aero_platform
    PUBLIC  aero::core
    PRIVATE SDL3::SDL3 aero::profiling
)
