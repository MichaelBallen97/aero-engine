# task 1.4.2 deliverable: the Phase 1 gate artifact — load a committed JSON scene from disk (through
# the VFS) into a World, and draw it every frame with SceneRenderer. Real path needs BOTH
# AERO_REFLECT_TOOLS (the generated component serializers, aero::scene_serialize) and
# AERO_SHADER_TOOLS (the cooked scene.{vert,frag} shaders, aero_shaders); either OFF compiles a stub
# main (logs + returns 1), mirroring phase-0-cube's single-option stub but ANDing two options.
add_executable(aero_sample_phase1_scene main.cpp)
target_link_libraries(aero_sample_phase1_scene
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
target_compile_definitions(aero_sample_phase1_scene PRIVATE
    AERO_PHASE1_SCENE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")  # locates the committed scene.json at runtime
if(AERO_REFLECT_TOOLS AND AERO_SHADER_TOOLS)
    target_link_libraries(aero_sample_phase1_scene PRIVATE aero::scene aero::scene_render aero::scene_serialize)
    add_dependencies(aero_sample_phase1_scene aero_shaders)  # scene.{vert,frag} cooked first (1.4.1)
    target_compile_definitions(aero_sample_phase1_scene PRIVATE
        AERO_PHASE1_SCENE_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
