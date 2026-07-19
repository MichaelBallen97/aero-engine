# tests/shaderc/verify_artifacts.cmake — byte-level assertions for the shaderc ctest harness (task
# 0.4.3, spec D9/§3.6). Included by run_case.cmake. Every function FATAL_ERRORs on mismatch, which is
# exactly the ctest failure (see run_case.cmake's header comment for why a raw add_test cannot express
# this on its own).

function(aero_verify_spirv_magic path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "verify: '${path}' does not exist")
    endif()
    file(READ "${path}" magic LIMIT 4 HEX)
    # SPIR-V magic 0x07230203, little-endian on disk -> bytes 03 02 23 07 (spec AC-4).
    if(NOT magic STREQUAL "03022307")
        message(FATAL_ERROR "verify: '${path}' does not start with the SPIR-V magic (got 0x${magic})")
    endif()
endfunction()

function(aero_verify_dxbc_fourcc path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "verify: '${path}' does not exist")
    endif()
    # HEX, not plain text (matches the SPIR-V check above): file(READ ... LIMIT n) without HEX was
    # empirically found to over-read on binary files past a few hundred bytes -- CMake 4.4.0 returns
    # a 5-character string ("DXBC\n"-shaped) for LIMIT 4 on this file's real (3+ KB) size, though the
    # same call is exact on small/truncated copies. HEX mode reads a fixed byte count unambiguously.
    file(READ "${path}" magic LIMIT 4 HEX)
    # "DXBC" -> 0x44 0x58 0x42 0x43.
    if(NOT magic STREQUAL "44584243")
        message(FATAL_ERROR "verify: '${path}' does not start with the DXBC fourcc (got 0x${magic})")
    endif()
endfunction()

function(aero_verify_msl_entry path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "verify: '${path}' does not exist")
    endif()
    file(READ "${path}" content)
    string(FIND "${content}" "main0" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "verify: '${path}' does not contain the MSL entry symbol 'main0' (spec E11)")
    endif()
endfunction()

function(aero_verify_contains path needle)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "verify: '${path}' does not exist")
    endif()
    file(READ "${path}" content)
    string(FIND "${content}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "verify: '${path}' does not contain expected text: ${needle}")
    endif()
endfunction()

# Verifies the D5 JSON key ORDER (schema, toolchain, name, stage, entryPoint, mslEntryPoint, the four
# counts, formats) via successive string(FIND ...) calls that must never move backward.
function(aero_verify_json_key_order path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "verify: '${path}' does not exist")
    endif()
    file(READ "${path}" content)
    set(keys "\"schema\"" "\"toolchain\"" "\"name\"" "\"stage\"" "\"entryPoint\"" "\"mslEntryPoint\""
        "\"samplerCount\"" "\"storageTextureCount\"" "\"storageBufferCount\"" "\"uniformBufferCount\"" "\"formats\"")
    set(cursor 0)
    foreach(key IN LISTS keys)
        string(FIND "${content}" "${key}" pos)
        if(pos EQUAL -1 OR pos LESS cursor)
            message(FATAL_ERROR "verify: '${path}' -- key ${key} missing or out of the D5 order")
        endif()
        set(cursor "${pos}")
    endforeach()
endfunction()

function(aero_verify_json_counts path samplerCount storageTextureCount storageBufferCount uniformBufferCount)
    aero_verify_json_key_order("${path}")
    aero_verify_contains("${path}" "\"samplerCount\": ${samplerCount}")
    aero_verify_contains("${path}" "\"storageTextureCount\": ${storageTextureCount}")
    aero_verify_contains("${path}" "\"storageBufferCount\": ${storageBufferCount}")
    aero_verify_contains("${path}" "\"uniformBufferCount\": ${uniformBufferCount}")
endfunction()

# The full "happy path" artifact set for a shader compiled with all three formats: 4 files, correct
# magics, MSL contains main0, JSON schema keys in D5 order with the given counts.
function(aero_verify_happy_set base stage samplerCount storageTextureCount storageBufferCount uniformBufferCount)
    aero_verify_spirv_magic("${base}.spv")
    aero_verify_dxbc_fourcc("${base}.dxil")
    aero_verify_msl_entry("${base}.msl")
    aero_verify_json_counts("${base}.json" "${samplerCount}" "${storageTextureCount}" "${storageBufferCount}"
        "${uniformBufferCount}")
    aero_verify_contains("${base}.json" "\"stage\": \"${stage}\"")
    aero_verify_contains("${base}.json" "\"formats\": [\"spirv\", \"msl\", \"dxil\"]")
endfunction()

function(aero_verify_files_identical path_a path_b)
    if(NOT EXISTS "${path_a}")
        message(FATAL_ERROR "verify: '${path_a}' does not exist")
    endif()
    if(NOT EXISTS "${path_b}")
        message(FATAL_ERROR "verify: '${path_b}' does not exist")
    endif()
    file(MD5 "${path_a}" hash_a)
    file(MD5 "${path_b}" hash_b)
    if(NOT hash_a STREQUAL hash_b)
        message(FATAL_ERROR "verify: '${path_a}' and '${path_b}' are not byte-identical (determinism, spec D14/AC-8)")
    endif()
endfunction()

function(aero_verify_no_files_in dir)
    file(GLOB leftover "${dir}/*")
    if(leftover)
        message(FATAL_ERROR "verify: expected NO output files under '${dir}', found: ${leftover}")
    endif()
endfunction()
