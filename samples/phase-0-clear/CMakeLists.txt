# 0.5.1 deliverable: FIRST PIXELS. A real window + GPU device + engine::render::Renderer clearing to a
# pulsing color, vsync-paced. CI builds this (compile-proof, 3 OSes); run it locally to see/resize/close
# the window. NOT run in CI (needs a real GPU + visible window), like phase-0-window.
add_executable(aero_sample_phase0_clear main.cpp)
target_link_libraries(aero_sample_phase0_clear
    PRIVATE aero::render aero::rhi aero::platform aero::core aero::profiling
)
