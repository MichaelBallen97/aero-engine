# tests/ — doctest unit tests (docs/04). Harness created by task 0.1.4.

find_package(doctest CONFIG REQUIRED)

# aero_tests links SDL3::SDL3 EXPLICITLY (not merely transitively via aero::platform) so the
# white-box pump test (platform_event_test.cpp) can #include <SDL3/SDL.h> and push synthetic SDL
# events through the real Context::pollEvent. Tests are OUTSIDE the boundary rule — docs/04 governs
# engine PUBLIC headers, not test TUs. This does not touch the boundary probes below (separate
# targets, each linking only its own engine library).
find_package(SDL3 CONFIG REQUIRED)

# One runner for the whole project: test TUs register TEST_CASEs, test_main.cpp
# only generates main(). This is the repo's first compiled+linked target.
add_executable(aero_tests
    test_main.cpp
    vulkan_stack_pin.cpp  # task 0.5.2 — Linux/LSan-only: pin the Vulkan loader stack mapped (no-op elsewhere)
    sanity_test.cpp
    profiler_test.cpp
    handle_test.cpp
    slot_map_test.cpp
    log_test.cpp
    math_test.cpp
    jobs_test.cpp
    time_test.cpp
    vfs_test.cpp
    platform_test.cpp
    platform_event_test.cpp
    input_test.cpp        # task 0.3.2 — black-box InputState (no SDL)
    audio_device_test.cpp # task 0.3.3 — black-box AudioDevice, null backend (no hardware)
    rhi_types_test.cpp    # task 0.4.1 — handle/desc/flag vocabulary (compile-time heavy, no GPU)
    rhi_format_test.cpp   # task 0.4.1 — format classification/size/toString (no GPU)
    rhi_device_test.cpp   # task 0.4.2 — tiers 0-1: lifecycle/resources/uploads/frame-flow/render-pass
    rhi_swapchain_test.cpp # task 0.4.2 — tier 2: real visible window, claim/acquire/present
    shader_loader_test.cpp       # task 0.4.4 — tier-0: parseShaderMetadata + selectArtifact (no GPU)
    rhi_shader_pipeline_test.cpp # task 0.4.4 — tier-1 (gated): load → valid pipeline on real GPU
    render_clear_test.cpp # task 0.5.1 — tier-0 config default + tier-2 clear/resize/move/leak
    render_cube_test.cpp   # task 0.5.2 — tier-0 mesh invariants + tier-2 textured-cube draw pipeline
)

# AC-9(i) (task 0.2.2): aero::core is linked; glm::glm deliberately IS NOT — that is intentional
# style, not a compile-time guarantee. Do not "fix" it by adding glm::glm here. (glm's ARCHIVE
# still reaches the link line via $<LINK_ONLY:> from aero_core's PRIVATE link — that is correct
# and carries no headers.)
#
# This target ALSO links doctest::doctest, which — via vcpkg's shared per-triplet include/
# directory — exports GLM's headers to every TU here regardless of what aero_core links (verified:
# a probe TU with this target's exact flags plus `#include <glm/vec3.hpp>` compiles clean). So a
# public math header pulling GLM in would NOT necessarily fail to compile here — this target does
# NOT prove the compile-time boundary the way an engine-layer target linking only aero::core does
# (verified with exactly that probe). See docs/02-adrs.md's ADR-005 implementation note and risk
# R12 in docs/08-risks.md; .github/scripts/check-math-boundary.sh (task 0.2.3, run by the `lint`
# job) is the real enforcement for tests/. The sibling aero_math_boundary_probe target below (also
# 0.2.3) covers the public math headers at compile time instead.
# ${CMAKE_DL_LIBS} (libdl on Linux, empty elsewhere): vulkan_stack_pin.cpp calls dlopen/dlerror
# directly to keep the Vulkan driver stack mapped for LSan (task 0.5.2).
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core aero::platform aero::render aero::rhi SDL3::SDL3 ${CMAKE_DL_LIBS})

