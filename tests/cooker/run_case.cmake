# tests/cooker/run_case.cmake — task 3.3.1's process-boundary harness for aero_cooker, driving BOTH
# subcommands since task 3.3.2: `mesh` (source model -> .aeromesh) and `texture` (source image ->
# KTX2). One driver, one case table, two grammars.
#
# Usage: cmake -DTOOL=<path to aero_cooker> -DCASE=<name> -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch
#              dir, unique per case> -P run_case.cmake
#
# WHY THIS EXISTS (rather than a plain `add_test(COMMAND aero_cooker ...)` per case): most cases here
# are NEGATIVE -- they expect aero_cooker itself to exit non-zero (1 for every grammar violation, 2
# for an import or cook error, 3 for an I/O error). A raw add_test treats ANY non-zero exit as a ctest
# FAILURE regardless of whether that was the expected behaviour. This driver runs the tool through
# execute_process, captures RESULT_VARIABLE, and only FATAL_ERRORs -- which is what ctest checks --
# when the actual exit code does not match the expected one for that case.
#
# CLONED from tests/shaderc/run_case.cmake rather than shared with it: the two tools have different
# artifact shapes, and one driver serving both would couple them for no gain.
cmake_minimum_required(VERSION 3.28)

foreach(required TOOL CASE SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_case.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Runs aero_cooker once. ASAN_OPTIONS is scoped to this one process, exactly as
# tests/shaderc/run_case.cmake scopes it and for the same reason: this is a one-shot process whose
# exit-time leaks are not what any case here is about, and aero_editor_core drags in four third-party
# parsers that leak on their own error paths (the two assimp frames tests/lsan.supp already names).
# UBSan and ASan memory-error detection stay live.
#
# task 3.3.3 added the two OPTIONAL keywords and nothing else:
#   ENV <NAME=VALUE>...   extra environment entries, appended AFTER the ASan pair so this function
#                         stays the ONE place that pair is spelled -- the golden_manifest arms'
#                         perturbed re-cook needs a hostile TZ and locale, and a second
#                         execute_process in the arm would make preserving ASAN_OPTIONS a convention
#                         between two copies instead of a property of one function.
#   WORKING_DIRECTORY <d> run from somewhere other than ctest's cwd. Passed through only when
#                         non-empty: execute_process REJECTS an empty WORKING_DIRECTORY rather than
#                         ignoring it.
function(aero_run_tool)
    cmake_parse_arguments(RT "" "OUT_RESULT;OUT_STDOUT;OUT_STDERR;WORKING_DIRECTORY" "ARGS;ENV" ${ARGN})
    set(_aero_where)
    if(RT_WORKING_DIRECTORY)
        set(_aero_where WORKING_DIRECTORY "${RT_WORKING_DIRECTORY}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ASAN_OPTIONS=detect_leaks=0 ${RT_ENV} "${TOOL}" ${RT_ARGS}
        ${_aero_where}
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

function(aero_expect_files)
    foreach(f IN LISTS ARGN)
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "case '${CASE}': expected file '${f}' to exist")
        endif()
    endforeach()
endfunction()

function(aero_expect_no_files)
    foreach(f IN LISTS ARGN)
        if(EXISTS "${f}")
            message(FATAL_ERROR "case '${CASE}': expected NO file at '${f}' but it exists")
        endif()
    endforeach()
endfunction()

# The container's eight-byte magic, assertable in pure CMake with no helper binary: file(READ ... HEX)
# yields 4145524f4d455348 for "AEROMESH". HEX mode reads a fixed byte count unambiguously, which plain
# LIMIT does not on a binary file (tests/shaderc/verify_artifacts.cmake measured that the hard way).
function(aero_expect_magic path)
    aero_expect_files("${path}")
    file(READ "${path}" magic LIMIT 8 HEX)
    if(NOT magic STREQUAL "4145524f4d455348")
        message(FATAL_ERROR "case '${CASE}': '${path}' does not start with the AEROMESH magic (got 0x${magic})")
    endif()
endfunction()

function(aero_expect_size path expected)
    aero_expect_files("${path}")
    file(SIZE "${path}" actual)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "case '${CASE}': '${path}' is ${actual} bytes, expected ${expected}")
    endif()
endfunction()

# `count` bytes at `offset`, as LOWERCASE hex, compared against a literal. This is what lets a case
# decode a header field from the process tier without linking anything: the whole format is
# little-endian, so 2.0f at a bounds offset reads "00000040" and a GUID half reads back-to-front.
function(aero_expect_hex_at path offset count expected)
    aero_expect_files("${path}")
    math(EXPR limit "${offset} + ${count}")
    file(READ "${path}" blob LIMIT ${limit} HEX)
    math(EXPR start "${offset} * 2")
    math(EXPR length "${count} * 2")
    string(SUBSTRING "${blob}" ${start} ${length} actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "case '${CASE}': '${path}' at byte ${offset} reads 0x${actual}, expected 0x${expected}")
    endif()
endfunction()

# Strictly smaller, never merely different: task 3.3.2's texture_no_mips asserts that dropping the mip
# chain removes bytes, which "not identical" would also be true of a chain that grew.
function(aero_expect_smaller_than path_a path_b)
    aero_expect_files("${path_a}" "${path_b}")
    file(SIZE "${path_a}" size_a)
    file(SIZE "${path_b}" size_b)
    if(NOT size_a LESS size_b)
        message(FATAL_ERROR "case '${CASE}': '${path_a}' is ${size_a} bytes, expected fewer than '${path_b}'"
                            " at ${size_b}")
    endif()
endfunction()

function(aero_expect_identical path_a path_b)
    aero_expect_files("${path_a}" "${path_b}")
    file(MD5 "${path_a}" hash_a)
    file(MD5 "${path_b}" hash_b)
    if(NOT hash_a STREQUAL hash_b)
        message(FATAL_ERROR "case '${CASE}': '${path_a}' and '${path_b}' are not byte-identical")
    endif()
endfunction()

function(aero_expect_contains text needle what)
    string(FIND "${text}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': ${what} does not contain '${needle}'")
    endif()
endfunction()

function(aero_expect_non_empty text what)
    if(text STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': expected non-empty ${what}, got nothing")
    endif()
endfunction()

# --- task 3.3.3: the frozen cook-determinism manifest -----------------------------------------------
#
# Parses tests/cooker/determinism.sha256 into two parallel lists. The format is FIXED WIDTH by
# construction -- 64 lowercase hex, exactly two spaces, then a name with no whitespace -- so the split
# is positional rather than a regex capture, which also makes every malformed shape its own message.
#
# file(STRINGS) strips \r, so a CRLF checkout parses identically (measured, not assumed); it also
# keeps BLANK lines as empty entries, which is why the empty check comes first and why every consumer
# of this file elsewhere filters with `grep -vE '^#|^$'` rather than `grep -v '^#'`.
#
# DUPLICATE DETECTION IS BY NAME AND NEVER BY HASH. mesh-triangle and mesh-external genuinely share a
# hash -- same geometry, no material, nil GUID, and where the buffer came from never reaches the file
# -- so a "the 13 hashes are distinct" check would be red on the day it was written.
function(aero_read_manifest out_names out_hashes)
    set(path "${SOURCE_DIR}/tests/cooker/determinism.sha256")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "case '${CASE}': the manifest '${path}' does not exist")
    endif()
    file(STRINGS "${path}" raw)
    set(names "")
    set(hashes "")
    foreach(line IN LISTS raw)
        if(line STREQUAL "")
            continue()
        endif()
        string(SUBSTRING "${line}" 0 1 lead)
        if(lead STREQUAL "#")
            continue()
        endif()
        string(LENGTH "${line}" len)
        if(len LESS 67)
            message(FATAL_ERROR "case '${CASE}': malformed manifest line (shorter than 67 characters): "
                                "'${line}'")
        endif()
        string(SUBSTRING "${line}" 0 64 hash)
        string(SUBSTRING "${line}" 64 2 gap)
        string(SUBSTRING "${line}" 66 -1 name)
        if(NOT hash MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "case '${CASE}': a manifest line must start with 64 LOWERCASE hex "
                                "digits: '${line}'")
        endif()
        if(NOT gap STREQUAL "  ")
            message(FATAL_ERROR "case '${CASE}': a manifest line needs exactly two spaces after the "
                                "hash (sha256sum -c check format): '${line}'")
        endif()
        if(name STREQUAL "" OR name MATCHES "[ \t]")
            message(FATAL_ERROR "case '${CASE}': a manifest name is empty or holds whitespace: '${line}'")
        endif()
        list(FIND names "${name}" dup)
        if(NOT dup EQUAL -1)
            message(FATAL_ERROR "case '${CASE}': the manifest lists '${name}' twice")
        endif()
        list(APPEND names "${name}")
        list(APPEND hashes "${hash}")
    endforeach()
    list(LENGTH names count)
    # A LITERAL 15, never a count derived from the file it is checking: a guard computed from its own
    # subject cannot see a line deleted. All three cases assert it, so a deletion reddens them at once.
    if(NOT count EQUAL 15)
        message(FATAL_ERROR "case '${CASE}': the manifest holds ${count} entries, expected exactly 15")
    endif()
    set(${out_names} "${names}" PARENT_SCOPE)
    set(${out_hashes} "${hashes}" PARENT_SCOPE)
