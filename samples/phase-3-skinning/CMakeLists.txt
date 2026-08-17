# 3.5.1 deliverable: GPU skinning made visible — the SAME committed cooked mesh drawn twice, once
# with an empty palette (frozen in its authored bind pose, the draw path's degradation arm shown
# deliberately) and once with a palette recomputed every frame from a procedurally posed five-joint
# chain. It is the first time this project draws a skinned vertex, the first consumer of a
# .aeroskel, and the first caller of ForwardRenderer::createMesh. CI builds this (compile-proof,
# 3 OSes); run it locally for the visual pass (editor/validation/3.5.1-skeleton-gpu-skinning.md).
# Needs the cooked scene.* AND scene_skinned.vert shaders (AERO_SHADER_TOOLS=ON) at runtime —
# without them this compiles a stub main that logs and returns 1 (the phase-0-cube precedent).
#
# No aero::reflect on the link line, unlike phase-3-materials: there is no .aeromat here, both
# materials are built in code, and the two cooked artifacts are read through aero::assets.
add_executable(aero_sample_phase3_skinning main.cpp)
target_link_libraries(aero_sample_phase3_skinning
    PRIVATE aero::render aero::assets aero::rhi aero::platform aero::core aero::profiling
)
target_compile_definitions(aero_sample_phase3_skinning
    PRIVATE AERO_PHASE3_SKINNING_DIR="${CMAKE_CURRENT_SOURCE_DIR}")  # arm.aeromesh/arm.aeroskel, committed
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_skinning aero_shaders)
    target_compile_definitions(aero_sample_phase3_skinning
        PRIVATE AERO_PHASE3_SKINNING_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
