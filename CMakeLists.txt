cmake_minimum_required(VERSION 3.28)

project(AeroEngine
    VERSION 0.1.0                        # provisional pre-release version; v1.0.0 tags at Phase 8
    DESCRIPTION "Open-source, cross-platform 3D game engine with an editor and TypeScript or C++ scripting"
    HOMEPAGE_URL "https://github.com/MichaelBallen97/aero-engine"
    LANGUAGES CXX
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

# docs/04 testing strategy: doctest suites run through ctest (test presets).
enable_testing()

# Layer order = dependency order (docs/03). The golden rule is physical here:
# nothing under engine/ or runtime/ may ever reference editor/.
add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(tools)
add_subdirectory(tests)

# Proves the vcpkg toolchain resolves manifest dependencies end-to-end.
# TEMPORARY at root: moves into engine/platform when its target lands (0.3.1).
find_package(SDL3 CONFIG REQUIRED)
