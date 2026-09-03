# E.1.1 deliverable: the debug line renderer made visible and MEASURABLE. A lit cube, sphere and
# floor plane through ForwardRenderer AND a batch through DebugDraw, both inside ONE PostProcess
# scene pass -- exactly as the editor does it. The batch is chosen so every property this task
# claims has a visible witness: a wire box whose FAR EDGES ARE HIDDEN by the cube (Tested), a
# magenta circle INSIDE the cube that shows through it (Overlay), a floor lattice whose alpha fades
# with distance (the mechanism E.1.2 will use, pushed through the lines(span) overload), five white
# disc billboards at increasing depth that MEASURE THE SAME WIDTH while the camera dollies, one disc
# behind the cube (hidden, Tested) and one at its centre (visible, Overlay), and the three world axes.
#
# The 3.6.3 method for the headline number: --tonemap=/--exposure=/--raw are copied from
# phase-3-tonemap so the PURE WHITE LINE's 8-bit reading is printed at startup from
# render::tonemapAndEncode and compared against a colour meter. --overflow pushes exactly 1000
# segments past the budget every frame to show the drop count and the SINGLE latched WARN;
# --no-lines skips the flush entirely, which is the cost row's A/B.
#
# CI builds this (compile-proof, 3 OSes); run it locally for the visual pass
# (editor/validation/E.1.1-debug-line-renderer.md). Needs the cooked scene.{vert,frag},
# fullscreen.vert/tonemap.frag and debug_line.*/debug_billboard.* shaders (AERO_SHADER_TOOLS=ON) at
# runtime -- without them this compiles a stub main that logs and returns 1 (the phase-0-cube
# precedent).
#
# No aero::assets on the link line, like phase-3-tonemap: this sample reads no cooked artifact and
# commits no fixture -- every mesh it draws is one of the renderer's three built-in primitives.
add_executable(aero_sample_phaseE_debug_draw main.cpp)
target_link_libraries(aero_sample_phaseE_debug_draw
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phaseE_debug_draw aero_shaders)
    target_compile_definitions(aero_sample_phaseE_debug_draw
        PRIVATE AERO_PHASEE_DEBUG_DRAW_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()
