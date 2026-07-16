# 0.3.1 deliverable: the first VISIBLE artifact — a real window owned by engine code. CI builds this
# (compile-proof on all 3 OSes); run it locally to see/resize/close a window. NOT run in CI (headless).
add_executable(aero_sample_phase0_window main.cpp)
target_link_libraries(aero_sample_phase0_window PRIVATE aero::platform aero::core aero::profiling)
