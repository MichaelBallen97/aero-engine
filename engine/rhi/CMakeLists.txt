# engine/rhi/ — the RHI: the engine's own rendering vocabulary over SDL_GPU (ADR-002, docs/03).
# THE SACRED WRAPPER: this is the escape hatch for the v3-v4 ray-tracing path. No SDL_GPU type may
# ever cross these public headers (project rule #3); enforcement machinery arrives in 0.4.5
# (grep script + compile-time probe, mirroring platform's pair).
#
# Task 0.4.1 ships the SURFACE: headers + the backend-independent format utilities. Device's member
# functions are declared but NOT defined here — src/sdl_gpu_backend.cpp (task 0.4.2) defines them.
# Until then, odr-using any Device member is a link error by design.

add_library(aero_rhi STATIC
    src/format.cpp
    # src/sdl_gpu_backend.cpp   # task 0.4.2 — the ONE TU that includes <SDL3/SDL_gpu.h>
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
# Task 0.4.2 adds: PRIVATE SDL3::SDL3 aero::profiling. Keep both PRIVATE forever — flipping SDL3 to
# PUBLIC reopens the boundary engine-wide (see engine/platform/CMakeLists.txt's warning).
target_link_libraries(aero_rhi
    PUBLIC aero::core aero::platform
)
