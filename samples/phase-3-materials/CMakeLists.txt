# 3.4.1 deliverable: the material system's proof — a 6x6 roughness x metallic sphere grid, a
# fully-mapped cube whose material loads from the committed mapped_cube.aeromat, and an alpha-mask
# cube — the first time a cooked .ktx2 in this project is consumed by a GPU. Textures resolve by
# GUID (the Phase 5 pak flow in miniature). CI builds this (compile-proof, 3 OSes); run it locally
# for the visual pass (editor/validation/3.4.1-material-asset-pbr-shader.md). Needs the cooked
# scene.* shaders (AERO_SHADER_TOOLS=ON) at runtime — without them this compiles a stub main that
# logs and returns 1 (the phase-0-cube conditional-define precedent).
add_executable(aero_sample_phase3_materials main.cpp)
target_link_libraries(aero_sample_phase3_materials
    PRIVATE aero::render aero::reflect aero::assets aero::rhi aero::platform aero::core aero::profiling
)
target_compile_definitions(aero_sample_phase3_materials
    PRIVATE AERO_PHASE3_MATERIALS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")  # fixtures, committed
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_materials aero_shaders)
    target_compile_definitions(aero_sample_phase3_materials
        PRIVATE AERO_PHASE3_MATERIALS_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
