# tests/sdk/template.cmake -- task 4.5.2: the C++ project the editor writes builds against the installed prefix with the
# commands its own README states, and runs. Needs sdk.install first (the fixture). WRITER is the tool target
# aero_sdk_project_writer (tests/sdk/project_writer_tool.cpp), which writes the four files under the demo name
# "Cube Demo <n-tilde>". The game needs a window and a GPU device: with none it exits 3 and prints
# `hello-cube: no GPU device`, which sdk_gpu_gate turns into the loud skip (a failure when AERO_REQUIRE_GPU is set).
#
# Ids: SDK22 the tool exits 0 and the four files exist, SDK23 a second run leaves every digest unchanged, SDK24 the
# README's two build commands, run from the project folder, configure and build, SDK25 `--frames 30` from another
# folder prints `hello-cube: frames=30 updates=30`, SDK26 the executable is game_Cube_Demo___ and the non-ASCII name
# compiled, SDK27 the compile database holds the prefix's include and no vcpkg.
cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/sdk_common.cmake")
if(NOT DEFINED WRITER)
    message(FATAL_ERROR "sdk driver: -DWRITER=... is required")
endif()

set(prefix "${WORK_ROOT}/prefix")
set(project "${WORK_ROOT}/template/my project")
set(elsewhere "${WORK_ROOT}/elsewhere")
set(files CMakeLists.txt src/main.cpp README.md .gitignore)
set(expectedTarget "game_Cube_Demo___")

if(NOT EXISTS "${prefix}/lib/cmake/AeroEngine/AeroEngineConfig.cmake")
    sdk_fail(SDK22 "no prefix at ${prefix}: sdk.install has not run")
endif()
if(NOT EXISTS "${WRITER}")
    sdk_fail(SDK22 "the writer tool ${WRITER} was not built")
endif()

# digests(<out>): the SHA-256 of each of the four files, in order.
function(digests out)
    set(all "")
    foreach(file IN LISTS files)
        file(SHA256 "${project}/${file}" digest)
        list(APPEND all "${file}=${digest}")
    endforeach()
    set(${out} "${all}" PARENT_SCOPE)
endfunction()

# ---- SDK22: a fresh folder, with a space in its name. ------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_ROOT}/template")
execute_process(COMMAND "${WRITER}" "${project}" RESULT_VARIABLE writerRc OUTPUT_VARIABLE writerOut
                ERROR_VARIABLE writerErr)
if(NOT writerRc EQUAL 0)
    sdk_fail(SDK22 "the writer exited ${writerRc}:\n${writerOut}${writerErr}")
endif()
foreach(file IN LISTS files)
    if(NOT EXISTS "${project}/${file}")
        sdk_fail(SDK22 "the writer left no ${file} in ${project}:\n${writerOut}")
    endif()
endforeach()

# ---- SDK23: writing again changes nothing. ------------------------------------------------------------------------
digests(before)
execute_process(COMMAND "${WRITER}" "${project}" RESULT_VARIABLE againRc OUTPUT_VARIABLE againOut
                ERROR_VARIABLE againErr)
digests(after)
if(NOT againRc EQUAL 0 OR NOT before STREQUAL after)
    sdk_fail(SDK23 "a second run exited ${againRc} and the files differ:\n${before}\n${after}\n${againOut}${againErr}")
endif()

# ---- SDK24: the README's own commands. They are read out of the README, so a changed command is a changed test. ---
file(READ "${project}/README.md" readme)
set(configureCommand "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<engine>/build/sdk/<os>-sdk")
set(buildCommand "cmake --build build --config Release")
string(FIND "${readme}" "${configureCommand}\n${buildCommand}\n" commandsAt)
if(commandsAt EQUAL -1)
    sdk_fail(SDK24 "the README does not hold, verbatim and in order:\n${configureCommand}\n${buildCommand}")
endif()
# The README's prefix names the engine checkout's install folder; the test's prefix is the one sdk.install made.
string(REPLACE "<engine>/build/sdk/<os>-sdk" "${prefix}" configureCommandHere "${configureCommand}")
string(REGEX REPLACE "^cmake " "" configureArguments "${configureCommandHere}")
string(REGEX REPLACE "^cmake " "" buildArguments "${buildCommand}")
separate_arguments(configureList UNIX_COMMAND "${configureArguments}")
separate_arguments(buildList UNIX_COMMAND "${buildArguments}")
# CMAKE_EXPORT_COMPILE_COMMANDS as an environment variable, so the configure line holds no -D the README lacks.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env --unset=VCPKG_ROOT --unset=CMAKE_TOOLCHAIN_FILE CMAKE_EXPORT_COMPILE_COMMANDS=ON
        ${CMAKE_COMMAND} ${configureList} -G "${CONSUMER_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${CONSUMER_MAKE_PROGRAM}"
        "-DCMAKE_CXX_COMPILER=${CONSUMER_CXX_COMPILER}"
    WORKING_DIRECTORY "${project}"
    RESULT_VARIABLE configureRc OUTPUT_VARIABLE configureOut ERROR_VARIABLE configureErr)
if(NOT configureRc EQUAL 0)
    sdk_fail(SDK24 "the README's configure command exited ${configureRc}:\n${configureOut}${configureErr}")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} ${buildList} --parallel 3
    WORKING_DIRECTORY "${project}"
    RESULT_VARIABLE buildRc OUTPUT_VARIABLE buildOut ERROR_VARIABLE buildErr)
if(NOT buildRc EQUAL 0)
    sdk_fail(SDK24 "the README's build command exited ${buildRc}:\n${buildOut}${buildErr}")
endif()

# ---- SDK26: the executable's name, the proof the non-ASCII name reached CMake and the compiler as an identifier. ----
set(exe "")
foreach(directory IN ITEMS "${project}/build" "${project}/build/Release")
    sdk_executable_path(candidate "${directory}" "${expectedTarget}")
    if(EXISTS "${candidate}")
        set(exe "${candidate}")
    endif()
endforeach()
if(exe STREQUAL "")
    file(GLOB_RECURSE builtGames LIST_DIRECTORIES false "${project}/build/game_*")
    sdk_fail(SDK26 "no executable named ${expectedTarget} under ${project}/build; game_* files: ${builtGames}")
endif()

# ---- SDK27: the prefix's include is on the compile line and no toolchain is. --------------------------------------
file(READ "${project}/build/compile_commands.json" compileCommands)
string(TOLOWER "${compileCommands}" loweredCommands)
string(FIND "${compileCommands}" "${prefix}/include" includeAt)
if(includeAt EQUAL -1)
    sdk_fail(SDK27 "the compile database never names ${prefix}/include:\n${compileCommands}")
endif()
if(loweredCommands MATCHES "vcpkg")
    sdk_fail(SDK27 "the compile database names vcpkg:\n${compileCommands}")
endif()

# ---- SDK25: the run, from a folder that is not the build folder. --------------------------------------------------
sdk_run_game(EXE "${exe}" WORKDIR "${elsewhere}" ARGS --frames 30 OUTPUT runOut RESULT runRc)
sdk_gpu_gate(runRc runOut)
if(SDK_SKIPPED)
    return()
endif()
string(FIND "${runOut}" "hello-cube: frames=30 updates=30\n" summaryAt)
if(NOT runRc EQUAL 0 OR summaryAt EQUAL -1)
    sdk_fail(SDK25 "--frames 30 from ${elsewhere} exited ${runRc} and printed:\n${runOut}")
endif()

message(STATUS "sdk.template: SDK22 to SDK27 passed")
