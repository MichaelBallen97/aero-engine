# 0.5.2 deliverable: the textured cube; 0.5.3 finalized it as the Phase 0 gate artifact — spin + fps
# readout + the macOS 60fps sign-off (Windows/Linux pending; see VALIDATION.md). A real window + GPU
# device + depth-enabled engine::render::Renderer drawing a checkerboard-textured, per-face-colored unit
# cube with a time-based MVP transform. CI builds this (compile-proof, 3 OSes); run it locally to
# see/resize/close the window. Needs the cooked cube shaders (AERO_SHADER_TOOLS=ON) at runtime — located
# via the AERO_CUBE_SHADERS_DIR build-tree path (D11).
add_executable(aero_sample_phase0_cube main.cpp)
target_link_libraries(aero_sample_phase0_cube
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase0_cube aero_shaders)   # cooked cube.* built before this runs
    target_compile_definitions(aero_sample_phase0_cube
        PRIVATE AERO_CUBE_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")  # == aero_add_shaders OUTPUT_DIR
endif()
