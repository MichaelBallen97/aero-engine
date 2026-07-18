# tools/shaderc — task 0.4.3. First-party HLSL -> SPIR-V/MSL/DXIL compiler CLI; see README.md for
# the frozen contracts (argv grammar, JSON schema, aero_add_shaders()) and the raw-DXC fallback (R4).
#
# aero_shaderc links NO engine target (spec §3.3): toolchain lib + SDL3 + the standard library only,
# so tools/ never drags spdlog/vcpkg usage requirements into a build-time-only binary.
if(NOT AERO_SHADER_TOOLS)
    message(STATUS "aero_shaderc: skipped (AERO_SHADER_TOOLS=OFF) — no shader compilation, "
                   "no shaderc tests")
    return()
endif()

include(bootstrap.cmake)  # guarantees AERO_SHADERCROSS_PREFIX + AERO_SHADERCROSS_SDL3_PREFIX

find_package(SDL3_shadercross CONFIG REQUIRED PATHS "${AERO_SHADERCROSS_PREFIX}" NO_DEFAULT_PATH)

add_executable(aero_shaderc src/main.cpp)
target_compile_features(aero_shaderc PRIVATE cxx_std_20)
target_link_libraries(aero_shaderc PRIVATE SDL3_shadercross::SDL3_shadercross)

# SDL3 itself: aero_shaderc calls SDL_free/SDL_GetError directly (spec §3.3), so it needs SDL3 too,
# on top of SDL3_shadercross. It must resolve to the EXACT SAME SDL3 instance SDL3_shadercross links
# internally, or SDL_GetError() silently reads the wrong instance's thread-local error state (proven
# empirically with a throwaway consumer probe during implementation — DXC's real diagnostic text only
# comes back when both binaries share one SDL3).
#
# Deliberately NOT a second find_package(SDL3 ...) call: CMake's find_package Config mode reuses the
# FIRST successful resolution's cached <Package>_DIR for every later call regardless of a different
# PATHS/NO_DEFAULT_PATH (verified during implementation — CMake's own documented behavior), and
# engine/platform already resolved "SDL3" once by the time add_subdirectory(tools) runs. A second call
# here would therefore silently keep pointing at THAT resolution rather than AERO_SHADERCROSS_SDL3_PREFIX
# on any host where the two differ, and — worse — permanently clobbering the shared SDL3_DIR cache
# entry for whatever runs after tools/ in this same configure (tests/CMakeLists.txt calls
# find_package(SDL3 CONFIG REQUIRED) again for the white-box pump test).
if(_aero_sdl3_already_dynamic)
    # Our own project's SDL3 is already dynamic (only true for x64-windows among our 3 hosts today) —
    # reuse the SAME already-resolved target directly; AERO_SHADERCROSS_SDL3_PREFIX == the normal
    # vcpkg prefix in this branch, so there is nothing new to point at.
    target_link_libraries(aero_shaderc PRIVATE SDL3::SDL3)
else()
    # arm64-osx / x64-linux default to a static-only SDL3 build; bootstrap.cmake acquired a private
    # dynamic-triplet SDL3 for this exact purpose. Linked by raw path (not an imported target) so this
    # NEVER touches the global "SDL3" find_package cache entry.
    target_include_directories(aero_shaderc PRIVATE "${AERO_SHADERCROSS_SDL3_PREFIX}/include")
    target_link_libraries(aero_shaderc PRIVATE
        "${AERO_SHADERCROSS_SDL3_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}SDL3${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

if(WIN32)
    # Windows: no rpath mechanism — copy the runtime DLLs beside the tool instead (spec §3.8, E17).
    # SDL3_shadercross.dll (+ its vendored dxcompiler/spirv-cross dylibs, all installed flat into
    # AERO_SHADERCROSS_PREFIX/bin by the sub-build) and SDL3.dll (copied there by bootstrap.cmake
    # step 6) live in the same directory, so one glob-free copy covers all of them.
    add_custom_command(TARGET aero_shaderc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${AERO_SHADERCROSS_PREFIX}/bin" "$<TARGET_FILE_DIR:aero_shaderc>"
        VERBATIM)
else()
    # mac/linux: the toolchain libs are shared — bake both prefixes' lib/ into the build-tree rpath
    # (SDL3_shadercross + its vendored dxcompiler/spirv-cross siblings live in AERO_SHADERCROSS_PREFIX;
    # the dynamic SDL3 -- private or reused -- lives in AERO_SHADERCROSS_SDL3_PREFIX, which can be a
    # different directory on arm64-osx/x64-linux). Never installed system-wide (E17); build-tree-local.
    set_target_properties(aero_shaderc PROPERTIES
        BUILD_RPATH "${AERO_SHADERCROSS_PREFIX}/lib;${AERO_SHADERCROSS_SDL3_PREFIX}/lib")
endif()
