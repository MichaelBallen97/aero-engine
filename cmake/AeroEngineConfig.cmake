# task 4.5.1: the package config of the installed C++ SDK (docs/04). Installed verbatim to
# <prefix>/lib/cmake/AeroEngine/, beside the two generated files it includes (cmake/sdk_install.cmake writes them) and
# aero_add_game.cmake. Every path below derives from this file's own location, which is what makes a prefix movable.
#
# A consumer says find_package(AeroEngine CONFIG REQUIRED) and gets aero::<layer> (eleven static libraries),
# aero::engine (all eleven), AERO_ENGINE_SHADERS_DIR and aero_add_game(). The third-party libraries behind the layers are
# aero_third_party::<name>, an implementation detail: they carry no include directory, so no third-party header can
# reach a consumer's compile line.

# MSVC first, so that `cmake -DMSVC=1 ... -P` reaches it with no project: under -P this file stops at find_dependency.
# The SDK uses the release DLL runtime (/MD), and MSVC cannot link /MDd code against it (LNK2038). Every target defined
# after find_package(AeroEngine) gets that runtime in every configuration, under every generator.
if(MSVC)
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
        message(FATAL_ERROR "AeroEngine: the SDK uses the release DLL runtime (/MD) in every configuration, and "
                            "CMAKE_MSVC_RUNTIME_LIBRARY is '${CMAKE_MSVC_RUNTIME_LIBRARY}'. "
                            "Set it to MultiThreadedDLL, or leave it unset.")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()
if(TARGET aero::engine)
    return()
endif()

get_filename_component(_aero_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/AeroEngineGraph.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AeroEngineFiles.cmake")

foreach(_aero_tp IN LISTS AERO_SDK_THIRD_PARTY)
    if(AERO_SDK_TP_TYPE_${_aero_tp} STREQUAL "SHARED_LIBRARY")
        add_library(aero_third_party::${_aero_tp} SHARED IMPORTED)
        set_target_properties(aero_third_party::${_aero_tp} PROPERTIES
            IMPORTED_IMPLIB "${_aero_prefix}/lib/third_party/${AERO_SDK_TP_LINKER_${_aero_tp}}"
            IMPORTED_LOCATION "${_aero_prefix}/bin/${AERO_SDK_TP_RUNTIME_${_aero_tp}}")
    else()
        add_library(aero_third_party::${_aero_tp} STATIC IMPORTED)
        set_target_properties(aero_third_party::${_aero_tp} PROPERTIES
            IMPORTED_LOCATION "${_aero_prefix}/lib/third_party/${AERO_SDK_TP_LINKER_${_aero_tp}}")
    endif()
    set_target_properties(aero_third_party::${_aero_tp} PROPERTIES
        INTERFACE_LINK_LIBRARIES "${AERO_SDK_TP_LINK_${_aero_tp}}"
        INTERFACE_LINK_OPTIONS "${AERO_SDK_TP_OPTIONS_${_aero_tp}}")
endforeach()

# aero::<layer>: a static library with the one include folder and the edges the build recorded.
foreach(_aero_layer IN LISTS AERO_SDK_LAYERS)
    add_library(aero::${_aero_layer} STATIC IMPORTED)
    set_target_properties(aero::${_aero_layer} PROPERTIES
        IMPORTED_LOCATION "${_aero_prefix}/lib/${AERO_SDK_FILE_${_aero_layer}}"
        INTERFACE_INCLUDE_DIRECTORIES "${_aero_prefix}/include"
        INTERFACE_LINK_LIBRARIES "${AERO_SDK_LINK_${_aero_layer}}")
    list(APPEND _aero_all_layers aero::${_aero_layer})
endforeach()

add_library(aero::engine INTERFACE IMPORTED)
set_target_properties(aero::engine PROPERTIES INTERFACE_LINK_LIBRARIES "${_aero_all_layers}")

set(AERO_ENGINE_SHADERS_DIR "${_aero_prefix}/share/aero/shaders")
include("${CMAKE_CURRENT_LIST_DIR}/aero_add_game.cmake")

unset(_aero_tp)
unset(_aero_layer)
unset(_aero_all_layers)
unset(_aero_prefix)
