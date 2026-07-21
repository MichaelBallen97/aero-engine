# tests/reflect-gen/run_case.cmake — task 1.1.1 net-new CTest harness (plan/spec D9/§3.6), mirroring
# tests/shaderc/run_case.cmake (task 0.4.3).
#
# Usage: cmake -DTOOL=<path to aero_reflect_gen> -DCASE=<name> -DSOURCE_DIR=<repo root>
#              -DWORK_DIR=<scratch dir, unique per case> "-DCLANG_ARGS=<;-list of clang flags>"
#              -P run_case.cmake
#
# WHY THIS EXISTS (rather than a plain `add_test(COMMAND aero_reflect_gen ...)` per case): several
# cases are NEGATIVE -- they expect aero_reflect_gen itself to exit non-zero (bad_syntax=2,
# missing_input=3, unknown_flag/double_input=1). A raw add_test treats ANY non-zero exit as a ctest
# FAILURE regardless of whether that was the expected behavior. This driver runs the tool via
# execute_process, captures RESULT_VARIABLE, and only FATAL_ERRORs (making run_case.cmake ITSELF exit
# non-zero, which IS what ctest checks) when the actual exit code -- or stdout/stderr content --
# does not match what that case expects.
cmake_minimum_required(VERSION 3.28)

foreach(required TOOL CASE SOURCE_DIR WORK_DIR CLANG_ARGS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_case.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(FIXTURES_DIR "${SOURCE_DIR}/tests/reflect-gen/fixtures")
set(ENGINE_INCLUDE "${SOURCE_DIR}/engine/core/include")
set(HANDLE_HPP "${ENGINE_INCLUDE}/aero/core/handle.hpp")

# Runs aero_reflect_gen once. Spec D8/C.6: ASAN_OPTIONS scoped to this one process --
# detect_leaks=0 only (libclang leaks by design at process exit: global initializers, the CXIndex
# diagnostic engine); UBSan and ASan memory-error detection stay live in aero_reflect_gen's own code
# either way. No build-time invocation exists in this task (unlike aero_shaderc's custom-command
# wrapping), so this is the ONLY place that env var is needed.
function(aero_run_tool)
    cmake_parse_arguments(RT "" "OUT_RESULT;OUT_STDOUT;OUT_STDERR" "ARGS" ${ARGN})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0 "${TOOL}" ${RT_ARGS}
        RESULT_VARIABLE _aero_result
        OUTPUT_VARIABLE _aero_stdout
        ERROR_VARIABLE _aero_stderr
    )
    set(${RT_OUT_RESULT} "${_aero_result}" PARENT_SCOPE)
    if(RT_OUT_STDOUT)
        set(${RT_OUT_STDOUT} "${_aero_stdout}" PARENT_SCOPE)
    endif()
    if(RT_OUT_STDERR)
        set(${RT_OUT_STDERR} "${_aero_stderr}" PARENT_SCOPE)
    endif()
endfunction()

function(aero_expect_exit actual expected)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "case '${CASE}': expected exit ${expected}, got ${actual}")
    endif()
endfunction()

# Like aero_expect_exit, but on a mismatch ALSO dumps the tool's captured stderr. The header-parsing
# cases otherwise swallow it, and the WHY of a parse failure (a clang / MSVC-STL diagnostic) lives
# there -- without this, a lane's parse regression reads only as "expected 0, got 2" with no cause.
function(aero_expect_exit_or_dump actual expected err)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "case '${CASE}': expected exit ${expected}, got ${actual}\n--- tool stderr ---\n${err}\n--- end tool stderr ---")
    endif()
endfunction()

function(aero_expect_stdout_contains out needle)
    string(FIND "${out}" "${needle}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': expected stdout to contain '${needle}', got:\n${out}")
    endif()
endfunction()

function(aero_expect_stderr_contains err needle)
    string(FIND "${err}" "${needle}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': expected stderr to contain '${needle}', got:\n${err}")
    endif()
endfunction()