# Task 0.4.4: the shader-loading tests need the compiled artifacts (aero_add_shaders -> aero_shaders,
# in build/<preset>/shaders). aero_shaders exists ONLY when AERO_SHADER_TOOLS is ON (cmake/shaders.cmake),
# and tests/ is add_subdirectory'd AFTER shaders/ (root CMakeLists.txt), so the target is in scope here.
# INVARIANT: AERO_SHADERS_DIR must always equal aero_add_shaders' OUTPUT_DIR (default
# ${CMAKE_BINARY_DIR}/shaders, used unchanged by shaders/CMakeLists.txt) -- move both together.
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_tests aero_shaders)  # artifacts built before ctest runs
    target_compile_definitions(aero_tests PRIVATE
        AERO_SHADER_TOOLS_ENABLED=1
        AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")  # MUST match aero_add_shaders' default OUTPUT_DIR (F9)
endif()

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)

# ---------------------------------------------------------------------------------------------
# The math-boundary COMPILE-TIME guard (task 0.2.3; docs/02-adrs.md:211).
#
# OBJECT, not an executable: compiling is the whole assertion, so there is nothing to link or run
# and no ctest entry to register. It builds as part of `all`, so every preset on every CI lane
# enforces it for free.
#
# THE LINK LINE IS THE TEST. aero::core links glm::glm PRIVATE, so a target that links ONLY
# aero::core never sees vcpkg's shared per-triplet include/ directory and cannot resolve
# <glm/...>. That is exactly what makes a leaked GLM include in a PUBLIC math header a compile
# error here (verified 2026-07-15: with GLM installed in the shared root, this flag set still
# reports `fatal error: 'glm/vec3.hpp' file not found`, while aero_tests' flag set compiles it
# clean — risk R12, docs/08-risks.md).
#
# DO NOT ADD ANY OTHER LINK LIBRARY. Not doctest::doctest, not aero::profiling (it links Tracy —
# a vcpkg package — in Release), not glm::glm. Any one of them puts the shared vcpkg include root
# back on this compile line and silently reduces this guard to a no-op WITHOUT turning CI red.
# That is the only way this guard can rot, so it is the one thing to never "simplify".
add_library(aero_math_boundary_probe OBJECT math_boundary_probe.cpp)
target_link_libraries(aero_math_boundary_probe PRIVATE aero::core)

# ---------------------------------------------------------------------------------------------
# The jobs-boundary COMPILE-TIME guard (task 0.2.5; docs/04 boundary rule).
#
# Same mechanism as aero_math_boundary_probe above, for a different backend: aero::core links
# enkiTS::enkiTS PRIVATE, so a target linking ONLY aero::core never sees vcpkg's shared per-triplet
# include/ directory and cannot resolve <TaskScheduler.h>. A leaked enkiTS include in a public header
# is therefore a compile error the moment it is written.
#
# A separate target from the math probe on purpose: each guard names its own backend, so a failure
# says which boundary broke. A future task may consolidate both into one aero_core_boundary_probe as
# 0.2.6 adds time/VFS headers — do NOT do that here; it is churn on a guard that just landed.
#
# DO NOT ADD ANY OTHER LINK LIBRARY — not doctest, not aero::profiling, not enkiTS::enkiTS. Any of
# them restores the shared vcpkg include root and silently reduces this guard to a no-op while CI
# stays green.
add_library(aero_jobs_boundary_probe OBJECT jobs_boundary_probe.cpp)
target_link_libraries(aero_jobs_boundary_probe PRIVATE aero::core)

# ---------------------------------------------------------------------------------------------
# The platform SDL-boundary COMPILE-TIME guard (task 0.3.1; docs/04 boundary rule).
#
# Same mechanism as the math/jobs probes, for SDL: aero::platform links SDL3 PRIVATE, so a target
# linking ONLY aero::platform never sees vcpkg's shared per-triplet include/ directory and cannot
# resolve <SDL3/...>. A leaked SDL include in a public platform header is therefore a compile error
# the moment it is written. (This CORRECTS the 0.2.5 handoff note that platform "must be greps":
# the probe works here exactly as for glm — spec D8.)
#
# DO NOT ADD ANY OTHER LINK LIBRARY — not doctest, not SDL3::SDL3, not aero::profiling. Any of them
# restores the shared vcpkg include root and silently reduces this guard to a no-op while CI stays
# green. That is the only way this guard can rot.
add_library(aero_platform_boundary_probe OBJECT platform_boundary_probe.cpp)
target_link_libraries(aero_platform_boundary_probe PRIVATE aero::platform)

