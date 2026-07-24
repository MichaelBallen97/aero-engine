# engine/scene_render/ — the World -> render bridge (task 1.4.1). The ONLY code in the tree that sees
# both engine::scene and engine::render — it sits ABOVE both (docs/03), so the layer law (render may
# never depend on scene) stays intact. No find_package, no boundary guard: this target wraps no
# third-party type and links no vcpkg package directly (0.5.1/1.2.1 precedent) — the boundary rule is
# satisfied by construction.
add_library(aero_scene_render STATIC src/scene_renderer.cpp)
add_library(aero::scene_render ALIAS aero_scene_render)

target_include_directories(aero_scene_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC aero::scene + aero::render: the bridge's header names World (scene) and render types. Both
# propagate only engine types (entt is PRIVATE-to-scene; rhi/SDL is PRIVATE-to-rhi), so no vcpkg header
# reaches consumers and no boundary guard is needed (0.5.1/1.2.1 precedent). NEVER aero::scene_internal
# (that target carries EnTT::EnTT INTERFACE by design — see check-scene-boundary.sh's own warning).
target_link_libraries(aero_scene_render
    PUBLIC aero::scene aero::render
    PRIVATE aero::profiling
)