endfunction()

# A comment-stripped source-text ordering gate, for the ONE property in this tool that no process-tier
# case can observe (see texture_nothing_written_on_failure). `//` comments are removed first, so prose
# naming either needle cannot stand in for the code that uses it.
#
# The search is SCOPED to one function, between `region_begin` and `region_end`, because the two
# subcommands are deliberate mirrors of each other: `writeTextFileAtomic(args.outputPath, artifact)`
# is character-for-character identical in runMesh and runTexture, so an unscoped search would compare
# a needle in one function against a needle in the other and report an ordering that means nothing.
function(aero_expect_source_order path region_begin region_end first second)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "case '${CASE}': expected source file '${path}' to exist")
    endif()
    file(READ "${path}" source)
    string(LENGTH "${source}" source_length)
    if(source_length LESS 1000)
        message(FATAL_ERROR "case '${CASE}': '${path}' is only ${source_length} bytes; that is not the file")
    endif()
    string(REGEX REPLACE "//[^\n]*" "" stripped "${source}")
    string(LENGTH "${stripped}" stripped_length)
    if(NOT stripped_length LESS source_length)   # anti-vacuity: the strip must have removed something
        message(FATAL_ERROR "case '${CASE}': stripping comments from '${path}' removed nothing")
    endif()

    string(FIND "${stripped}" "${region_begin}" region_at)
    if(region_at EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': '${path}' does not contain the region opener '${region_begin}'")
    endif()
    string(SUBSTRING "${stripped}" ${region_at} -1 region)
    string(FIND "${region}" "${region_end}" region_length)
    if(region_length EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': '${path}' does not contain the region closer '${region_end}'")
    endif()
    string(SUBSTRING "${region}" 0 ${region_length} region)

    string(FIND "${region}" "${first}" first_at)
    string(FIND "${region}" "${second}" second_at)
    if(first_at EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': '${path}' does not contain '${first}' in that region, outside"
                            " a comment")
    endif()
    if(second_at EQUAL -1)
        message(FATAL_ERROR "case '${CASE}': '${path}' does not contain '${second}' in that region, outside"
                            " a comment")
    endif()
    if(NOT first_at LESS second_at)
        message(FATAL_ERROR "case '${CASE}': in '${path}', '${first}' (at ${first_at}) must come BEFORE"
                            " '${second}' (at ${second_at})")
    endif()
    string(FIND "${region}" "${second}" second_again REVERSE)
    if(NOT second_again EQUAL second_at)
        message(FATAL_ERROR "case '${CASE}': '${second}' occurs more than once in that region of"
                            " '${path}', so 'before' is ambiguous")
    endif()
endfunction()

function(aero_verify_no_files_in dir)
    file(GLOB leftover "${dir}/*")
    if(leftover)
        message(FATAL_ERROR "case '${CASE}': expected NO files under '${dir}', found: ${leftover}")
    endif()
endfunction()

# Two fixture roots: the ones the editor suites already share, and the seven this tool owns. The driver
# composes both from SOURCE_DIR, which is what lets the process tier reach a committed fixture without
# hard-coding a relative path (tests/cooker/fixtures/README.md lists what each one is for).
set(ANY_INPUT "${SOURCE_DIR}/tests/fixtures/assets/triangle.gltf")
set(FIXTURES "${SOURCE_DIR}/tests/cooker/fixtures")
set(OUT "${WORK_DIR}/out.aeromesh")

# task 3.3.2: the tree's first two committed images, shared with aero_editor_shell_test's TK battery.
# The 5x3 is opaque and odd in BOTH axes (so `auto` answers BC1 and the polyphase filter runs on both
# axes); the 8x8 has a half-transparent quadrant (so `auto` answers BC3).
set(TEXTURE_OPAQUE "${SOURCE_DIR}/tests/fixtures/assets/texture-rgb-5x3.png")
set(TEXTURE_ALPHA "${SOURCE_DIR}/tests/fixtures/assets/texture-rgba-8x8.png")
set(TEXOUT "${WORK_DIR}/out.ktx2")

# KTX2 field offsets, restated here because the process tier cannot include a header.
# engine/assets/include/aero/assets/cooked_texture.hpp is the normative copy; a disagreement between
# these numbers and that header is a real defect in one of them.
set(KTX2_IDENTIFIER_HEX "ab4b5458203230bb0d0a1a0a")
set(OFFSET_VK_FORMAT 12)
set(OFFSET_LEVEL_COUNT 40)
# 80 header+Index + 24*3 level records = 152, + 44 (the BC1 descriptor) = 196 kvdByteOffset, + 4
# (keyAndValueByteLength) + 15 ("AeroSourceGuid\0") = 215, where the 32 lowercase hex characters start.
# The offset is exact for a 3-level BC1 file and for nothing else, which is why texture_guid_written
# cooks exactly that shape.
set(OFFSET_BC1_3LEVEL_GUID_VALUE 215)

# Golden B's own size and two of its own field offsets, restated here because the process tier cannot
# include a header. tests/cooked_mesh_golden.hpp is the normative copy; a disagreement between these
# numbers and that array is a real defect in one of them.
set(TRIANGLE_ARTIFACT_BYTES 272)
set(OFFSET_FORMAT_VERSION 8)
set(OFFSET_SOURCE_GUID 16)
set(OFFSET_BOUNDS_MAX_Y 80)
# 96 header + 8*1 attribute + 32*1 section = 136, then M_MATERIAL at +12.
set(OFFSET_FIRST_SUBMESH_MATERIAL 148)

# task 3.5.1: the skeleton subcommand and its own fixture. The .aeroskel header puts formatVersion at
# byte 8 and the source GUID at byte 16 -- the SAME two offsets .aeromesh uses, by that format's own
# design, so the two constants above are REUSED here rather than restated under new names; a
# disagreement between the two layouts would be a real defect in one of them.
#
# A skeleton's size is its record count and nothing else -- 64 + 128 * jointCount, with no padding
# site anywhere -- so 448 bytes IS "three records": the two palette joints skinned-quad.gltf declares
# plus the one non-joint root its ancestor closure adds.
set(SKINNED_QUAD "${SOURCE_DIR}/tests/cooker/fixtures/skinned-quad.gltf")
set(SKELOUT "${WORK_DIR}/out.aeroskel")
set(SKELETON_MAGIC_HEX "4145524f534b454c")   # "AEROSKEL"
set(SKINNED_QUAD_SKELETON_BYTES 448)

# task 3.5.2: the animation subcommand. Its input is the tree's ONLY multi-clip glTF -- three
# animations, one per interpolation mode, four joint nodes and no meshes -- and it is the SAME file
# aero_editor_shell_test's AS battery drives, so the animation arms commit no new fixture at all.
# ASSETS is scoped inside the manifest arm below, which is why this needs its own top-level constant
# beside SKINNED_QUAD rather than borrowing that one.
#
# The .aeroanim header puts formatVersion at byte 8 and the source GUID at byte 16 -- the SAME two
# offsets .aeromesh and .aeroskel use, by that format's own design, so OFFSET_FORMAT_VERSION and
# OFFSET_SOURCE_GUID above are REUSED here rather than restated under new names; a disagreement
# between the three layouts would be a real defect in one of them.
#
# Clip 0 is a 3-key STEP translation on ONE node, so its size is the format's whole arithmetic in one
# number: 80 header + 32 x 1 channel record = 112 times offset, + 12 B of times, + 4 B at the format's
# single padding site = 128 values offset, + 3 x 16 B of values = 176.
set(SKINNED_ANIM "${SOURCE_DIR}/tests/fixtures/assets/skinned.gltf")
set(ANIMOUT "${WORK_DIR}/out.aeroanim")
set(ANIMATION_MAGIC_HEX "4145524f414e494d")   # "AEROANIM"
set(SKINNED_ANIM_CLIP0_BYTES 176)

# --- the case table -------------------------------------------------------------------------------

if(CASE STREQUAL "help")
    aero_run_tool(ARGS --help OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    aero_expect_non_empty("${out}" "stdout")
    aero_expect_contains("${out}" "aero_cooker mesh --input" "the usage text")
    # task 3.5.1: the third subcommand's own usage line. A subcommand a user cannot discover from
    # --help is a subcommand they will not use, and the texture_help arm already makes the same
    # assertion for the second one.
    aero_expect_contains("${out}" "aero_cooker skeleton --input" "the usage text")
    # task 3.5.2: the fourth subcommand's own usage line, on exactly the same terms.
    aero_expect_contains("${out}" "aero_cooker animation --input" "the usage text")
    # stdout is reserved for --help/--version; diagnostics go to stderr only, so this one is silent.
    if(NOT err STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': --help wrote to stderr: ${err}")
    endif()

elseif(CASE STREQUAL "version")
    aero_run_tool(ARGS --version OUT_RESULT result OUT_STDOUT out)
    aero_expect_exit("${result}" 0)
    aero_expect_contains("${out}" "aero_cooker" "stdout")

elseif(CASE STREQUAL "no_subcommand")
    aero_run_tool(ARGS --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_non_empty("${err}" "stderr")
    # task 3.3.2 changed this literal from "(expected: mesh)", task 3.5.1 changed it again from
    # "(expected: mesh or texture)", and task 3.5.2 a third time from "(expected: mesh, texture or
    # skeleton)". Asserted, so the next subcommand added cannot leave the message naming a subset of
    # what the tool actually accepts -- which is exactly what this literal caught all three times.
    aero_expect_contains("${err}" "expected: mesh, texture, skeleton or animation" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "unknown_subcommand")
    # REWRITTEN AT TASK 3.3.2, and this is the whole reason it needed rewriting: it used to pass
    # `texture` as the unknown token, which was true at 3.3.1 and became FALSE the moment the second
    # subcommand landed. Worse, it would have kept passing -- `texture --input x --output y` with no
    # colour-space flag is still exit 1 with "texture" in the message -- so the case would have gone
    # on looking green while asserting something that no longer existed. A genuinely unknown token,
    # and the message that names every real one -- four of them since task 3.5.2.
    aero_run_tool(ARGS sound --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "unknown subcommand 'sound'" "stderr")
    aero_expect_contains("${err}" "expected: mesh, texture, skeleton or animation" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "unknown_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --frobnicate OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--frobnicate" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_input_flag")
    aero_run_tool(ARGS mesh --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--input" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_output_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--output" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_flag_value")
    # A value-taking flag at the very end of argv names itself rather than reading past the end.
    aero_run_tool(ARGS mesh --input OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "requires a value" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "repeated_flag")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --input "${ANY_INPUT}" --output "${OUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "at most once" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

    # AND THE FOUR TEXTURE-ONLY FLAGS (AC-32). The shared prefix -- --input, --output, --guid -- is
    # covered by the mesh arm above, because the flag loop reaches those three arms identically for
    # both subcommands. --srgb, --linear, --format and --no-mips are reached ONLY under `texture`, so
    # their refuseRepeat guards had no cover at all: deleting the --format one made
    # `texture --format bc1 --format rgba8` take the LAST value and exit 0 with nothing reddening.
    #
    # Each arm needs the exit code AND the message, because exit 1 alone is what a texture command
    # with no colour space also produces. And each pair is chosen so the seeded-guard behaviour is
    # exit 0, never another exit 1: with the guard gone, `--srgb --srgb` cooks, `--format bc1
    # --format rgba8` cooks as rgba8, and `--no-mips --no-mips` cooks with one level.
    foreach(pair "--srgb;--srgb" "--linear;--linear" "--no-mips;--no-mips")
        list(GET pair 0 first)
        list(GET pair 1 second)
        # --linear accompanies the two that are not themselves a colour space, so the command is
        # otherwise complete and the ONLY thing wrong with it is the repeat.
        set(colorspace "--linear")
        if(first STREQUAL "--srgb" OR first STREQUAL "--linear")
            set(colorspace "")
        endif()
        aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" ${colorspace}
            "${first}" "${second}" OUT_RESULT repeatResult OUT_STDERR repeatErr)
        aero_expect_exit("${repeatResult}" 1)
        aero_expect_contains("${repeatErr}" "at most once" "stderr")
        aero_expect_contains("${repeatErr}" "${first}" "stderr")
    endforeach()

    # --format takes a VALUE, so its repeat is spelled with two different values -- which is what makes
    # "the last one silently wins" the failure this arm catches, rather than a duplicate that happens
    # to be harmless.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --linear
        --format bc1 --format rgba8 OUT_RESULT formatResult OUT_STDERR formatErr)
    aero_expect_exit("${formatResult}" 1)
    aero_expect_contains("${formatErr}" "at most once" "stderr")
    aero_expect_contains("${formatErr}" "--format" "stderr")

    # NOTHING was written by any of the five commands above -- the repeat is diagnosed inside the flag
    # loop, before the input is even read.
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "bad_guid")
    # Too short, and the dashed 36-character form: parseGuid takes EXACTLY 32 hex digits and nothing
    # else, so a dashed value is a usage error rather than a silently normalized success.
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --guid 0123 OUT_RESULT shortResult)
    aero_expect_exit("${shortResult}" 1)
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}"
        --guid 01234567-89ab-cdef-fedc-ba9876543210 OUT_RESULT dashedResult)
    aero_expect_exit("${dashedResult}" 1)
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "bad_scale")
    # Not a number; a trailing character the parse does not consume; and a non-finite value, which a
    # locale-classic istringstream reads happily and which would NaN every cooked position.
    foreach(bad abc 1.0x nan)
        aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --scale "${bad}" OUT_RESULT result)
        aero_expect_exit("${result}" 1)
    endforeach()
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "missing_file")
    # The NAME is importable, so the tool gets as far as the read and reports an I/O error there --
    # which is exactly what distinguishes exit 3 from the exit 2 blend_refused takes on the name alone.
    aero_run_tool(ARGS mesh --input "${WORK_DIR}/does-not-exist.gltf" --output "${OUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "blend_refused")
    # A one-byte .blend, never parsed: isImportableModelName is false for it by design (task 3.2.4's
    # D15), so the refusal happens on the file NAME, before a byte is read and without a process.
    aero_run_tool(ARGS mesh --input "${FIXTURES}/cube.blend" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" "Import Details" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "gltf_happy")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_magic("${OUT}")
    # The SAME 272 bytes tests/cooked_mesh_golden.hpp's Golden B pins from the cook alone: the
    # end-to-end agreement between the process tier and the byte tier.
    aero_expect_size("${OUT}" "${TRIANGLE_ARTIFACT_BYTES}")
    aero_expect_hex_at("${OUT}" "${OFFSET_FORMAT_VERSION}" 4 "01000000")

elseif(CASE STREQUAL "gltf_external_bin")
    # The happy half: the .gltf beside its own .bin, which is the only input in the tree that drives
    # the Structure pass and the external-buffer read at all.
    aero_run_tool(ARGS mesh --input "${FIXTURES}/external.gltf" --output "${OUT}" OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_magic("${OUT}")
    aero_expect_size("${OUT}" "${TRIANGLE_ARTIFACT_BYTES}")

    # THE NEGATIVE TWIN, in the same case: the same document copied WITHOUT its sibling. The buffer
    # read warns and skips, and the Full pass then reports a missing buffer -- exit 2, no artifact.
    set(orphan "${WORK_DIR}/nobin")
    file(MAKE_DIRECTORY "${orphan}")
    file(COPY "${FIXTURES}/external.gltf" DESTINATION "${orphan}")
    aero_run_tool(ARGS mesh --input "${orphan}/external.gltf" --output "${orphan}/out.aeromesh"
        OUT_RESULT orphanResult OUT_STDERR orphanErr)
    aero_expect_exit("${orphanResult}" 2)
    aero_expect_contains("${orphanErr}" "external.bin" "stderr")
    aero_expect_no_files("${orphan}/out.aeromesh")

elseif(CASE STREQUAL "obj_missing_mtl")
    # A missing .mtl is a WARNING, never a failure (task 3.2.3's D7): the geometry was in the .obj all
    # along, so there is still a mesh and there is still an artifact.
    aero_run_tool(ARGS mesh --input "${FIXTURES}/no-mtl.obj" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    aero_expect_contains("${err}" "missing.mtl" "stderr")
    aero_expect_magic("${OUT}")

elseif(CASE STREQUAL "guid_written")
    # hi = 0x0123456789abcdef, lo = 0xfedcba9876543210, each stored little-endian, so the sixteen
    # bytes read back-to-front per half. Every one of them is non-zero, which is what makes this a
    # statement about byte ORDER and not merely about presence.
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${OUT}" "${OFFSET_SOURCE_GUID}" 16 "efcdab89674523011032547698badcfe")

elseif(CASE STREQUAL "scale_applied")
    # --scale is the importer's, never the cook's: the layout is untouched, so the artifact is still
    # 272 bytes and only the numbers move. boundsMax.y goes from 1.0f (00 00 80 3f) to 2.0f.
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${OUT}" --scale 2 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_size("${OUT}" "${TRIANGLE_ARTIFACT_BYTES}")
    aero_expect_hex_at("${OUT}" "${OFFSET_BOUNDS_MAX_Y}" 4 "00000040")

    set(plain "${WORK_DIR}/plain.aeromesh")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${plain}" OUT_RESULT plainResult)
    aero_expect_exit("${plainResult}" 0)
    aero_expect_hex_at("${plain}" "${OFFSET_BOUNDS_MAX_Y}" 4 "0000803f")

elseif(CASE STREQUAL "no_materials")
    # BOTH halves, because one alone proves nothing: with the flag the first submesh's materialIndex
    # is the absent-material sentinel, and without it the same submesh names material 0.
    aero_run_tool(ARGS mesh --input "${FIXTURES}/material.gltf" --output "${OUT}" --no-materials OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${OUT}" "${OFFSET_FIRST_SUBMESH_MATERIAL}" 4 "ffffffff")

    set(withMaterials "${WORK_DIR}/with.aeromesh")
    aero_run_tool(ARGS mesh --input "${FIXTURES}/material.gltf" --output "${withMaterials}" OUT_RESULT keptResult)
    aero_expect_exit("${keptResult}" 0)
    aero_expect_hex_at("${withMaterials}" "${OFFSET_FIRST_SUBMESH_MATERIAL}" 4 "00000000")

elseif(CASE STREQUAL "determinism")
    # Two processes, two directories, one byte sequence. No timestamp, no path, no hostname and no
    # build id reaches the container -- the only provenance fields are the cooker version and the
    # GUID the caller supplied.
    set(dir1 "${WORK_DIR}/run1")
    set(dir2 "${WORK_DIR}/run2")
    file(MAKE_DIRECTORY "${dir1}")
    file(MAKE_DIRECTORY "${dir2}")
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${dir1}/out.aeromesh"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${dir2}/out.aeromesh"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result2)
    aero_expect_exit("${result2}" 0)
    aero_expect_identical("${dir1}/out.aeromesh" "${dir2}/out.aeromesh")

elseif(CASE STREQUAL "nothing_written_on_failure")
    # AC-36: the output path is opened only after the cook returned a complete byte vector, so a
    # failing input leaves the working directory EMPTY -- of the artifact and of the .aero-tmp file
    # writeTextFileAtomic would have created on its way to it.
    aero_run_tool(ARGS mesh --input "${FIXTURES}/broken.gltf" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_non_empty("${err}" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")
    aero_expect_no_files("${OUT}" "${OUT}.aero-tmp")

elseif(CASE STREQUAL "output_dir_missing")
    # The tool creates NO directory: a build-time tool that invents them is how a typo becomes a
    # mystery tree. aero_shaderc does create its --output-dir, and the difference is deliberate --
    # that flag names a directory, this one names a file.
    aero_run_tool(ARGS mesh --input "${ANY_INPUT}" --output "${WORK_DIR}/nope/out.aeromesh"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${WORK_DIR}/nope")
    aero_verify_no_files_in("${WORK_DIR}")

# --- task 3.3.2: the texture subcommand -----------------------------------------------------------

elseif(CASE STREQUAL "texture_help")
    # One usage text serves both subcommands, and BOTH colour-space flags must appear in it: a
    # mandatory flag a user cannot discover from --help is a mandatory flag they will not give.
    aero_run_tool(ARGS texture --help OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    aero_expect_contains("${out}" "aero_cooker texture --input" "the usage text")
    aero_expect_contains("${out}" "--srgb" "the usage text")
    aero_expect_contains("${out}" "--linear" "the usage text")
    aero_expect_contains("${out}" "--no-mips" "the usage text")
    if(NOT err STREQUAL "")
        message(FATAL_ERROR "case '${CASE}': --help wrote to stderr: ${err}")
    endif()

elseif(CASE STREQUAL "texture_no_colorspace")
    # THERE IS NO DEFAULT, and the message has to say why rather than merely say "required": every
    # default is wrong for some large class of textures, and unlike most wrong defaults this one
    # produces an image that still looks like a texture, just too dark or too washed out.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--srgb" "stderr")
    aero_expect_contains("${err}" "--linear" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "texture_both_colorspace")
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --srgb --linear
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--srgb" "stderr")
    aero_expect_contains("${err}" "--linear" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "texture_srgb_bc5_conflict")
    # Vulkan enumerates 139 BC4_UNORM, 140 BC4_SNORM, 141 BC5_UNORM, 142 BC5_SNORM with no sRGB value
    # among them, so this combination is not unsupported by us -- it does not exist. Both halves,
    # because one alone would not show the refusal is about the FORMAT rather than about --srgb.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --srgb --format bc5
        OUT_RESULT bc5Result OUT_STDERR bc5Err)
    aero_expect_exit("${bc5Result}" 1)
    aero_expect_contains("${bc5Err}" "bc5" "stderr")
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --srgb --format bc4
        OUT_RESULT bc4Result OUT_STDERR bc4Err)
    aero_expect_exit("${bc4Result}" 1)
    aero_expect_contains("${bc4Err}" "bc4" "stderr")
    # And the SAME formats with --linear are perfectly legal, which is what makes the two refusals
    # above statements about the colour space rather than about the tokens.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --linear --format bc5
        OUT_RESULT okResult)
    aero_expect_exit("${okResult}" 0)
    aero_expect_files("${TEXOUT}")

elseif(CASE STREQUAL "texture_unknown_format")
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --linear --format bc7
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "bc7" "stderr")
    aero_expect_contains("${err}" "bc1, bc3, bc4, bc5, rgba8 or auto" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "texture_hdr_refused")
    # THE CASE THAT PROVES THE NAME DECIDES BEFORE THE READ. readFileBytes refuses an over-cap file
    # WITHOUT OPENING IT, so a tool that read first would answer this 65 MiB .hdr with "too large"
    # (exit 3) instead of "HDR is not supported" (exit 2). The file is generated here rather than
    # committed, and in 1 MiB appends rather than one huge file(WRITE), which is slow.
    set(bigHdr "${WORK_DIR}/sky.hdr")
    string(REPEAT "0123456789abcdef" 65536 chunk)   # exactly 1 MiB
    file(WRITE "${bigHdr}" "")
    foreach(i RANGE 1 65)                           # 65 MiB, just over the 64 MiB read cap
        file(APPEND "${bigHdr}" "${chunk}")
    endforeach()
    file(SIZE "${bigHdr}" hdrSize)
    if(hdrSize LESS_EQUAL 67108864)
        message(FATAL_ERROR "case '${CASE}': the .hdr is ${hdrSize} bytes, which is NOT over the 64 MiB read "
                            "cap -- this case would prove nothing")
    endif()
    aero_run_tool(ARGS texture --input "${bigHdr}" --output "${TEXOUT}" --linear OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)              # 2, NOT 3: the name decided, the read never happened
    aero_expect_contains("${err}" "high-dynamic-range" "stderr")
    aero_expect_no_files("${TEXOUT}")

elseif(CASE STREQUAL "texture_unclaimed_extension")
    # A .ktx2 as --input: it is not stb-decodable, and re-cooking a cooked artifact is not a workflow.
    # The file is real and non-empty, so the refusal is provably about the NAME and not about the read.
    set(cooked "${WORK_DIR}/already.ktx2")
    file(WRITE "${cooked}" "not really a ktx2, and it does not matter")
    aero_run_tool(ARGS texture --input "${cooked}" --output "${TEXOUT}" --linear OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" ".png .jpg .jpeg .tga .bmp .gif .psd" "stderr")
    aero_expect_no_files("${TEXOUT}")

elseif(CASE STREQUAL "texture_png_happy")
    # 5x3, opaque, odd in both axes, --linear and no --format: `auto` therefore answers 131
    # BC1_RGB_UNORM. The identifier is asserted as hex, which is also what proves the write was BINARY:
    # it ends in 0D 0A 1A 0A, so a text-mode write would corrupt this file at byte 8 on one lane alone.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --linear OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${TEXOUT}" 0 12 "${KTX2_IDENTIFIER_HEX}")
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_VK_FORMAT}" 4 "83000000")     # 131 BC1_RGB_UNORM
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_LEVEL_COUNT}" 4 "03000000")   # the full chain for 5x3
    # 80 + 24*3 + 44 (DFD) + 120 (KVD) = 316, aligned up to 320, then 8 + 8 + 16 bytes of level data.
    aero_expect_size("${TEXOUT}" 352)

elseif(CASE STREQUAL "texture_auto_picks_bc3")
    # The 8x8 fixture's bottom-right quadrant is at alpha 128, so `auto` must answer BC3 -- and with
    # --srgb that is 138 BC3_SRGB, the one format whose descriptor carries the corrected 1F alpha
    # qualifier. The negative twin is texture_png_happy above, whose opaque input answers BC1.
    aero_run_tool(ARGS texture --input "${TEXTURE_ALPHA}" --output "${TEXOUT}" --srgb OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_VK_FORMAT}" 4 "8a000000")     # 138 BC3_SRGB
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_LEVEL_COUNT}" 4 "04000000")   # floor(log2(8)) + 1

elseif(CASE STREQUAL "texture_no_mips")
    # BOTH halves, because one alone proves nothing: with the flag levelCount is 1 and the file is
    # strictly smaller; without it the same input produces the full four-level chain.
    aero_run_tool(ARGS texture --input "${TEXTURE_ALPHA}" --output "${TEXOUT}" --srgb --no-mips OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_LEVEL_COUNT}" 4 "01000000")

    set(mipped "${WORK_DIR}/mipped.ktx2")
    aero_run_tool(ARGS texture --input "${TEXTURE_ALPHA}" --output "${mipped}" --srgb OUT_RESULT mippedResult)
    aero_expect_exit("${mippedResult}" 0)
    aero_expect_hex_at("${mipped}" "${OFFSET_LEVEL_COUNT}" 4 "04000000")
    aero_expect_smaller_than("${TEXOUT}" "${mipped}")

elseif(CASE STREQUAL "texture_guid_written")
    # The AeroSourceGuid value is 32 LOWERCASE hex characters, high half first, written unconditionally
    # -- so the assertion is on the ASCII bytes of that text at an offset derived from the layout, not
    # merely on the value's presence somewhere in the file. Both halves of the GUID are non-zero, which
    # is what makes this a statement about ORDER as well as content.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${TEXOUT}" --linear
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${TEXOUT}" "${OFFSET_BC1_3LEVEL_GUID_VALUE}" 32
        "3031323334353637383961626364656666656463626139383736353433323130")

elseif(CASE STREQUAL "texture_determinism")
    # Two processes, two directories, one byte sequence. No timestamp, no path, no hostname and no
    # build id reaches the container -- and, unlike the mesh cook, no floating point either, which is
    # what makes this hold across the three CI lanes rather than merely across two runs on one.
    set(dir1 "${WORK_DIR}/run1")
    set(dir2 "${WORK_DIR}/run2")
    file(MAKE_DIRECTORY "${dir1}")
    file(MAKE_DIRECTORY "${dir2}")
    aero_run_tool(ARGS texture --input "${TEXTURE_ALPHA}" --output "${dir1}/out.ktx2" --srgb
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS texture --input "${TEXTURE_ALPHA}" --output "${dir2}/out.ktx2" --srgb
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result2)
    aero_expect_exit("${result2}" 0)
    aero_expect_identical("${dir1}/out.ktx2" "${dir2}/out.ktx2")

elseif(CASE STREQUAL "texture_nothing_written_on_failure")
    # The output path is opened only after the cook returned a complete byte vector, so a failing input
    # leaves NEITHER the artifact NOR the .aero-tmp file writeTextFileAtomic would have created on its
    # way to it. The input's NAME is claimed, so the refusal comes from the decode -- which is what
    # separates this case from texture_unclaimed_extension.
    # ASCII garbage under a CLAIMED name, rather than a truncated copy of the real fixture: CMake's
    # file(READ) stops at the first NUL byte and has no binary-truncating write, so a "first 20 bytes
    # of a real PNG" input is not expressible in this driver at all. The branch under test is the same
    # one either way -- stb_image refuses the header and the tool exits 2 with its reason -- and the
    # truncated-PNG shape is covered at the unit tier instead, by TK13.
    set(broken "${WORK_DIR}/broken.png")
    file(WRITE "${broken}" "this is not a png at all, and its extension is a claimed one")
    aero_run_tool(ARGS texture --input "${broken}" --output "${TEXOUT}" --linear OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${TEXOUT}" "${TEXOUT}.aero-tmp")

    # AND THE ORDERING THE BEHAVIOURAL HALF ABOVE CANNOT REACH. The refusal above is the DECODE's; the
    # COOK's own refusal cannot be provoked through this CLI at any affordable cost, because
    # decodeImageRgba8 bounds every axis at MAX_TEXTURE_DIMENSION long before cookTexture's byte cap
    # can trip, and the one input that would trip it (a ~16384x8192 image) costs a 537 MB decode on
    # every lane on every run. So moving the write ABOVE the cook-status check -- sabotage seed S39 --
    # reddened NOTHING: an empty artifact would have been written and the tool would still have exited
    # 0. This is the source-text half, the CM50/TX48 shape one tier up, and SOURCE_DIR is already here
    # so it needs no new plumbing. Comments are stripped first, so the prose above the check cannot
    # stand in for the check.
    aero_expect_source_order("${SOURCE_DIR}/tools/cooker/src/main.cpp"
        "ExitCode runTexture(const Args& args) {" "ExitCode runMain(int argc, char** argv) {"
        "cooked.status == engine::assets::TextureCookStatus::Refused"
        "writeTextFileAtomic(args.outputPath")

elseif(CASE STREQUAL "texture_output_dir_missing")
    # The tool creates NO directory, for either subcommand: a build-time tool that invents them is how
    # a typo becomes a mystery tree.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${WORK_DIR}/nope/out.ktx2" --linear
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${WORK_DIR}/nope")
    aero_verify_no_files_in("${WORK_DIR}")

# --- task 3.5.1: the skeleton subcommand ----------------------------------------------------------
#
# Ten arms, ungated like every case above, so `ctest -N` moves in ALL THREE build configurations
# again. Nine drive `skeleton`; the tenth (no_skins_gltf) drives `mesh` twice and is the
# artifact-level witness that --no-skins finally does something for the glTF path.
#
# They sit ABOVE the manifest block rather than below it because that block is this file's heavy tail
# and grows again at this task's own Step 11 -- keeping every manifest arm together is what makes the
# KIND_PREFIX orphan check readable at a glance.

elseif(CASE STREQUAL "skeleton_happy")
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${SKELOUT}" OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    # aero_expect_magic is AEROMESH-specific by construction, so a second container kind asserts its
    # own magic through the generic hex helper instead of widening that one.
    aero_expect_hex_at("${SKELOUT}" 0 8 "${SKELETON_MAGIC_HEX}")
    # 64 + 128 * 3. Two palette joints plus the non-joint root the ancestor closure had to add, so at
    # this tier the file's SIZE is the closure's own witness: without it the artifact would be 320.
    aero_expect_size("${SKELOUT}" "${SKINNED_QUAD_SKELETON_BYTES}")
    aero_expect_hex_at("${SKELOUT}" "${OFFSET_FORMAT_VERSION}" 4 "01000000")

elseif(CASE STREQUAL "skeleton_unknown_flag")
    # A MESH-ONLY flag, not an invented one. The flag arms are subcommand-scoped, so --no-materials is
    # genuinely unknown under `skeleton` and falls through to the arm that names it; an invented token
    # would prove only that the fallback exists, which the unknown_flag case already proves.
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${SKELOUT}" --no-materials
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "--no-materials" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "skeleton_bad_skin")
    # Not a number; a trailing character the parse does not consume; and a leading sign, which
    # std::from_chars's UNSIGNED overload refuses at the first character rather than wrapping -1 into
    # 4294967295. Each names the flag, so the message cannot degrade into a bare "usage error".
    foreach(bad abc 1x -1)
        aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${SKELOUT}" --skin "${bad}"
            OUT_RESULT result OUT_STDERR err)
        aero_expect_exit("${result}" 1)
        aero_expect_contains("${err}" "--skin" "stderr")
    endforeach()
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "skeleton_no_skin")
    # triangle.gltf declares no skins at all, and the refusal names what EXISTS -- "0 skin(s)" is the
    # honest answer rather than a special case. Exit 2, not 1: the model imported perfectly well and
    # what failed is the cook.
    aero_run_tool(ARGS skeleton --input "${ANY_INPUT}" --output "${SKELOUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" "has 0 skin" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "skeleton_skin_out_of_range")
    # The SAME refusal one skin higher: skinned-quad.gltf has exactly one, so --skin 1 is out of range
    # and the count comes from the model. The zero case above cannot show that on its own -- a message
    # printing a hard-coded 0 would satisfy it.
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${SKELOUT}" --skin 1
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" "has 1 skin" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "skeleton_guid_written")
    # hi = 0x0123456789abcdef, lo = 0xfedcba9876543210, each stored little-endian, so the sixteen
    # bytes read back-to-front per half. Every one of them is non-zero, which is what makes this a
    # statement about byte ORDER and not merely about presence -- the mesh path's guid_written
    # assertion, at the same offset, because this format puts its source GUID exactly where that one
    # does.
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${SKELOUT}"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${SKELOUT}" "${OFFSET_SOURCE_GUID}" 16 "efcdab89674523011032547698badcfe")

