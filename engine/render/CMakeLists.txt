# engine/render/ — render v0 (Epic 0.5, docs/03). The fourth engine layer (core -> platform -> rhi ->
# render). Task 0.5.1 opens it with the smallest real renderer: it owns a window's swapchain and drives
# begin -> clear -> present over the RHI. Uses ONLY engine types (no SDL, no vcpkg package) — the same
# "no find_package" shape rhi had at 0.4.1. No boundary guard is added: check-platform-boundary.sh
# (engine/*/include/*) and check-rhi-boundary.sh (engine + runtime) already scan this tree, and there
# is no third-party type here for them to catch.
add_library(aero_render STATIC
    src/renderer.cpp
    src/forward_renderer.cpp  # task 1.4.1
    src/primitives.cpp        # task 1.4.1
    src/render_target.cpp     # task 2.2.3 -- the offscreen sibling of Renderer
    src/texture_upload.cpp    # task 3.4.1 -- the cooked-texture (.ktx2) -> GPU bridge
    src/skinning.cpp          # task 3.5.1 -- joint poses, the matrix palette, the palette row packer
    src/animation.cpp         # task 3.5.2 -- the clip sampler: bind once, sample per frame
    src/culling.cpp           # task 3.6.1 -- Aabb/Plane/Frustum, the [0,1] extraction, the six-plane test
    src/shadow.cpp            # task 3.6.2 -- the directional shadow fit: corners, sphere, basis, snap
    src/tonemap.cpp           # task 3.6.3 -- the transfer chain: exposure, curve, sRGB OETF
    src/post_process.cpp      # task 3.6.3 -- the owned HDR target and the fullscreen resolve
    src/debug_draw.cpp        # task E.1.1 -- the pure batch, four pipelines, the flush
    src/debug_grid.cpp        # task E.1.2 -- the pure ground-grid emitter and its cadence
    src/selection_outline.cpp # task E.1.4 -- the fullscreen edge-detect composite
    src/environment.cpp       # task E.2.1 -- the pure environment vocabulary: modes, resolvers, the
                              #               gradient's reference evaluator
)
add_library(aero::render ALIAS aero_render)

target_include_directories(aero_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# aero::rhi PUBLIC: renderer.hpp's whole surface is rhi types (Color/Extent2D/handles/TextureFormat)
# plus platform::Window (named only as a by-reference create() parameter). aero::rhi re-exports
# aero::platform and aero::core PUBLICly and carries no vcpkg header to consumers (its backends are
# PRIVATE), so this one edge gives render everything it needs and nothing it shouldn't.
# aero::profiling PRIVATE: Tracy zones in the frame path; $<LINK_ONLY:> keeps it off consumers.
#
# aero::assets PUBLIC (task 3.4.1): texture_upload.hpp -- the cooked-texture bridge -- names
# assets::CookedTextureView in its signature, so the edge has to be PUBLIC for consumers to call it.
# Downward and cycle-free in the as-built layer order (core -> assets -> platform -> rhi -> render,
# CLAUDE.md's layer block): engine/assets links aero::core and nothing else, carries no vcpkg package
# at all, and aero_cooker already links it from outside the engine. This is the ONLY link-line change
# this task makes anywhere.
target_link_libraries(aero_render
    PUBLIC aero::rhi aero::assets
    PRIVATE aero::profiling
)
