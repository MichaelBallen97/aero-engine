# engine/script/ -- the JavaScript VM layer (task 4.1.1; ADR-007: TypeScript on quickjs-ng). It owns the
# quickjs-ng runtime and context behind engine::script::Vm. Since task 4.1.2 it also loads ES modules, reading them
# only through engine::VirtualFileSystem (aero::core). Since task 4.2.1 it sits above aero::scene: the component
# binding descriptors (internal/aero/script/internal/component_binding.hpp) are what it will expose to scripts, and
# they name engine::scene's ComponentTypeId. aero::reflect arrives through aero_scene's own PUBLIC link.
#
# docs/03 draws script at the top of the engine stack, which is why it is added LAST in engine/CMakeLists.txt.
#
# BOUNDARY RULE (R12, docs/08-risks.md): qjs is PRIVATE. CMake propagates a PRIVATE link of a STATIC
# library as $<LINK_ONLY:qjs> -- the archive reaches every executable, its include root reaches no
# consumer. vcpkg installs quickjs.h into the ONE shared per-triplet include/ root, so this target's own
# compile line carries that root and a consumer that links any vcpkg package can still resolve the
# header: the textual half is .github/scripts/check-script-boundary.sh, and the compile-time half is
# tests/script_boundary_probe.cpp, which links aero::script ALONE. Flipping qjs to PUBLIC would void the
# probe silently while CI stayed green. Do not "simplify" it.
#
# The port's `libc` feature is deliberately NOT requested (vcpkg.json): quickjs-libc's std/os modules
# give scripts file, process and timer access, and nothing may be script-visible here.

find_package(qjs CONFIG REQUIRED)
# The port exports its target UN-namespaced (`qjs`, no `::`). An un-namespaced name that is not a
# target is silently treated as a plain library name (-lqjs) instead of failing the configure, so the
# target is asserted rather than assumed.
if(NOT TARGET qjs)
    message(FATAL_ERROR "engine/script: find_package(qjs) defined no 'qjs' target -- check the quickjs-ng port")
endif()

# The INTERNAL allocator seam (the aero::rhi_internal precedent, engine/rhi/CMakeLists.txt:39-47): a
# header-only INTERFACE target exposing engine/script/internal/ -- the backend-free HeapAccounting the
# JavaScript heap runs on and the module loader's pure half (module_graph.hpp) -- to aero_script itself and to
# aero_tests, which tests its contract directly. It carries ONE include directory and nothing else: no library,
# no vcpkg root, no quickjs. NEVER link it into a boundary probe (check-boundary-probes.sh refuses any *_internal
# there).
add_library(aero_script_internal INTERFACE)
add_library(aero::script_internal ALIAS aero_script_internal)
target_include_directories(aero_script_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/internal)

add_library(aero_script STATIC
    src/vm.cpp
    src/vm_errors.cpp
    src/vm_modules.cpp
    src/vm_scripts.cpp
    src/heap_accounting.cpp
    src/module_graph.cpp
    src/bytecode_format.cpp
    src/bytecode_cook.cpp
    src/component_bindings.cpp
    src/script_api.cpp
    src/script_api_values.cpp
    src/script_api_entity.cpp
)
add_library(aero::script ALIAS aero_script)

target_include_directories(aero_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# aero::assets is PRIVATE: the cooked-script container format and the byte primitives, never a public dependency.
target_link_libraries(aero_script PUBLIC aero::core aero::scene PRIVATE qjs aero::script_internal aero::assets)

# The parse-side -I's both generators use: the set aero_scene_serialize uses.
set(_aero_script_parse_dirs "${CMAKE_SOURCE_DIR}/engine/core/include" "${CMAKE_SOURCE_DIR}/engine/scene/include"
                            "${CMAKE_SOURCE_DIR}/engine/reflect/include")

# task 4.2.1: the built-in component binding table, generated from the same header list the serializer and the
# inspector use. The generated code is quickjs-free (it calls the descriptor templates only). With the tools OFF the
# table is empty (component_bindings.cpp).
if(AERO_REFLECT_TOOLS)
    aero_reflect_generate_script(aero_script
        HEADERS ${AERO_BUILTIN_COMPONENT_HEADERS}
        INCLUDE_DIRS ${_aero_script_parse_dirs})
    target_compile_definitions(aero_script PRIVATE AERO_SCRIPT_COMPONENT_BINDINGS=1)
else()
    message(STATUS "aero_script: no component bindings (AERO_REFLECT_TOOLS=OFF) -- "
                   "engine holds Vec3, Quat and Entity only")
endif()

# task 4.2.2: aero_script_declarations owns <build>/engine/script/types/aero-engine.d.ts -- the TypeScript view of the
# frozen `engine` namespace: types/prelude.d.ts (Vec3, Quat, Entity, written by hand) followed by one reflect-gen
# --emit-dts block per built-in component header. aero_script does NOT depend on it, so the runtime ships none of its
# text; the editor and the tests read the file through the target's AERO_DECLARATIONS_FILE property. Called OUTSIDE
# the tools gate: with the tools OFF the artifact is the prelude alone.
aero_reflect_generate_declarations(aero_script_declarations
    PARSE_TARGET aero_script
    HEADERS ${AERO_BUILTIN_COMPONENT_HEADERS}
    INCLUDE_DIRS ${_aero_script_parse_dirs}
    PRELUDE "${CMAKE_CURRENT_SOURCE_DIR}/types/prelude.d.ts"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/types/aero-engine.d.ts")