elseif(CASE STREQUAL "skeleton_determinism")
    # Two processes, two directories, one byte sequence. As with both older containers no timestamp,
    # no path, no hostname and no build id reaches the artifact -- and, as with the texture container,
    # no floating point is COMPUTED anywhere: every TRS and IBM cell is bit-copied from the importer's
    # own float through putF32.
    set(dir1 "${WORK_DIR}/run1")
    set(dir2 "${WORK_DIR}/run2")
    file(MAKE_DIRECTORY "${dir1}")
    file(MAKE_DIRECTORY "${dir2}")
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${dir1}/out.aeroskel"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${dir2}/out.aeroskel"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result2)
    aero_expect_exit("${result2}" 0)
    aero_expect_identical("${dir1}/out.aeroskel" "${dir2}/out.aeroskel")

elseif(CASE STREQUAL "skeleton_nothing_written_on_failure")
    # The output path is opened only after cookSkeleton returned a complete byte vector, so a model
    # with no skin leaves the working directory EMPTY -- of the artifact and of the .aero-tmp file
    # writeTextFileAtomic would have created on its way to it. The input imports perfectly and it is
    # the COOK that refuses, which is what separates this arm from skeleton_output_dir_missing.
    aero_run_tool(ARGS skeleton --input "${ANY_INPUT}" --output "${SKELOUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_non_empty("${err}" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")
    aero_expect_no_files("${SKELOUT}" "${SKELOUT}.aero-tmp")

