// Aero Engine -- the bytecode format's backend-free half (task 4.3.2). See the header for the contract. Every
// offset and rule here is a mirror of the pinned backend's byte format (quickjs-ng 0.15.1, quickjs.c) and is
// pinned by tests/script_bytecode_format_test.cpp.

#include <aero/assets/cooked_mesh.hpp>  // getU32 / putU32: the one little-endian pair (its static_assert is the proof)
#include <aero/assets/cooked_script.hpp>
#include <aero/script/internal/bytecode_format.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::script::internal {
namespace {

constexpr std::uint32_t CHECKSUM_MULTIPLIER = 0x9e370001U;
constexpr std::size_t HEAD_BYTES = 5;                // the version byte and the checksum word
constexpr std::uint8_t ATOM_TYPE_STRING = 1;         // JS_ATOM_TYPE_STRING (quickjs.c:593); 0 is a predefined atom
constexpr std::size_t MAX_LEB128_BYTES = 5;          // get_leb128 stops after five bytes (quickjs.c:7638-7656)
constexpr std::size_t MAX_NAME_BYTES = 0x3FFFFFFFU;  // length << 1 must fit a 32-bit LEB128 word

// get_leb128: at most five bytes, 7 bits each, low group first, in 32-bit arithmetic. False at the end of the span or
// after five continuation bytes.
[[nodiscard]] bool readLeb128(std::span<const std::byte> bytes, std::size_t& position, std::uint32_t& value) noexcept {
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < MAX_LEB128_BYTES; ++i) {
        if (position >= bytes.size()) {
            return false;
        }
        const auto b = std::to_integer<std::uint32_t>(bytes[position]);
        ++position;
        result |= (b & 0x7FU) << (7U * i);
        if ((b & 0x80U) == 0) {
            value = result;
            return true;
        }
    }
    return false;
}

// dbuf_put_leb128 (quickjs.c:7617-7631): the minimal encoding, 7 bits per byte, the high bit on all but the last.
void appendLeb128(std::vector<std::byte>& out, std::uint32_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::byte>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::byte>(value));
}

