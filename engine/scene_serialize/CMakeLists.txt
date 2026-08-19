# task 1.4.2 — the World <-> SceneDocument bridge, and the FIRST production consumer of
# aero_reflect_generate_json (until now only tests generated component serializers). GATED on
# AERO_REFLECT_TOOLS: the generated aeroReadJson/aeroWriteJson are the module's reason to exist and
# are never committed, so with the tool off there is nothing to build (the AERO_SHADER_TOOLS/
# phase-0-cube precedent). engine/reflect still builds unconditionally — it hides no generated code;
# this module IS generated code + the dispatch around it.
if(NOT AERO_REFLECT_TOOLS)
    message(STATUS "aero_scene_serialize: SKIPPED (AERO_REFLECT_TOOLS=OFF — no generated component serializers)")
    return()
endif()

add_library(aero_scene_serialize STATIC src/scene_serialize.cpp)
add_library(aero::scene_serialize ALIAS aero_scene_serialize)
target_include_directories(aero_scene_serialize PUBLIC include)
# PUBLIC aero::scene + aero::reflect: the header names World + SceneDocument/JsonValue/SceneError, all
# engine types. entt is PRIVATE-in-scene, so no vcpkg header reaches consumers -> no boundary guard
# (1.2.1 precedent). NEVER aero::scene_internal (it carries EnTT INTERFACE, D10).
target_link_libraries(aero_scene_serialize
    PUBLIC  aero::scene aero::reflect
    PRIVATE aero::profiling)

# Generate aeroReadJson/aeroWriteJson for the 5 built-ins INTO this target. 4 headers -> 5 components
# (light.hpp carries DirectionalLight + PointLight). Same INCLUDE_DIRS the json test uses
# (tests/CMakeLists.txt) — the libclang PARSE side needs core/scene/reflect headers.
aero_reflect_generate_json(aero_scene_serialize
    HEADERS ${AERO_BUILTIN_COMPONENT_HEADERS}   # task 3.5.2 (D7): named once, at root scope
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"
                 "${CMAKE_SOURCE_DIR}/engine/scene/include"
                 "${CMAKE_SOURCE_DIR}/engine/reflect/include")
