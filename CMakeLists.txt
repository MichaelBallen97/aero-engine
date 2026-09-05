cmake_minimum_required(VERSION 3.28)

project(AeroEngine
    VERSION 0.1.0                        # provisional pre-release version; v1.0.0 tags at Phase 8
    DESCRIPTION "Open-source, cross-platform 3D game engine with an editor and TypeScript or C++ scripting"
    HOMEPAGE_URL "https://github.com/MichaelBallen97/aero-engine"
    LANGUAGES CXX C                      # C: ONLY for two vendored single-file implementation TUs --
                                         # engine/platform/src/miniaudio_impl.c (task 0.3.3) and
                                         # editor/third_party/ufbx/ufbx.c (task 3.2.2). All first-party
                                         # engine and editor code is C++20; no first-party .c exists.
)

# docs/04: out-of-source builds only.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Configure with a preset: cmake --preset <name>")
endif()

# docs/04: C++20 baseline; C++23 features only where Clang, MSVC, and GCC all support them
# (opted into per-feature later, never by raising this global default).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ADR-001 mitigation #3: ASan/UBSan on every Debug CI build. OFF by default;
# each *-debug preset turns it on. Per-compiler mapping: cmake/sanitizers.cmake.
option(AERO_ENABLE_SANITIZERS "Build all project targets with ASan (+ UBSan where the compiler has it)" OFF)
include(cmake/sanitizers.cmake)

# Tracy profiler (docs/01, docs/03; task 0.1.5). DEV-BUILDS-ONLY: OFF by default; the *-release presets
# turn it on. cmake/profiling.cmake defines aero::profiling (wrappers in engine/core/include/aero/core).
option(AERO_ENABLE_PROFILING "Link the Tracy client and activate profiling macros (dev builds only)" OFF)
include(cmake/profiling.cmake)

# tools/shaderc (task 0.4.3): HLSL -> SPIR-V/MSL/DXIL, offline, at build time (docs/04). Default ON;
# OFF is the escape hatch for constrained/offline dev machines (never set OFF in CI, spec E18) — the
# function must be defined before any add_subdirectory() calls it (shaders/CMakeLists.txt, below).
option(AERO_SHADER_TOOLS "Bootstrap SDL_shadercross and compile shaders/ during the build" ON)
include(cmake/shaders.cmake)

# tools/reflect-gen (task 1.1.1): system-discovered libclang 18 harness — parses a translation unit
# and walks its AST (ADR-004, R2). Default ON; OFF is the escape hatch for constrained/offline dev
# machines (never set OFF in CI), mirroring AERO_SHADER_TOOLS.
option(AERO_REFLECT_TOOLS "Discover system libclang and build tools/reflect-gen" ON)
include(cmake/reflect.cmake)   # task 1.1.4: aero_reflect_generate() + AERO_LLVM_ROOT discovery + the
                               # per-OS AERO_REFLECT_CLANG_ARGS, at ROOT scope so any add_subdirectory
                               # call site (engine/ for 1.3.2) sees them on a fresh configure (D2).

# task 2.1.2: aero_assert_golden_rule() -- project rule #1 (docs/00, docs/03) enforced at the LINK and
# INCLUDE-DIRECTORY level, called at the very end of this file (below). No option() gate, unlike every
# other module in this cluster: none of the existing AERO_* options disables an INVARIANT.
include(cmake/golden_rule.cmake)

# docs/04 testing strategy: doctest suites run through ctest (test presets).
enable_testing()

# task 3.5.2 (D7): the built-in component headers, named ONCE. Four sites generate reflection
# artifacts from this list -- engine/scene_serialize (the shipping JSON serializers), editor
# (the inspector's entt::meta) and the two reflect-gen test targets -- and before this variable they
# spelled it four times. Three of those four are silently optional: a component added to the editor's
# list and not the serializer's is registered, inspectable, editable and NOT SAVED, with every test
# green. In registerBuiltinComponents order (engine/scene/src/transform.cpp), which is also the order
# every generated aggregator calls its per-header register function in.
#
# A component that should be REGISTERED but not SERIALIZED does not fork this variable in place --
# it introduces a second, differently-named one, so no site ever silently means something other than
# what it says.
set(AERO_BUILTIN_COMPONENT_HEADERS
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/transform.hpp       # 1.3.2
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/camera.hpp          # 1.3.3
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/light.hpp           # 1.3.3 -- two components
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/mesh_renderer.hpp   # 1.4.1
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/animation_player.hpp  # 3.5.2
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/audio_source.hpp      # 3.7.2 -- 7th
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/audio_listener.hpp    # 3.7.2 -- 8th
    ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/environment.hpp       # E.2.1 -- 9th
)

# Layer order = dependency order (docs/03). The golden rule is physical here:
# nothing under engine/ or runtime/ may ever reference editor/.
#
# tools/ now precedes engine/ (task 1.4.2): engine/scene_serialize is the first ENGINE-tree call site
# of aero_reflect_generate_json/aero_reflect_generate, both of which require the aero_reflect_gen
# TARGET to already exist at the point they run (cmake/reflect.cmake's D10 tool-resolution check is
# a configure-time `if(TARGET aero_reflect_gen)`, not deferred) — tools/ links no engine target
# (tools/reflect-gen/CMakeLists.txt, tools/shaderc/CMakeLists.txt), so moving it first is safe and
# does not touch the golden rule (only editor/ may never be depended on, and tools/ is not editor/).
add_subdirectory(tools)
add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(shaders)  # task 0.4.3; must follow tools/ (aero_shaderc) and cmake/shaders.cmake
add_subdirectory(tests)
add_subdirectory(samples)   # validation apps (docs/03); first populated by 0.3.1's phase-0-window

# Project rule #1 (docs/00, docs/03). The engine NEVER depends on the editor -- enforced here at the
# LINK and INCLUDE-DIRECTORY level, and textually by .github/scripts/check-golden-rule.sh. Must be
# LAST: it enumerates targets, so every add_subdirectory() above must already have run. Task 2.1.2.
aero_assert_golden_rule(
    CONSUMER_DIRS   "${CMAKE_SOURCE_DIR}/engine" "${CMAKE_SOURCE_DIR}/runtime"
    FORBIDDEN_DIRS  "${CMAKE_SOURCE_DIR}/editor"
    CANARY_CONSUMER aero_editor_core
    CANARY_DEP      aero_core
)
