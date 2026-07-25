# editor/ — the editor layer (docs/03; Epic 2.1 opens it). ImGui/panels/importers/exporter, 3
# desktop platforms. Depends on engine/; NEVER the reverse (golden rule — 2.1.2 makes it a CI
# failure). ImGui + SDL3 + the two internal seams are PRIVATE to aero_editor_core, so ImGui is
# confined to this layer's .cpp TUs and the aero_editor exe / the smoke test stay ImGui-free.

find_package(imgui CONFIG REQUIRED)    # 1.92.8 docking + sdl3 + sdlgpu3 bindings (vcpkg.json, D1)
find_package(SDL3  CONFIG REQUIRED)    # editor speaks SDL/SDL_GPU directly (far side of the boundary)

add_library(aero_editor_core STATIC
    src/imgui_layer.cpp
    src/panel_registry.cpp
    src/editor_ui.cpp
)
add_library(aero::editor_core ALIAS aero_editor_core)
target_include_directories(aero_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aero_editor_core
    PUBLIC  aero::rhi aero::platform aero::core
    PRIVATE aero::profiling imgui::imgui SDL3::SDL3 aero::platform_internal aero::rhi_internal
)

add_executable(aero_editor src/main.cpp)
target_link_libraries(aero_editor PRIVATE aero::editor_core aero::core)
