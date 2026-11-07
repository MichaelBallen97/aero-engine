# task 4.5.1: the installable C++ SDK (docs/04). Included by the root CMakeLists when AERO_SDK is ON, AFTER every
# add_subdirectory(): root scope reaches no guarded target, and the layers' aliases exist by now.
#
# `aero_sdk` is what the sdk build presets build: the eleven layers and the compiled shaders. add_dependencies takes an
# alias, and spelling the raw aero_<layer> names here would redden audio-boundary's Part 1d.
add_custom_target(aero_sdk)
foreach(_sdk_layer IN ITEMS core assets audio platform rhi render reflect scene scene_render scene_audio
                            scene_serialize)
    add_dependencies(aero_sdk aero::${_sdk_layer})
endforeach()
unset(_sdk_layer)
add_dependencies(aero_sdk aero_shaders)

# The prefix is laid out by install(FILES) and install(DIRECTORY) and described by a hand-written package, never
# install(TARGETS) or install(EXPORT): this file reads what an export would have generated from the build instead of
# keeping it by hand, and a layer that links anything outside the lists below is a configure error.
#
# The classification below tests NAMES, in this order, and never if(TARGET): an imported target is directory-scoped, so
# EnTT::EnTT and glm::glm do not exist at this scope, and a TARGET test would pass them through into the package.
# The eleven layers, the third-party roster, and what links nothing:
set(_sdk_layers core assets audio platform rhi render reflect scene scene_render scene_audio scene_serialize)
set(_sdk_roster spdlog fmt enkiTS SDL3)
set(_sdk_names_spdlog spdlog::spdlog)
set(_sdk_names_fmt fmt::fmt)
set(_sdk_names_enkiTS enkiTS::enkiTS)
set(_sdk_names_SDL3 SDL3::SDL3 SDL3::SDL3-static SDL3::SDL3-shared)
set(_sdk_no_binary EnTT::EnTT glm::glm SDL3::Headers)
# Headers no game gets, by file name: Tracy is dev-builds-only, cooking is tool-time, and .aeroscript exists only in
# TypeScript projects. Everything else in the eleven include trees installs, so a new public header needs no edit here.
set(_sdk_excluded profiler.hpp animation_cook.hpp audio_cook.hpp mesh_cook.hpp skeleton_cook.hpp texture_cook.hpp
                  bc_block.hpp cooked_script.hpp)

