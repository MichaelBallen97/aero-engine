# cmake/reflect.cmake — task 1.1.4 (spec D2/D3/D4/D5). Owns, at root-include time, BOTH the
# AERO_LLVM_ROOT discovery (moved from tools/reflect-gen/CMakeLists.txt) and the per-OS libclang parse
# args (moved from tests/CMakeLists.txt), and defines aero_reflect_generate(). Included at root scope
# BEFORE any add_subdirectory() so an engine-layer call site (task 1.3.2) sees AERO_LLVM_ROOT and
# AERO_REFLECT_CLANG_ARGS on a fresh configure (kills the F8 ordering trap, D2). Mirrors
# cmake/shaders.cmake's aero_add_shaders() (OFF => no-op + STATUS).
#
# aero_reflect_generate(<target>
#     HEADERS <hdr>...            # component headers to parse (explicit list, D3 — no glob/auto-scan)
#     [INCLUDE_DIRS <dir>...]     # extra parse -I's the target can't expose itself (e.g. aero::core's
#                                 # PUBLIC dir — NOT target-derivable, D9)
#     [DEFINES <NAME[=VAL]>...]   # extra parse -D's
#     [AGGREGATOR <name>])        # override the default aggregator fn name (D4)

if(AERO_REFLECT_TOOLS)
    # === 1. AERO_LLVM_ROOT discovery — moved VERBATIM from tools/reflect-gen/CMakeLists.txt:12-31 ===
    #     (same -D > env > brew prefix > /usr/lib/llvm-18 > %ProgramFiles%/LLVM chain; log prefix
    #      "reflect:"). The tool's CMakeLists keeps find_package(Clang)/find_library — that half LINKS
    #      the tool; this half only supplies the discovery hint.
    if(DEFINED AERO_LLVM_ROOT)
        # already set (prior-configure cache entry or explicit -D): leave it.
    elseif(DEFINED ENV{AERO_LLVM_ROOT})
        set(AERO_LLVM_ROOT "$ENV{AERO_LLVM_ROOT}" CACHE PATH "LLVM 18 install prefix for libclang")
    elseif(APPLE)
        execute_process(COMMAND brew --prefix llvm@18 OUTPUT_VARIABLE _aero_brew_llvm
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(AERO_LLVM_ROOT "${_aero_brew_llvm}" CACHE PATH "LLVM 18 install prefix for libclang")
        unset(_aero_brew_llvm)
    elseif(UNIX)
        set(AERO_LLVM_ROOT "/usr/lib/llvm-18" CACHE PATH "LLVM 18 install prefix for libclang")
    else()
        set(AERO_LLVM_ROOT "$ENV{ProgramFiles}/LLVM" CACHE PATH "LLVM 18 install prefix for libclang")
    endif()
    message(STATUS "reflect: AERO_LLVM_ROOT = ${AERO_LLVM_ROOT}")

    # === 2. Per-OS parse args — moved VERBATIM from tests/CMakeLists.txt:198-296, renamed to the
    #        directory-scope var AERO_REFLECT_CLANG_ARGS (NOT cache: recomputed each configure so an
    #        SDK/LLVM change is picked up). Log prefix "reflect:"; content byte-identical (AC-1/V7). ===
    set(AERO_REFLECT_CLANG_ARGS -std=c++20)
    if(EXISTS "${AERO_LLVM_ROOT}/lib/clang/18")
        list(APPEND AERO_REFLECT_CLANG_ARGS -resource-dir "${AERO_LLVM_ROOT}/lib/clang/18")
    else()
        file(GLOB _aero_reflect_resource_dirs LIST_DIRECTORIES true "${AERO_LLVM_ROOT}/lib/clang/*")
        if(_aero_reflect_resource_dirs)
            list(GET _aero_reflect_resource_dirs 0 _aero_reflect_resource_dir)
            message(STATUS "reflect: LLVM 18 resource dir absent; falling back to ${_aero_reflect_resource_dir}")
            list(APPEND AERO_REFLECT_CLANG_ARGS -resource-dir "${_aero_reflect_resource_dir}")
            unset(_aero_reflect_resource_dir)
        else()
            message(WARNING "reflect: no clang resource dir under ${AERO_LLVM_ROOT}/lib/clang — stdlib parses will fail.")
        endif()
        unset(_aero_reflect_resource_dirs)
    endif()
    if(APPLE)
        # (verbatim macOS macosx15.4 sysroot pin chain from tests/CMakeLists.txt:228-260 — xcrun first,
        #  then CMAKE_OSX_SYSROOT, then a loud WARNING; NOT gated on CMAKE_OSX_SYSROOT being truthy.)
        execute_process(COMMAND xcrun --sdk macosx15.4 --show-sdk-path
                        OUTPUT_VARIABLE _aero_reflect_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _aero_reflect_sysroot_result)
        if(_aero_reflect_sysroot_result EQUAL 0 AND EXISTS "${_aero_reflect_sysroot}")
            message(STATUS "reflect: pinning -isysroot to ${_aero_reflect_sysroot}")
            list(APPEND AERO_REFLECT_CLANG_ARGS -isysroot "${_aero_reflect_sysroot}")
        elseif(CMAKE_OSX_SYSROOT)
            list(APPEND AERO_REFLECT_CLANG_ARGS -isysroot "${CMAKE_OSX_SYSROOT}")
        else()
            message(WARNING "reflect: could not resolve a macOS SDK — libc++ parses may fail on the default SDK")
        endif()
        unset(_aero_reflect_sysroot)
        unset(_aero_reflect_sysroot_result)
    endif()
    if(WIN32)
        list(APPEND AERO_REFLECT_CLANG_ARGS -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)  # STL1000 escape hatch
        if(DEFINED ENV{INCLUDE})
            file(TO_CMAKE_PATH "$ENV{INCLUDE}" _aero_reflect_msvc_includes)
            foreach(_inc IN LISTS _aero_reflect_msvc_includes)
                if(_inc AND IS_DIRECTORY "${_inc}")
                    list(APPEND AERO_REFLECT_CLANG_ARGS -I "${_inc}")
                endif()
            endforeach()
            unset(_aero_reflect_msvc_includes)
        else()
            message(WARNING "reflect: %INCLUDE% unset at configure — MSVC dev-cmd must run before configure (F10)")
        endif()
    endif()
    message(STATUS "reflect: CLANG_ARGS = ${AERO_REFLECT_CLANG_ARGS}")
endif()

# === 3. The reusable function ======================================================================
function(aero_reflect_generate TARGET)
    if(NOT AERO_REFLECT_TOOLS)
        message(STATUS "aero_reflect_generate(${TARGET}): skipped (AERO_REFLECT_TOOLS=OFF) — no reflection codegen")
        return()  # D11 / E15: a defined no-op; callers needing registration gate themselves
    endif()

    set(options)
    set(oneValueArgs AGGREGATOR)
    set(multiValueArgs HEADERS INCLUDE_DIRS DEFINES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # --- guards (E3/E7/D18) ---
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "aero_reflect_generate(${TARGET}): no such target — call AFTER "
                            "add_executable/add_library(${TARGET} ...) in the SAME CMakeLists.txt (E7)")
    endif()
    if(NOT ARG_HEADERS)
        message(FATAL_ERROR "aero_reflect_generate(${TARGET}): HEADERS must list >=1 component header (E3)")
    endif()
    get_target_property(_aero_wired ${TARGET} AERO_REFLECT_WIRED)
    if(_aero_wired)
        message(FATAL_ERROR "aero_reflect_generate(${TARGET}): called twice on one target (D18) — "
                            "pass the full HEADERS list in ONE call")
    endif()
    set_target_properties(${TARGET} PROPERTIES AERO_REFLECT_WIRED TRUE)

    # --- tool resolution (D10) ---
    if(DEFINED AERO_REFLECT_GEN_EXECUTABLE)
        set(_aero_tool "${AERO_REFLECT_GEN_EXECUTABLE}")   # nested probe project's prebuilt binary
        set(_aero_tool_dep "")                             # a raw path => no target to DEPENDS on
    elseif(TARGET aero_reflect_gen)
        set(_aero_tool "$<TARGET_FILE:aero_reflect_gen>")
        set(_aero_tool_dep aero_reflect_gen)
    else()
        message(FATAL_ERROR "aero_reflect_generate(${TARGET}): set AERO_REFLECT_GEN_EXECUTABLE or "
                            "define the aero_reflect_gen target before calling (D10)")
    endif()

    set(_aero_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/reflect-generated/${TARGET}")  # per-target => collision-free (D5)

    # --- aggregator fn name (D4) — MAKE_C_IDENTIFIER == the tool's sanitizeIdentifier (F5) ---
    if(ARG_AGGREGATOR)
        set(_aero_aggname "${ARG_AGGREGATOR}")
    else()
        string(MAKE_C_IDENTIFIER "${TARGET}" _aero_target_id)
        set(_aero_aggname "aero_reflect_register_all_${_aero_target_id}")
    endif()

    # --- explicit call-site parse args (D9) ---
    set(_aero_extra_args)
    foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND _aero_extra_args -I "${_dir}")
    endforeach()
    foreach(_def IN LISTS ARG_DEFINES)
        list(APPEND _aero_extra_args -D "${_def}")
    endforeach()

    set(_aero_gen_sources)
    set(_aero_header_dirs)
    set(_aero_fwd_decls)
    set(_aero_calls)
    set(_aero_seen_ids)
    foreach(_hdr IN LISTS ARG_HEADERS)
        get_filename_component(_hdr_abs "${_hdr}" ABSOLUTE)      # relative => CMAKE_CURRENT_SOURCE_DIR
        get_filename_component(_hdr_name "${_hdr_abs}" NAME)
        get_filename_component(_hdr_dir "${_hdr_abs}" DIRECTORY)
        # stem = strip the LAST extension ONLY (F5/E10). NOT get_filename_component(NAME_WE), which
        # strips from the FIRST dot and would desync from the tool's fs::path::stem() => a link error.
        string(REGEX REPLACE "\\.[^.]*$" "" _stem "${_hdr_name}")
        string(MAKE_C_IDENTIFIER "${_stem}" _id)
        if(_id IN_LIST _aero_seen_ids)
            message(FATAL_ERROR "aero_reflect_generate(${TARGET}): two HEADERS share stem/id '${_id}' "
                                "(would collide on one generated file + register fn) — E9")
        endif()
        list(APPEND _aero_seen_ids "${_id}")

        set(_out "${_aero_gen_dir}/${_stem}.meta.gen.cpp")
        set(_dep "${_out}.d")

        add_custom_command(
            OUTPUT  "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_aero_gen_dir}"   # Ninja won't pre-create it
            COMMAND "${_aero_tool}" --emit-meta "${_hdr_abs}" -o "${_out}" --depfile "${_dep}"
                    -- ${AERO_REFLECT_CLANG_ARGS}
                       ${_aero_extra_args}
                       -I "${_hdr_dir}"    # F6: the header's own dir (its #include "<basename>")
                       "$<$<BOOL:$<TARGET_PROPERTY:${TARGET},INCLUDE_DIRECTORIES>>:-I$<JOIN:$<TARGET_PROPERTY:${TARGET},INCLUDE_DIRECTORIES>,;-I>>"
                       "$<$<BOOL:$<TARGET_PROPERTY:${TARGET},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${TARGET},COMPILE_DEFINITIONS>,;-D>>"
            DEPFILE "${_dep}"
            DEPENDS ${_aero_tool_dep} "${_hdr_abs}"   # belt-and-suspenders: covers first build + depfile-deleted
            COMMENT "reflect-gen: ${_stem}.meta.gen.cpp (${TARGET})"   # LOAD-BEARING: incremental_e2e greps this
            COMMAND_EXPAND_LISTS VERBATIM)

        list(APPEND _aero_gen_sources "${_out}")
        list(APPEND _aero_header_dirs "${_hdr_dir}")
        string(APPEND _aero_fwd_decls "void aero_reflect_register_${_id}();\n")
        string(APPEND _aero_calls "    aero_reflect_register_${_id}();\n")
    endforeach()

    # --- aggregator TU (D4/D15): fwd-decls + one fn calling them in HEADERS order ---
    set(_aero_agg_file "${_aero_gen_dir}/${TARGET}.aggregator.gen.cpp")
    set(_aero_agg_content
"// GENERATED by aero_reflect_generate(${TARGET}) — DO NOT EDIT.
// Registers every HEADERS-listed component in HEADERS-list order (D4/D15).
${_aero_fwd_decls}
void ${_aero_aggname}() {
${_aero_calls}}
")
    file(MAKE_DIRECTORY "${_aero_gen_dir}")
    # write-if-different (V1): a reconfigure with unchanged HEADERS leaves the mtime untouched (AC-7).
    # @ONLY makes substitution inert (the C++ has no @VAR@); no genexes in the content (configure-time
    # stems only). Fallback if the pinned CMake ever regresses: file(WRITE .in) + copy_if_different.
    file(CONFIGURE OUTPUT "${_aero_agg_file}" CONTENT "${_aero_agg_content}" @ONLY)

    # --- wire generated sources + header dirs into the target (D5/F6) ---
    target_sources(${TARGET} PRIVATE ${_aero_gen_sources} "${_aero_agg_file}")
    list(REMOVE_DUPLICATES _aero_header_dirs)
    target_include_directories(${TARGET} PRIVATE ${_aero_header_dirs})   # so each generated TU finds its header (F6)
endfunction()