[[nodiscard]] bool printableAscii(std::string_view name) noexcept {
    if (name.empty() || name.size() > MAX_NAME_BYTES) {
        return false;
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == 0 || byte >= 0x80U) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::uint32_t bytecodeChecksum(std::span<const std::byte> bytes) noexcept {
    std::uint32_t h = 0;
    std::size_t i = 0;
    for (; i + 4 < bytes.size(); i += 4) {
        h += assets::getU32(bytes, i);  // the backend reads a native u32; every target is little-endian
        h *= CHECKSUM_MULTIPLIER;
    }
    const std::size_t rest = bytes.size() - i;  // 0 only for an empty span; otherwise 1..4
    std::uint32_t tail = 0;
    if (rest >= 1 && rest <= 3) {  // no arm for 4: that word never enters the sum
        tail = std::to_integer<std::uint32_t>(bytes[i]);
        if (rest >= 2) {
            tail |= std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U;
        }
        if (rest == 3) {
            tail |= std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U;
        }
    }
    h += tail;
    h *= CHECKSUM_MULTIPLIER;
    return h;
}

std::string_view bytecodeHeadStatusText(BytecodeHeadStatus status) noexcept {
    switch (status) {
        case BytecodeHeadStatus::Ok:
            return "";
        case BytecodeHeadStatus::Truncated:
            return "its bytecode ends inside the format head";
        case BytecodeHeadStatus::WrongVersion:
            return "its payload's bytecode version byte is not 26";
        case BytecodeHeadStatus::ChecksumBypass:
            return "its backend checksum is the bypass value FF FF FF FF";
        case BytecodeHeadStatus::ChecksumMismatch:
            return "its backend checksum does not match its bytes";
        case BytecodeHeadStatus::NoAtoms:
            return "its atom table is empty";
        case BytecodeHeadStatus::NameNotAString:
            return "its module name is not a narrow string atom";
        case BytecodeHeadStatus::NameNotPlaceholder:
            return "its module name is not the cook's placeholder (it was not written by this engine's cook)";
        case BytecodeHeadStatus::InvalidNewName:
            return "the module's name is not printable ASCII";
    }
    return "";
}

RenamedBytecode renameBytecodeModule(std::span<const std::byte> payload, std::string_view expectedName,
                                     std::string_view newName) {
    const auto refuse = [](BytecodeHeadStatus status) { return RenamedBytecode{status, {}}; };
    if (!printableAscii(newName)) {
        return refuse(BytecodeHeadStatus::InvalidNewName);
    }
    if (payload.size() < HEAD_BYTES) {
        return refuse(BytecodeHeadStatus::Truncated);
    }
    if (std::to_integer<std::uint8_t>(payload[0]) != SCRIPT_BYTECODE_VERSION) {
        return refuse(BytecodeHeadStatus::WrongVersion);
    }
    const std::uint32_t stored = assets::getU32(payload, 1);
    if (stored == BYTECODE_CHECKSUM_BYPASS) {
        return refuse(BytecodeHeadStatus::ChecksumBypass);
    }
    if (stored != bytecodeChecksum(payload.subspan(HEAD_BYTES))) {
        return refuse(BytecodeHeadStatus::ChecksumMismatch);
    }
    std::size_t p = HEAD_BYTES;
    std::uint32_t count = 0;
    if (!readLeb128(payload, p, count)) {
        return refuse(BytecodeHeadStatus::Truncated);
    }
    if (count == 0) {
        return refuse(BytecodeHeadStatus::NoAtoms);
    }
    if (p >= payload.size()) {
        return refuse(BytecodeHeadStatus::Truncated);
    }
    const std::size_t typeAt = p;
    if (std::to_integer<std::uint8_t>(payload[p]) != ATOM_TYPE_STRING) {
        return refuse(BytecodeHeadStatus::NameNotAString);
    }
    ++p;
    std::uint32_t word = 0;
    if (!readLeb128(payload, p, word)) {
        return refuse(BytecodeHeadStatus::Truncated);
    }
    if ((word & 1U) != 0) {  // a wide (16-bit) string
        return refuse(BytecodeHeadStatus::NameNotAString);
    }
    const std::size_t length = word >> 1U;
    if (length > payload.size() - p) {
        return refuse(BytecodeHeadStatus::Truncated);
    }
    const std::string_view storedName{reinterpret_cast<const char*>(payload.data() + p), length};
    if (storedName != expectedName) {
        return refuse(BytecodeHeadStatus::NameNotPlaceholder);
    }

    RenamedBytecode result;
    result.bytes.reserve(payload.size() - length + newName.size() + MAX_LEB128_BYTES);
    result.bytes.insert(result.bytes.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(typeAt + 1));
    appendLeb128(result.bytes, static_cast<std::uint32_t>(newName.size()) << 1U);
    for (const char c : newName) {
        result.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    result.bytes.insert(result.bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(p + length), payload.end());
    const std::span<std::byte> out{result.bytes};
    assets::putU32(out, 1, bytecodeChecksum(std::span<const std::byte>{out}.subspan(HEAD_BYTES)));
    return result;
}

std::size_t bytecodeReadHeadroom(std::size_t payloadBytes) noexcept {
    constexpr std::size_t MAX = std::numeric_limits<std::size_t>::max();
    if (payloadBytes > (MAX - BYTECODE_READ_FIXED_BYTES) / BYTECODE_READ_BYTES_PER_PAYLOAD_BYTE) {
        return MAX;
    }
    return BYTECODE_READ_FIXED_BYTES + BYTECODE_READ_BYTES_PER_PAYLOAD_BYTE * payloadBytes;
}

PreparedBytecode prepareCookedModule(std::span<const std::byte> container, std::string_view moduleName,
                                     std::uint32_t backendRelease) {
    PreparedBytecode out;
    const assets::CookedScriptParseResult parsed = assets::parseCookedScript(container);
    if (parsed.status != assets::CookedScriptStatus::Ok) {
        out.refusal = parsed.message;
        return out;
    }
    const assets::CookedScript& script = parsed.script;
    if (script.backendRelease != backendRelease) {
        const auto part = [](std::uint32_t release, unsigned shift) { return (release >> shift) & 0xFFU; };
        out.refusal =
            std::format("it was cooked for quickjs-ng {}.{}.{}, and this engine runs {}.{}.{}",
                        part(script.backendRelease, 16), part(script.backendRelease, 8), part(script.backendRelease, 0),
                        part(backendRelease, 16), part(backendRelease, 8), part(backendRelease, 0));
        return out;
    }
    if (script.bytecodeVersion != SCRIPT_BYTECODE_VERSION) {
        out.refusal = std::format("its header declares bytecode version {}, and this engine reads version {}",
                                  script.bytecodeVersion, static_cast<unsigned>(SCRIPT_BYTECODE_VERSION));
        return out;
    }
    RenamedBytecode renamed = renameBytecodeModule(script.payload, COOK_MODULE_NAME, moduleName);
    if (renamed.status != BytecodeHeadStatus::Ok) {
        out.refusal = std::string{bytecodeHeadStatusText(renamed.status)};
        return out;
    }
    out.payload = std::move(renamed.bytes);
    return out;
}

}  // namespace engine::script::internal
