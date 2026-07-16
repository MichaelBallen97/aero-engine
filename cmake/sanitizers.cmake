# cmake/sanitizers.cmake — ASan/UBSan wiring (ADR-001 mitigation #3; docs/04).
# Enabled by the *-debug presets via AERO_ENABLE_SANITIZERS (OFF by default).
# Included from the root CMakeLists BEFORE any add_subdirectory(), so the
# directory-scope flags below reach every project target. vcpkg dependencies
# are NOT affected (they build in their own toolchain invocation); ASan
# tolerates uninstrumented code, so linking them stays safe.

if(NOT AERO_ENABLE_SANITIZERS)
    return()
endif()

if(MSVC)
    # MSVC ships ASan only — /fsanitize=undefined does not exist (spec 0.1.4
    # §5a). ASan is incompatible with /RTC1 (present in CMake's default Debug
    # flags) and with incremental linking, so strip the former and force the
    # latter off. MSVC's linker honors the LAST /INCREMENTAL option, and
    # add_link_options() lands after the built-in Debug link flags.
    add_compile_options(/fsanitize=address)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")   # task 0.3.3: miniaudio_impl.c
    add_link_options(/INCREMENTAL:NO)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # AppleClang matches "Clang". -fno-sanitize-recover=all is load-bearing:
    # without it UBSan prints its report and exits 0, and CI stays green
    # (spec 0.1.4 §5e). Frame pointers keep ASan stacks readable.
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(-fsanitize=address,undefined)
else()
    # Mitigations are enforced, not encouraged (docs/04): an unmapped compiler
    # must fail loudly rather than silently build unsanitized.
    message(FATAL_ERROR "AERO_ENABLE_SANITIZERS=ON but compiler '${CMAKE_CXX_COMPILER_ID}' has no sanitizer mapping (cmake/sanitizers.cmake)")
endif()