# ---------------------------------------------------------------------------------------------
# The RHI SDL_GPU-boundary COMPILE-TIME guard (task 0.4.5; ADR-002 "the sacred wrapper").
#
# Same mechanism as the math/jobs/platform probes, for SDL_GPU: aero::rhi links SDL3 PRIVATE (and its
# PUBLIC deps aero::core/aero::platform propagate no vcpkg header), so a target linking ONLY aero::rhi
# never sees vcpkg's shared per-triplet include/ directory and cannot resolve <SDL3/SDL_gpu.h>. A
# leaked SDL include in a public rhi header is therefore a compile error the moment it is written
# (verified 2026-07-19: rhi.hpp's whole surface compiles with only aero::rhi/aero::core includes,
# while a seeded <SDL3/SDL_gpu.h> reports 'file not found' -- risk R12, docs/08-risks.md).
#
# The textual grep guard .github/scripts/check-rhi-boundary.sh (also 0.4.5, run by the `lint` job) is
# the enforcement that reaches tests/ and every engine/runtime .cpp, where R12 breaks this compile
# boundary. This probe holds the public-header half.
#
# DO NOT ADD ANY OTHER LINK LIBRARY -- not doctest, not SDL3::SDL3, not aero::profiling (Tracy, a
# vcpkg package, in Release). Any of them restores the shared vcpkg include root and silently reduces
# this guard to a no-op while CI stays green. That is the only way this guard can rot.
add_library(aero_rhi_boundary_probe OBJECT rhi_boundary_probe.cpp)
target_link_libraries(aero_rhi_boundary_probe PRIVATE aero::rhi)

# ---------------------------------------------------------------------------------------------
# The shaderc end-to-end CTest suite (task 0.4.3; spec D9/§3.6) -- a net-new harness (no prior
# cmake -P / external-binary add_test precedent in this repo). Each case runs the REAL aero_shaderc
# binary through tests/shaderc/run_case.cmake, which is what actually asserts exit codes -- several
# cases are NEGATIVE (the tool is expected to exit non-zero), and a raw `add_test(COMMAND
# aero_shaderc ...)` would treat any non-zero exit as a ctest FAILURE regardless of whether that was
# the expected behavior (see run_case.cmake's own header comment).
#
# No doctest TU for the tool, and no tool code links into aero_tests (spec D9) -- a CLI's honest test
# is its process boundary: argv in, exit code + files out.
#
# Gated on AERO_SHADER_TOOLS: when OFF, aero_shaderc is never built and these tests must not be
# registered (spec E18) -- the ctest count visibly shrinking is deliberate, not a bug.
if(AERO_SHADER_TOOLS)
    set(_aero_shaderc_cases
        help vert_happy frag_happy reflect_ubo subset_msl include_dir defines
        bad_syntax missing_input bad_stage unknown_flag double_include determinism
    )
    foreach(_aero_case IN LISTS _aero_shaderc_cases)
        add_test(
            NAME shaderc.${_aero_case}
            COMMAND "${CMAKE_COMMAND}"
                    -DTOOL=$<TARGET_FILE:aero_shaderc>
                    -DCASE=${_aero_case}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/shaderc/${_aero_case}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/shaderc/run_case.cmake
        )
    endforeach()
    unset(_aero_shaderc_cases)
endif()

