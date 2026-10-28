# tests/typescript/run_tsc.cmake -- task 4.2.2. Run as `cmake -P` by the four typescript.* ctest cases.
#
# It type-checks the declarations a script author's editor sees: a project is assembled in WORK_DIR (assets/, the
# declarations under Library/types/, and the tsconfig the editor writes), and the PINNED compiler named by
# tests/typescript/package.json is run over it. The compiler is the one VS Code bundles, so a verdict here is the
# verdict the squiggles give.
#
# Inputs: CASE (clean | broken | prelude_only | driver_e2e), SOURCE_DIR, WORK_DIR, DECLARATIONS (the build's
# aero-engine.d.ts), PRELUDE (engine/script/types/prelude.d.ts), JOIN (cmake/reflect_declarations_join.cmake),
# TEMPLATE (editor/templates/tsconfig.json.in). Two overrides only driver_e2e passes: TSC_ROOT (the folder holding
# package.json and node_modules; default tests/typescript) and NODE (default: find_program at run time).
#
# A missing Node or a missing compiler is a loud skip -- one line the ctest SKIP_REGULAR_EXPRESSION reads -- unless
# AERO_REQUIRE_TSC is 1, when it is a failure. A compiler whose version differs from the pin is ALWAYS a failure.
cmake_minimum_required(VERSION 3.28)

foreach(variable IN ITEMS CASE SOURCE_DIR WORK_DIR DECLARATIONS PRELUDE JOIN TEMPLATE)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "typescript: ${variable} is required")
    endif()
endforeach()
set(_known_cases clean broken prelude_only driver_e2e)
if(NOT CASE IN_LIST _known_cases)
    message(FATAL_ERROR "typescript: unknown CASE '${CASE}'")
endif()
if(NOT DEFINED TSC_ROOT OR TSC_ROOT STREQUAL "")
    set(TSC_ROOT "${SOURCE_DIR}/tests/typescript")
endif()
set(_fixtures "${CMAKE_CURRENT_LIST_DIR}/fixtures")

# Splits TEXT into lines without a list (a line may hold ';' or brackets, and an empty line must keep its number):
# sets ${PREFIX}_COUNT and ${PREFIX}_<n> (1-based) in the caller's scope. A CR is dropped first.
function(aero_ts_split_lines TEXT PREFIX)
    string(REPLACE "\r" "" rest "${TEXT}")
    set(count 0)
    while(NOT rest STREQUAL "")
        string(REGEX MATCH "^[^\n]*" line "${rest}")
        string(LENGTH "${line}" lineLength)
        math(EXPR next "${lineLength} + 1")
        string(SUBSTRING "${rest}" ${next} -1 rest)
        math(EXPR count "${count} + 1")
        set(${PREFIX}_${count} "${line}" PARENT_SCOPE)
    endwhile()
    set(${PREFIX}_COUNT ${count} PARENT_SCOPE)
endfunction()