elseif(CASE STREQUAL "skeleton_output_dir_missing")
    # The tool creates NO directory, for any subcommand: a build-time tool that invents them is how a
    # typo becomes a mystery tree.
    aero_run_tool(ARGS skeleton --input "${SKINNED_QUAD}" --output "${WORK_DIR}/nope/out.aeroskel"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${WORK_DIR}/nope")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "animation_happy")
    # skinned.gltf's clip 0 is StepAnim: a 3-key STEP translation on one node, plus a `weights`
    # channel the IMPORTER drops before the cook ever sees it. So the surviving channel count is 1 and
    # the size is the format's whole arithmetic in one number -- see SKINNED_ANIM_CLIP0_BYTES above.
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${ANIMOUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    # aero_expect_magic is AEROMESH-specific by construction, so the third container kind asserts its
    # own magic through the generic hex helper, exactly as the skeleton arm does.
    aero_expect_hex_at("${ANIMOUT}" 0 8 "${ANIMATION_MAGIC_HEX}")
    aero_expect_size("${ANIMOUT}" "${SKINNED_ANIM_CLIP0_BYTES}")
    aero_expect_hex_at("${ANIMOUT}" "${OFFSET_FORMAT_VERSION}" 4 "01000000")
    # The multi-clip advisory names the TOTAL, so cooking one clip of three can never look like
    # cooking the only clip there was.
    aero_expect_contains("${err}" "3 animations" "stderr")

elseif(CASE STREQUAL "animation_unknown_flag")
    # Flags that are real elsewhere, never invented ones: --scale and --skin belong to other
    # subcommands and --no-animations is a mesh import flag, so each is genuinely unknown under
    # `animation` and must fall through to the arm that NAMES it. An invented token would prove only
    # that the fallback exists, which the unknown_flag case already proves. --scale is the pointed one
    # -- it is refused rather than silently accepted-and-ignored, which is the whole of D13.
    foreach(badflag "--scale;2" "--skin;0" "--no-animations")
        list(GET badflag 0 flagname)
        aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${ANIMOUT}" ${badflag}
            OUT_RESULT result OUT_STDERR err)
        aero_expect_exit("${result}" 1)
        aero_expect_contains("${err}" "${flagname}" "stderr")
    endforeach()
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "animation_bad_clip")
    # --clip is --skin's twin and inherits its parse verbatim: not a number; a trailing character the
    # parse does not consume; and a leading sign, which std::from_chars's UNSIGNED overload refuses at
    # the first character rather than wrapping -1 into 4294967295.
    foreach(bad abc 1x -1)
        aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${ANIMOUT}" --clip "${bad}"
            OUT_RESULT result OUT_STDERR err)
        aero_expect_exit("${result}" 1)
        aero_expect_contains("${err}" "--clip" "stderr")
    endforeach()
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "animation_no_animation")
    # triangle.gltf declares no animations at all, and the refusal names what EXISTS -- "0 animation(s)"
    # is the honest answer rather than a special case. Exit 2, not 1: the model imported perfectly well
    # and what failed is the cook. A .aeroanim is never empty, so there is no artifact to write.
    aero_run_tool(ARGS animation --input "${ANY_INPUT}" --output "${ANIMOUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" "has 0 animation" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "animation_clip_out_of_range")
    # The SAME refusal three clips higher, and this arm carries more weight than its twin on the
    # skeleton side: it is the CLI witness that --clip is actually FORWARDED. A --clip parsed and then
    # never passed to the cook cooks clip 0 every time, which turns this case's exit 2 into exit 0.
    # The zero case above cannot show that -- a message printing a hard-coded 0 would satisfy it.
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${ANIMOUT}" --clip 3
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_contains("${err}" "has 3 animation" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "animation_guid_written")
    # hi = 0x0123456789abcdef, lo = 0xfedcba9876543210, each stored little-endian, so the sixteen
    # bytes read back-to-front per half. Every one of them is non-zero, which is what makes this a
    # statement about byte ORDER and not merely about presence. The offset is REUSED from the two
    # older containers rather than restated: this format puts its source GUID exactly where they do.
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${ANIMOUT}"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result)
    aero_expect_exit("${result}" 0)
    aero_expect_hex_at("${ANIMOUT}" "${OFFSET_SOURCE_GUID}" 16 "efcdab89674523011032547698badcfe")

