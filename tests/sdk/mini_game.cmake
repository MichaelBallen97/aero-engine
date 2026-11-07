# tests/sdk/mini_game.cmake -- task 4.5.1: the C++ mini-game builds against the installed prefix alone and plays its
# scripted run to the integers samples/phase-4-scripting/cpp/README.md derives by hand. Needs sdk.install first (the
# fixture). The game needs a window and a GPU device: with none it exits 3 and prints `mini_game: no GPU device`, which
# this driver turns into the loud skip sdk_gpu_gate defines (a failure when AERO_REQUIRE_GPU is set).
#
# Ids: SDK17 the sample builds against the prefix with no vcpkg in its compile line, SDK18 --scripted from another
# folder exits 0 and prints exactly one `mini_game:` line, SDK19 that line holds the pinned integers, SDK20 `drawn` is
# an integer of at least 1, SDK21 the executable with its shaders/ and assets/ alone, the build folder gone, gives the
# same line.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/sdk_common.cmake")

set(prefix "${WORK_ROOT}/prefix")
set(binary "${WORK_ROOT}/mini-game-build")
set(away "${WORK_ROOT}/mini-game-build-away")
set(shipped "${WORK_ROOT}/shipped game")
set(elsewhere "${WORK_ROOT}/elsewhere")
set(sampleSource "${SOURCE_DIR}/samples/phase-4-scripting/cpp")
set(expectedLine "frames=600 fired=8 hits=3 bullets=0 entities=4")

if(NOT EXISTS "${prefix}/lib/cmake/AeroEngine/AeroEngineConfig.cmake")
    sdk_fail(SDK17 "no prefix at ${prefix}: sdk.install has not run")
endif()
if(NOT EXISTS "${sampleSource}/CMakeLists.txt")
    sdk_fail(SDK17 "${sampleSource} has no CMakeLists.txt: the mini-game is not there")
endif()

# summaryLine(<out> <text>): the `mini_game:` lines of a run, as a list in <out>.
function(summary_lines out text)
    string(REGEX MATCHALL "(^|\n)mini_game:[^\n]*" found "${text}")
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# ---- SDK17: configure and build against the prefix, from a folder that holds nothing of this tree. ----------------
file(REMOVE_RECURSE "${away}" "${shipped}")
sdk_configure_consumer(SOURCE "${sampleSource}" BINARY "${binary}" PREFIX "${prefix}" OUTPUT configureOut
                       RESULT configureRc)
if(NOT configureRc EQUAL 0)
    sdk_fail(SDK17 "the mini-game did not configure against the prefix (exit ${configureRc}):\n${configureOut}")
endif()
sdk_build_consumer(BINARY "${binary}" OUTPUT buildOut RESULT buildRc)
if(NOT buildRc EQUAL 0)
    sdk_fail(SDK17 "the mini-game did not build against the prefix (exit ${buildRc}):\n${buildOut}")
endif()
file(READ "${binary}/compile_commands.json" compileCommands)
string(TOLOWER "${compileCommands}" loweredCommands)
if(loweredCommands MATCHES "vcpkg")
    sdk_fail(SDK17 "the mini-game's compile line names vcpkg:\n${compileCommands}")
endif()

# ---- SDK18-SDK20: the scripted run, from a folder that is not the build folder. --------------------------------------
sdk_executable_path(exe "${binary}" aero_mini_game)
sdk_run_game(EXE "${exe}" WORKDIR "${elsewhere}" ARGS --scripted OUTPUT runOut RESULT runRc)
sdk_gpu_gate(runRc runOut)
if(SDK_SKIPPED)
    return()
endif()
summary_lines(lines "${runOut}")
list(LENGTH lines lineCount)
if(NOT runRc EQUAL 0 OR NOT lineCount EQUAL 1)
    sdk_fail(SDK18 "--scripted from ${elsewhere} exited ${runRc} with ${lineCount} mini_game: lines:\n${runOut}")
endif()
string(FIND "${lines}" "${expectedLine}" expectedAt)
if(expectedAt EQUAL -1)
    sdk_fail(SDK19 "the summary does not hold '${expectedLine}':\n${lines}")
endif()
if(NOT lines MATCHES " drawn=([0-9]+)$")
    sdk_fail(SDK20 "the summary ends in no integer 'drawn' field:\n${lines}")
endif()
if(CMAKE_MATCH_1 LESS 1)
    sdk_fail(SDK20 "drawn is ${CMAKE_MATCH_1}: the game simulated and never drew:\n${lines}")
endif()
string(REGEX REPLACE " drawn=[0-9]+$" "" pinned "${lines}")

# ---- SDK21: ship the game alone. The build folder is renamed away for the run and put back after, so a red run never
# leaves the next one without it. -----------------------------------------------------------------------------------
file(MAKE_DIRECTORY "${shipped}")
file(COPY "${exe}" DESTINATION "${shipped}")
file(GLOB shippedLibraries LIST_DIRECTORIES false "${binary}/*.dll")
foreach(library IN LISTS shippedLibraries)
    file(COPY "${library}" DESTINATION "${shipped}")
endforeach()
file(COPY "${binary}/shaders" DESTINATION "${shipped}")
file(COPY "${binary}/assets" DESTINATION "${shipped}")
file(RENAME "${binary}" "${away}")
sdk_executable_path(shippedExe "${shipped}" aero_mini_game)
sdk_run_game(EXE "${shippedExe}" WORKDIR "${elsewhere}" ARGS --scripted OUTPUT shippedOut RESULT shippedRc)
file(RENAME "${away}" "${binary}")
summary_lines(shippedLines "${shippedOut}")
string(REGEX REPLACE " drawn=[0-9]+$" "" shippedPinned "${shippedLines}")
if(NOT shippedRc EQUAL 0 OR NOT shippedPinned STREQUAL pinned)
    sdk_fail(SDK21 "the shipped copy exited ${shippedRc} and printed '${shippedLines}', not '${lines}':\n${shippedOut}")
endif()

message(STATUS "sdk.mini_game: SDK17 to SDK21 passed")
