// Aero Engine -- the cooked script container v1: the status label, the magic probe, the hostile-input
// parser and the writer (task 4.3.2). See cooked_script.hpp for the contract and
// docs/09-file-formats.md section 16 for the normative format. NEVER THROWS. NEVER READS A FILE.
// NEVER LOGS. The payload is opaque here: it is hashed, copied by the writer and aliased by the parser.
#include <aero/assets/cooked_script.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

using cooked_script_detail::H_BACKEND_RELEASE;
using cooked_script_detail::H_BYTECODE_VERSION;
using cooked_script_detail::H_COOKER_VERSION;
using cooked_script_detail::H_FORMAT_VERSION;
using cooked_script_detail::H_GUID_HI;
using cooked_script_detail::H_GUID_LO;
using cooked_script_detail::H_MAGIC;
using cooked_script_detail::H_PAYLOAD_BYTES;
using cooked_script_detail::H_PAYLOAD_HASH_HI;
using cooked_script_detail::H_PAYLOAD_HASH_LO;
using cooked_script_detail::H_RESERVED;
using cooked_script_detail::H_RESERVED_FLAGS;
using cooked_script_detail::H_STRIP_FLAGS;

[[nodiscard]] CookedScriptParseResult refuse(CookedScriptStatus status, std::string message) {
    CookedScriptParseResult out;
    out.status = status;
    out.message = std::move(message);
    return out;
}

[[nodiscard]] std::string emptyPayloadMessage() { return "the header declares an empty payload"; }

[[nodiscard]] std::string unknownFlagsMessage(std::uint32_t flags) {
    return std::format("the header's strip flags 0x{:x} hold a bit this build does not know (known: 0x{:x})", flags,
                       COOKED_SCRIPT_KNOWN_STRIP_FLAGS);
}

}  // namespace

