# engine/core/ — the zero-dependency foundation layer (docs/03: handles, math, log, jobs, time, VFS).
# core sits at the bottom of the layer order and depends on NOTHING above it.
#
# Created as an INTERFACE library by 0.2.1 (Handle and SlotMap are templates); converted to STATIC
# by 0.2.4 (logging) — the first core service that needs a translation unit, because hiding spdlog
# requires one. 0.2.2/0.2.5 append their sources to the list below.
#
# BOUNDARY RULE: every third-party backend stays PRIVATE — no dependency's headers may reach
# a consumer of aero::core.

find_package(spdlog CONFIG REQUIRED)

add_library(aero_core STATIC
    src/log.cpp
)
add_library(aero::core ALIAS aero_core)

# Public include root: consumers see <aero/core/handle.hpp>, <aero/core/slot_map.hpp>,
# <aero/core/log.hpp> (and the existing <aero/core/profiler.hpp>). PUBLIC, not INTERFACE: the
# target now compiles its own sources, which need this root on their own compile line too —
# a STATIC library with INTERFACE-only includes fails to find its own headers.
# aero::profiling exposes this same root and CMake de-duplicates the path, so linking both stays
# harmless. core deliberately does NOT link aero::profiling (spec 0.2.4 D11) — a consumer linking
# only aero::core still gets profiler.hpp, whose macros no-op without AERO_PROFILING_ENABLED.
target_include_directories(aero_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PRIVATE is load-bearing: spdlog is an implementation detail of src/log.cpp and must never
# reach a consumer's compile line. For a STATIC library CMake still propagates it as
# $<LINK_ONLY:...>, so the final executable links spdlog while nobody inherits its headers.
# NOTE: PRIVATE alone cannot enforce the *header* boundary — vcpkg installs every dependency
# into one flat -isystem include dir that any vcpkg-linking target already sees. The CI grep
# in the `lint` job (task 0.2.4, spec 3.8) is what actually enforces it.
target_link_libraries(aero_core PRIVATE spdlog::spdlog)
