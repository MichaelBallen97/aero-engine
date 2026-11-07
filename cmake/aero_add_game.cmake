# task 4.5.1: aero_add_game(<target> <source>...) -- one function for the tree and for the installed package, so a
# game's CMakeLists reads the same in both. Included by the root CMakeLists and by cmake/AeroEngineConfig.cmake.
#
# It builds an executable linked to aero::engine under C++20 and copies the compiled shaders beside it, to
# <exe dir>/shaders, where platform::basePath() finds them. On Windows the third-party libraries are DLLs, and they
# are copied beside the executable too.
function(aero_add_game target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE aero::engine)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(TARGET aero_shaders)  # in-tree only: the artifacts are this build's own output
        add_dependencies(${target} aero_shaders)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${AERO_ENGINE_SHADERS_DIR}" "$<TARGET_FILE_DIR:${target}>/shaders"
        VERBATIM)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_RUNTIME_DLLS:${target}>"
                    "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS VERBATIM)
    endif()
endfunction()