function(aero_expect_stderr_nonempty err)
    if(err STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': expected non-empty stderr")
    endif()
endfunction()

function(aero_expect_file_contains path needle)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "case '${CASE}': expected output file '${path}' to exist")
    endif()
    file(READ "${path}" _contents)
    string(FIND "${_contents}" "${needle}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': expected file '${path}' to contain '${needle}', got:\n${_contents}")
    endif()
endfunction()

# --- the case table (plan §F / spec §3.6) ----------------------------------------------------------

if(CASE STREQUAL "help")
    aero_run_tool(ARGS --help OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    if(out STREQUAL "")
        message(FATAL_ERROR "case 'help': expected usage text on stdout, got nothing")
    endif()

elseif(CASE STREQUAL "version")
    aero_run_tool(ARGS --version OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    aero_expect_stdout_contains("${out}" "clang")

elseif(CASE STREQUAL "walk_plain")
    # No sysroot at all (self-contained fixture) -- the all-OS deterministic floor (AC-3).
    aero_run_tool(ARGS "${FIXTURES_DIR}/plain_component.hpp" -- -std=c++20 OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    aero_expect_stdout_contains("${out}" "StructDecl 'Demo'")
    aero_expect_stdout_contains("${out}" "FieldDecl 'position'")
    aero_expect_stdout_contains("${out}" "FieldDecl 'mass'")
    aero_expect_stdout_contains("${out}" "FieldDecl 'hitPoints'")

elseif(CASE STREQUAL "parse_engine_header")
    # The real, unmodified engine header -- AC-4's "parses engine headers" proof. No -I needed:
    # handle.hpp's only include is <cstdint> (plan C.1).
    #
    # AC-4 requires exit 0 with zero ERROR/FATAL diagnostics; exit 0 already encodes exactly that (the
    # tool exits 2 on any error/fatal diagnostic, spec D4). Benign WARNINGS on stderr are explicitly
    # allowed by AC-4 and must NOT fail this case -- a stricter empty-stderr assertion would spuriously
    # red a lane whose libclang emits a harmless driver/toolchain note while still exiting 0 (e.g. a
    # newer-MSVC version warning on Windows). Keep only a defensive check that no error-severity line
    # slipped through despite exit 0 (which would be a real tool bug, not a portability artifact).
    aero_run_tool(ARGS "${HANDLE_HPP}" -- ${CLANG_ARGS} OUT_RESULT result OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    string(FIND "${err}" "error:" _idx_err)
    if(NOT _idx_err EQUAL -1)
        message(FATAL_ERROR "case 'parse_engine_header': exit 0, but an error-severity diagnostic appeared "
                            "on stderr parsing an unmodified engine header:\n${err}")
    endif()

elseif(CASE STREQUAL "engine_types")
    aero_run_tool(ARGS "${FIXTURES_DIR}/engine_component.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "Demo")

elseif(CASE STREQUAL "all_flag")
    aero_run_tool(ARGS "${FIXTURES_DIR}/engine_component.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result_default OUT_STDOUT out_default OUT_STDERR err_default)
    aero_expect_exit_or_dump("${result_default}" 0 "${err_default}")
    aero_run_tool(ARGS "${FIXTURES_DIR}/engine_component.hpp" --all -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result_all OUT_STDOUT out_all OUT_STDERR err_all)
    aero_expect_exit_or_dump("${result_all}" 0 "${err_all}")

    string(LENGTH "${out_default}" _len_default)
    string(LENGTH "${out_all}" _len_all)
    if(NOT _len_all GREATER _len_default)
        message(FATAL_ERROR "case 'all_flag': expected --all stdout to be strictly longer than the default "
                            "(default=${_len_default} chars, all=${_len_all} chars)")
    endif()

    # --all must show a Vec3-originated cursor (declared inside math/vec3.hpp, never engine_component.hpp
    # itself) that the main-file-only default omits (AC-7).
    aero_expect_stdout_contains("${out_all}" "StructDecl 'Vec3'")
    string(FIND "${out_default}" "StructDecl 'Vec3'" _idx_default_vec3)
    if(NOT _idx_default_vec3 EQUAL -1)
        message(FATAL_ERROR "case 'all_flag': expected the DEFAULT (main-file-only) walk to NOT contain "
                            "\"StructDecl 'Vec3'\" -- that is the --all-only differential this case proves")
    endif()

elseif(CASE STREQUAL "annotation_visible")
    aero_run_tool(ARGS "${FIXTURES_DIR}/engine_component.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    # F5 (verified live, 2026-07-21): clang-18's actual diagnostic text is "unknown attribute
    # 'component' ignored" -- the scoped-attribute vendor namespace ("engine::") is NOT included in the
    # quoted attribute name, unlike the spec's illustrative quote. "unknown attribute" is the reliably
    # present substring; do not assert the literal string "engine::component" here.
    aero_expect_stderr_contains("${err}" "unknown attribute")

elseif(CASE STREQUAL "bad_syntax")
    file(WRITE "${WORK_DIR}/bad.hpp" "struct { ) Broken")
    aero_run_tool(ARGS "${WORK_DIR}/bad.hpp" -- -x c++ -std=c++20 OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_stderr_nonempty("${err}")

elseif(CASE STREQUAL "missing_input")
    aero_run_tool(ARGS "${WORK_DIR}/nope.hpp" -- -std=c++20 OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 3)
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "case 'missing_input': expected nothing on stdout, got:\n${out}")
    endif()

elseif(CASE STREQUAL "unknown_flag")
    aero_run_tool(ARGS --frobnicate "${FIXTURES_DIR}/plain_component.hpp" OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "double_input")
    aero_run_tool(ARGS a.hpp b.hpp OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "no_input")
    # AC-2's "missing <input> -> exit 1" usage error: a recognized flag but NO positional <input>
    # token. Distinct from `missing_input`, which passes a NON-EXISTENT file path (a real positional
    # argument) and so reaches the I/O path -> exit 3.
    aero_run_tool(ARGS --main-file-only OUT_RESULT result)
    aero_expect_exit("${result}" 1)

elseif(CASE STREQUAL "determinism")
    aero_run_tool(ARGS "${FIXTURES_DIR}/plain_component.hpp" -- -std=c++20 OUT_RESULT result1 OUT_STDOUT out1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS "${FIXTURES_DIR}/plain_component.hpp" -- -std=c++20 OUT_RESULT result2 OUT_STDOUT out2)
    aero_expect_exit("${result2}" 0)
    if(NOT out1 STREQUAL out2)
        message(FATAL_ERROR "case 'determinism': two runs over the same input+flags produced different stdout")
    endif()

elseif(CASE STREQUAL "components_basic")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "component Transform")
    aero_expect_stdout_contains("${out}" "field position : engine::Vec3 [vec3]")
    aero_expect_stdout_contains("${out}" "field rotation : engine::Quat [quat]")
    aero_expect_stdout_contains("${out}" "field mass : float [primitive]")
    aero_expect_stdout_contains("${out}" "field hitPoints : int [primitive]")
    aero_expect_stdout_contains("${out}" "field active : bool [primitive]")
    # non-component sibling, static member, and method must NOT be collected
    foreach(_absent "NotAComponent" "field SCHEMA_VERSION" "field lengthSquared")
        string(FIND "${out}" "${_absent}" _idx)
        if(NOT _idx EQUAL -1)
            message(FATAL_ERROR "case 'components_basic': '${_absent}' must not appear, got:\n${out}")
        endif()
    endforeach()
    # declaration order (AC-9): position before rotation before mass
    string(FIND "${out}" "field position" _p)
    string(FIND "${out}" "field rotation" _r)
    string(FIND "${out}" "field mass" _m)
    if(NOT (_p LESS _r AND _r LESS _m))
        message(FATAL_ERROR "case 'components_basic': fields not in declaration order:\n${out}")
    endif()

elseif(CASE STREQUAL "components_categories")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "[vec3]")
    aero_expect_stdout_contains("${out}" "[quat]")
    aero_expect_stdout_contains("${out}" "[primitive]")

elseif(CASE STREQUAL "components_unsupported")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_unsupported.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "field velocity : engine::Vec4 [unsupported]")
    aero_expect_stderr_contains("${err}" "velocity")
    aero_expect_stderr_contains("${err}" "subset")

elseif(CASE STREQUAL "components_multi")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_multi.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "component Player")
    aero_expect_stdout_contains("${out}" "component engine::demo::Light")
    aero_expect_stdout_contains("${out}" "field color : engine::Vec3 [vec3]")
    aero_expect_stdout_contains("${out}" "field intensity : double [primitive]")

elseif(CASE STREQUAL "components_none")
    # plain_component.hpp uses the LITERAL [[engine::component]] → no annotate cursor → zero components
    # (F1/E1). The injected -DAERO_REFLECT_PARSE is inert here; the unknown-attribute WARNING on stderr
    # is expected and not asserted. Self-contained → bare -std=c++20.
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/plain_component.hpp" -- -std=c++20
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    string(FIND "${out}" "component " _idx)
    if(NOT _idx EQUAL -1)
        message(FATAL_ERROR "case 'components_none': literal attr must detect ZERO components, got:\n${out}")
    endif()

elseif(CASE STREQUAL "components_tag")
    # zero-field tag component: detected, NO field lines (E3/AC-9). Self-contained → bare -std=c++20.
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_tag.hpp" -- -std=c++20
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "component Tag")
    string(FIND "${out}" "field " _idx)
    if(NOT _idx EQUAL -1)
        message(FATAL_ERROR "case 'components_tag': a zero-field component must list no fields, got:\n${out}")
    endif()

elseif(CASE STREQUAL "components_determinism")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result1 OUT_STDOUT out1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result2 OUT_STDOUT out2)
    aero_expect_exit("${result2}" 0)
    if(NOT out1 STREQUAL out2)
        message(FATAL_ERROR "case 'components_determinism': two runs produced different stdout")
    endif()

elseif(CASE STREQUAL "components_bad_syntax")
    # AC-10 under --components: a malformed TU still exits 2. The parse + diagnostics + verdict path is
    # shared with walk-mode and runs regardless of the detection dispatch. Mirrors bad_syntax.
    file(WRITE "${WORK_DIR}/bad.hpp" "struct { ) Broken")
    aero_run_tool(ARGS --components "${WORK_DIR}/bad.hpp" -- -x c++ -std=c++20 OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_stderr_nonempty("${err}")

elseif(CASE STREQUAL "components_missing_input")
    # AC-10 under --components: an unreadable <input> exits 3 (isReadableFile precedes the dispatch).
    aero_run_tool(ARGS --components "${WORK_DIR}/nope.hpp" -- -std=c++20 OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 3)
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "case 'components_missing_input': expected nothing on stdout, got:\n${out}")
    endif()

elseif(CASE STREQUAL "emit_basic")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "void aero_reflect_register_component_basic(")
    aero_expect_stdout_contains("${out}" "entt::meta_factory<Transform>")
    aero_expect_stdout_contains("${out}" ".type(\"Transform\"_hs")
    aero_expect_stdout_contains("${out}" ".data<&Transform::position>(\"position\"_hs")
    aero_expect_stdout_contains("${out}" ".data<&Transform::rotation>")
    aero_expect_stdout_contains("${out}" ".data<&Transform::mass>")
    aero_expect_stdout_contains("${out}" ".data<&Transform::hitPoints>")
    aero_expect_stdout_contains("${out}" ".data<&Transform::active>")
    foreach(_absent "SCHEMA_VERSION" "lengthSquared" "NotAComponent")
        string(FIND "${out}" "${_absent}" _idx)
        if(NOT _idx EQUAL -1)
            message(FATAL_ERROR "case 'emit_basic': '${_absent}' must not appear, got:\n${out}")
        endif()
    endforeach()

