# engine/platform/ — the SDL3 wrapper layer (docs/03): window, event pump, input, audio device.
# The FIRST engine layer above core, and the FIRST engine target to link a vcpkg package (SDL3)
# directly. Depends on core; may depend on NO layer above it, and never on editor/ (golden rule).
#
# BOUNDARY RULE (docs/04, project rule #3): no SDL/miniaudio type crosses a public engine header.
# Both are linked PRIVATE and used ONLY in src/platform.cpp (SDL) and src/{audio_device.cpp,
# miniaudio_impl.c} (miniaudio). Two guards enforce it, not convention: the `lint` job's
# check-platform-boundary.sh over engine/*/include, and the compile-time probe
# tests/platform_boundary_probe.cpp (which links only aero::platform).

find_package(SDL3 CONFIG REQUIRED)   # package SDL3, target SDL3::SDL3

# miniaudio (task 0.3.3; ADR-006). The vcpkg port is HEADER-ONLY: it installs only miniaudio.h, with no
# CMake config and no prebuilt lib. So (a) we compile the implementation ourselves in ONE C TU
# (src/miniaudio_impl.c — see D5) and (b) we locate the header explicitly and add it PRIVATE, so it never
# reaches an aero_platform consumer (the boundary rule; keeps tests/platform_boundary_probe.cpp a real
# compile-time guard for miniaudio, exactly as it is for SDL).
find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

add_library(aero_platform STATIC
    src/platform.cpp
    src/input.cpp        # task 0.3.2 — InputState fold (SDL-free)
    src/audio_device.cpp # task 0.3.3 — AudioDevice wrapper (miniaudio DECLARATIONS only)
    src/miniaudio_impl.c # task 0.3.3 — the ONE MINIAUDIO_IMPLEMENTATION unit (C; unlinted by extension)
)
add_library(aero::platform ALIAS aero_platform)

# PUBLIC: consumers see <aero/platform/context.hpp>, window.hpp, event.hpp, audio.hpp, platform.hpp.
target_include_directories(aero_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# PRIVATE: miniaudio.h is used by aero_platform's own TUs only and must not propagate to consumers.
target_include_directories(aero_platform PRIVATE ${MINIAUDIO_INCLUDE_DIR})

# miniaudio's always-required system libs per OS. Default runtime-linking pulls ALSA/PulseAudio via
# dlopen, so NO audio -dev packages are needed to build (hence no ci.yml apt change). The native backend
# is still compiled per OS (so a dev machine can open a real silent device), which is why these are
# needed at final-link time even though CI only ever opens the null backend.
#   macOS  : CoreFoundation + CoreAudio + AudioToolbox frameworks
#   Linux  : pthread + libdl + libm
#   Windows: none (miniaudio uses LoadLibrary at runtime)
if(APPLE)
    target_link_libraries(aero_platform PRIVATE
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(aero_platform PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# aero::core is PUBLIC — public platform headers surface engine types and the impl logs through
# <aero/core/log.hpp>; consumers (and the boundary probe) get core's include root transitively.
# core's own third-party backends are PRIVATE-to-core, so this propagates NO vcpkg headers.
#
# SDL3::SDL3 and aero::profiling are PRIVATE. For a STATIC library CMake propagates a PRIVATE link
# as $<LINK_ONLY:...>: the archive reaches the final binary, but the usage requirements — SDL's
# headers AND vcpkg's shared per-triplet include/ root; Tracy's headers — do NOT reach consumers.
# That is exactly what keeps SDL out of every aero::platform consumer, and what makes
# tests/platform_boundary_probe.cpp a real compile-time guard. Flipping SDL3 to PUBLIC would reopen
# the boundary engine-wide — do not "simplify" it. (Same reasoning as engine/core/CMakeLists.txt.)
# miniaudio's PRIVATE include above is the same discipline: its usage requirements do not reach
# consumers or the boundary probe.
target_link_libraries(aero_platform
    PUBLIC  aero::core
    PRIVATE SDL3::SDL3 aero::profiling
)

# The INTERNAL native-handle seam (task 0.4.2): a header-only INTERFACE target exposing
# engine/platform/internal/ to the ONE consumer allowed to see native handles — aero_rhi's backend
# TU (linked PRIVATE there). It is not installed, not in aero_platform's PUBLIC include path, and
# outside the engine/*/include/* glob the boundary script scans. aero_platform itself needs the
# directory PRIVATE only to compile the accessor's definition in src/platform.cpp.
add_library(aero_platform_internal INTERFACE)
add_library(aero::platform_internal ALIAS aero_platform_internal)
target_include_directories(aero_platform_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/internal)
target_include_directories(aero_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/internal)
