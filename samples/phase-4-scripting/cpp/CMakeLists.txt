# task 4.5.1: the C++ half of the Phase 4 gate demo, written against the installed SDK and nothing else. In this tree
# the engine is already here; alone, `-DCMAKE_PREFIX_PATH=<prefix>` finds it (tests/sdk/mini_game.cmake does exactly
# that). The game reads its shaders/ and assets/ from beside its executable (platform::basePath()).
cmake_minimum_required(VERSION 3.28)
project(aero_mini_game LANGUAGES CXX)
if(NOT TARGET aero::engine)
    find_package(AeroEngine CONFIG REQUIRED)
endif()
aero_add_game(aero_mini_game main.cpp)
add_custom_command(TARGET aero_mini_game POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:aero_mini_game>/assets"
    VERBATIM)
