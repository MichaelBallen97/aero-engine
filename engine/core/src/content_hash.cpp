// Aero Engine — engine::ContentHash: MurmurHash3 x64_128 (seed 0) and its text codec (task 3.1.2).
// EXPLICIT little-endian byte loads throughout -- NEVER a reinterpret_cast to const uint64_t*, which
// is both an alignment UB and an endianness bug, and the Debug lanes run UBSan. All arithmetic is
// std::uint64_t -- defined wraparound, UBSan-clean. NO LOGGING anywhere in this file -- core services
// below aero::log do not log.
//
// WORD-ORDER NOTE (plan D2's trap): the canonical reference (aappleby/smhasher) writes h1 then h2 into
// a 16-byte output buffer as two LITTLE-endian words. This tree emits `hi` (= h1) FIRST, BIG-endian
// per word, so the 32-hex text this file produces is NOT the hex dump of the reference's output
// buffer. Any cross-check against the reference must convert -- see the plan's Step 1 procedure.
#include <aero/core/content_hash.hpp>

#include <array>
#include <bit>  // std::rotl
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine {
namespace {

constexpr std::uint64_t MURMUR_C1 = 0x87C37B91114253D5ULL;
constexpr std::uint64_t MURMUR_C2 = 0x4CF5AD432745937FULL;
constexpr std::uint64_t MURMUR_ROUND_1 = 0x52DCE729ULL;
constexpr std::uint64_t MURMUR_ROUND_2 = 0x38495AB5ULL;
constexpr std::uint64_t FMIX_A = 0xFF51AFD7ED558CCDULL;
constexpr std::uint64_t FMIX_B = 0xC4CEB9FE1A85EC53ULL;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

// Eight explicit std::to_integer<uint8_t> shifts -- never a reinterpret_cast to const uint64_t*
// (alignment UB, and wrong on a big-endian target).
constexpr std::uint64_t readLe64(const std::byte* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
    }
    return value;
}

// MurmurHash3's own 64-bit finalization mix. fmix64(0) == 0 (plan A4): every step is an xor-shift or
// a multiply, so the empty input's all-zero h1/h2 stays all-zero through this too.
constexpr std::uint64_t fmix64(std::uint64_t k) noexcept {
    k ^= k >> 33U;
    k *= FMIX_A;
    k ^= k >> 33U;
    k *= FMIX_B;
    k ^= k >> 33U;
    return k;
}

// One 16-byte block: k1 from the first 8 bytes, k2 from the second 8 -- the reference's body loop,
// applied to exactly one block.
void processBlock(const std::byte* block, std::uint64_t& h1, std::uint64_t& h2) noexcept {
    std::uint64_t k1 = readLe64(block);
    std::uint64_t k2 = readLe64(block + 8);

    k1 *= MURMUR_C1;
    k1 = std::rotl(k1, 31);
    k1 *= MURMUR_C2;
    h1 ^= k1;

    h1 = std::rotl(h1, 27);
    h1 += h2;
    h1 = h1 * 5ULL + MURMUR_ROUND_1;

    k2 *= MURMUR_C2;
    k2 = std::rotl(k2, 33);
    k2 *= MURMUR_C1;
    h2 ^= k2;

    h2 = std::rotl(h2, 31);
    h2 += h1;
    h2 = h2 * 5ULL + MURMUR_ROUND_2;
}

// The reference's tail switch (15 -> 1, falling through exactly as it does), applied to the leftover
// `carryLength` (< 16, since a full 16 bytes is always folded into processBlock by update() and never
// left here) bytes held in `carry`.
void mixTail(const std::array<std::byte, 16>& carry, std::size_t carryLength, std::uint64_t& h1,
             std::uint64_t& h2) noexcept {
    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;
    const auto byteAt = [&carry](std::size_t index) noexcept {
        return static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(carry[index]));
    };

    switch (carryLength) {
        case 15:
            k2 ^= byteAt(14) << 48U;
            [[fallthrough]];
        case 14:
            k2 ^= byteAt(13) << 40U;
            [[fallthrough]];
        case 13:
            k2 ^= byteAt(12) << 32U;
            [[fallthrough]];
        case 12:
            k2 ^= byteAt(11) << 24U;
            [[fallthrough]];
        case 11:
            k2 ^= byteAt(10) << 16U;
            [[fallthrough]];
        case 10:
            k2 ^= byteAt(9) << 8U;
            [[fallthrough]];
        case 9:
            k2 ^= byteAt(8) << 0U;
            k2 *= MURMUR_C2;
            k2 = std::rotl(k2, 33);
            k2 *= MURMUR_C1;
            h2 ^= k2;
            [[fallthrough]];
        case 8:
            k1 ^= byteAt(7) << 56U;
            [[fallthrough]];
        case 7:
            k1 ^= byteAt(6) << 48U;
            [[fallthrough]];
        case 6:
            k1 ^= byteAt(5) << 40U;
            [[fallthrough]];
        case 5:
            k1 ^= byteAt(4) << 32U;
            [[fallthrough]];
        case 4:
            k1 ^= byteAt(3) << 24U;
            [[fallthrough]];
        case 3:
            k1 ^= byteAt(2) << 16U;
            [[fallthrough]];
        case 2:
            k1 ^= byteAt(1) << 8U;
            [[fallthrough]];
        case 1:
            k1 ^= byteAt(0) << 0U;
            k1 *= MURMUR_C1;
            k1 = std::rotl(k1, 31);
            k1 *= MURMUR_C2;
            h1 ^= k1;
            [[fallthrough]];
        default:
            break;
    }
}

