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

function(aero_verify_no_files_in dir)
    file(GLOB leftover "${dir}/*")
    if(leftover)
        message(FATAL_ERROR "case '${CASE}': expected NO files under '${dir}', found: ${leftover}")
    endif()
endfunction()

# Two fixture roots: the ones the editor suites already share, and the six this tool owns. The driver
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

# --- the case table -------------------------------------------------------------------------------

if(CASE STREQUAL "help")
    aero_run_tool(ARGS --help OUT_RESULT result OUT_STDOUT out OUT_STDERR err)
    aero_expect_exit("${result}" 0)
    aero_expect_non_empty("${out}" "stdout")
    aero_expect_contains("${out}" "aero_cooker mesh --input" "the usage text")
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
    # task 3.3.2 changed this literal from "(expected: mesh)". Asserted, so the next subcommand added
    # cannot leave the message naming a subset of what the tool actually accepts.
    aero_expect_contains("${err}" "expected: mesh or texture" "stderr")
    aero_verify_no_files_in("${WORK_DIR}")

elseif(CASE STREQUAL "unknown_subcommand")
    # REWRITTEN AT TASK 3.3.2, and this is the whole reason it needed rewriting: it used to pass
    # `texture` as the unknown token, which was true at 3.3.1 and became FALSE the moment the second
    # subcommand landed. Worse, it would have kept passing -- `texture --input x --output y` with no
    # colour-space flag is still exit 1 with "texture" in the message -- so the case would have gone
    # on looking green while asserting something that no longer existed. A genuinely unknown token,
    # and the message that names both real ones.
    aero_run_tool(ARGS sound --input "${ANY_INPUT}" --output "${OUT}" OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 1)
    aero_expect_contains("${err}" "unknown subcommand 'sound'" "stderr")
    aero_expect_contains("${err}" "expected: mesh or texture" "stderr")
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

elseif(CASE STREQUAL "texture_output_dir_missing")
    # The tool creates NO directory, for either subcommand: a build-time tool that invents them is how
    # a typo becomes a mystery tree.
    aero_run_tool(ARGS texture --input "${TEXTURE_OPAQUE}" --output "${WORK_DIR}/nope/out.ktx2" --linear
        OUT_RESULT result OUT_STDERR err)
    aero_expect_exit("${result}" 3)
    aero_expect_non_empty("${err}" "stderr")
    aero_expect_no_files("${WORK_DIR}/nope")
    aero_verify_no_files_in("${WORK_DIR}")

else()
    message(FATAL_ERROR "run_case.cmake: unknown CASE '${CASE}'")
endif()

message(STATUS "cooker ctest case '${CASE}': OK")
