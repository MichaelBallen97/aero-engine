# engine/rhi/ — the RHI: the engine's own rendering vocabulary over SDL_GPU (ADR-002, docs/03).
# THE SACRED WRAPPER: this is the escape hatch for the v3-v4 ray-tracing path. No SDL_GPU type may
# ever cross these public headers (project rule #3); enforcement machinery arrives in 0.4.5
# (grep script + compile-time probe, mirroring platform's pair).
#
# Task 0.4.1 shipped the SURFACE: headers + the backend-independent format utilities. Task 0.4.2
# lands the backend: src/sdl_gpu_backend.cpp is the ONE TU in the whole repo that includes
# <SDL3/SDL_gpu.h> and defines every Device member.

find_package(SDL3 CONFIG REQUIRED)

add_library(aero_rhi STATIC
    src/format.cpp
    src/sdl_gpu_backend.cpp  # task 0.4.2 — the ONE TU that includes <SDL3/SDL_gpu.h>
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
# and vcpkg's shared include root off consumers — that is what will make 0.4.5's probe a real guard.
# aero::platform_internal carries only the native-window seam header (task 0.4.2, §3.1).
target_link_libraries(aero_rhi
    PUBLIC aero::core aero::platform
    PRIVATE SDL3::SDL3 aero::profiling aero::platform_internal
)
