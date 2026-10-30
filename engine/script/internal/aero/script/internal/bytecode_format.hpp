#pragma once
// Aero Engine -- the bytecode format's backend-free half (task 4.3.2; internal to engine/script): the pinned format
// constants, the checksum mirror, the format-head reader and the load-time module rename, and the read headroom
// rule. It mirrors the backend's byte format with plain arithmetic and names no backend symbol, so every rule here is
// tested byte for byte at tier 0 (tests/script_bytecode_format_test.cpp, BY1-BY7) through aero::script_internal; the
// calls into the backend stay in src/vm_modules.cpp and src/bytecode_cook.cpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::script::internal {

// The bytecode version byte the pinned backend writes first (quickjs.c:37656, BC_VERSION). The engine pins it itself:
// the backend's own constant is private, and a port bump that changes it must fail loudly rather than load.
inline constexpr std::uint8_t SCRIPT_BYTECODE_VERSION = 26;
// The checksum word that tells the backend's reader to skip its own check (quickjs.c:39666-39667). The cook refuses
// a payload carrying it, and so does the rename.
inline constexpr std::uint32_t BYTECODE_CHECKSUM_BYPASS = 0xFFFFFFFFU;
// The module names the cook compiles under. Each holds a '?', which no specifier can resolve to and no module can be
// given as a name, so a script can neither import nor collide with either. Neither is a prefix of the other, and the
// lengths differ, so the rename proof (cook under one, rename to the other) moves the length word.
inline constexpr std::string_view COOK_MODULE_NAME = "res://?cooked";     // 13 bytes
inline constexpr std::string_view COOK_PROBE_NAME = "res://?cook-probe";  // 17 bytes
// The room a bytecode read is given before it starts: a fixed part for a small module's own cost and the run that
// follows, plus a multiple of the payload (a measured peak of 2.3-2.6x an unstripped payload, 3.6-4.2x a stripped one).
inline constexpr std::size_t BYTECODE_READ_FIXED_BYTES = std::size_t{64} * 1024U;
inline constexpr std::size_t BYTECODE_READ_BYTES_PER_PAYLOAD_BYTE = 6;

// The backend's checksum over a payload's bytes after its five-byte head (bc_csum, quickjs.c:37806-37829), byte for
// byte -- the GAP INCLUDED: its tail switch has no arm for a remainder of 4, so when the span's length is a multiple of
// four the last word never enters the sum. Never "fix" that: the backend verifies with its own copy.
[[nodiscard]] std::uint32_t bytecodeChecksum(std::span<const std::byte> bytes) noexcept;

enum class BytecodeHeadStatus : std::uint8_t {
    Ok = 0,
    Truncated,           // the payload ends inside the head, or a LEB128 runs past five bytes
    WrongVersion,        // the first byte is not SCRIPT_BYTECODE_VERSION
    ChecksumBypass,      // the checksum word is BYTECODE_CHECKSUM_BYPASS
    ChecksumMismatch,    // the checksum word is not the sum of the bytes after it
    NoAtoms,             // the atom table announces no atom
    NameNotAString,      // the first atom is a predefined one, or a wide string
    NameNotPlaceholder,  // the first atom is not the name the caller expects
    InvalidNewName,      // the new name is empty, over the format's length, or not printable ASCII
};
// The reason a refusal quotes ("" for Ok): a fragment that follows "its ..." in a load error.
[[nodiscard]] std::string_view bytecodeHeadStatusText(BytecodeHeadStatus status) noexcept;

struct RenamedBytecode {
    BytecodeHeadStatus status = BytecodeHeadStatus::Ok;
    std::vector<std::byte> bytes;  // the renamed payload iff Ok
};
// Walks the head as the backend's reader does (JS_ReadObjectAtoms, quickjs.c:39649-39709), checks the first atom is
// the narrow string `expectedName`, and returns the payload with that atom replaced by `newName` and the checksum
// recomputed. `newName` must be printable ASCII: such a name is stored narrow, so the renamed atom is the very atom
// the backend makes from the name. `payload` is never modified.
[[nodiscard]] RenamedBytecode renameBytecodeModule(std::span<const std::byte> payload, std::string_view expectedName,
                                                   std::string_view newName);

// BYTECODE_READ_FIXED_BYTES + BYTECODE_READ_BYTES_PER_PAYLOAD_BYTE * payloadBytes, saturating at SIZE_MAX.
[[nodiscard]] std::size_t bytecodeReadHeadroom(std::size_t payloadBytes) noexcept;

}  // namespace engine::script::internal
