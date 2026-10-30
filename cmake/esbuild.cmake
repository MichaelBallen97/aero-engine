# cmake/esbuild.cmake -- task 4.3.1 (spec D2). The editor transpiles a project's TypeScript with a pinned esbuild,
# fetched here once per machine and verified against the SHA-512 table below. Ungated: both reduced configurations
# fetch it too. Editor-time only: nothing under engine/ or runtime/ names it, and only aero_editor gets a copy.
set(AERO_ESBUILD_VERSION "0.28.2")

# The four desktop hosts (docs/00). Values: npm platform, member inside the tarball, executable name, sha512 of the
# tarball (npm dist.integrity, decoded to hex, re-verified locally at 4.3.1).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_aero_esbuild_platform "darwin-arm64")
    set(_aero_esbuild_sha512 "9f82aa90e42badac4725c82333546fc1b8a07d028828956933bc69f8ab318b2512ad17485e7b7bdd586b3c0c747d5e388607e62152a3c4c37d275b798f249593")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_aero_esbuild_platform "darwin-x64")
    set(_aero_esbuild_sha512 "baaeacb885983f7eeacc675d04a3f0e501103e2e8788b1ac3bb5269297f2698350dc3fab37ac3a59fc07fa7baa7065d6bdac06c31384ae83b86069c587de5acf")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(_aero_esbuild_platform "win32-x64")
    set(_aero_esbuild_sha512 "e5e6e9c6bde758cceb2ffae7508ef9e4992e79ed1b1cbfc6ab459317d96f729bfbdf0029e5ebbc31f0548162bd6e15af6638fbc97f1eb5fffcb48f0d7b2ebde6")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_aero_esbuild_platform "linux-x64")
    set(_aero_esbuild_sha512 "e314d9af5154992a105b85c85a68addedcd0ad44d933e3773f45d5f3144e298179d177c8ef178ef74fb56d9bcdc3122e7d0f610d0551247e588603d517c03f1d")
else()
    message(FATAL_ERROR "esbuild ${AERO_ESBUILD_VERSION}: no pinned binary for host "
        "'${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}'. The supported hosts are the table at the top of "
        "cmake/esbuild.cmake: darwin-arm64, darwin-x64, win32-x64, linux-x64.")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(AERO_ESBUILD_EXE_NAME "esbuild.exe")
    set(_aero_esbuild_member "package/esbuild.exe")
else()
    set(AERO_ESBUILD_EXE_NAME "esbuild")
    set(_aero_esbuild_member "package/bin/esbuild")
endif()

# The per-account cache root: the SAME convention tools/shaderc/bootstrap.cmake resolves for shadercross, restated here
# because that file is included only with AERO_SHADER_TOOLS=ON. Change the two together.
if(NOT DEFINED AERO_ESBUILD_ROOT OR AERO_ESBUILD_ROOT STREQUAL "")
    if(WIN32)
        if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
            set(_aero_esbuild_default "$ENV{LOCALAPPDATA}/aero-engine/esbuild")
        else()
            set(_aero_esbuild_default "$ENV{USERPROFILE}/AppData/Local/aero-engine/esbuild")
        endif()
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_aero_esbuild_default "$ENV{XDG_CACHE_HOME}/aero-engine/esbuild")
    else()
        set(_aero_esbuild_default "$ENV{HOME}/.cache/aero-engine/esbuild")
    endif()
    file(TO_CMAKE_PATH "${_aero_esbuild_default}" _aero_esbuild_default)
    set(AERO_ESBUILD_ROOT "${_aero_esbuild_default}" CACHE PATH "Per-account cache for the pinned esbuild (task 4.3.1)")
endif()

set(_aero_esbuild_dir "${AERO_ESBUILD_ROOT}/${AERO_ESBUILD_VERSION}/${_aero_esbuild_platform}")
set(_aero_esbuild_tgz "${_aero_esbuild_dir}/${_aero_esbuild_platform}-${AERO_ESBUILD_VERSION}.tgz")
set(AERO_ESBUILD_EXECUTABLE "${_aero_esbuild_dir}/${AERO_ESBUILD_EXE_NAME}")
file(MAKE_DIRECTORY "${_aero_esbuild_dir}")
file(LOCK "${_aero_esbuild_dir}/aero.lock" GUARD FILE TIMEOUT 600 RESULT_VARIABLE _aero_esbuild_lock)
if(NOT _aero_esbuild_lock STREQUAL "0")
    message(FATAL_ERROR "esbuild: could not lock '${_aero_esbuild_dir}' -- ${_aero_esbuild_lock}")
endif()
if(NOT EXISTS "${AERO_ESBUILD_EXECUTABLE}")
    # EXPECTED_HASH skips the network when a matching tarball is already cached (measured), and a mismatch is a hard
    # configure error by itself, STATUS or not.
    set(_aero_esbuild_url
        "https://registry.npmjs.org/@esbuild/${_aero_esbuild_platform}/-/${_aero_esbuild_platform}-${AERO_ESBUILD_VERSION}.tgz")
    file(DOWNLOAD "${_aero_esbuild_url}" "${_aero_esbuild_tgz}"
         EXPECTED_HASH SHA512=${_aero_esbuild_sha512} TLS_VERIFY ON STATUS _aero_esbuild_status)
    list(GET _aero_esbuild_status 0 _aero_esbuild_code)
    if(NOT _aero_esbuild_code EQUAL 0)
        message(FATAL_ERROR "esbuild: downloading '${_aero_esbuild_url}' failed (${_aero_esbuild_status}). The first "
            "configure on a machine needs the network once; later configures are offline.")
    endif()
    set(_aero_esbuild_stage "${_aero_esbuild_dir}/extract")
    file(REMOVE_RECURSE "${_aero_esbuild_stage}")
    file(ARCHIVE_EXTRACT INPUT "${_aero_esbuild_tgz}" DESTINATION "${_aero_esbuild_stage}"
         PATTERNS "${_aero_esbuild_member}")
    file(RENAME "${_aero_esbuild_stage}/${_aero_esbuild_member}" "${AERO_ESBUILD_EXECUTABLE}")
    file(REMOVE_RECURSE "${_aero_esbuild_stage}")
endif()
file(LOCK "${_aero_esbuild_dir}/aero.lock" RELEASE)
message(STATUS "esbuild ${AERO_ESBUILD_VERSION} (${_aero_esbuild_platform}): ${AERO_ESBUILD_EXECUTABLE}")
