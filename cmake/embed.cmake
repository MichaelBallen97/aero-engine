# cmake/embed.cmake -- task E.6.1: binary files into generated C++ translation units at BUILD time.
#
#     aero_embed_binary_files(<target> HEADER <abs path> NAMESPACE <ns> FILES <accessor>=<abs path>...)
#
# One add_custom_command PER FILE runs `cmake -P embed_run.cmake`, writing
# ${CMAKE_CURRENT_BINARY_DIR}/generated/embed_<accessor>.cpp -- one anonymous-namespace array and one
# accessor returning std::span<const std::uint8_t> -- and adds it to <target>. ALWAYS ON (no tools gate):
# the editor cannot start without its fonts. The inputs are checked here at CONFIGURE time (exists,
# non-empty, TrueType magic) and again by the script at build time, so a Git LFS pointer or an HTML error
# page fails before any compiler sees it.
#
# ONE TU PER FILE, never one TU for all: the largest input is ~0.9 MB, MSVC's per-TU cost grows with the
# initialiser, and replacing one font then recompiles one TU. The array is an INTEGER initialiser, never a
# string literal -- MSVC caps one string literal at 16 380 bytes and a concatenated one at 65 535 (C2026).
#
# The HEADER is passed ABSOLUTE, but the generated TU includes it by NAME, and only the generated sources get
# the header's directory as an include directory (set_source_files_properties below) -- the tree's generator
# precedent, cmake/reflect.cmake, whose generated TUs include their header by basename: the TU lives in the
# build tree and the header's directory is not on <target>'s include path. Never by absolute path inside the
# file: MSVC builds this tree without /utf-8, so it decodes a source file through the system code page, and a
# non-ASCII checkout path spelled in an #include (C:/Users/<a name with an umlaut>/...) fails there with
# C1083. On the command line the directory travels exactly as every source path of the build does.

function(aero_embed_binary_files target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "HEADER;NAMESPACE" "FILES")
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "embed: '${target}' is not a target")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "embed: unexpected arguments '${arg_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT arg_HEADER OR NOT IS_ABSOLUTE "${arg_HEADER}")
        message(FATAL_ERROR "embed: HEADER must be an absolute path (got '${arg_HEADER}')")
    endif()
    if(NOT EXISTS "${arg_HEADER}")
        message(FATAL_ERROR "embed: HEADER '${arg_HEADER}' does not exist")
    endif()
    get_filename_component(headerDir "${arg_HEADER}" DIRECTORY)
    if(NOT arg_NAMESPACE)
        message(FATAL_ERROR "embed: NAMESPACE is required")
    endif()
    if(NOT arg_FILES)
        message(FATAL_ERROR "embed: FILES is empty")
    endif()

    foreach(entry IN LISTS arg_FILES)
        if(NOT entry MATCHES "^([a-z][A-Za-z0-9]*)=(.+)$")
            message(FATAL_ERROR "embed: '${entry}' is not <accessor>=<path> with a camelCase accessor")
        endif()
        set(accessor "${CMAKE_MATCH_1}")
        set(input "${CMAKE_MATCH_2}")
        if(NOT EXISTS "${input}")
            message(FATAL_ERROR "embed: input '${input}' does not exist")
        endif()
        file(SIZE "${input}" inputSize)
        if(inputSize EQUAL 0)
            message(FATAL_ERROR "embed: input '${input}' is empty")
        endif()
        file(READ "${input}" magic LIMIT 4 HEX)
        if(NOT magic STREQUAL "00010000")
            message(FATAL_ERROR "embed: input '${input}' is not a TrueType font (first bytes '${magic}')")
        endif()

        set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/embed_${accessor}.cpp")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DOUTPUT=${output}" "-DACCESSOR=${accessor}"
                    "-DHEADER=${arg_HEADER}" "-DNAMESPACE=${arg_NAMESPACE}"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_run.cmake"
            DEPENDS "${input}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_run.cmake"
                    "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            COMMENT "embed: ${accessor}"
            VERBATIM)
        target_sources(${target} PRIVATE "${output}")
        # The header's directory for THIS source only, so <target>'s own include path is unchanged.
        set_source_files_properties("${output}" PROPERTIES INCLUDE_DIRECTORIES "${headerDir}")
    endforeach()
endfunction()