elseif(CASE STREQUAL "emit_output_file")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_basic.hpp" -o "${WORK_DIR}/out.cpp"
        -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "case 'emit_output_file': -o must write nothing to stdout, got:\n${out}")
    endif()
    aero_expect_file_contains("${WORK_DIR}/out.cpp" "entt::meta_factory<Transform>")
    aero_expect_file_contains("${WORK_DIR}/out.cpp" ".data<&Transform::position>(\"position\"_hs")

elseif(CASE STREQUAL "emit_unsupported")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_unsupported.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" ".data<&Transform::position>")
    aero_expect_stdout_contains("${out}" ".data<&Transform::mass>")
    aero_expect_stdout_contains("${out}" "// skipped: velocity")
    string(FIND "${out}" ".data<&Transform::velocity>" _idx)
    if(NOT _idx EQUAL -1)
        message(FATAL_ERROR "case 'emit_unsupported': velocity must be skipped, got:\n${out}")
    endif()
    aero_expect_stderr_contains("${err}" "velocity")
    aero_expect_stderr_contains("${err}" "subset")

elseif(CASE STREQUAL "emit_multi")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_multi.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "entt::meta_factory<Player>")
    aero_expect_stdout_contains("${out}" "entt::meta_factory<engine::demo::Light>")
    aero_expect_stdout_contains("${out}" ".type(\"engine::demo::Light\"_hs")
    aero_expect_stdout_contains("${out}" ".data<&engine::demo::Light::color>")

