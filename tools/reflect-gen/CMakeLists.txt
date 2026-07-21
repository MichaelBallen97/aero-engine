# tools/reflect-gen — task 1.1.1. First-party libclang harness (ADR-004, R2); see README.md for the
# frozen CLI contract (argv grammar, exit codes) that 1.1.2-1.1.4 extend but never break.
#
# aero_reflect_gen links NO engine target and NO vcpkg package (spec D5): libclang's stable C API
# (<clang-c/Index.h>) + the standard library only, so /tools never drags GLM/spdlog/vcpkg usage
# requirements (or R12's shared include root) into a build-time-only binary.
if(NOT AERO_REFLECT_TOOLS)
    message(STATUS "aero_reflect_gen: skipped (AERO_REFLECT_TOOLS=OFF) — no reflect-gen, no reflect-gen tests")
    return()
endif()

# AERO_LLVM_ROOT is discovered at root-include time by cmake/reflect.cmake (task 1.1.4, D2); it is a
# CACHE PATH set before add_subdirectory(tools), so it is guaranteed set here.

# Preferred: the CMake config package (an imported `libclang` target carrying include dir + location +
# a version match, in one shot). QUIET so the fallback below can run without a scary first-pass error.
# HINTS include LLVM's own config dir too: ClangConfig.cmake does a nested find_dependency(LLVM ...
# REQUIRED) internally, and without it discoverable this can silently push us to the fallback (plan
# C.3). Verified locally (2026-07-21, llvm@18 18.1.8): ClangConfigVersion.cmake's compatibility check
# requires an EXACT major.minor match against the requested version string, so requesting bare "18"
# (major only, as here) does not satisfy it against installed "18.1.8" — the fallback below is what
# actually fires on at least this lane, exactly as plan C.3 anticipates and blesses ("that is fine — a
# C-API-only consumer needs none of LLVM's CMake machinery"). find_package(Clang 18 CONFIG QUIET ...)
# fails over to "no libclang target" quietly (proven: it does not abort configure), never a hard error.
find_package(Clang 18 CONFIG QUIET
    HINTS "${AERO_LLVM_ROOT}" "${AERO_LLVM_ROOT}/lib/cmake/clang" "${AERO_LLVM_ROOT}/lib/cmake/llvm")

if(TARGET libclang)
    message(STATUS "aero_reflect_gen: libclang found via find_package(Clang CONFIG) fast path")
    set(_aero_libclang libclang)
else()
    message(STATUS "aero_reflect_gen: find_package(Clang CONFIG) did not resolve a libclang target "
                   "(expected on at least some lanes, plan C.3) — falling back to find_library/find_path")
    # Fallback: the runtime lib + clang-c headers are present without (or despite) the CMake package.
    find_library(AERO_LIBCLANG NAMES clang libclang HINTS "${AERO_LLVM_ROOT}/lib" REQUIRED)
    find_path(AERO_CLANG_C_INCLUDE clang-c/Index.h HINTS "${AERO_LLVM_ROOT}/include" REQUIRED)
    message(STATUS "aero_reflect_gen: AERO_LIBCLANG = ${AERO_LIBCLANG}")
    message(STATUS "aero_reflect_gen: AERO_CLANG_C_INCLUDE = ${AERO_CLANG_C_INCLUDE}")
    add_library(aero_libclang_imported UNKNOWN IMPORTED)
    set_target_properties(aero_libclang_imported PROPERTIES
        IMPORTED_LOCATION "${AERO_LIBCLANG}"
        INTERFACE_INCLUDE_DIRECTORIES "${AERO_CLANG_C_INCLUDE}")
    set(_aero_libclang aero_libclang_imported)

    # E4's version-drift gate, kept alive on the fallback path (plan C.3): a light, cheap assertion.
    # NOT hard-required — the Windows LLVM installer may omit llvm-config (D10 leans on find_package +
    # the pinned choco version there instead).
    find_program(AERO_LLVM_CONFIG NAMES llvm-config llvm-config-18 HINTS "${AERO_LLVM_ROOT}/bin")
    if(AERO_LLVM_CONFIG)
        execute_process(COMMAND "${AERO_LLVM_CONFIG}" --version OUTPUT_VARIABLE _aero_llvm_version
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT _aero_llvm_version MATCHES "^18\\.")
            message(FATAL_ERROR "aero_reflect_gen: AERO_LLVM_ROOT (${AERO_LLVM_ROOT}) reports LLVM "
                                "'${_aero_llvm_version}', not the pinned 18.x (spec D2/E4). Install LLVM "
                                "18, point -DAERO_LLVM_ROOT at a matching install, or configure with "
                                "-DAERO_REFLECT_TOOLS=OFF.")
        endif()
        message(STATUS "aero_reflect_gen: llvm-config confirms LLVM ${_aero_llvm_version}")
        unset(_aero_llvm_version)
    endif()
endif()

add_executable(aero_reflect_gen src/main.cpp)
target_compile_features(aero_reflect_gen PRIVATE cxx_std_20)
target_link_libraries(aero_reflect_gen PRIVATE ${_aero_libclang})

# libclang lives in a non-standard prefix (brew keg / /usr/lib/llvm-18 / the LLVM installer dir); make
# the tool find it at runtime. mac/linux: bake the libdir into the build-tree rpath (never installed
# system-wide, spec E7). windows: no rpath mechanism — copy libclang.dll beside the .exe POST_BUILD,
# mirroring aero_shaderc's precedent (tools/shaderc/CMakeLists.txt).
if(WIN32)
    add_custom_command(TARGET aero_reflect_gen POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AERO_LLVM_ROOT}/bin/libclang.dll" "$<TARGET_FILE_DIR:aero_reflect_gen>"
        VERBATIM)
else()
    set_target_properties(aero_reflect_gen PROPERTIES BUILD_RPATH "${AERO_LLVM_ROOT}/lib")
endif()
