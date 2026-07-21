cmake_minimum_required(VERSION 3.28)
project(AeroReflectIncrementalProbe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Driver-supplied cache vars: AERO_REPO_ROOT, AERO_REFLECT_GEN_EXECUTABLE, AERO_LLVM_ROOT,
# AERO_ENTT_INCLUDE, AERO_REFLECT_TOOLS=ON. The REAL module under test:
include(${AERO_REPO_ROOT}/cmake/reflect.cmake)

# probe_main.cpp is file(WRITE)ten at configure time so no probe .cpp is tracked (F9). Constant
# content => idempotent across configures; it is compiled, not depfile-tracked.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/probe_main.cpp
"void aero_reflect_register_all_reflect_probe();\nint main() {\n    aero_reflect_register_all_reflect_probe();\n    return 0;\n}\n")

add_executable(reflect_probe ${CMAKE_CURRENT_BINARY_DIR}/probe_main.cpp)
target_include_directories(reflect_probe PRIVATE ${AERO_ENTT_INCLUDE})   # entt is header-only (V4)
aero_reflect_generate(reflect_probe HEADERS fixtures/widget.hpp)