elseif(CASE STREQUAL "emit_tag")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_tag.hpp" -- -std=c++20
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "entt::meta_factory<Tag>{}")
    aero_expect_stdout_contains("${out}" ".type(\"Tag\"_hs")
    string(FIND "${out}" ".data<" _idx)
    if(NOT _idx EQUAL -1)
        message(FATAL_ERROR "case 'emit_tag': a zero-field tag must have no .data< line, got:\n${out}")
    endif()

elseif(CASE STREQUAL "emit_none")
    # plain_component.hpp uses the LITERAL [[engine::component]] -> AST-invisible -> zero components (1.1.2 F1).
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/plain_component.hpp" -- -std=c++20
        OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_stdout_contains("${out}" "void aero_reflect_register_plain_component(")
    string(FIND "${out}" "entt::meta_factory<" _idx)
    if(NOT _idx EQUAL -1)
        message(FATAL_ERROR "case 'emit_none': literal attr must emit NO meta_factory chain, got:\n${out}")
    endif()

elseif(CASE STREQUAL "emit_determinism")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result1 OUT_STDOUT out1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_basic.hpp" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}"
        OUT_RESULT result2 OUT_STDOUT out2)
    aero_expect_exit("${result2}" 0)
    if(NOT out1 STREQUAL out2)
        message(FATAL_ERROR "case 'emit_determinism': two --emit-meta runs produced different stdout")
    endif()