find_package(spdlog CONFIG REQUIRED)  # fmt::fmt and Threads::Threads arrive with it
find_package(enkiTS CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

function(_sdk_classify out item from)
    if(item MATCHES "^\\$<LINK_ONLY:(.*)>$")
        set(item "${CMAKE_MATCH_1}")
    endif()
    if(item MATCHES "^aero::(.+)$")
        set(name "${CMAKE_MATCH_1}")
        if(name IN_LIST _sdk_layers)
            set(${out} "${item}" PARENT_SCOPE)
        elseif(name STREQUAL "profiling" OR name MATCHES "_internal$")
            set(${out} "" PARENT_SCOPE)
        else()
            message(FATAL_ERROR "SDK: ${from} links ${item}, which is outside the SDK's layer list")
        endif()
        return()
    endif()
    if(item IN_LIST _sdk_no_binary)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    if(item STREQUAL "Threads::Threads")
        set(${out} "Threads::Threads" PARENT_SCOPE)  # the config repeats find_package(Threads)
        return()
    endif()
    foreach(tp IN LISTS _sdk_roster)
        if(item IN_LIST _sdk_names_${tp})
            set(${out} "aero_third_party::${tp}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    if(item MATCHES "::")
        message(FATAL_ERROR "SDK: ${from} links target ${item}, which the SDK does not know: "
                            "add it to the roster or to the no-binary list in cmake/sdk_install.cmake")
    endif()
    if(IS_ABSOLUTE "${item}")
        message(FATAL_ERROR "SDK: ${from} links the absolute path ${item}: it cannot move with the prefix")
    endif()
    set(${out} "${item}" PARENT_SCOPE)  # a system library, a framework or a $<LINK_LIBRARY:...> form, verbatim
endfunction()

# _sdk_classify_list(<out> <from> <item>...): classify each, drop what links nothing, de-duplicate.
function(_sdk_classify_list out from)
    set(kept "")
    foreach(item IN LISTS ARGN)
        _sdk_classify(classified "${item}" "${from}")
        if(NOT classified STREQUAL "")
            list(APPEND kept "${classified}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES kept)
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

# The graph (AeroEngineGraph.cmake) is plain text, written by file(WRITE): it must carry $<LINK_LIBRARY:...> forms
# verbatim, and file(GENERATE) refuses them outside a link context. The file names (AeroEngineFiles.cmake) are
# resolved per OS by file(GENERATE), which evaluates $<TARGET_FILE_NAME:...>. One file cannot do both jobs.
set(_sdk_graph "set(AERO_SDK_LAYERS \"${_sdk_layers}\")\n")
string(APPEND _sdk_graph "set(AERO_SDK_THIRD_PARTY \"${_sdk_roster}\")\n")
set(_sdk_files "")

foreach(_sdk_layer IN LISTS _sdk_layers)
    get_target_property(_sdk_items aero::${_sdk_layer} INTERFACE_LINK_LIBRARIES)
    if(NOT _sdk_items)
        set(_sdk_items "")
    endif()
    _sdk_classify_list(_sdk_edges "aero::${_sdk_layer}" ${_sdk_items})
    string(APPEND _sdk_graph "set(AERO_SDK_LINK_${_sdk_layer} \"${_sdk_edges}\")\n")
    string(APPEND _sdk_files "set(AERO_SDK_FILE_${_sdk_layer} \"$<TARGET_FILE_NAME:aero::${_sdk_layer}>\")\n")
endforeach()

foreach(_sdk_tp IN LISTS _sdk_roster)
    set(_sdk_real "")
    foreach(_sdk_candidate IN LISTS _sdk_names_${_sdk_tp})
        if(NOT _sdk_real AND TARGET ${_sdk_candidate})
            set(_sdk_real ${_sdk_candidate})
        endif()
    endforeach()
    if(NOT _sdk_real)
        message(FATAL_ERROR "SDK: no target named any of ${_sdk_names_${_sdk_tp}} (${_sdk_tp}) exists at root scope")
    endif()
    get_target_property(_sdk_aliased ${_sdk_real} ALIASED_TARGET)
    if(_sdk_aliased)
        set(_sdk_real ${_sdk_aliased})
    endif()
    get_target_property(_sdk_type ${_sdk_real} TYPE)
    get_target_property(_sdk_items ${_sdk_real} INTERFACE_LINK_LIBRARIES)
    get_target_property(_sdk_options ${_sdk_real} INTERFACE_LINK_OPTIONS)
    if(NOT _sdk_items)
        set(_sdk_items "")
    endif()
    if(NOT _sdk_options)
        set(_sdk_options "")
    endif()
    _sdk_classify_list(_sdk_edges "${_sdk_tp}" ${_sdk_items})
    string(APPEND _sdk_graph "set(AERO_SDK_TP_TYPE_${_sdk_tp} \"${_sdk_type}\")\n")
    string(APPEND _sdk_graph "set(AERO_SDK_TP_LINK_${_sdk_tp} \"${_sdk_edges}\")\n")
    string(APPEND _sdk_graph "set(AERO_SDK_TP_OPTIONS_${_sdk_tp} \"${_sdk_options}\")\n")
    set(_sdk_runtime "")
    if(_sdk_type STREQUAL "SHARED_LIBRARY")
        set(_sdk_runtime "$<TARGET_FILE_NAME:${_sdk_real}>")
    endif()
    string(APPEND _sdk_files "set(AERO_SDK_TP_LINKER_${_sdk_tp} \"$<TARGET_LINKER_FILE_NAME:${_sdk_real}>\")\n")
    string(APPEND _sdk_files "set(AERO_SDK_TP_RUNTIME_${_sdk_tp} \"${_sdk_runtime}\")\n")

    install(FILES "$<TARGET_LINKER_FILE:${_sdk_real}>" DESTINATION lib/third_party)
    if(_sdk_type STREQUAL "SHARED_LIBRARY")
        install(FILES "$<TARGET_FILE:${_sdk_real}>" DESTINATION bin)
    endif()
endforeach()

file(WRITE "${CMAKE_BINARY_DIR}/sdk/AeroEngineGraph.cmake" "${_sdk_graph}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/sdk/AeroEngineFiles.cmake" CONTENT "${_sdk_files}")

set(_sdk_exclude_args "")
foreach(_sdk_name IN LISTS _sdk_excluded)
    list(APPEND _sdk_exclude_args PATTERN "${_sdk_name}" EXCLUDE)
endforeach()
foreach(_sdk_layer IN LISTS _sdk_layers)
    install(FILES "$<TARGET_FILE:aero::${_sdk_layer}>" DESTINATION lib)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/${_sdk_layer}/include/" DESTINATION include ${_sdk_exclude_args})
endforeach()

install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders/" DESTINATION share/aero/shaders
        FILES_MATCHING PATTERN "*.spv" PATTERN "*.msl" PATTERN "*.dxil" PATTERN "*.json"
        PATTERN "CMakeFiles" EXCLUDE)

# The archives are per architecture, so the version file is not ARCH_INDEPENDENT.
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_BINARY_DIR}/sdk/AeroEngineConfigVersion.cmake"
                                 VERSION ${AeroEngine_VERSION} COMPATIBILITY ExactVersion)
install(FILES
            "${CMAKE_BINARY_DIR}/sdk/AeroEngineGraph.cmake"
            "${CMAKE_BINARY_DIR}/sdk/AeroEngineFiles.cmake"
            "${CMAKE_BINARY_DIR}/sdk/AeroEngineConfigVersion.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/AeroEngineConfig.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/aero_add_game.cmake"
        DESTINATION lib/cmake/AeroEngine)
