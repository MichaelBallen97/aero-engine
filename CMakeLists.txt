cmake_minimum_required(VERSION 3.28)

project(AeroEngine
    VERSION 0.1.0                        # provisional pre-release version; v1.0.0 tags at Phase 8
    DESCRIPTION "Open-source, cross-platform 3D game engine with an editor and TypeScript or C++ scripting"
    HOMEPAGE_URL "https://github.com/MichaelBallen97/aero-engine"
    LANGUAGES CXX C                      # C: ONLY for miniaudio's single-file implementation TU
                                         # (engine/platform/src/miniaudio_impl.c, task 0.3.3). All
                                         # first-party engine code is C++20; no other .c exists.
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
# machines (never set OFF in CI), mirroring AERO_SHADER_TOOLS. No cmake/*.cmake module in 1.1.1 —
# discovery lives inline in tools/reflect-gen/CMakeLists.txt (spec D3).
option(AERO_REFLECT_TOOLS "Discover system libclang and build tools/reflect-gen" ON)

# docs/04 testing strategy: doctest suites run through ctest (test presets).
enable_testing()

# Layer order = dependency order (docs/03). The golden rule is physical here:
# nothing under engine/ or runtime/ may ever reference editor/.
add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(tools)
add_subdirectory(shaders)  # task 0.4.3; must follow tools/ (aero_shaderc) and cmake/shaders.cmake
add_subdirectory(tests)
add_subdirectory(samples)   # validation apps (docs/03); first populated by 0.3.1's phase-0-window