elseif(CASE STREQUAL "emit_bad_syntax")
    file(WRITE "${WORK_DIR}/bad.hpp" "struct { ) Broken")
    aero_run_tool(ARGS --emit-meta "${WORK_DIR}/bad.hpp" -- -x c++ -std=c++20 OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)

elseif(CASE STREQUAL "emit_missing_input")
    aero_run_tool(ARGS --emit-meta "${WORK_DIR}/nope.hpp" -- -std=c++20 OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 3)
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "case 'emit_missing_input': expected nothing on stdout, got:\n${out}")
    endif()

elseif(CASE STREQUAL "depfile_basic")
    # Same flag shape as the landed 1.1.3 build command; aero_reflect.hpp resolves includer-relative.
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_codegen.hpp"
        -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/out.d"
        -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_file_contains("${WORK_DIR}/out.cpp" "entt::meta_factory<ReflectSample>")   # sanity
    aero_expect_file_contains("${WORK_DIR}/out.d" "out.cpp")             # rule target
    aero_expect_file_contains("${WORK_DIR}/out.d" "component_codegen.hpp")
    aero_expect_file_contains("${WORK_DIR}/out.d" "aero_reflect.hpp")    # direct
    aero_expect_file_contains("${WORK_DIR}/out.d" "math.hpp")            # direct
    aero_expect_file_contains("${WORK_DIR}/out.d" "math/constants.hpp")  # TRANSITIVE (depth 2, F2/AC-3)

