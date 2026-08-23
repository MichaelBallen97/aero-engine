# engine/scene_audio/ — the World -> audio bridge (task 3.7.2). The ONLY code in the tree that sees
# both engine::scene and engine::audio — it sits ABOVE both (docs/03), which is what keeps
# engine/audio scene-free and engine/scene audio-free. The engine/scene_render shape exactly, one
# layer over.
#
# WHY A NEW TARGET AT ALL, in one sentence: folding the walk into engine/audio would give aero_audio
# a PUBLIC aero::scene, so EVERY BINARY THAT LINKS AUDIO WOULD DRAG ENTT IN -- including the Phase 5
# runtime, which per ADR-008 is handed a .pak and may reasonably want to play a sound without
# instantiating a World at all. Sample-only wiring is the other rejected shape: the ENGINE would then
# ship no World -> audio path, and the editor's future play mode and the runtime would each rewrite
# it. scene_render exists for the same reason and the parallel is exact.
#
# No find_package, no boundary guard: this target wraps no third-party type and links no vcpkg
# package directly (the 0.5.1/1.2.1/1.4.1 precedent) -- the boundary rule is satisfied by
# construction.
add_library(aero_scene_audio STATIC
    src/scene_audio.cpp
)
add_library(aero::scene_audio ALIAS aero_scene_audio)

target_include_directories(aero_scene_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC aero::scene + aero::audio: the bridge's header names World (scene) and audio types. Both
# propagate only engine types (entt is PRIVATE-to-scene; aero_audio links no vcpkg package at all),
# so no third-party header reaches consumers and no boundary guard is needed. NEVER
# aero::scene_internal (that target carries EnTT::EnTT INTERFACE by design -- see
# check-scene-boundary.sh's own warning).
target_link_libraries(aero_scene_audio
    PUBLIC aero::scene aero::audio
    PRIVATE aero::profiling
)
