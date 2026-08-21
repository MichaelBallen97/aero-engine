# 3.6.1 deliverable: frustum culling made visible and measurable — an N x N x N grid of built-in
# primitives seen from a fixed eye whose look direction yaws a full turn every 12 seconds, printing
# the drawn/culled split and the ForwardRenderer::draw record time every frame. `--no-cull` runs the
# identical grid with culling off, which is what isolates the cull term: nothing exposes cull time
# separately, because the cull runs inside draw(). CI builds this (compile-proof, 3 OSes); run it
# locally for the visual pass (editor/validation/3.6.1-frustum-culling.md).
# Needs the cooked scene.{vert,frag} shaders (AERO_SHADER_TOOLS=ON) at runtime — without them this
# compiles a stub main that logs and returns 1 (the phase-0-cube precedent).
#
# No aero::assets on the link line, unlike phase-3-skinning: this sample reads no cooked artifact and
# commits no fixture — every mesh it draws is one of the renderer's three built-in primitives, whose
# bounds are folded at create() from the geometry itself.
add_executable(aero_sample_phase3_culling main.cpp)
target_link_libraries(aero_sample_phase3_culling
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_culling aero_shaders)
    target_compile_definitions(aero_sample_phase3_culling
        PRIVATE AERO_PHASE3_CULLING_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
