# cmake/shaders.cmake — aero_add_shaders() (task 0.4.3, spec D6/§3.4).
#
# aero_add_shaders(<target> SHADERS <src.hlsl>... [OUTPUT_DIR <dir>] [INCLUDE_DIR <dir>]
#                   [DEFINES <NAME[=VALUE]>...])
#
# Stage comes from the filename: <name>.vert.hlsl | <name>.frag.hlsl (FATAL_ERROR otherwise -- the
# FUNCTION is the stage oracle; aero_shaderc trusts its own --stage flag and never sniffs filenames,
# spec E8). Each shader gets one add_custom_command emitting <base>.{spv,msl,dxil,json} into
# OUTPUT_DIR (default ${CMAKE_BINARY_DIR}/shaders); <target> aggregates every shader's outputs and is
# added to ALL, so building any preset is a living compile test of every shader in the tree.
#
# When AERO_SHADER_TOOLS is OFF this degrades to a no-op + STATUS (spec E18) -- callers never need
# their own guard.

function(aero_add_shaders TARGET)
    if(NOT AERO_SHADER_TOOLS)
        message(STATUS "aero_add_shaders(${TARGET}): skipped (AERO_SHADER_TOOLS=OFF) — no shader compilation")
        return()
    endif()

    set(options)
    set(oneValueArgs OUTPUT_DIR INCLUDE_DIR)
    set(multiValueArgs SHADERS DEFINES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_SHADERS)
        message(FATAL_ERROR "aero_add_shaders(${TARGET}): SHADERS must list at least one .hlsl source")
    endif()

    if(ARG_OUTPUT_DIR)
        set(out_dir "${ARG_OUTPUT_DIR}")
    else()
        set(out_dir "${CMAKE_BINARY_DIR}/shaders")
    endif()

    set(include_args)
    set(include_glob)
    if(ARG_INCLUDE_DIR)
        list(APPEND include_args --include "${ARG_INCLUDE_DIR}")
        file(GLOB include_glob CONFIGURE_DEPENDS "${ARG_INCLUDE_DIR}/*.hlsli")
    endif()

    set(define_args)
    foreach(def IN LISTS ARG_DEFINES)
        list(APPEND define_args --define "${def}")
    endforeach()

    set(all_outputs)
    foreach(src IN LISTS ARG_SHADERS)
        get_filename_component(src_abs "${src}" ABSOLUTE)
        get_filename_component(src_name "${src}" NAME)

        # The stage oracle (spec E8): the mandatory .vert.hlsl / .frag.hlsl suffix, checked at
        # configure time. aero_shaderc itself never sniffs filenames -- it obeys --stage only.
        if(src_name MATCHES "\\.vert\\.hlsl$")
            set(stage "vertex")
            string(REGEX REPLACE "\\.hlsl$" "" base "${src_name}")
        elseif(src_name MATCHES "\\.frag\\.hlsl$")
            set(stage "fragment")
            string(REGEX REPLACE "\\.hlsl$" "" base "${src_name}")
        else()
            message(FATAL_ERROR
                "aero_add_shaders(${TARGET}): '${src_name}' does not end in .vert.hlsl or .frag.hlsl "
                "-- the stage cannot be inferred (spec E8). Rename the file.")
        endif()

        set(spv "${out_dir}/${base}.spv")
        set(msl "${out_dir}/${base}.msl")
        set(dxil "${out_dir}/${base}.dxil")
        set(json "${out_dir}/${base}.json")

        add_custom_command(
            OUTPUT "${spv}" "${msl}" "${dxil}" "${json}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
            # D8: LLVM-family tools leak by design at process exit; ASan's leak detector would fail
            # every shader compile on third-party frames we can't fix. new_delete_type_mismatch=0 is
            # the same class: GCC's libasan (Linux) aborts on a benign new/delete size mismatch INSIDE
            # the uninstrumented vendored DXC (libdxcompiler) that AppleClang's ASan does not flag --
            # neither is our bug, both are in third-party code we build uninstrumented (Release sub-build).
            # Scoped to this invocation only -- UBSan and ASan memory-error detection stay fully live in
            # aero_shaderc's own code.
            COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0:new_delete_type_mismatch=0
                    "$<TARGET_FILE:aero_shaderc>"
                    --input "${src_abs}"
                    --stage "${stage}"
                    --output-dir "${out_dir}"
                    ${include_args}
                    ${define_args}
            DEPENDS "${src_abs}" aero_shaderc ${include_glob}
            COMMENT "aero_shaderc: ${base}"
            VERBATIM
        )
        list(APPEND all_outputs "${spv}" "${msl}" "${dxil}" "${json}")
    endforeach()

    add_custom_target(${TARGET} ALL DEPENDS ${all_outputs})
endfunction()
