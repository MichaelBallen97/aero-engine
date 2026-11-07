# The consumer of the installed SDK (tests/sdk/prefix.cmake): configured against a prefix with no toolchain file, it
# builds through aero_add_game and links all eleven layers.
#
# consumer.cpp.in is copied to the build folder instead of being listed as a .cpp here: CI's clang-tidy reads every
# tracked *.cpp through the Linux tree's compile database, and this file is compiled only against a prefix, never by
# that tree.
cmake_minimum_required(VERSION 3.28)
project(aero_sdk_consumer LANGUAGES CXX)

find_package(AeroEngine CONFIG REQUIRED)

configure_file(consumer.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp" COPYONLY)
aero_add_game(aero_sdk_consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp")