# ---------------------------------------------------------------------------------------------
# The reflect-gen end-to-end CTest suite (task 1.1.1; plan/spec D9/§3.6) -- mirrors the shaderc
# harness immediately above: each case runs the REAL aero_reflect_gen binary through
# tests/reflect-gen/run_case.cmake, which is what actually asserts exit codes -- several cases are
# NEGATIVE (bad_syntax=2, missing_input=3, unknown_flag/double_input=1), and a raw
# `add_test(COMMAND aero_reflect_gen ...)` would treat any non-zero exit as a ctest FAILURE
# regardless of whether that was the expected behavior (see run_case.cmake's own header comment).
#
# No doctest TU for the tool, and no tool code links into aero_tests (spec D9) -- a CLI's honest test
# is its process boundary: argv in, exit code + stdout/stderr out.
#
# Gated on AERO_REFLECT_TOOLS: when OFF, aero_reflect_gen is never built and these tests must not be
# registered -- the ctest count visibly shrinking is deliberate (AC-10), not a bug.
if(AERO_REFLECT_TOOLS)
    # Per-OS clang flags libclang needs to find the C++ stdlib/SDK and its OWN builtin headers (spec
    # F4). Computed once here, where CMAKE_OSX_SYSROOT and AERO_LLVM_ROOT (a cache variable set by
    # tools/reflect-gen/CMakeLists.txt, which add_subdirectory(tools) already ran before
    # add_subdirectory(tests), root CMakeLists.txt) are both known; passed to the driver as a ;-list.
    set(_aero_reflect_clang_args -std=c++20)

    # -resource-dir: found while de-risking this task locally -- NOT anticipated by the plan/spec.
    # aero_reflect_gen is a standalone executable living OUTSIDE the LLVM install prefix, so libclang's
    # internal driver cannot locate its own resource directory (the compiler-builtin headers, e.g.
    # stdarg.h -- normally found relative to a REAL clang binary's own path) purely from argv[0]. Without
    # this flag, EVERY parse that includes anything -- even <cstdint> alone -- fails with
    # "'stdarg.h' file not found" and a cascade of "unknown type name ..." diagnostics, regardless of
    # -isysroot (verified: parse_engine_header fails exit 2 without it, exit 0 with it, isysroot held
    # constant). This is the standard LLVM install-tree layout (<prefix>/lib/clang/<major>/include) on
    # every platform's package (Homebrew, apt, the official Windows installer), so it is applied
    # unconditionally on all OSes, not just macOS.
    if(EXISTS "${AERO_LLVM_ROOT}/lib/clang/18")
        list(APPEND _aero_reflect_clang_args -resource-dir "${AERO_LLVM_ROOT}/lib/clang/18")
    else()
        file(GLOB _aero_reflect_resource_dirs LIST_DIRECTORIES true "${AERO_LLVM_ROOT}/lib/clang/*")
        if(_aero_reflect_resource_dirs)
            list(GET _aero_reflect_resource_dirs 0 _aero_reflect_resource_dir)
            message(STATUS "reflect-gen tests: LLVM 18's usual resource dir layout is absent; falling "
                           "back to ${_aero_reflect_resource_dir}")
            list(APPEND _aero_reflect_clang_args -resource-dir "${_aero_reflect_resource_dir}")
            unset(_aero_reflect_resource_dir)
        else()
            message(WARNING "reflect-gen tests: could not find a clang resource directory under "
                            "${AERO_LLVM_ROOT}/lib/clang -- parses that include any standard header will "
                            "fail. Check AERO_LLVM_ROOT.")
        endif()
        unset(_aero_reflect_resource_dirs)
    endif()

    if(APPLE)
        message(STATUS "reflect-gen tests: CMAKE_OSX_SYSROOT = ${CMAKE_OSX_SYSROOT}")
        # macOS-local contingency (plan C.1) -- REFINED after verifying live on this dev machine
        # (2026-07-21): gating this branch on "CMAKE_OSX_SYSROOT is truthy" (the literal C.1 wording)
        # turns out to be unsafe for THIS repo/machine combination -- CMAKE_OSX_SYSROOT is EMPTY here in
        # practice (CMake's Darwin platform initialization does not always repopulate it on an
        # incremental reconfigure of an existing build tree once compiler-ABI detection is already
        # cached), which would silently skip -isysroot entirely rather than "use the raw default" as
        # C.1 assumed. And even when CMAKE_OSX_SYSROOT (or clang's own auto-detected default) IS
        # populated, that default is Xcode's MacOSX26.sdk, too new for clang-18's libc++ headers on a
        # real parse (engine_types/parse_engine_header fail with libc++-internal errors even with
        # -resource-dir fixed). So this ALWAYS tries the pinned macosx15.4 SDK first, on every configure
        # -- not conditionally only when CMAKE_OSX_SYSROOT happens to already be set -- falling back to
        # CMAKE_OSX_SYSROOT (if non-empty) and then to omitting -isysroot (with a loud WARNING, never
        # silently) if neither resolves. CI's macos-15 runner ships a matching 15.x SDK already, so the
        # pinned attempt is expected to succeed there too (verify-at-implementation, spec §3.8).
        execute_process(COMMAND xcrun --sdk macosx15.4 --show-sdk-path
                        OUTPUT_VARIABLE _aero_reflect_sysroot
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                        RESULT_VARIABLE _aero_reflect_sysroot_result)
        if(_aero_reflect_sysroot_result EQUAL 0 AND EXISTS "${_aero_reflect_sysroot}")
            message(STATUS "reflect-gen tests: pinning -isysroot to ${_aero_reflect_sysroot}")
            list(APPEND _aero_reflect_clang_args -isysroot "${_aero_reflect_sysroot}")
        elseif(CMAKE_OSX_SYSROOT)
            list(APPEND _aero_reflect_clang_args -isysroot "${CMAKE_OSX_SYSROOT}")
        else()
            message(WARNING "reflect-gen tests: could not resolve a macOS SDK (neither the pinned "
                            "macosx15.4 SDK via xcrun, nor CMAKE_OSX_SYSROOT) -- parses of headers that "
                            "pull in libc++ may fail on clang's own auto-detected default SDK")
        endif()
        unset(_aero_reflect_sysroot)
        unset(_aero_reflect_sysroot_result)
    endif()
    # Linux: clang-18 auto-detects the Ubuntu-24.04 GCC-13 toolchain -- no extra flag needed (the Linux
    # CI lane parses handle.hpp and the engine headers green with bare -std=c++20 + -resource-dir).
    if(WIN32)
        # The windows-2022 runner's MSVC STL (14.44.x) hard-#errors "STL1000: Unexpected compiler
        # version" in <yvals_core.h> when parsed by a Clang older than it expects. clang-18 is older
        # than that STL's baseline, so even <cstdint> (which pulls <yvals_core.h>) fails to parse --
        # exit 2, verified on CI 2026-07-21 (ref actions/runner-images#8153).
        # _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH is MSVC's documented escape hatch that disables that
        # version guard: harmless everywhere, load-bearing on this lane.
        list(APPEND _aero_reflect_clang_args -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)

        # Windows/MSVC: unlike Linux/macOS, libclang does NOT reliably resolve the MSVC C++ stdlib +
        # Windows-SDK headers on its own here -- verified on CI (2026-07-21): the four cases that pull a
        # standard header (parse_engine_header / engine_types / all_flag / annotation_visible) fail
        # "'cstdint' file not found" (exit 2) while the self-contained walk_plain floor and every
        # usage/negative case pass. The MSVC dev-environment step (ilammy/msvc-dev-cmd) runs before this
        # configure and exports %INCLUDE% with every MSVC + Windows-SDK include directory; forward each
        # as an explicit -I so libclang's header search is deterministic rather than dependent on its own
        # MSVC auto-detection. This is the plan's pre-decided Windows contingency (D10 / §S6 / §3.8, ii).
        if(DEFINED ENV{INCLUDE})
            file(TO_CMAKE_PATH "$ENV{INCLUDE}" _aero_reflect_msvc_includes)  # ;-list, native -> forward slashes
            foreach(_inc IN LISTS _aero_reflect_msvc_includes)
                if(_inc AND IS_DIRECTORY "${_inc}")
                    list(APPEND _aero_reflect_clang_args -I "${_inc}")
                endif()
            endforeach()
            unset(_aero_reflect_msvc_includes)
        else()
            message(WARNING "reflect-gen tests: %INCLUDE% is unset at configure time -- the MSVC dev "
                            "environment step must run before this configure, or the full-stdlib parse "
                            "cases (parse_engine_header / engine_types / all_flag / annotation_visible) "
                            "will fail to find <cstdint> et al.")
        endif()
    endif()

    message(STATUS "reflect-gen tests: CLANG_ARGS = ${_aero_reflect_clang_args}")

    set(_aero_reflect_cases
        help version walk_plain parse_engine_header engine_types all_flag
        annotation_visible bad_syntax missing_input no_input unknown_flag double_input determinism
        components_basic components_categories components_unsupported components_multi
        components_none components_tag components_determinism
        components_bad_syntax components_missing_input)
    foreach(_aero_reflect_case IN LISTS _aero_reflect_cases)
        add_test(NAME reflect-gen.${_aero_reflect_case}
            COMMAND "${CMAKE_COMMAND}"
                    -DTOOL=$<TARGET_FILE:aero_reflect_gen>
                    -DCASE=${_aero_reflect_case}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/reflect-gen/${_aero_reflect_case}
                    "-DCLANG_ARGS=${_aero_reflect_clang_args}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/reflect-gen/run_case.cmake)
    endforeach()
    unset(_aero_reflect_cases)
    unset(_aero_reflect_clang_args)
endif()
