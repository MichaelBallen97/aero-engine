# tools/shaderc/bootstrap.cmake — task 0.4.3, spec D3/§3.2.
#
# Acquires SDL_shadercross (pinned commit 1ca46e0e..., vendored DXC + SPIRV-Cross) from source into
# a per-user, per-machine shared prefix, then exports AERO_SHADERCROSS_PREFIX +
# AERO_SHADERCROSS_SDL3_PREFIX to the includer (tools/shaderc/CMakeLists.txt). This is a plain
# `include()`, not `add_subdirectory()` — every `set()` below lands directly in the caller's
# directory scope, so no PARENT_SCOPE tricks are needed to export them.
#
# Why from source, and why identical on all three hosts: see docs/specs/0.4.3-tools-shaderc.md F1-F5
# (local-only, gitignored) and the CLAUDE.md 0.4.3 entry. Short version: vcpkg's sdl3-shadercross port
# depends unconditionally on directx-dxc, which vcpkg marks unsupported on arm64-osx — a real dry-run
# proves it. Building upstream's own vendored (DXC-from-source) configuration is the one path their
# own CI tests on macOS, so it is used symmetrically everywhere: vcpkg.json and the /vcpkg submodule
# stay byte-untouched.
#
# The sub-build is an ISOLATED child `cmake` process (execute_process), never add_subdirectory(): our
# per-preset ASan/UBSan compile flags (cmake/sanitizers.cmake, applied via add_compile_options at
# directory scope) must never reach an LLVM-scale third-party build, and the toolchain is built once,
# Release, regardless of which preset triggered it.
#
# DEVIATION FROM THE ORIGINAL PLAN (recorded here, not just in the PR description, so it survives a
# fresh read of this file): the plan assumed our project's own vcpkg-installed SDL3 could satisfy
# SDL_shadercross's `-DSDLSHADERCROSS_SHARED=ON` build (which upstream's CMakeLists.txt hard-requires
# the `SDL3::SDL3-shared` component for). Proven false empirically: vcpkg's `arm64-osx` AND `x64-linux`
# triplets both default to VCPKG_LIBRARY_LINKAGE=static (only `x64-windows` is dynamic by default), so
# our own SDL3 install exposes no shared variant on two of the three hosts. The fix (steps 2a below):
# acquire a SEPARATE, private, dynamic-triplet SDL3 via a classic-mode install of the SAME pinned
# vcpkg tree (no vcpkg.json / manifest / submodule change — this never touches manifest mode), used
# ONLY to satisfy this one sub-build's link requirement. `aero_shaderc` itself must then link that
# SAME dynamic SDL3 (not our engine's static one) so `SDL_GetError()` reads the correct instance's
# thread-local error state — proven empirically with a throwaway consumer probe (mixing a static and
# a dynamic SDL3 in one process would silently read the WRONG error string). This stays entirely
# inside tools/shaderc/ and touches none of the plan's untouchables.
#
# CORRECTED REALITY vs. the spec's F6 ("the built SDL3_shadercross shared library is self-contained,
# no dxcompiler runtime file"): that is EMPIRICALLY FALSE at this pin. `cmake --install` genuinely
# installs `libdxcompiler`, `libdxil`, and `libspirv-cross-c-shared` as real, separate shared objects
# alongside `libSDL3_shadercross` in the prefix's `lib/` (proven by inspecting the installed prefix and
# `otool -L`/`ldd` on the installed library — it is dynamically linked against its siblings, not
# statically self-contained). The toolchain is therefore self-contained only at the PREFIX level (every
# sibling a given library needs lives in that same prefix's `lib/`), never at the single-file level —
# each installed `.dylib`/`.so` must be able to locate its siblings in that same directory at load time.
# macOS gets this for free (Step 5's rpath story below + `aero_shaderc`'s own `BUILD_RPATH`, since dyld
# resolves a dependent dylib's `@rpath` against the *main executable's* `LC_RPATH`). Linux's
# `DT_RUNPATH` is **not** transitive the same way — it only resolves the immediate object's own direct
# deps — so `libSDL3_shadercross.so` needs its OWN `$ORIGIN`-relative `DT_RUNPATH` baked in at its own
# install time to find `libdxcompiler.so`/`libspirv-cross-c-shared.so` beside it; Step 5 sets that via
# `CMAKE_INSTALL_RPATH=$ORIGIN` on the Linux sub-configure (code-review finding, task 0.4.3).

