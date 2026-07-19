# engine/rhi/ — the RHI: the engine's own rendering vocabulary over SDL_GPU (ADR-002, docs/03).
# THE SACRED WRAPPER: this is the escape hatch for the v3-v4 ray-tracing path. No SDL_GPU type may
# ever cross these public headers (project rule #3); enforcement machinery is
# `.github/scripts/check-rhi-boundary.sh` + the compile-time probe `tests/rhi_boundary_probe.cpp`
# (task 0.4.5, landed) — mirrors the math pair (repo-wide scan + one allowlisted file), not
# platform's header-only one.
#
# Task 0.4.1 shipped the SURFACE: headers + the backend-independent format utilities. Task 0.4.2
# lands the backend: src/sdl_gpu_backend.cpp is the ONE TU in the whole repo that includes
# <SDL3/SDL_gpu.h> and defines every Device member.

find_package(SDL3 CONFIG REQUIRED)

add_library(aero_rhi STATIC
    src/format.cpp
    src/sdl_gpu_backend.cpp  # task 0.4.2 — the ONE TU that includes <SDL3/SDL_gpu.h>
    src/shader_loader.cpp    # task 0.4.4 — SDL-free: VFS + Device API + hand-rolled schema-1 parser
)
add_library(aero::rhi ALIAS aero_rhi)

target_include_directories(aero_rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# aero::core PUBLIC: the handle vocabulary IS core's Handle<Tag>, surfaced in every rhi header.
# aero::platform PUBLIC: Device::createSwapchain names platform::Window in its signature (the header
# only forward-declares it; consumers who call it already own a Window and therefore already link
# platform — PUBLIC records the layer edge honestly in the build graph, docs/03 order).
# Neither propagates any vcpkg header (their backends are PRIVATE to them), so a future probe target
# linking only aero::rhi still cannot resolve <SDL3/...> — R12's good case, same as platform's probe.
#
# PRIVATE forever (see engine/platform/CMakeLists.txt's warning): $<LINK_ONLY:> keeps SDL's headers
# and vcpkg's shared include root off consumers — that is what makes `tests/rhi_boundary_probe.cpp`
# a real guard (task 0.4.5, landed).
# aero::platform_internal carries only the native-window seam header (task 0.4.2, §3.1).
target_link_libraries(aero_rhi
    PUBLIC aero::core aero::platform
    PRIVATE SDL3::SDL3 aero::profiling aero::platform_internal
)
