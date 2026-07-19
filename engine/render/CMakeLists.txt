# engine/render/ — render v0 (Epic 0.5, docs/03). The fourth engine layer (core -> platform -> rhi ->
# render). Task 0.5.1 opens it with the smallest real renderer: it owns a window's swapchain and drives
# begin -> clear -> present over the RHI. Uses ONLY engine types (no SDL, no vcpkg package) — the same
# "no find_package" shape rhi had at 0.4.1. No boundary guard is added: check-platform-boundary.sh
# (engine/*/include/*) and check-rhi-boundary.sh (engine + runtime) already scan this tree, and there
# is no third-party type here for them to catch.
add_library(aero_render STATIC
    src/renderer.cpp
)
add_library(aero::render ALIAS aero_render)

target_include_directories(aero_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# aero::rhi PUBLIC: renderer.hpp's whole surface is rhi types (Color/Extent2D/handles/TextureFormat)
# plus platform::Window (named only as a by-reference create() parameter). aero::rhi re-exports
# aero::platform and aero::core PUBLICly and carries no vcpkg header to consumers (its backends are
# PRIVATE), so this one edge gives render everything it needs and nothing it shouldn't.
# aero::profiling PRIVATE: Tracy zones in the frame path; $<LINK_ONLY:> keeps it off consumers.
target_link_libraries(aero_render
    PUBLIC aero::rhi
    PRIVATE aero::profiling
)
