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

    # task 3.2.3: MSVC's STL stamps every object it compiles with an 'annotate_string'/
    # 'annotate_vector' record (1 with ASan on, 0 without), and the linker's /FAILIFMISMATCH
    # treats a disagreement between two objects in the same binary as LNK2038, not a warning.
    # vcpkg's prebuilt static libraries (tinyobjloader.lib first; see editor/src/text_input.hpp
    # for the identical mechanism hitting vcpkg's imguid.lib one TU earlier) are never built with
    # /fsanitize=address, so they always stamp 0 — a hard link failure the moment one lands in a
    # binary built from this project's own ASan-instrumented objects, which is every binary here.
    # Disabling the annotations project-wide makes OUR objects stamp 0 too, matching every vcpkg
    # prebuilt we will ever link.
    #
    # The cost, stated precisely and not overstated: MSVC's container annotations detect an
    # overread strictly BETWEEN size() and capacity() -- inside a container's own
    # allocated-but-unused region, memory ASan's ordinary redzone checks cannot flag because it
    # genuinely belongs to the heap allocation. Disabling the annotations loses only that one
    # class of bug, on the Windows Debug lane alone. Every out-of-allocation heap overflow is
    # unaffected on all three lanes -- including this task's own INV-O4 protection, where a bad
    # Wavefront index reads far past the end of the allocation and remains a plain
    # AddressSanitizer heap-buffer-overflow.
    #
    # Considered and rejected: compiling tinyobjloader in-tree, the way
    # editor/third_party/ufbx/CMakeLists.txt compiles ufbx.c, would restore full instrumentation
    # for the parser too. Rejected here because this project vendors only libraries with NO vcpkg
    # port available -- ufbx's own reason for being vendored -- and tinyobjloader has one. Left as
    # a future option, not a rejected idea: if a Wavefront parser bug ever needs container-level
    # instrumentation specifically, in-tree compilation is the move.
    add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
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