# --- Step 1: resolve the per-user cache root + the pinned-commit prefix. ------------------------

# The exact SDL_shadercross commit the pinned vcpkg baseline's sdl3-shadercross port pins (verified
# against vcpkg/ports/sdl3-shadercross/portfile.cmake's `REF`). Bumping this SHA is how the toolchain
# is ever upgraded (H4 in the spec) — nothing else in this file should need to change for a same-shape
# bump.
set(_aero_shadercross_sha "1ca46e0ef7a9e50c706e7be6ef73ce467bac3b2e")
set(_aero_shadercross_sha12 "1ca46e0ef7a9")

if(NOT DEFINED AERO_SHADER_TOOLS_ROOT OR AERO_SHADER_TOOLS_ROOT STREQUAL "")
    if(WIN32)
        if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
            set(_aero_default_root "$ENV{LOCALAPPDATA}/aero-engine/shadercross")
        else()
            # Fallback for the rare environment without LOCALAPPDATA set.
            set(_aero_default_root "$ENV{USERPROFILE}/AppData/Local/aero-engine/shadercross")
        endif()
    else()
        # macOS and Linux share one convention (spec D3): honor XDG_CACHE_HOME, else ~/.cache.
        if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
            set(_aero_default_root "$ENV{XDG_CACHE_HOME}/aero-engine/shadercross")
        else()
            set(_aero_default_root "$ENV{HOME}/.cache/aero-engine/shadercross")
        endif()
    endif()
    set(AERO_SHADER_TOOLS_ROOT "${_aero_default_root}" CACHE PATH
        "Root cache directory for the bootstrapped SDL_shadercross toolchain (task 0.4.3)")
endif()

set(_aero_prefix "${AERO_SHADER_TOOLS_ROOT}/${_aero_shadercross_sha12}")
set(_aero_stamp_file "${_aero_prefix}/aero-stamp")