elseif(CASE STREQUAL "animation_determinism")
    # Two processes, two directories, one byte sequence. As with all three older containers no
    # timestamp, no path, no hostname and no build id reaches the artifact -- and, as with the
    # skeleton container, no floating point is COMPUTED anywhere: every time and every value component
    # is bit-copied from the importer's own float through putF32.
    set(dir1 "${WORK_DIR}/run1")
    set(dir2 "${WORK_DIR}/run2")
    file(MAKE_DIRECTORY "${dir1}")
    file(MAKE_DIRECTORY "${dir2}")
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${dir1}/out.aeroanim"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result1)
    aero_expect_exit("${result1}" 0)
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${dir2}/out.aeroanim"
        --guid 0123456789abcdeffedcba9876543210 OUT_RESULT result2)
    aero_expect_exit("${result2}" 0)
    aero_expect_identical("${dir1}/out.aeroanim" "${dir2}/out.aeroanim")

elseif(CASE STREQUAL "animation_nothing_written_on_failure")
    # The output path is opened only after cookAnimation returned a complete byte vector, so a model
    # with no animation leaves the working directory EMPTY -- of the artifact and of the .aero-tmp file
    # writeTextFileAtomic would have created on its way to it. The input imports perfectly and it is
    # the COOK that refuses, which is what separates this arm from animation_output_dir_missing.
    aero_run_tool(ARGS animation --input "${ANY_INPUT}" --output "${ANIMOUT}"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 2)
    aero_expect_non_empty("${err}" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")
    aero_expect_no_files("${ANIMOUT}" "${ANIMOUT}.aero-tmp")

elseif(CASE STREQUAL "animation_output_dir_missing")
    # The tool creates NO directory, for any subcommand: a build-time tool that invents them is how a
    # typo becomes a mystery tree.
    aero_run_tool(ARGS animation --input "${SKINNED_ANIM}" --output "${WORK_DIR}/nope/out.aeroanim"
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${WORK_DIR}/nope")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "no_skins_gltf")
    # THE ARTIFACT-LEVEL WITNESS for the flag whose README row this task had to rewrite: until the
    # glTF importer's JOINTS_0/WEIGHTS_0 reads were gated on importSkins, --no-skins changed NOTHING
    # for a .gltf -- it suppressed the skin table, which the mesh container does not store anyway.
    # BOTH halves, because one alone proves nothing: with the flag the cooked mesh carries no Joints0
    # and no Weights0 section and is strictly smaller; without it the same input carries both.
    set(withSkins "${WORK_DIR}/with-skins.aeromesh")
    set(noSkins "${WORK_DIR}/no-skins.aeromesh")
    aero_run_tool(ARGS mesh --input "${SKINNED_QUAD}" --output "${withSkins}" OUT_RESULT withResult)
    aero_expect_exit("${withResult}" 0)
    aero_expect_magic("${withSkins}")
    aero_run_tool(ARGS mesh --input "${SKINNED_QUAD}" --output "${noSkins}" --no-skins
        OUT_RESULT withoutResult)
    aero_expect_exit("${withoutResult}" 0)
    aero_expect_magic("${noSkins}")
    aero_expect_smaller_than("${noSkins}" "${withSkins}")

# --- task 3.3.3: the frozen cook-determinism manifest ---------------------------------------------
#
# Three cases, one shape (task 3.5.1 added the third). Each cooks its tuples ONCE through the real
# binary and requires the artifact's SHA-256 to equal the line tests/cooker/determinism.sha256 records
# for that name. Because the cooker takes no gate flag, all three register in all three build
# configurations, and because CI runs ctest in Debug and Release on three lanes, the manifest is
# checked NINE times per push: all nine green means every lane and both configurations equal the
# manifest, therefore they equal each other.
#
# A THIRD ARM RATHER THAN A WIDER TUPLE TABLE, and that is structural: aero_manifest_tuple reads the
# arm-level SUBCOMMAND, and the KIND_PREFIX orphan check below ("every manifest line of THIS case's
# kind was actually cooked") stays sound only while every line's prefix is claimed by exactly one arm.
#
# The artifacts land in ${WORK_DIR}/artifacts/ and the CI job uploads exactly that directory. The
# perturbed re-cook lands in ${WORK_DIR}/perturbed/ so it cannot enter the upload set, which is
# fifteen files across the three cases and nothing else.

elseif(CASE STREQUAL "golden_manifest" OR CASE STREQUAL "texture_golden_manifest"
       OR CASE STREQUAL "skeleton_golden_manifest")
    set(ASSETS "${SOURCE_DIR}/tests/fixtures/assets")
    set(ARTIFACTS "${WORK_DIR}/artifacts")
    file(MAKE_DIRECTORY "${ARTIFACTS}")
    set(TEST_GUID 0123456789abcdeffedcba9876543210)

    aero_read_manifest(MANIFEST_NAMES MANIFEST_HASHES)
    set(TUPLES_COOKED 0)
    set(PERTURBED_RUNS 0)
    set(CONSUMED "")
    set(MISMATCHES "")
    set(FIRST_NAME "")
    set(FIRST_ARGS "")

    # One tuple: cook it, hash it, compare it against the manifest line of the same name. Defined
    # inside this arm, immediately above the two tuple blocks that use it, because it accumulates into
    # the arm's own scope -- a macro, not a function, for exactly that reason. (It cannot sit BETWEEN
    # the arms: CMake would attach a top-level macro() there to the PRECEDING arm's body, so it would
    # be defined only when texture_output_dir_missing ran. Measured with a throwaway script.)
    #
    # There is deliberately NO tuple TABLE. A CMake list cannot carry an argv list: every `;` inside a
    # quoted element is flattened by the enclosing set(), so a five-row table reports LENGTH 14 and an
    # eight-row one reports 39 -- and a row-count guard derived from that is a guard that cannot see a
    # deleted row. Each tuple is one call, and the anti-vacuity guard counts CALLS THAT RAN.
    #
    # Mismatches are COLLECTED rather than fatal on the first, so one run prints every replacement line.
    macro(aero_manifest_tuple tupleName)
        list(FIND MANIFEST_NAMES "${tupleName}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "case '${CASE}': the manifest has no line for '${tupleName}' -- a tuple the "
                                "frozen expectation does not know about is not covered by anything")
        endif()
        list(GET MANIFEST_HASHES ${_at} _expected)
        aero_run_tool(ARGS ${SUBCOMMAND} ${ARGN} --output "${ARTIFACTS}/${tupleName}"
            OUT_RESULT _result OUT_STDERR _err)
        aero_expect_exit("${_result}" 0)
        aero_expect_files("${ARTIFACTS}/${tupleName}")
        file(SHA256 "${ARTIFACTS}/${tupleName}" _actual)
        if(NOT _actual STREQUAL _expected)
            list(APPEND MISMATCHES "${_actual}  ${tupleName}")
        endif()
        list(APPEND CONSUMED "${tupleName}")
        math(EXPR TUPLES_COOKED "${TUPLES_COOKED} + 1")
        if(FIRST_NAME STREQUAL "")
            set(FIRST_NAME "${tupleName}")
            set(FIRST_ARGS ${ARGN})
        endif()
    endmacro()

    if(CASE STREQUAL "golden_manifest")
        set(SUBCOMMAND mesh)
        set(KIND_PREFIX "mesh-")
        set(TUPLE_COUNT 6)              # LITERAL, beside the six calls it counts
        # The minimal path, and the one tuple with a cross-tier tie: these 272 bytes ARE
        # tests/cooked_mesh_golden.hpp's COOKED_GOLDEN_TRIANGLE, so this line's hash is checkable
        # from the golden header alone, with no cooker involved.
        aero_manifest_tuple(mesh-triangle.aeromesh        --input "${ASSETS}/triangle.gltf")
        # A non-trivial bounds fold -- the header's six float fields carry real bit patterns.
        aero_manifest_tuple(mesh-asymmetric.aeromesh      --input "${ASSETS}/asymmetric.gltf")
        # The tree's only two-file glTF: the external-buffer read, end to end. Its BYTES equal
        # mesh-triangle's on purpose (same geometry, no material, nil GUID); what this tuple covers
        # is the PATH, not a distinct byte string.
        aero_manifest_tuple(mesh-external.aeromesh        --input "${FIXTURES}/external.gltf")
        # The importer's one float multiply, at a DYADIC factor so no decimal-parse last-ulp question
        # enters the contract; plus a real GUID and a submesh that names material 0.
        aero_manifest_tuple(mesh-material-scaled.aeromesh --input "${FIXTURES}/material.gltf"
                                                          --scale 1.5 --guid "${TEST_GUID}")
        # The only input whose bytes depend on the sort running: two meshes, three primitives, and a
        # first-declared primitive whose richer mask sends it LAST. sectionCount 2, submeshCount 3.
        aero_manifest_tuple(mesh-multi.aeromesh           --input "${FIXTURES}/multi.gltf")
        # task 3.5.1 -- the FIRST manifest artifact carrying Joints0/Weights0 sections. Every mesh
        # tuple above is unskinned, so until this line the joint/weight emit path had no cross-lane
        # witness at all. No --guid, the mesh-triangle posture: what this pins is the layout.
        aero_manifest_tuple(mesh-skinned.aeromesh         --input "${FIXTURES}/skinned-quad.gltf")
    elseif(CASE STREQUAL "texture_golden_manifest")
        set(SUBCOMMAND texture)
        set(KIND_PREFIX "texture-")
        set(TUPLE_COUNT 8)              # LITERAL, beside the eight calls it counts
        # `auto` on the half-transparent 8x8 -> 138 BC3_SRGB: the one format carrying the corrected
        # 1F alpha qualifier. TWO of the eight tuples carry no --format and therefore resolve the
        # format from pixels -- this one and -nomips below. MEASURED, not read off the table: seeding
        # chooseTextureFormat's alpha threshold, and separately the BC3_SRGB descriptor's byte 31,
        # reddens exactly those two lines and no other.
        aero_manifest_tuple(texture-rgba8x8-srgb-auto.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --srgb)
        # BC1 plus the sRGB gamma tables on a power-of-two chain.
        aero_manifest_tuple(texture-rgba8x8-srgb-bc1.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --srgb --format bc1)
        # The BC4 encoder -- the format with neither a byte golden nor a golden-pinned sRGB sibling.
        aero_manifest_tuple(texture-rgba8x8-linear-bc4.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --linear --format bc4)
        # BC5's red-then-green composition.
        aero_manifest_tuple(texture-rgba8x8-linear-bc5.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --linear --format bc5)
        # Uncompressed passthrough plus a real GUID in the key/value data.
        aero_manifest_tuple(texture-rgba8x8-linear-rgba8-guid.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --linear --format rgba8 --guid "${TEST_GUID}")
        # levelCount 1 -- the single-level layout. It carries no --format either, so it is the SECOND
        # tuple resolving through `auto`: it reddens together with -srgb-auto above, never alone.
        aero_manifest_tuple(texture-rgba8x8-srgb-nomips.ktx2
            --input "${ASSETS}/texture-rgba-8x8.png" --srgb --no-mips)
        # Odd in BOTH axes: the polyphase filter runs on both, on the uncompressed path, in sRGB.
        aero_manifest_tuple(texture-rgb5x3-srgb-rgba8.ktx2
            --input "${ASSETS}/texture-rgb-5x3.png" --srgb --format rgba8)
        # Partial edge blocks -- the clamp-never-zero-fill path, NPOT, linear.
        aero_manifest_tuple(texture-rgb5x3-linear-bc1.ktx2
            --input "${ASSETS}/texture-rgb-5x3.png" --linear --format bc1)
    else()
        set(SUBCOMMAND skeleton)
        set(KIND_PREFIX "skeleton-")
        set(TUPLE_COUNT 1)              # LITERAL, beside the one call it counts
        # task 3.5.1 -- the .aeroskel format anchored cross-lane, cross-configuration and cross-time
        # from the day it shipped, rather than after the first divergence. One tuple is enough because
        # the input is the one that exercises the whole cook: two SIBLING palette joints under a
        # non-joint root, so the ancestor closure, Kahn's tie and the source-order independence all
        # run. It carries a REAL --guid, unlike the mesh posture, so the header's hi/lo emit order is
        # pinned across lanes too -- swapping those two u64 writes is otherwise invisible to any
        # single-lane check, since our own parser reads them back in the order our writer wrote them.
        aero_manifest_tuple(skeleton-skinned.aeroskel
            --input "${FIXTURES}/skinned-quad.gltf" --guid "${TEST_GUID}")
    endif()

    # --- the mismatch report ------------------------------------------------------------------------
    # message(NOTICE) prints its argument BYTE FOR BYTE with no prefix and no wrapping. FATAL_ERROR and
    # WARNING hard-wrap at ~76 columns and indent by two, which would break an 87-character
    # `<sha256>  <name>` line across two lines and make "ready to paste" false. So the lines go out as
    # NOTICE, one call each, and the explanation goes out as the failure.
    list(LENGTH MISMATCHES _badCount)
    if(_badCount GREATER 0)
        message(NOTICE "")
        message(NOTICE "case '${CASE}': ${_badCount} artifact(s) no longer match "
                       "tests/cooker/determinism.sha256. If -- AND ONLY IF -- the change is deliberate, "
                       "replace those lines with exactly these, in the same commit as the cook change "
                       "and the matching COOKED_*_COOKER_VERSION bump:")
        foreach(bad IN LISTS MISMATCHES)
            message(NOTICE "${bad}")
        endforeach()
        message(NOTICE "")
        message(FATAL_ERROR "case '${CASE}': ${_badCount} of ${TUPLES_COOKED} artifacts cooked to a "
                            "different byte sequence than the frozen manifest records. The replacement "
                            "lines are printed above, verbatim. NEVER edit a hash to green a red run -- "
                            "see the manifest's own header for the regeneration ritual.")
    endif()

    # --- anti-vacuity, in the order that makes each seed name itself -------------------------------
    # 1. every manifest line of THIS case's kind was actually cooked. An orphan line is a lie about
    #    coverage, and this check names the orphan, which a bare count cannot.
    foreach(entry IN LISTS MANIFEST_NAMES)
        if(entry MATCHES "^${KIND_PREFIX}")
            list(FIND CONSUMED "${entry}" _consumedAt)
            if(_consumedAt EQUAL -1)
                message(FATAL_ERROR "case '${CASE}': the manifest lists '${entry}' but no tuple in this "
                                    "case cooks it")
            endif()
        endif()
    endforeach()
    # 2. the LITERAL tuple count, set beside the calls it counts.
    if(NOT TUPLES_COOKED EQUAL TUPLE_COUNT)
        message(FATAL_ERROR "case '${CASE}': ${TUPLES_COOKED} tuples cooked, expected ${TUPLE_COUNT}")
    endif()
    # --- the environment-perturbed re-cook ---------------------------------------------------------
    # THIS ARM CANNOT FAIL TODAY, and saying so is the point. aero_cooker never calls setlocale (its
    # one float parse imbues std::locale::classic()), never reads the clock, and is handed absolute
    # paths, so TZ, LC_ALL and the working directory are all structurally inert. The arm exists for the
    # regression class: a transitively-initialised library calling setlocale(LC_ALL, "") -- common in
    # GUI init paths, and aero_editor_core already puts four parsers and ImGui on this binary's link
    # line -- or a CWD-relative probe growing into the tool.
    #
    # Its STRENGTH is lane-dependent even though its OUTCOME is defined everywhere: macOS ships
    # tr_TR.UTF-8, a Linux runner generally generates only C/C++.UTF-8/en_US.UTF-8 (an unavailable
    # locale makes setlocale return NULL and change nothing), and the Windows CRT does not read LC_ALL
    # from the environment at all. Do not read this arm as uniform coverage.
    set(PERTURBED_DIR "${WORK_DIR}/perturbed")
    set(ELSEWHERE "${WORK_DIR}/elsewhere")
    file(MAKE_DIRECTORY "${PERTURBED_DIR}")
    file(MAKE_DIRECTORY "${ELSEWHERE}")
    list(FIND MANIFEST_NAMES "${FIRST_NAME}" _firstAt)
    list(GET MANIFEST_HASHES ${_firstAt} _firstExpected)
    aero_run_tool(ARGS ${SUBCOMMAND} ${FIRST_ARGS} --output "${PERTURBED_DIR}/${FIRST_NAME}"
        ENV TZ=Pacific/Kiritimati LC_ALL=tr_TR.UTF-8
        WORKING_DIRECTORY "${ELSEWHERE}"
        OUT_RESULT _pResult OUT_STDERR _pErr)
    aero_expect_exit("${_pResult}" 0)
    file(SHA256 "${PERTURBED_DIR}/${FIRST_NAME}" _pActual)
    if(NOT _pActual STREQUAL "${_firstExpected}")
        message(FATAL_ERROR "case '${CASE}': re-cooking '${FIRST_NAME}' under TZ=Pacific/Kiritimati, "
                            "LC_ALL=tr_TR.UTF-8 and a different working directory produced ${_pActual}, "
                            "not the manifest's ${_firstExpected}. The tool has grown an environment "
                            "dependency.")
    endif()
    math(EXPR PERTURBED_RUNS "${PERTURBED_RUNS} + 1")
    # 3. the arm's OWN literal count, separate from the tuple count on purpose: one shared counter
    #    makes "a tuple row was deleted" and "the arm was skipped" produce the identical message.
    if(NOT PERTURBED_RUNS EQUAL 1)
        message(FATAL_ERROR "case '${CASE}': ${PERTURBED_RUNS} perturbed runs, expected exactly 1")
    endif()

    # 4. the upload set, enforceable LOCALLY rather than only in ci.yml. Misdirecting a tuple's
    #    --output leaves every hash correct and every count above satisfied, so without this the only
    #    enforcer of "this directory holds exactly the manifest's own files" would live in YAML and
    #    could not be run on a developer's machine at all.
    #
    #    IT RUNS LAST, AND THAT PLACEMENT IS THE WHOLE POINT. Anything this arm writes into
    #    ${ARTIFACTS} after the check is a file no local run can see and only the job's own
    #    `find | wc -l -ne 13` would catch -- which is precisely the single-enforcer-in-YAML shape
    #    this check exists to eliminate. The perturbed re-cook above deliberately writes to
    #    ${WORK_DIR}/perturbed instead, so today the count is clean; keep any future addition to this
    #    arm above this block only if it writes nothing here, and re-run it last regardless.
    file(GLOB _produced "${ARTIFACTS}/*")
    list(LENGTH _produced _producedCount)
    if(NOT _producedCount EQUAL TUPLE_COUNT)
        message(FATAL_ERROR "case '${CASE}': ${ARTIFACTS} holds ${_producedCount} files, expected "
                            "${TUPLE_COUNT} -- this directory IS the CI upload set")
    endif()

else()
    message(FATAL_ERROR "run_case.cmake: unknown CASE '${CASE}'")
endif()

message(STATUS "cooker ctest case '${CASE}': OK")
