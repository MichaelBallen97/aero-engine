# 3.6.3 deliverable: the tonemap/gamma pass made visible and MEASURABLE — a row of eleven
# emissive-only patches across the top of the screen whose linear values are known EXACTLY, above a
# row of lit spheres for the qualitative judgment a flat patch cannot answer. The sample prints the
# expected 8-bit reading for all three operators AND for --raw at startup, so a validation pass
# compares a colour-meter reading against a number this build computed rather than against a number
# copied from a document.
#
# `--raw` bypasses PostProcess entirely and draws straight into the swapchain exactly as every sample
# did before this task. That is the A/B control and the direct descendant of 3.6.1's `--no-cull`: the
# headline is one pair of readings at the 0.21404 patch, which encodes to sRGB 0.5.
#
# Unlike both editor consumers, this sample resolves into a frame that CARRIES DEPTH
# (RendererConfig{.depth = true}), so it is the one production caller exercising
# PostProcessConfig::outputDepthFormat's non-Invalid arm.
#
# CI builds this (compile-proof, 3 OSes); run it locally for the visual pass
# (editor/validation/3.6.3-tonemap-gamma.md). Needs the cooked scene.{vert,frag} and
# fullscreen.vert/tonemap.frag shaders (AERO_SHADER_TOOLS=ON) at runtime — without them this compiles
# a stub main that logs and returns 1 (the phase-0-cube precedent).
#
# No aero::assets on the link line, like phase-3-culling: this sample reads no cooked artifact and
# commits no fixture — every mesh it draws is one of the renderer's three built-in primitives.
add_executable(aero_sample_phase3_tonemap main.cpp)
target_link_libraries(aero_sample_phase3_tonemap
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_tonemap aero_shaders)
    target_compile_definitions(aero_sample_phase3_tonemap
        PRIVATE AERO_PHASE3_TONEMAP_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