# ---- driver_e2e: the skip, require and version-mismatch paths, with no Node at all -------------------------------
if(CASE STREQUAL "driver_e2e")
    file(REMOVE_RECURSE "${WORK_DIR}")
    set(_empty_root "${WORK_DIR}/empty-root")
    set(_fake_root "${WORK_DIR}/fake-root")
    file(MAKE_DIRECTORY "${_empty_root}" "${_fake_root}/node_modules/typescript/bin")
    file(WRITE "${_fake_root}/package.json" "{\"devDependencies\": {\"typescript\": \"1.2.3\"}}\n")
    file(WRITE "${_fake_root}/node_modules/typescript/package.json" "{\"version\": \"4.5.6\"}\n")
    file(WRITE "${_fake_root}/node_modules/typescript/bin/tsc" "\n")

    # Each stage re-runs this very file as the case `prelude_only`, which reaches the locate step first.
    function(aero_ts_stage NAME ENV_ARG ROOT NODE_PATH)
        set(node_arg "")
        if(NOT NODE_PATH STREQUAL "")
            set(node_arg "-DNODE=${NODE_PATH}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARG}
                "${CMAKE_COMMAND}" -DCASE=prelude_only -DSOURCE_DIR=${SOURCE_DIR} -DWORK_DIR=${WORK_DIR}/child
                -DDECLARATIONS=${DECLARATIONS} -DPRELUDE=${PRELUDE} -DJOIN=${JOIN} -DTEMPLATE=${TEMPLATE}
                -DTSC_ROOT=${ROOT} ${node_arg} -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE rv OUTPUT_VARIABLE out ERROR_VARIABLE err)
        set(${NAME}_RV "${rv}" PARENT_SCOPE)
        set(${NAME}_OUT "${out}${err}" PARENT_SCOPE)
    endfunction()

    # (1) No compiler, AERO_REQUIRE_TSC unset: exit 0 and the SKIPPED line.
    aero_ts_stage(SKIP "--unset=AERO_REQUIRE_TSC" "${_empty_root}" "")
    if(NOT SKIP_RV EQUAL 0 OR NOT SKIP_OUT MATCHES "typescript: SKIPPED")
        message(FATAL_ERROR "typescript: driver_e2e (1): expected exit 0 and the SKIPPED line, got ${SKIP_RV}:\n${SKIP_OUT}")
    endif()
    # (2) The same with AERO_REQUIRE_TSC=1: a failure naming the variable.
    aero_ts_stage(REQUIRE "AERO_REQUIRE_TSC=1" "${_empty_root}" "")
    if(REQUIRE_RV EQUAL 0 OR NOT REQUIRE_OUT MATCHES "AERO_REQUIRE_TSC" OR REQUIRE_OUT MATCHES "typescript: SKIPPED")
        message(FATAL_ERROR "typescript: driver_e2e (2): expected a failure naming AERO_REQUIRE_TSC, got ${REQUIRE_RV}:\n${REQUIRE_OUT}")
    endif()
    # (3) A compiler whose version differs from the pin: a failure naming both versions, whatever the variable says.
    # NODE is any existing program: the version check runs before anything is executed.
    aero_ts_stage(MISMATCH "--unset=AERO_REQUIRE_TSC" "${_fake_root}" "${CMAKE_COMMAND}")
    if(MISMATCH_RV EQUAL 0 OR NOT MISMATCH_OUT MATCHES "1\\.2\\.3" OR NOT MISMATCH_OUT MATCHES "4\\.5\\.6"
       OR MISMATCH_OUT MATCHES "typescript: SKIPPED")
        message(FATAL_ERROR "typescript: driver_e2e (3): expected a failure naming 1.2.3 and 4.5.6, got ${MISMATCH_RV}:\n${MISMATCH_OUT}")
    endif()
    message(STATUS "typescript: driver_e2e OK -- skip, require and version-mismatch paths")
    return()
endif()

# ---- Locate the compiler, or skip loudly -------------------------------------------------------------------------
if(NOT DEFINED NODE OR NODE STREQUAL "")
    find_program(_aero_node node)
    set(NODE "${_aero_node}")
endif()
set(_tsc "${TSC_ROOT}/node_modules/typescript/bin/tsc")
set(_missing "")
if(NODE STREQUAL "" OR NODE MATCHES "NOTFOUND")
    set(_missing "node is not on PATH")
elseif(NOT EXISTS "${_tsc}")
    set(_missing "${_tsc} does not exist")
endif()
if(NOT _missing STREQUAL "")
    if("$ENV{AERO_REQUIRE_TSC}" STREQUAL "1")
        message(FATAL_ERROR "typescript: ${_missing}, and AERO_REQUIRE_TSC=1 makes that a failure; run npm ci in tests/typescript")
    endif()
    message("typescript: SKIPPED -- ${_missing}; run npm ci in tests/typescript")
    return()
endif()

# ---- The version is checked before anything runs; a mismatch always fails ----------------------------------------
file(READ "${TSC_ROOT}/package.json" _pin_json)
string(JSON _pin GET "${_pin_json}" devDependencies typescript)
file(READ "${TSC_ROOT}/node_modules/typescript/package.json" _installed_json)
string(JSON _installed GET "${_installed_json}" version)
if(NOT _pin STREQUAL _installed)
    message(FATAL_ERROR "typescript: the installed compiler is ${_installed} but ${TSC_ROOT}/package.json pins ${_pin}; run npm ci in tests/typescript")
endif()

# ---- Assemble the project ----------------------------------------------------------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/assets" "${WORK_DIR}/Library/types")
if(CASE STREQUAL "prelude_only")
    # The reflect-tools-OFF artifact: the real join over the prelude alone.
    file(WRITE "${WORK_DIR}/prelude-only.parts" "${PRELUDE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DPARTS=${WORK_DIR}/prelude-only.parts
            -DOUTPUT=${WORK_DIR}/Library/types/aero-engine.d.ts -P "${JOIN}"
        RESULT_VARIABLE rv OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rv EQUAL 0)
        message(FATAL_ERROR "typescript: the join over the prelude failed (${rv}):\n${out}${err}")
    endif()
else()
    file(COPY_FILE "${DECLARATIONS}" "${WORK_DIR}/Library/types/aero-engine.d.ts")