# --- Plan §C.2: derive the SDL3 prefix the sub-build's find_package(SDL3) must see from the vcpkg
# variables the toolchain file sets during OUR configure — never a hardcoded triplet (it would rot
# on Windows/Linux and on other Mac architectures). VCPKG_INSTALLED_DIR is the untripleted root
# (default ${CMAKE_BINARY_DIR}/vcpkg_installed); the triplet subdirectory is what find_package needs.
if(DEFINED VCPKG_INSTALLED_DIR AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL ""
   AND DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    set(_aero_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
else()
    set(_aero_vcpkg_prefix "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
endif()
message(STATUS "aero_shaderc bootstrap: resolved vcpkg SDL3 prefix = ${_aero_vcpkg_prefix}")
if(NOT EXISTS "${_aero_vcpkg_prefix}/share/sdl3/SDL3Config.cmake")
    message(WARNING
        "aero_shaderc bootstrap: '${_aero_vcpkg_prefix}/share/sdl3/SDL3Config.cmake' was not found. "
        "The sub-build's find_package(SDL3) may fail to resolve. Check VCPKG_INSTALLED_DIR / "
        "VCPKG_TARGET_TRIPLET for this preset.")
endif()

# --- Step 1b: which SDL3 satisfies SDL_shadercross's SHARED build (see the deviation note above).
# Upstream's CMakeLists.txt hard-requires `SDL3::SDL3-shared` when SDLSHADERCROSS_SHARED=ON. If our
# own project's SDL3 was already built dynamic (only true for x64-windows among our 3 hosts today —
# arm64-osx and x64-linux both default to static), reuse it directly: no extra acquisition, no extra
# rpath entry, and Windows' existing E17 DLL-copy step already covers it. Otherwise a private,
# dynamic-triplet SDL3 is acquired below (step 2a, cold path only) and this path is pre-computed here
# so it is available on BOTH the warm short-circuit and the cold path without ever invoking vcpkg on
# a warm configure (AC-14: a warm reconfigure must stay network-free and near-instant).
set(_aero_sdl3_already_dynamic FALSE)
# SDL3sharedTargets.cmake is the file vcpkg's sdl3 port installs only for a dynamic-linkage triplet
# build (verified on disk at this pin: present for a private arm64-osx-dynamic install, absent for the
# static-default arm64-osx/x64-linux triplets) — code review confirmed this detection is correct and
# x64-windows (already dynamic) never falls through to the nonexistent "x64-windows-dynamic" triplet.
if(EXISTS "${_aero_vcpkg_prefix}/share/sdl3/SDL3sharedTargets.cmake")
    set(_aero_sdl3_already_dynamic TRUE)
endif()
set(_aero_dynamic_triplet "${VCPKG_TARGET_TRIPLET}-dynamic")
if(_aero_sdl3_already_dynamic)
    set(AERO_SHADERCROSS_SDL3_PREFIX "${_aero_vcpkg_prefix}")
else()
    set(AERO_SHADERCROSS_SDL3_PREFIX "${_aero_prefix}-sdl3-dynamic/${_aero_dynamic_triplet}")
endif()

# Code-review finding 3: the warm-short-circuit stamp (below) lives inside ${_aero_prefix} and only
# ever attests to THAT directory's contents — it says nothing about the sibling private dynamic-SDL3
# prefix AERO_SHADERCROSS_SDL3_PREFIX also points at (a SEPARATE directory tree, ${_aero_prefix}-sdl3-
# dynamic). If that sibling is deleted by hand (or never existed because the triplet configuration
# changed) while the stamp file survives, a warm short-circuit would hand the includer a dangling
# prefix. Checked once, up front, and folded into BOTH warm-check conditions below; SDL3Config.cmake
# is present in every vcpkg SDL3 install (the already-dynamic branch above already depends on files
# under this same share/sdl3/ directory existing, so this check is free/consistent either way).
if(EXISTS "${AERO_SHADERCROSS_SDL3_PREFIX}/share/sdl3/SDL3Config.cmake")
    set(_aero_sdl3_prefix_ok TRUE)
else()
    set(_aero_sdl3_prefix_ok FALSE)
endif()

# The stamp's comparison line: bumping either the pinned SHA (above) or any sub-build option below
# invalidates every existing prefix on next configure. The generator and the dynamic-SDL3 acquisition
# strategy are included because a stale build tree / a stale private-SDL3 prefix from a different
# generator or a different fix revision cannot be reused in place. LINUXRPATH=1 (code review): a prefix
# built before the Linux $ORIGIN install-rpath fix (Step 5) lacks it on its installed libraries and
# must be rebuilt, not silently reused warm.
set(_aero_option_set
    "SHARED=ON;STATIC=OFF;CLI=ON;VENDORED=ON;BUILD_TYPE=Release;GENERATOR=${CMAKE_GENERATOR};SDL3DYNAMIC=2;LINUXRPATH=1")
string(MD5 _aero_option_hash "${_aero_option_set}")
set(_aero_stamp_expected "${_aero_shadercross_sha}:${_aero_option_hash}")

# --- Step 2: warm short-circuit — no network, no lock needed to just read a file. ----------------
if(EXISTS "${_aero_stamp_file}")
    file(READ "${_aero_stamp_file}" _aero_stamp_content)
    string(REGEX MATCH "^[^\n]*" _aero_stamp_line "${_aero_stamp_content}")
    if(_aero_stamp_line STREQUAL "${_aero_stamp_expected}" AND _aero_sdl3_prefix_ok)
        message(STATUS "shadercross toolchain: warm at ${_aero_prefix}")
        set(AERO_SHADERCROSS_PREFIX "${_aero_prefix}")
        return()
    endif()
endif()

message(STATUS "shadercross toolchain: cold — bootstrapping SDL_shadercross @ ${_aero_shadercross_sha12} "
               "into ${_aero_prefix} (from-source vendored DXC build; ~10-20 min locally, needs network). "
               "Set -DAERO_SHADER_TOOLS=OFF to skip shader tooling entirely.")

# --- Step 3: file(LOCK) — two presets / worktrees configuring concurrently must not race the same
# prefix (E13). GUARD PROCESS ties release to this cmake process exiting (normally OR via
# FATAL_ERROR), so a failed bootstrap never leaves a stale lock behind.
file(MAKE_DIRECTORY "${AERO_SHADER_TOOLS_ROOT}")
file(LOCK "${_aero_prefix}.lock" GUARD PROCESS TIMEOUT 3600 RESULT_VARIABLE _aero_lock_result)
if(NOT _aero_lock_result STREQUAL "0")
    message(FATAL_ERROR
        "aero_shaderc bootstrap: failed to acquire the toolchain build lock at "
        "'${_aero_prefix}.lock' (${_aero_lock_result}). If this is stale (a previous configure was "
        "killed), remove the .lock file and retry. To skip shader tooling, reconfigure with "
        "-DAERO_SHADER_TOOLS=OFF.")
endif()

# Double-checked: a concurrent configure may have finished the build while we waited for the lock.
# _aero_sdl3_prefix_ok is re-checked fresh here too (not just reused from before the lock wait) —
# a concurrent configure could have finished populating the sibling prefix while we waited.
if(EXISTS "${AERO_SHADERCROSS_SDL3_PREFIX}/share/sdl3/SDL3Config.cmake")
    set(_aero_sdl3_prefix_ok TRUE)
else()
    set(_aero_sdl3_prefix_ok FALSE)
endif()
if(EXISTS "${_aero_stamp_file}")
    file(READ "${_aero_stamp_file}" _aero_stamp_content2)
    string(REGEX MATCH "^[^\n]*" _aero_stamp_line2 "${_aero_stamp_content2}")
    if(_aero_stamp_line2 STREQUAL "${_aero_stamp_expected}" AND _aero_sdl3_prefix_ok)
        message(STATUS "shadercross toolchain: warm at ${_aero_prefix} (built by a concurrent configure)")
        set(AERO_SHADERCROSS_PREFIX "${_aero_prefix}")
        return()
    endif()
endif()

# A stale partial attempt (killed mid-build) must not confuse the clone/configure below.
file(REMOVE_RECURSE "${_aero_prefix}-src" "${_aero_prefix}-bld")

# Every step below FATAL_ERRORs naming the failing step + the AERO_SHADER_TOOLS=OFF escape hatch
# (D1/E14) — this is what makes an offline machine fail fast with an actionable message instead of
# hanging or half-configuring.
macro(_aero_shadercross_run description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _aero_step_result)
    if(NOT _aero_step_result EQUAL 0)
        message(FATAL_ERROR
            "aero_shaderc bootstrap: step '${description}' failed (exit code ${_aero_step_result}). "
            "See the command output above. This step needs network access to "
            "https://github.com/libsdl-org/SDL_shadercross.git and its submodules. If you are "
            "offline, or want to skip shader tooling entirely, reconfigure with "
            "-DAERO_SHADER_TOOLS=OFF.")
    endif()
endmacro()

# --- Step 2a: acquire a private, dynamic-triplet SDL3 if our own project's SDL3 is static-only (see
# the deviation note at the top of this file). Uses the SAME pinned vcpkg tool + baseline as our own
# manifest install — a classic-mode (non-manifest) call, so vcpkg.json is never consulted or touched.
# The working directory is deliberately OUTSIDE the repo so vcpkg's upward search for a vcpkg.json
# manifest never finds this repo's manifest and switches modes.
if(NOT _aero_sdl3_already_dynamic)
    set(_aero_vcpkg_exe "${CMAKE_SOURCE_DIR}/vcpkg/vcpkg")
    if(WIN32)
        set(_aero_vcpkg_exe "${CMAKE_SOURCE_DIR}/vcpkg/vcpkg.exe")
    endif()
    if(NOT EXISTS "${_aero_vcpkg_exe}")
        message(FATAL_ERROR
            "aero_shaderc bootstrap: '${_aero_vcpkg_exe}' not found. It should already have been "
            "bootstrapped by this project's own vcpkg toolchain file. To skip shader tooling, "
            "reconfigure with -DAERO_SHADER_TOOLS=OFF.")
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/triplets/community/${_aero_dynamic_triplet}.cmake"
       AND NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/triplets/${_aero_dynamic_triplet}.cmake")
        message(FATAL_ERROR
            "aero_shaderc bootstrap: no '${_aero_dynamic_triplet}' vcpkg triplet is available to "
            "build a dynamic SDL3 for the SDL_shadercross sub-build (needed because "
            "'${VCPKG_TARGET_TRIPLET}' is static-only). To skip shader tooling, reconfigure with "
            "-DAERO_SHADER_TOOLS=OFF.")
    endif()
    message(STATUS "aero_shaderc bootstrap: acquiring a private dynamic SDL3 (${_aero_dynamic_triplet}) "
                   "for the SDL_shadercross sub-build — '${VCPKG_TARGET_TRIPLET}' has no shared variant")
    # --classic is defense-in-depth on top of WORKING_DIRECTORY already being outside the repo
    # (empirically confirmed necessary: vcpkg checks the PROCESS'S CWD for a vcpkg.json and refuses
    # classic-style port arguments the instant it finds one, regardless of --x-install-root).
    _aero_shadercross_run("vcpkg install sdl3:${_aero_dynamic_triplet}"
        "${_aero_vcpkg_exe}" install "sdl3" "--triplet=${_aero_dynamic_triplet}" --classic
            "--x-install-root=${_aero_prefix}-sdl3-dynamic"
        WORKING_DIRECTORY "${AERO_SHADER_TOOLS_ROOT}")
endif()

# --- Step 4: clone at the exact pinned commit + its four vendored submodules (F5). ---------------
_aero_shadercross_run("git clone"
    git clone --no-checkout https://github.com/libsdl-org/SDL_shadercross.git "${_aero_prefix}-src")
_aero_shadercross_run("git checkout ${_aero_shadercross_sha12}"
    git -C "${_aero_prefix}-src" checkout "${_aero_shadercross_sha}")
_aero_shadercross_run("git submodule update --init --recursive"
    git -C "${_aero_prefix}-src" submodule update --init --recursive)

# --- Step 5: sub-configure/build/install, Release, vendored DXC + SPIRV-Cross (spec §3.2 step 5).
# -G matches our own generator (every preset in this repo pins Ninja) rather than letting the host
# pick its own default, so the sub-build behaves the same way our own build does.
#
# macOS-only, conditional: this dev machine's default SDK (MacOSX26.sdk-family, AppleClang 21) rejects
# the vendored LLVM/DXC-vintage code with `-Winvalid-specialization` (libc++'s newer
# `_Clang::__no_specializations__` hardening on `std::is_nothrow_constructible`, which predates this
# LLVM snapshot). CI's macos-15 runner ships an older, unaffected Xcode and will never hit this, so the
# override only applies (and only CAN apply) when the older SDK is actually present locally — see the
# 0.1.6 clang-tidy MacOSX15.4.sdk precedent (same root cause class, different tool).
set(_aero_extra_sub_configure_args)
if(APPLE AND EXISTS "/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk")
    list(APPEND _aero_extra_sub_configure_args
        "-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk")
endif()

# Linux-only, conditional (code-review finding, task 0.4.3): the installed `libSDL3_shadercross.so`
# is a real runtime dependent of its installed vendored siblings (`libdxcompiler.so`,
# `libspirv-cross-c-shared.so`, `libdxil.so`) in the SAME prefix `lib/` — this is the "corrected
# reality" noted at the top of this file (spec F6's "self-contained" claim does not hold at this pin).
# On macOS this resolves for free: dyld resolves a dependent dylib's bare `@rpath` reference against
# the *loading executable's* `LC_RPATH` (aero_shaderc's own `BUILD_RPATH`, tools/shaderc/CMakeLists.txt),
# and that lookup is effectively transitive for our one-hop dependency graph. Linux's ELF equivalent,
# `DT_RUNPATH`, is explicitly NOT transitive — it resolves only the DIRECT dependencies of the object
# that carries it, so `libSDL3_shadercross.so`'s own `DT_RUNPATH` (or lack of one) is what governs
# whether IT can find `libdxcompiler.so` etc., not `aero_shaderc`'s. Baking `$ORIGIN` into every library
# THIS sub-build installs (via the global `CMAKE_INSTALL_RPATH`, which `install(TARGETS ...)` uses as
# the default installed RPATH unless a target sets its own) makes each installed `.so` resolve its own
# siblings via ITS OWN directory — exactly closing that non-transitivity gap, with no
# `--disable-new-dtags` needed (we WANT `DT_RUNPATH`, which correctly does not leak to further
# transitive consumers). `libSDL3.so` is unaffected either way: `aero_shaderc` links it directly by raw
# path (tools/shaderc/CMakeLists.txt), never as a transitive dependency of `libSDL3_shadercross.so`.
if(UNIX AND NOT APPLE)
    list(APPEND _aero_extra_sub_configure_args "-DCMAKE_INSTALL_RPATH=$ORIGIN")
endif()

_aero_shadercross_run("sub-configure"
    ${CMAKE_COMMAND}
        -S "${_aero_prefix}-src"
        -B "${_aero_prefix}-bld"
        -G "${CMAKE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Release
        ${_aero_extra_sub_configure_args}
        -DSDLSHADERCROSS_VENDORED=ON
        -DSDLSHADERCROSS_SHARED=ON
        -DSDLSHADERCROSS_STATIC=OFF
        -DSDLSHADERCROSS_CLI=ON
        -DSDLSHADERCROSS_INSTALL=ON
        "-DCMAKE_INSTALL_PREFIX=${_aero_prefix}"
        "-DCMAKE_PREFIX_PATH=${AERO_SHADERCROSS_SDL3_PREFIX}")
_aero_shadercross_run("sub-build"
    ${CMAKE_COMMAND} --build "${_aero_prefix}-bld" --config Release --parallel)
_aero_shadercross_run("sub-install"
    ${CMAKE_COMMAND} --install "${_aero_prefix}-bld" --config Release)

# --- Step 6: Windows only — SDL3.dll must sit beside SDL3_shadercross.dll for the tool to load it
# (E17); mac/linux instead bake an rpath onto aero_shaderc itself (tools/shaderc/CMakeLists.txt).
if(WIN32)
    set(_aero_sdl3_dll "${AERO_SHADERCROSS_SDL3_PREFIX}/bin/SDL3.dll")
    if(EXISTS "${_aero_sdl3_dll}")
        file(COPY "${_aero_sdl3_dll}" DESTINATION "${_aero_prefix}/bin")
    else()
        message(WARNING "aero_shaderc bootstrap: '${_aero_sdl3_dll}' not found — SDL3_shadercross.dll "
                        "may fail to load at runtime.")
    endif()
endif()

# --- Step 7: stamp, then reclaim disk — the prefix is fully reproducible from the pin, so only the
# installed artifacts (tens of MB) persist and get CI-cached (D10); the multi-GB source+build trees
# (E15) do not. The private dynamic-SDL3 prefix (if any) is NOT deleted here: aero_shaderc's own
# find_package(SDL3) needs it every configure, not just this one-time bootstrap.
string(TIMESTAMP _aero_stamp_date UTC)
file(WRITE "${_aero_stamp_file}" "${_aero_stamp_expected}\n# built ${_aero_stamp_date} UTC\n")
file(REMOVE_RECURSE "${_aero_prefix}-src" "${_aero_prefix}-bld")

message(STATUS "shadercross toolchain: bootstrapped at ${_aero_prefix}")
set(AERO_SHADERCROSS_PREFIX "${_aero_prefix}")
