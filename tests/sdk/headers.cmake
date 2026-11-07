# tests/sdk/headers.cmake -- task 4.5.1: every installed header compiles alone, includes nothing third-party, and a
# header that WOULD leak is caught. Needs sdk.install first (the fixture). No vcpkg root is on this compile line, so a
# third-party include in an installed header is a hard error on every lane.
#
# Ids: SDK12 one translation unit per header builds, SDK13 the include allowlist holds over every installed header,
# SDK14 the allowlist refuses what it must, SDK15 a third-party include fails to build, SDK16 the two compile-time
# contracts of World::registerComponent hold.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/sdk_common.cmake")

set(prefix "${WORK_ROOT}/prefix")
set(work "${WORK_ROOT}/headers")
if(NOT EXISTS "${prefix}/lib/cmake/AeroEngine/AeroEngineConfig.cmake")
    sdk_fail(SDK12 "no prefix at ${prefix}: sdk.install has not run")
endif()

# sdk_include_allowed(<out> <operand> <include root>): the operand of one `#include` line is allowed when it is
# <aero/...hpp> naming a file under the root, or a bare lower-case standard header such as <cstdint>.
function(sdk_include_allowed out operand root)
    if(operand MATCHES "^<(aero/[A-Za-z0-9_/]+\\.hpp)>$")
        if(EXISTS "${root}/${CMAKE_MATCH_1}")
            set(${out} "aero" PARENT_SCOPE)
        else()
            set(${out} "" PARENT_SCOPE)
        endif()
    elseif(operand MATCHES "^<[a-z0-9_]+>$")
        set(${out} "std" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

file(GLOB_RECURSE headers LIST_DIRECTORIES false RELATIVE "${prefix}/include" "${prefix}/include/*.hpp")
list(SORT headers)
list(LENGTH headers headerCount)
if(headerCount LESS 87)
    sdk_fail(SDK12 "only ${headerCount} headers are installed")
endif()

# ---- SDK13 and SDK14 need no compiler, so they run first. ---------------------------------------------------------
set(linesScanned 0)
set(aeroSeen 0)
set(stdSeen 0)
foreach(header IN LISTS headers)
    file(READ "${prefix}/include/${header}" text)
    # A semicolon or bracket in a trailing comment would split or join list elements.
    string(REGEX REPLACE "[][;]" "," text "${text}")
    string(REGEX MATCHALL "(^|\n)[ \t]*#[ \t]*include[^\r\n]*" includeLines "${text}")
    foreach(line IN LISTS includeLines)
        string(REGEX REPLACE "^[\r\n \t]*#[ \t]*include[ \t]*" "" operand "${line}")
        string(STRIP "${operand}" operand)
        # The operand is the first <...> or "..." token; a trailing comment is not part of it.
        if(operand MATCHES "^(<[^>]*>|\"[^\"]*\")")
            set(operand "${CMAKE_MATCH_1}")
        endif()
        sdk_include_allowed(kind "${operand}" "${prefix}/include")
        if(kind STREQUAL "")
            sdk_fail(SDK13 "${header} includes ${operand}, which an installed header may not")
        endif()
        math(EXPR linesScanned "${linesScanned} + 1")
        if(kind STREQUAL "aero")
            math(EXPR aeroSeen "${aeroSeen} + 1")
        else()
            math(EXPR stdSeen "${stdSeen} + 1")
        endif()
    endforeach()
endforeach()
if(linesScanned LESS 87 OR aeroSeen EQUAL 0 OR stdSeen EQUAL 0)
    sdk_fail(SDK13 "the scan saw ${linesScanned} include lines (${aeroSeen} aero, ${stdSeen} standard): too few to trust")
endif()

foreach(bad IN ITEMS "<entt/entt.hpp>" "<tracy/Tracy.hpp>" "<SDL3/SDL.h>" "<miniaudio.h>" "\"local.hpp\"")
    sdk_include_allowed(kind "${bad}" "${prefix}/include")
    if(NOT kind STREQUAL "")
        sdk_fail(SDK14 "the allowlist accepted ${bad}")
    endif()
endforeach()
sdk_include_allowed(kind "<aero/core/profiler.hpp>" "${prefix}/include")
if(NOT kind STREQUAL "")
    sdk_fail(SDK14 "the allowlist accepted an include of the excluded profiler.hpp")
endif()

# ---- SDK12: one TU per header. -----------------------------------------------------------------------------------
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/each")
set(units "")
set(index 0)
foreach(header IN LISTS headers)
    math(EXPR index "${index} + 1")
    set(unit "each/tu_${index}.cpp")
    file(WRITE "${work}/${unit}" "#include <${header}>\n")
    list(APPEND units "${unit}")
endforeach()
file(WRITE "${work}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(sdk_headers LANGUAGES CXX)
find_package(AeroEngine CONFIG REQUIRED)
add_library(headers OBJECT ${units})
target_link_libraries(headers PRIVATE aero::engine)
target_compile_features(headers PRIVATE cxx_std_20)
")
sdk_configure_consumer(SOURCE "${work}" BINARY "${work}/build" PREFIX "${prefix}" OUTPUT out RESULT rc)
if(NOT rc EQUAL 0)
    sdk_fail(SDK12 "the header project did not configure (exit ${rc}):\n${out}")
endif()
sdk_build_consumer(BINARY "${work}/build" OUTPUT out RESULT rc)
if(NOT rc EQUAL 0)
    sdk_fail(SDK12 "an installed header does not compile alone (exit ${rc}):\n${out}")
endif()

# ---- SDK15 and SDK16: the negative controls. Each fails for the reason it names, or the claim falls. ---------------
set(negative "${WORK_ROOT}/headers-negative")
file(REMOVE_RECURSE "${negative}")
file(MAKE_DIRECTORY "${negative}")
file(WRITE "${negative}/third_party.cpp" "#include <entt/entt.hpp>\n")
file(WRITE "${negative}/over_aligned.cpp" "#include <aero/scene/world.hpp>
struct alignas(64) OverAligned {
    float value = 0.0f;
};
void registerIt(engine::World& world) { world.registerComponent<OverAligned>(\"OverAligned\"); }
")
file(WRITE "${negative}/throwing_move.cpp" "#include <aero/scene/world.hpp>
struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    float value = 0.0f;
};
void registerIt(engine::World& world) { world.registerComponent<ThrowingMove>(\"ThrowingMove\"); }
")
file(WRITE "${negative}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(sdk_negative LANGUAGES CXX)
find_package(AeroEngine CONFIG REQUIRED)
foreach(unit IN ITEMS third_party over_aligned throwing_move)
    add_library(neg_\${unit} OBJECT \${unit}.cpp)
    target_link_libraries(neg_\${unit} PRIVATE aero::engine)
    target_compile_features(neg_\${unit} PRIVATE cxx_std_20)
endforeach()
")
sdk_configure_consumer(SOURCE "${negative}" BINARY "${negative}/build" PREFIX "${prefix}" OUTPUT out RESULT rc)
if(NOT rc EQUAL 0)
    sdk_fail(SDK15 "the negative project did not configure (exit ${rc}):\n${out}")
endif()

# sdk_expect_build_failure(<id> <target> <needle>)
function(sdk_expect_build_failure id target needle)
    sdk_build_consumer(BINARY "${negative}/build" TARGET "${target}" OUTPUT out RESULT rc)
    if(rc EQUAL 0)
        sdk_fail(${id} "${target} built, and it must not")
    endif()
    string(FIND "${out}" "${needle}" at)
    if(at EQUAL -1)
        sdk_fail(${id} "${target} failed, but its output does not name '${needle}':\n${out}")
    endif()
endfunction()

sdk_expect_build_failure(SDK15 neg_third_party "entt/entt.hpp")
sdk_expect_build_failure(SDK16 neg_over_aligned "drop the alignas")
sdk_expect_build_failure(SDK16 neg_throwing_move "mark its move constructor noexcept")

message(STATUS "sdk.headers: SDK12 to SDK16 passed (${headerCount} headers)")
