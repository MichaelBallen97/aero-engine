# editor/ — the editor layer (docs/03; Epic 2.1 opens it). ImGui/panels/importers/exporter, 3
# desktop platforms. Depends on engine/; NEVER the reverse (golden rule — 2.1.2 makes it a CI
# failure). ImGui + SDL3 + the two internal seams are PRIVATE to aero_editor_core, so ImGui is
# confined to this layer's .cpp TUs and the aero_editor exe / the smoke test stay ImGui-free.

find_package(imgui CONFIG REQUIRED)    # 1.92.8 docking + sdl3 + sdlgpu3 bindings (vcpkg.json, D1)
find_package(SDL3  CONFIG REQUIRED)    # editor speaks SDL/SDL_GPU directly (far side of the boundary)

add_library(aero_editor_core STATIC
    src/imgui_layer.cpp
    src/panel_registry.cpp
    src/editor_app.cpp
    src/shell_ui.cpp
    src/placeholder_panel.cpp
    src/selection.cpp           # task 2.2.1
    src/entity_ops.cpp          # task 2.2.1
    src/tree_walk.cpp           # task 2.2.1, review round 2 (Gap 1)
    src/hierarchy_panel.cpp     # task 2.2.1
    src/component_ops.cpp       # task 2.2.2 -- the D2 seam (entt-using, ImGui-free)
    src/inspector_model.cpp     # task 2.2.2 -- the meta walk (entt-using, ImGui-free, const World&)
    src/meta_utils.cpp          # task 2.2.2 -- the ONE 17-type map, shared by the two above
    src/editor_reflection.cpp   # task 2.2.2 -- the D16 bootstrap (tools-ON/OFF via one #if)
    src/text_input.cpp          # task 2.2.2 -- inputTextString, promoted verbatim from hierarchy_panel.cpp
    src/inspector_panel.cpp     # task 2.2.2 -- the reflection-driven Inspector panel
    src/viewport_panel.cpp      # task 2.2.3 -- the scene rendered to texture inside a panel
)
add_library(aero::editor_core ALIAS aero_editor_core)
target_include_directories(aero_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(EnTT CONFIG REQUIRED)   # header-only; a manifest dependency since 1.1.3 -- no vcpkg change
target_link_libraries(aero_editor_core
    # aero::scene is PUBLIC because panel_context.hpp / editor_app.hpp surface engine::World and
    # engine::Entity in PUBLIC editor headers (task 2.2.1). It propagates NO vcpkg include path:
    # aero_scene links EnTT::EnTT PRIVATE (task 1.3.1) and its PUBLIC deps (aero::core, aero::reflect)
    # link no vcpkg package either -- the same property tests/scene_boundary_probe.cpp rests on. It is
    # also NOT gated on AERO_REFLECT_TOOLS (only engine/scene_serialize is), so the tools-OFF build and
    # the ungated tier-0 shell test both survive (2.1.3 AC-9 preserved).
    PUBLIC  aero::rhi aero::platform aero::scene aero::core
    # EnTT is PRIVATE for the SAME reason imgui is (D1/2.1.3): the editor's PUBLIC headers stay
    # entt-free BY FILE PLACEMENT -- every entt entry point lives in editor/src/. That is convention +
    # review, NOT enforcement: this target's consumers link doctest/vcpkg roots, so a leaked
    # <entt/entt.hpp> in a public editor header would still compile (risk R12, docs/08 -- the same
    # limitation 2.1.3 documented for imgui.h and proved by seeding it). Do not "fix" it with a probe.
    # aero::scene_render is PRIVATE (D17): no PUBLIC editor header names a render/scene_render type --
    # viewport_panel.hpp is src-private. It brings aero::render transitively (PUBLIC on
    # aero_scene_render) and, like aero::scene, propagates no vcpkg include path. It is NOT gated on
    # AERO_REFLECT_TOOLS (engine/scene_render/CMakeLists.txt has no gate), so the tools-OFF build and
    # the ungated tier-0 shell test both survive.
    PRIVATE aero::profiling imgui::imgui SDL3::SDL3 EnTT::EnTT aero::scene_render
            aero::platform_internal aero::rhi_internal
)

if(AERO_REFLECT_TOOLS)
    # Task 2.2.2 (D16/§3.12): the four built-in component headers, compiled INTO the editor as
    # generated entt::meta registration + one aggregator, aero_reflect_register_all_aero_editor_core
    # (cmake/reflect.cmake's MAKE_C_IDENTIFIER rule). The INCLUDE_DIRS trio is the F19/scene_serialize
    # set. The entt-using TUs compile UNCONDITIONALLY: with tools OFF nothing is registered, resolve
    # misses, and D12's degradation falls out with zero #if outside editor_reflection.cpp.
    aero_reflect_generate(aero_editor_core
        HEADERS ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/transform.hpp
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/camera.hpp
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/light.hpp
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/mesh_renderer.hpp
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"
                     "${CMAKE_SOURCE_DIR}/engine/scene/include"
                     "${CMAKE_SOURCE_DIR}/engine/reflect/include")
    target_compile_definitions(aero_editor_core PRIVATE AERO_EDITOR_REFLECTION=1)
endif()

if(AERO_SHADER_TOOLS)
    # Task 2.2.3: the viewport needs the cooked scene.{vert,frag}. Same shape as
    # samples/phase-1-scene (F22). INVARIANT: AERO_SHADERS_DIR must equal aero_add_shaders'
    # OUTPUT_DIR -- move both together (tests/CMakeLists.txt records the same rule).
    add_dependencies(aero_editor_core aero_shaders)
    target_compile_definitions(aero_editor_core PRIVATE
        AERO_EDITOR_SHADERS=1 AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()

add_executable(aero_editor src/main.cpp)
target_link_libraries(aero_editor PRIVATE aero::editor_core aero::core)
