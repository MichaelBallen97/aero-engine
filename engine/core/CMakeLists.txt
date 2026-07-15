# engine/core/ — the zero-dependency foundation layer (docs/03: handles, math, log, jobs, time, VFS).
# core sits at the bottom of the layer order and depends on NOTHING above it.
#
# Created as an INTERFACE library by 0.2.1 (Handle and SlotMap are templates); converted to STATIC
# by 0.2.4 (logging, the first core service that needs a translation unit) and extended by 0.2.2
# (math, GLM backend) in the same round — both append their sources to the list below. 0.2.5
# appends its own in turn.
#
# BOUNDARY RULE: every third-party backend stays PRIVATE — no dependency's headers may reach
# a consumer of aero::core.

find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
# NOTE: the vcpkg PORT is `enkits` (lowercase, as in vcpkg.json) but the CMake PACKAGE is `enkiTS`
# (mixed case) and the target is enkiTS::enkiTS. See vcpkg/ports/enkits/usage. Getting the case
# wrong fails at configure time, not build time.
find_package(enkiTS CONFIG REQUIRED)

add_library(aero_core STATIC
    src/log.cpp
    src/math/glm_backend.cpp
    src/jobs.cpp
)
add_library(aero::core ALIAS aero_core)

# Public include root: consumers see <aero/core/handle.hpp>, <aero/core/slot_map.hpp>,
# <aero/core/log.hpp>, <aero/core/math.hpp> (and the existing <aero/core/profiler.hpp>). PUBLIC,
# not INTERFACE: the target now compiles its own sources, which need this root on their own
# compile line too — a STATIC library with INTERFACE-only includes fails to find its own headers.
# aero::profiling exposes this same root and CMake de-duplicates the path, so linking both stays
# harmless. core DOES link aero::profiling as of task 0.2.5 (PRIVATE — see the link line below);
# a consumer linking only aero::core still gets profiler.hpp from this root, whose macros no-op
# without AERO_PROFILING_ENABLED.
target_include_directories(aero_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PRIVATE is load-bearing for EVERY backend: spdlog is an implementation detail of src/log.cpp,
# glm is an implementation detail of src/math/glm_backend.cpp, enkiTS is an implementation detail
# of src/jobs.cpp, and none of their headers may reach a
# consumer's compile line. For a STATIC library CMake still propagates PRIVATE as
# $<LINK_ONLY:...>, so the final executable links both archives while nobody inherits their
# headers — that is correct and not a leak.
#
# NOTE (verified, task 0.2.2 and 0.2.4 independently): PRIVATE alone cannot enforce the *header*
# boundary — vcpkg installs every dependency into ONE flat per-triplet include/ directory that
# lands on the compile line (-isystem) of ANY target linking ANY vcpkg package (e.g.
# tests/ -> doctest::doctest), regardless of what aero_core itself links. This DOES still hold as
# a hard compile error for engine-layer targets that link no vcpkg package directly (proven with a
# throwaway probe target linking only aero::core — see docs/02-adrs.md's ADR-005 implementation
# note and risk R12 in docs/08-risks.md). The CI grep in the `lint` job (task 0.2.4, spec 3.8, for
# spdlog/fmt; .github/scripts/check-math-boundary.sh, task 0.2.3, for glm) is what enforces the
# boundary everywhere else.
# Flipping any of these to PUBLIC would silently reopen the boundary engine-wide — do not
# "simplify" it.
#
# aero::profiling is PRIVATE too, and that keyword is doing MORE work here than it looks (task
# 0.2.5, spec D7). aero::profiling is an INTERFACE library that, in Release, carries
# Tracy::TracyClient and therefore vcpkg's shared per-triplet include/ root as a usage requirement.
# PRIVATE means:
#   * src/jobs.cpp DOES get AERO_PROFILING_ENABLED and Tracy's headers (a target's own PRIVATE
#     usage requirements apply to its own compile line) — that is what makes AERO_PROFILE_* live;
#   * consumers do NOT — CMake propagates a PRIVATE link on a STATIC lib as $<LINK_ONLY:...>,
#     which carries the archive but explicitly discards usage requirements.
# Flipping this one to PUBLIC would put vcpkg's shared include root back on the compile line of
# every aero::core consumer — including tests/math_boundary_probe.cpp, whose entire guarantee
# (task 0.2.3) is that it CANNOT resolve <glm/...>. That would silently reduce 0.2.3's guard to a
# no-op while leaving CI green. Verified empirically at 0.2.5 (spec §6.1). Do not "simplify" this.
target_link_libraries(aero_core PRIVATE spdlog::spdlog glm::glm enkiTS::enkiTS aero::profiling)