endif()
file(COPY "${_fixtures}/${CASE}/" DESTINATION "${WORK_DIR}/assets")
if(CASE STREQUAL "clean")
    # A type error under Library/Trash/, which the template's `exclude` must keep unchecked.
    file(MAKE_DIRECTORY "${WORK_DIR}/Library/Trash/0001")
    file(COPY_FILE "${_fixtures}/trash/old.ts" "${WORK_DIR}/Library/Trash/0001/old.ts")
endif()
# The tsconfig is the editor's template with its one placeholder filled in, exactly as the editor writes it.
file(READ "${TEMPLATE}" _template)
string(FIND "${_template}" "@AERO_SCRIPT_INCLUDE@" _first)
string(FIND "${_template}" "@AERO_SCRIPT_INCLUDE@" _last REVERSE)
if(_first EQUAL -1 OR NOT _first EQUAL _last)
    message(FATAL_ERROR "typescript: ${TEMPLATE} must hold @AERO_SCRIPT_INCLUDE@ exactly once")
endif()
# `clean` uses the glob a project whose assets root is its own root gets (editor renderTsconfig(".")), the one that
# reaches Library/Trash/, so a dropped `exclude` is caught; the other cases use the usual assets glob.
set(_include "assets/**/*.ts")
if(CASE STREQUAL "clean")
    set(_include "./**/*.ts")
endif()
string(REPLACE "@AERO_SCRIPT_INCLUDE@" "\"${_include}\"" _tsconfig "${_template}")
file(WRITE "${WORK_DIR}/tsconfig.json" "${_tsconfig}")

# ---- Run the compiler --------------------------------------------------------------------------------------------
execute_process(
    COMMAND "${NODE}" "${_tsc}" -p . --pretty false
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rv OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
set(_report "${_out}${_err}")

if(NOT CASE STREQUAL "broken")
    if(NOT _rv EQUAL 0 OR _report MATCHES "error TS")
        message(FATAL_ERROR "typescript.${CASE}: expected zero errors, tsc exited ${_rv}:\n${_report}")
    endif()
    message(STATUS "typescript.${CASE}: OK -- zero errors")
    return()
endif()

# ---- broken: every `// expect TS<n>` line draws exactly that error, and nothing else draws one -------------------
file(READ "${WORK_DIR}/assets/broken.ts" _source)
aero_ts_split_lines("${_source}" SRC)
set(_tagged 0)
set(_failures "")
foreach(n RANGE 1 ${SRC_COUNT})
    if(SRC_${n} MATCHES "// expect (TS[0-9]+)$")
        set(expect_${n} "${CMAKE_MATCH_1}")
        math(EXPR _tagged "${_tagged} + 1")
    endif()
endforeach()
if(_tagged EQUAL 0)
    message(FATAL_ERROR "typescript.broken: the fixture carries no `// expect TS<n>` tag")
endif()
if(_rv EQUAL 0)
    string(APPEND _failures "tsc exited 0 over a fixture full of errors\n")
endif()
aero_ts_split_lines("${_report}" OUT)
foreach(n RANGE 0 ${OUT_COUNT})
    set(line "${OUT_${n}}")
    if(line STREQUAL "" OR line MATCHES "^[ \t]")
        continue()    # a continuation of the previous diagnostic
    endif()
    if(line MATCHES "^assets[/\\\\]broken\\.ts\\(([0-9]+),[0-9]+\\): error (TS[0-9]+):")
        set(at "${CMAKE_MATCH_1}")
        list(APPEND got_${at} "${CMAKE_MATCH_2}")
    else()
        string(APPEND _failures "unexpected output line: ${line}\n")
    endif()
endforeach()
foreach(n RANGE 1 ${SRC_COUNT})
    if(DEFINED expect_${n})
        if(NOT DEFINED got_${n})
            string(APPEND _failures "line ${n}: expected ${expect_${n}}, got no error\n")
        elseif(NOT "${got_${n}}" STREQUAL "${expect_${n}}")
            string(APPEND _failures "line ${n}: expected exactly ${expect_${n}}, got ${got_${n}}\n")
        endif()
    elseif(DEFINED got_${n})
        string(APPEND _failures "line ${n}: untagged line drew ${got_${n}}\n")
    endif()
endforeach()
if(NOT _failures STREQUAL "")
    message(FATAL_ERROR "typescript.broken: the diagnostics differ from the tags:\n${_failures}\ntsc output:\n${_report}")
endif()
message(STATUS "typescript.broken: OK -- ${_tagged} tagged lines, each exactly its code")