// -1 when `c` is not an ASCII hex digit. unsigned char in, never char: a UTF-8 continuation byte is
// negative as char, which is UB (project_files.cpp:44-46's rule; guid.cpp:43's precedent verbatim).
constexpr int hexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<int>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<int>(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<int>(c - 'A') + 10;
    }
    return -1;
}

}  // namespace

void ContentHasher::update(std::span<const std::byte> bytes) noexcept {
    totalLength += bytes.size();
    std::size_t offset = 0;
    const std::size_t size = bytes.size();

    // Top up an existing partial carry from the FRONT of `bytes`.
    if (carryLength > 0) {
        const std::size_t need = 16U - carryLength;
        const std::size_t take = (need < size) ? need : size;
        for (std::size_t i = 0; i < take; ++i) {
            carry[carryLength + i] = bytes[i];
        }
        carryLength += take;
        offset += take;
        if (carryLength < 16U) {
            return;  // still not a full block
        }
        processBlock(carry.data(), h1, h2);
        carryLength = 0;
    }

    // Whole 16-byte blocks DIRECTLY out of `bytes` -- never byte-by-byte through the carry, which is
    // the difference between ~6 GB/s and ~0.2 GB/s (plan D-2).
    while (offset + 16U <= size) {
        processBlock(bytes.data() + offset, h1, h2);
        offset += 16U;
    }

    // The remaining < 16 bytes go into the carry for the next call (or finish()).
    const std::size_t remaining = size - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        carry[i] = bytes[offset + i];
    }
    carryLength = remaining;
}

ContentHash ContentHasher::finish() const noexcept {
    // LOCAL copies -- finish() is const and idempotent (plan A12): it never mutates the hasher, so
    // calling it twice gives the same answer and calling update() afterwards simply continues the
    // stream.
    std::uint64_t localH1 = h1;
    std::uint64_t localH2 = h2;
    mixTail(carry, carryLength, localH1, localH2);

    localH1 ^= totalLength;
    localH2 ^= totalLength;
    localH1 += localH2;
    localH2 += localH1;
    localH1 = fmix64(localH1);
    localH2 = fmix64(localH2);
    localH1 += localH2;
    localH2 += localH1;
    return ContentHash{localH1, localH2};
}

ContentHash hashBytes(std::span<const std::byte> bytes) noexcept {
    ContentHasher hasher;
    hasher.update(bytes);
    return hasher.finish();
}

std::string formatContentHash(ContentHash hash) {
    std::string text(CONTENT_HASH_TEXT_LENGTH, '0');  // zero-padded by construction
    std::size_t index = 0;
    for (const std::uint64_t half : {hash.hi, hash.lo}) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto nibble = static_cast<unsigned>((half >> static_cast<unsigned>(shift)) & 0xFULL);
            text[index] = HEX_DIGITS[nibble];
            ++index;
        }
    }
    return text;
}

std::optional<ContentHash> parseContentHash(std::string_view text) noexcept {
    // Length check FIRST: a dashed or braced value is a parse ERROR, never silently normalized
    // (guid.cpp's D3 rule, applied here too).
    if (text.size() != CONTENT_HASH_TEXT_LENGTH) {
        return std::nullopt;
    }
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (std::size_t i = 0; i < CONTENT_HASH_TEXT_LENGTH; ++i) {
        const int value = hexValue(static_cast<unsigned char>(text[i]));
        if (value < 0) {
            return std::nullopt;  // reject on the first non-hex byte
        }
        std::uint64_t& half = (i < CONTENT_HASH_TEXT_LENGTH / 2U) ? hi : lo;
        half = (half << 4U) | static_cast<std::uint64_t>(value);
    }
    return ContentHash{hi, lo};  // ALL-ZERO IS ACCEPTED (plan A4) -- nil is a legitimate digest
}

}  // namespace engine

std::size_t std::hash<engine::ContentHash>::operator()(const engine::ContentHash& h) const noexcept {
    // NOT hi ^ lo (AC-2): that collides for swapped halves. An asymmetric combine
    // (boost::hash_combine's shape), finalised through THIS file's own fmix64 -- the MurmurHash3
    // finalizer, not guid.cpp's splitmix64 mix64. `engine::fmix64` names the anonymous-namespace
    // function above: an unnamed namespace's members are visible through their enclosing namespace
    // within the defining translation unit.
    std::uint64_t combined = h.hi;
    combined ^= h.lo + 0x9E3779B97F4A7C15ULL + (combined << 6U) + (combined >> 2U);
    return static_cast<std::size_t>(engine::fmix64(combined));
}
