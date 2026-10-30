#pragma once
// Aero Engine -- the cooked script container v1 (.aeroscript, task 4.3.2). One ES module's compiled
// bytecode behind an 80-byte header. The tree's fifth first-party binary format and the first whose
// payload belongs to a third party: this library never looks inside it, it only frames it, hashes it
// and hands it back as a span over the caller's own buffer.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro.
// Nothing here names the script backend (the script guard scans engine/assets).
//
// The normative specification of this format is docs/09-file-formats.md section 16. THIS HEADER IS NOT
// THE SPEC -- if the two ever disagree, docs/09 wins and one of them is a bug. The constants below are
// the ONLY sizes: `sizeof` is never taken of an on-disk record.
//
// There is no payload cap on purpose: the VM refuses a file larger than its heap limit before reading
// it, and the loader's headroom guard bounds the read; a format cap would bound nothing those do not.
//
// The container detects accidents (a truncated copy, a flipped byte), not attacks: the payload is
// trusted input, written by the project's own cooker.
#include <aero/assets/cooked_mesh.hpp>  // the eight byte primitives + their endianness static_assert
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ---------------------------------------------------------------------------------
// Eight bytes, the FIRST a NUL: a text-mode reader or a source loader that sees this file stops at once.
inline constexpr std::string_view COOKED_SCRIPT_MAGIC{"\0AEROSCR", 8};
inline constexpr std::uint32_t COOKED_SCRIPT_FORMAT_VERSION = 1;
// "The same input now cooks to different bytes", and nothing else. It NEVER gates a parse
// (docs/09 section 9.11).
inline constexpr std::uint32_t COOKED_SCRIPT_COOKER_VERSION = 1;

// ---- sizes. The ONLY sizes. ---------------------------------------------------------------------
inline constexpr std::size_t COOKED_SCRIPT_HEADER_BYTES = 80;

// ---- strip flags ----------------------------------------------------------------------------------
inline constexpr std::uint32_t COOKED_SCRIPT_STRIP_SOURCE = 1U;  // bit 0: function source text stripped
inline constexpr std::uint32_t COOKED_SCRIPT_KNOWN_STRIP_FLAGS = COOKED_SCRIPT_STRIP_SOURCE;

// ---- the header's field offsets, named ONCE for the whole format --------------------------------
// docs/09 section 16.1 is the normative table; these mirror it.
namespace detail {
inline constexpr std::size_t H_MAGIC = 0;
inline constexpr std::size_t H_FORMAT_VERSION = 8;
inline constexpr std::size_t H_COOKER_VERSION = 12;
inline constexpr std::size_t H_GUID_HI = 16;
inline constexpr std::size_t H_GUID_LO = 24;
inline constexpr std::size_t H_BACKEND_RELEASE = 32;
inline constexpr std::size_t H_BYTECODE_VERSION = 36;
inline constexpr std::size_t H_STRIP_FLAGS = 40;
inline constexpr std::size_t H_RESERVED_FLAGS = 44;
inline constexpr std::size_t H_PAYLOAD_BYTES = 48;
inline constexpr std::size_t H_PAYLOAD_HASH_HI = 56;
inline constexpr std::size_t H_PAYLOAD_HASH_LO = 64;
inline constexpr std::size_t H_RESERVED = 72;
static_assert(H_RESERVED + 8 == COOKED_SCRIPT_HEADER_BYTES);
}  // namespace detail

// LIFETIME: `payload` ALIASES the buffer handed to parseCookedScript. It is never a copy; a CookedScript
// outliving its buffer is a dangling read.
struct CookedScript {
    std::uint32_t formatVersion = 0;
    std::uint32_t cookerVersion = 0;
    Guid sourceGuid;
    std::uint32_t backendRelease = 0;  // stored, not judged here: the loader compares
    std::uint32_t bytecodeVersion = 0;
    std::uint32_t stripFlags = 0;
    std::uint64_t payloadBytes = 0;
    ContentHash payloadHash;
    std::span<const std::byte> payload;
};

enum class CookedScriptStatus : std::uint8_t {
    Ok = 0,
    TooSmall,            // shorter than the header
    BadMagic,            //
    UnsupportedVersion,  // formatVersion is not 1
    ReservedNotZero,     // reservedFlags or the trailing reserved word
    UnknownFlags,        // a strip bit this build does not know
    EmptyPayload,        // payloadBytes is zero
    SizeMismatch,        // payloadBytes is not exactly what follows the header
    HashMismatch,        // the payload does not hash to the header's value
};
// A switch with NO `default:`, so a new enumerator is a -Wswitch failure. NOT named toString: an engine
// toString(SomeEnum) is found by ADL inside doctest's stringifier and breaks every lane.
[[nodiscard]] std::string_view cookedScriptStatusLabel(CookedScriptStatus status) noexcept;

// True when the buffer's first eight bytes are COOKED_SCRIPT_MAGIC. It decides nothing else: a loader
// uses it to tell a container from JavaScript text, and parseCookedScript then judges the rest.
[[nodiscard]] bool hasCookedScriptMagic(std::span<const std::byte> bytes) noexcept;

struct CookedScriptParseResult {
    CookedScriptStatus status = CookedScriptStatus::Ok;
    std::string message;  // "" IFF Ok
    CookedScript script;  // meaningful only when Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NEVER COPIES. Checks, in this order and stopping at the
// first failure: size, magic, formatVersion, reserved fields, strip flags, an empty payload, the payload
// size (by subtraction, never an addition that can wrap), and last the payload hash, so a damaged or
// foreign header never costs a pass over its payload.
[[nodiscard]] CookedScriptParseResult parseCookedScript(std::span<const std::byte> bytes);

struct CookedScriptInput {
    Guid sourceGuid;  // nil is legal and deterministic
    std::uint32_t backendRelease = 0;
    std::uint32_t bytecodeVersion = 0;
    std::uint32_t stripFlags = 0;
    std::span<const std::byte> payload;  // >= 1 byte
};

struct CookedScriptWriteResult {
    CookedScriptStatus status = CookedScriptStatus::Ok;
    std::string message;
    std::vector<std::byte> bytes;  // empty IFF refused
};

// Refuses an empty payload (EmptyPayload) and unknown strip bits (UnknownFlags), before sizing its
// buffer; otherwise the output always parses.
[[nodiscard]] CookedScriptWriteResult writeCookedScript(const CookedScriptInput& input);

}  // namespace engine::assets