elseif(CASE STREQUAL "depfile_requires_output")
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_codegen.hpp" --depfile "${WORK_DIR}/out.d"
        -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT result)
    aero_expect_exit("${result}" 1)
    if(EXISTS "${WORK_DIR}/out.d")
        message(FATAL_ERROR "case 'depfile_requires_output': --depfile without -o must not write a depfile")
    endif()

elseif(CASE STREQUAL "depfile_requires_emit")
    aero_run_tool(ARGS --components "${FIXTURES_DIR}/component_codegen.hpp" -o "${WORK_DIR}/o.cpp"
        --depfile "${WORK_DIR}/out.d" -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT result)
    aero_expect_exit("${result}" 1)
    if(EXISTS "${WORK_DIR}/out.d" OR EXISTS "${WORK_DIR}/o.cpp")
        message(FATAL_ERROR "case 'depfile_requires_emit': --depfile without --emit-meta must write nothing")
    endif()

elseif(CASE STREQUAL "depfile_parse_fail")
    file(WRITE "${WORK_DIR}/bad.hpp" "struct { ) Broken")
    aero_run_tool(ARGS --emit-meta "${WORK_DIR}/bad.hpp" -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/out.d"
        -- -x c++ -std=c++20 OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    if(EXISTS "${WORK_DIR}/out.cpp" OR EXISTS "${WORK_DIR}/out.d")
        message(FATAL_ERROR "case 'depfile_parse_fail': parse failure must leave NEITHER file (D8)")
    endif()

elseif(CASE STREQUAL "depfile_space_escape")
    # component_tag.hpp is self-contained (bare -std=c++20, no sysroot) so the only -I is the space dir.
    file(COPY "${FIXTURES_DIR}/component_tag.hpp" "${FIXTURES_DIR}/aero_reflect.hpp"
        DESTINATION "${WORK_DIR}/dir with space")
    aero_run_tool(ARGS --emit-meta "${WORK_DIR}/dir with space/component_tag.hpp"
        -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/out.d"
        -- -std=c++20 -I "${WORK_DIR}/dir with space" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit_or_dump("${result}" 0 "${err}")
    aero_expect_file_contains("${WORK_DIR}/out.d" "dir\\ with\\ space")   # backslash-escaped space (D7)

elseif(CASE STREQUAL "depfile_determinism")
    # SAME -o path both runs (identical target token by construction), two DIFFERENT --depfile paths.
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_codegen.hpp"
        -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/a.d"
        -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT r1)
    aero_expect_exit("${r1}" 0)
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_codegen.hpp"
        -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/b.d"
        -- ${CLANG_ARGS} -I "${ENGINE_INCLUDE}" OUT_RESULT r2)
    aero_expect_exit("${r2}" 0)
    file(READ "${WORK_DIR}/a.d" _a)
    file(READ "${WORK_DIR}/b.d" _b)
    if(NOT _a STREQUAL _b)
        message(FATAL_ERROR "case 'depfile_determinism': two depfiles differ:\n---a---\n${_a}\n---b---\n${_b}")
    endif()

elseif(CASE STREQUAL "depfile_unwritable")
    # AC-2: an unwritable --depfile exits 3 with a stderr message. A non-existent PARENT dir (nope/) is
    # the portable, deterministic way to force the ofstream open to fail — no chmod/permission trick
    # (Linux CI runs as root and would bypass a permission bit). -o is writable, so it succeeds first;
    # only the depfile open fails, exercising writeDepfile's exit-3 path (the -o mirror, V5).
    aero_run_tool(ARGS --emit-meta "${FIXTURES_DIR}/component_tag.hpp"
        -o "${WORK_DIR}/out.cpp" --depfile "${WORK_DIR}/nope/out.d"
        -- -std=c++20 OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_stderr_contains("${err}" "depfile")   # the depfile path failed, not -o

else()
    message(FATAL_ERROR "run_case.cmake: unknown CASE '${CASE}'")
endif()

message(STATUS "reflect-gen ctest case '${CASE}': OK")