std::string_view cookedScriptStatusLabel(CookedScriptStatus status) noexcept {
    switch (status) {
        case CookedScriptStatus::Ok:
            return "Ok";
        case CookedScriptStatus::TooSmall:
            return "Too small";
        case CookedScriptStatus::BadMagic:
            return "Bad magic";
        case CookedScriptStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedScriptStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedScriptStatus::UnknownFlags:
            return "Unknown flags";
        case CookedScriptStatus::EmptyPayload:
            return "Empty payload";
        case CookedScriptStatus::SizeMismatch:
            return "Size mismatch";
        case CookedScriptStatus::HashMismatch:
            return "Hash mismatch";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

bool hasCookedScriptMagic(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < COOKED_SCRIPT_MAGIC.size()) {
        return false;
    }
    for (std::size_t i = 0; i < COOKED_SCRIPT_MAGIC.size(); ++i) {
        if (bytes[H_MAGIC + i] != static_cast<std::byte>(COOKED_SCRIPT_MAGIC[i])) {
            return false;
        }
    }
    return true;
}

CookedScriptParseResult parseCookedScript(std::span<const std::byte> bytes) {
    // 1. shorter than the header.
    if (bytes.size() < COOKED_SCRIPT_HEADER_BYTES) {
        return refuse(CookedScriptStatus::TooSmall,
                      std::format("the buffer is {} bytes, shorter than the {}-byte header", bytes.size(),
                                  COOKED_SCRIPT_HEADER_BYTES));
    }

    // 2. magic, all eight bytes (the NUL included).
    if (!hasCookedScriptMagic(bytes)) {
        return refuse(CookedScriptStatus::BadMagic,
                      "the buffer does not begin with the cooked script magic (a NUL, then AEROSCR)");
    }

    // 3. version.
    const std::uint32_t formatVersion = getU32(bytes, H_FORMAT_VERSION);
    if (formatVersion != COOKED_SCRIPT_FORMAT_VERSION) {
        return refuse(CookedScriptStatus::UnsupportedVersion,
                      std::format("cooked script format version {} (this build reads version {})", formatVersion,
                                  COOKED_SCRIPT_FORMAT_VERSION));
    }

    // 4. the header's reserved space. A REFUSAL: occupying it is a formatVersion bump.
    if (getU32(bytes, H_RESERVED_FLAGS) != 0) {
        return refuse(CookedScriptStatus::ReservedNotZero, "the header's reserved flags field is not zero");
    }
    if (getU64(bytes, H_RESERVED) != 0) {
        return refuse(CookedScriptStatus::ReservedNotZero, "the header's reserved field at offset 72 is not zero");
    }

    // 5. strip flags.
    const std::uint32_t stripFlags = getU32(bytes, H_STRIP_FLAGS);
    if ((stripFlags & ~COOKED_SCRIPT_KNOWN_STRIP_FLAGS) != 0) {
        return refuse(CookedScriptStatus::UnknownFlags, unknownFlagsMessage(stripFlags));
    }

    // 6. the payload size. Zero is refused; the match is a SUBTRACTION, never 80 + payloadBytes.
    const std::uint64_t payloadBytes = getU64(bytes, H_PAYLOAD_BYTES);
    if (payloadBytes == 0) {
        return refuse(CookedScriptStatus::EmptyPayload, emptyPayloadMessage());
    }
    const std::uint64_t following = bytes.size() - COOKED_SCRIPT_HEADER_BYTES;
    if (payloadBytes != following) {
        return refuse(CookedScriptStatus::SizeMismatch,
                      std::format("the header declares a {}-byte payload but {} bytes follow the header", payloadBytes,
                                  following));
    }

    // 7. the hash, last: it is the only check that costs a pass over the payload.
    const std::span<const std::byte> payload = bytes.subspan(COOKED_SCRIPT_HEADER_BYTES);
    const ContentHash expected{getU64(bytes, H_PAYLOAD_HASH_HI), getU64(bytes, H_PAYLOAD_HASH_LO)};
    if (!(hashBytes(payload) == expected)) {
        return refuse(CookedScriptStatus::HashMismatch,
                      "the payload's hash does not match the header's (the file is damaged)");
    }

    CookedScriptParseResult out;
    out.script.formatVersion = formatVersion;
    out.script.cookerVersion = getU32(bytes, H_COOKER_VERSION);
    out.script.sourceGuid.hi = getU64(bytes, H_GUID_HI);
    out.script.sourceGuid.lo = getU64(bytes, H_GUID_LO);
    out.script.backendRelease = getU32(bytes, H_BACKEND_RELEASE);
    out.script.bytecodeVersion = getU32(bytes, H_BYTECODE_VERSION);
    out.script.stripFlags = stripFlags;
    out.script.payloadBytes = payloadBytes;
    out.script.payloadHash = expected;
    out.script.payload = payload;  // THE SPAN IS THE BUFFER. Never a copy.
    return out;
}

CookedScriptWriteResult writeCookedScript(const CookedScriptInput& input) {
    CookedScriptWriteResult out;
    if (input.payload.empty()) {
        out.status = CookedScriptStatus::EmptyPayload;
        out.message = emptyPayloadMessage();
        return out;
    }
    if ((input.stripFlags & ~COOKED_SCRIPT_KNOWN_STRIP_FLAGS) != 0) {
        out.status = CookedScriptStatus::UnknownFlags;
        out.message = unknownFlagsMessage(input.stripFlags);
        return out;
    }

    out.bytes.assign(COOKED_SCRIPT_HEADER_BYTES + input.payload.size(), std::byte{0});
    const std::span<std::byte> buffer{out.bytes};
    for (std::size_t i = 0; i < COOKED_SCRIPT_MAGIC.size(); ++i) {
        buffer[H_MAGIC + i] = static_cast<std::byte>(COOKED_SCRIPT_MAGIC[i]);
    }
    const ContentHash hash = hashBytes(input.payload);
    putU32(buffer, H_FORMAT_VERSION, COOKED_SCRIPT_FORMAT_VERSION);
    putU32(buffer, H_COOKER_VERSION, COOKED_SCRIPT_COOKER_VERSION);
    putU64(buffer, H_GUID_HI, input.sourceGuid.hi);
    putU64(buffer, H_GUID_LO, input.sourceGuid.lo);
    putU32(buffer, H_BACKEND_RELEASE, input.backendRelease);
    putU32(buffer, H_BYTECODE_VERSION, input.bytecodeVersion);
    putU32(buffer, H_STRIP_FLAGS, input.stripFlags);
    putU64(buffer, H_PAYLOAD_BYTES, static_cast<std::uint64_t>(input.payload.size()));
    putU64(buffer, H_PAYLOAD_HASH_HI, hash.hi);
    putU64(buffer, H_PAYLOAD_HASH_LO, hash.lo);
    std::copy(input.payload.begin(), input.payload.end(), out.bytes.begin() + COOKED_SCRIPT_HEADER_BYTES);
    return out;
}

}  // namespace engine::assets
