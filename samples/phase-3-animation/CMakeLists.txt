# 3.5.2 deliverable: keyframe playback made visible — the SAME committed cooked mesh drawn three
# times, once with an empty palette (frozen in its authored bind pose, the reference), once sampled
# from a real .aeroanim at speed 1 and looping, and once at speed 0.25 and NOT looping, so it visibly
# freezes on its final pose. The clip drives all three glTF paths through all three interpolation
# modes. It is the first consumer of a .aeroanim, the first caller of render::sampleAnimation, and
# the first sample to build a real World: each copy is an entity with a Transform, and the two driven
# ones carry an engine::AnimationPlayer the loop advances and reads back every frame.
# CI builds this (compile-proof, 3 OSes); run it locally for the visual pass
# (editor/validation/3.5.2-clip-playback.md).
# Needs the cooked scene.* AND scene_skinned.vert shaders (AERO_SHADER_TOOLS=ON) at runtime —
# without them this compiles a stub main that logs and returns 1 (the phase-0-cube precedent).
#
# aero::scene is the ONE addition relative to phase-3-skinning's link line, and it is a SAMPLE's link
# line rather than an engine boundary: engine/render still knows nothing about a World. No
# aero::reflect either, for phase-3-skinning's reason — there is no .aeromat here, all three
# materials are built in code, and the three cooked artifacts are read through aero::assets.
add_executable(aero_sample_phase3_animation main.cpp)
target_link_libraries(aero_sample_phase3_animation
    PRIVATE aero::render aero::assets aero::scene aero::rhi aero::platform aero::core aero::profiling
)
target_compile_definitions(aero_sample_phase3_animation
    PRIVATE AERO_PHASE3_ANIMATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}")  # wave.aero{mesh,skel,anim}, committed
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_animation aero_shaders)
    target_compile_definitions(aero_sample_phase3_animation
        PRIVATE AERO_PHASE3_ANIMATION_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
