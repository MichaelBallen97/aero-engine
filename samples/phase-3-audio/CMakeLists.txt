# 3.7.2 deliverable: THE FIRST NOISE THIS ENGINE HAS EVER MADE. A real World with three entities — a
# listener at the origin, a source orbiting it on a radius-3 circle, and a beacon at 90 % of
# maxDistance — driven through engine::scene_audio::SceneAudio every frame and handed either to a real
# platform::AudioDevice or, with --dump-pcm, to a file.
#
# THIS IS THE FIRST SAMPLE IN THE TREE WITH NO if(AERO_SHADER_TOOLS) BLOCK AND NO STUB-MAIN FALLBACK,
# AND THE ABSENCE IS THE POINT: it opens no window, creates no rhi::Device and compiles no shader, so
# there is nothing for a shader toolchain to gate. A reader who knows the other nine samples will look
# for that block; it is not missing, it is not needed.
#
# Deliberately NOT aero::rhi and NOT aero::render, for the same reason: this sample draws nothing.
# CI builds it (compile-proof, 3 OSes) and never runs it — no lane has audio hardware, and the null
# backend makes no sound. Run it locally for the audible pass
# (editor/validation/3.7.2-playback-api-components.md).
add_executable(aero_sample_phase3_audio main.cpp)
target_link_libraries(aero_sample_phase3_audio
    PRIVATE aero::scene_audio aero::audio aero::scene aero::platform aero::core aero::profiling
)
target_compile_definitions(aero_sample_phase3_audio
    PRIVATE AERO_PHASE3_AUDIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")  # orbit/beacon.aerowave, committed
