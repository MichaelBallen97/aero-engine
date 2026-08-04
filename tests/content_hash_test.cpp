// tests/content_hash_test.cpp -- task 3.1.2: engine::ContentHash's value semantics, text codec,
// ordering, incremental hasher and the MurmurHash3 x64_128 algorithm itself. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// CH21/CH22's two pinned literals were produced by this implementation and INDEPENDENTLY confirmed
// against the canonical public-domain MurmurHash3 reference (aappleby/smhasher, SHA-256
// 30f121ed155ebf336af398aabb7d8d157afdfafc8d981e7b48d2a1ceb4b63e4e), converted per the word-order
// rule content_hash.cpp's banner states (the reference writes h1 then h2 LITTLE-endian; this tree
// emits hi=h1 first, BIG-endian per word). A second, independent leg reproduced the published
// SMHasher VerificationTest value 0x6384BA69 for Murmur3F ("Murmur3F") from a seeded transliteration
// of this file's own algorithm. Full record: docs/10-engineering-log.md's task 3.1.2 entry.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

using engine::CONTENT_HASH_TEXT_LENGTH;
using engine::ContentHash;
using engine::ContentHasher;
using engine::formatContentHash;
using engine::hashBytes;
using engine::parseContentHash;

namespace {

std::span<const std::byte> bytesOf(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

// The deterministic pattern D-1's plan specifies for the large pinned literal and the
// adversarial-chunking battery: buf[i] = (i*31+7) & 0xFF -- NOT block-aligned at any of the sizes
// used here, which is the point (it exercises the carry crossing a 16-byte boundary).
std::vector<std::byte> makePattern(std::size_t size) {
    std::vector<std::byte> buf(size);
    for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<std::byte>((i * 31U + 7U) & 0xFFU);
    }
    return buf;
}

// Feeds `data` through a fresh ContentHasher in chunks whose sizes cycle through `chunkSizes`,
// clipping the final chunk to whatever remains. Used for the repeating-pattern arms of CH25.
ContentHash hashInCyclicChunks(std::span<const std::byte> data, const std::vector<std::size_t>& chunkSizes) {
    ContentHasher hasher;
    std::size_t offset = 0;
    std::size_t cursor = 0;
    while (offset < data.size()) {
        const std::size_t want = chunkSizes[cursor % chunkSizes.size()];
        const std::size_t take = std::min(want, data.size() - offset);
        hasher.update(data.subspan(offset, take));
        offset += take;
        ++cursor;
    }
    return hasher.finish();
}

}  // namespace

TEST_CASE("ContentHash: default-constructed is all-zero and invalid (CH1)") {
    const ContentHash h;
    CHECK(h.hi == 0);
    CHECK(h.lo == 0);
    CHECK_FALSE(h.valid());
}

TEST_CASE("ContentHash: layout matches Guid's precedent (CH2)") {
    CHECK(sizeof(ContentHash) == 16);
    CHECK(std::is_trivially_copyable_v<ContentHash>);
}

TEST_CASE("ContentHash: NOT convertible to/from Guid, in either direction (CH3, INV-C2)") {
    // A ContentHash is content, a Guid is identity -- distinct concerns with distinct lifetimes.
    // A `using ContentHash = Guid` would compile and make findByGuid(hash) a silent no-op forever.
    static_assert(!std::is_convertible_v<engine::ContentHash, engine::Guid>);
    static_assert(!std::is_convertible_v<engine::Guid, engine::ContentHash>);
    static_assert(!std::is_constructible_v<engine::Guid, engine::ContentHash>);
    static_assert(!std::is_constructible_v<engine::ContentHash, engine::Guid>);
}

TEST_CASE("ContentHash: valid() is true whenever either half is non-zero (CH4)") {
    CHECK(ContentHash{1, 0}.valid());
    CHECK(ContentHash{0, 1}.valid());
    CHECK(ContentHash{1, 1}.valid());
    CHECK_FALSE(ContentHash{0, 0}.valid());
}

TEST_CASE("ContentHash: operator== / != compare both halves (CH5)") {
    CHECK(ContentHash{1, 2} == ContentHash{1, 2});
    CHECK(ContentHash{1, 2} != ContentHash{2, 2});
    CHECK(ContentHash{1, 2} != ContentHash{1, 3});
    CHECK(ContentHash{0, 0} == ContentHash{});
}

TEST_CASE("ContentHash: operator< is a strict total order by (hi, lo) (CH6)") {
    CHECK(ContentHash{1, 0} < ContentHash{1, 1});
    CHECK(ContentHash{1, 1} < ContentHash{2, 0});
    CHECK(ContentHash{1, 0} < ContentHash{2, 0});
    CHECK_FALSE(ContentHash{1, 1} < ContentHash{1, 1});  // irreflexive
    CHECK_FALSE(ContentHash{2, 0} < ContentHash{1, 1});
}

TEST_CASE("ContentHash: std::hash differs for swapped halves (CH7, AC-2)") {
    const ContentHash a{0x1111111111111111ULL, 0x2222222222222222ULL};
    const ContentHash b{0x2222222222222222ULL, 0x1111111111111111ULL};
    CHECK(a != b);
    CHECK(std::hash<ContentHash>{}(a) != std::hash<ContentHash>{}(b));
}

TEST_CASE("ContentHash: std::hash is stable and usable as a set key (CH8)") {
    const ContentHash h{0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL};
    CHECK(std::hash<ContentHash>{}(h) == std::hash<ContentHash>{}(h));

    std::unordered_set<ContentHash> set;
    set.insert(h);
    set.insert(h);
    CHECK(set.size() == 1);
    set.insert(ContentHash{1, 2});
    CHECK(set.size() == 2);
}

TEST_CASE("ContentHash: formatContentHash returns exactly 32 lowercase hex chars (CH9, AC-3)") {
    const std::string text = formatContentHash(ContentHash{0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL});
    REQUIRE(text.size() == CONTENT_HASH_TEXT_LENGTH);
    for (const char c : text) {
        const bool isLowerHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isLowerHex);
    }
}

TEST_CASE("ContentHash: formatContentHash(ContentHash{}) is 32 zeros (CH10, AC-3)") {
    CHECK(formatContentHash(ContentHash{}) == std::string(32, '0'));
}

TEST_CASE("ContentHash: formatContentHash zero-pads (CH11, AC-3)") {
    CHECK(formatContentHash(ContentHash{1, 1}) == "00000000000000010000000000000001");
}

TEST_CASE("ContentHash: formatContentHash emits hi FIRST (CH12, AC-3, seed S5)") {
    CHECK(formatContentHash(ContentHash{0x0123456789ABCDEFULL, 0}) == "0123456789abcdef0000000000000000");
}

TEST_CASE("ContentHash: parseContentHash is case-tolerant (CH13, AC-3)") {
    const std::optional<ContentHash> lower = parseContentHash("a3f1c07e5b8d42198e6f0c3d7a2b4b92");
    const std::optional<ContentHash> upper = parseContentHash("A3F1C07E5B8D42198E6F0C3D7A2B4B92");
    const std::optional<ContentHash> mixed = parseContentHash("a3F1c07E5b8D42198E6f0C3d7A2b4B92");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE(mixed.has_value());
    CHECK(*lower == *upper);
    CHECK(*lower == *mixed);
}

TEST_CASE("ContentHash: parseContentHash rejects the wrong length (CH14, AC-3)") {
    CHECK_FALSE(parseContentHash("").has_value());
    CHECK_FALSE(parseContentHash(std::string(31, 'a')).has_value());
    CHECK_FALSE(parseContentHash(std::string(33, 'a')).has_value());
}

TEST_CASE("ContentHash: parseContentHash rejects dashed and braced forms (CH15, AC-3)") {
    CHECK_FALSE(parseContentHash("a3f1c07e-5b8d-4219-8e6f-0c3d7a2b4b92").has_value());
    CHECK_FALSE(parseContentHash("{a3f1c07e5b8d42198e6f0c3d7a2b4b92}").has_value());
}

TEST_CASE("ContentHash: parseContentHash rejects whitespace and a 0x prefix (CH16, AC-3)") {
    CHECK_FALSE(parseContentHash(" a3f1c07e5b8d42198e6f0c3d7a2b4b92").has_value());
    CHECK_FALSE(parseContentHash("a3f1c07e5b8d42198e6f0c3d7a2b4b92 ").has_value());
    CHECK_FALSE(parseContentHash("0xa3f1c07e5b8d42198e6f0c3d7a2b4b9").has_value());
}

TEST_CASE("ContentHash: parseContentHash rejects a non-hex byte anywhere (CH17, AC-3)") {
    CHECK_FALSE(parseContentHash("g3f1c07e5b8d42198e6f0c3d7a2b4b92").has_value());  // first
    CHECK_FALSE(parseContentHash("a3f1c07e5bZd42198e6f0c3d7a2b4b92").has_value());  // middle
    CHECK_FALSE(parseContentHash("a3f1c07e5b8d42198e6f0c3d7a2b4b9g").has_value());  // last
}

TEST_CASE("ContentHash: parseContentHash rejects an embedded NUL (CH18, AC-3)") {
    // std::array, not a C array: modernize-avoid-c-arrays is --warnings-as-errors in CI
    // (guid_test.cpp GU17's precedent verbatim).
    static constexpr std::array<char, 33> RAW{
        'a', '3',  'f', '1', 'c', '0', '7', 'e', '5', '8', 'b', '8', 'd', '4', '2', '1', '9',
        '8', '\0', 'f', '0', 'c', '3', 'd', '7', 'a', '2', 'b', '4', 'b', '9', '2', 'x',
    };
    const std::string_view withNul(RAW.data(), RAW.size() - 1U);  // 32 bytes, one of them '\0'
    CHECK_FALSE(parseContentHash(withNul).has_value());
}

TEST_CASE("ContentHash: round-trip for nil, all-f and a fixed literal (CH19, AC-3)") {
    const ContentHash nil{};
    const ContentHash allF{0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
    const ContentHash literal{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    for (const ContentHash& h : {nil, allF, literal}) {
        const std::optional<ContentHash> parsed = parseContentHash(formatContentHash(h));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == h);
    }
}

TEST_CASE("ContentHash: hashBytes({}) is ALL-ZERO and valid() is FALSE (CH20, AC-6/AC-8, A4)") {
    // The contract case, not a discriminator (plan A4): MurmurHash3 x64_128 of the empty input with
    // seed 0 is all zeros by arithmetic (every finalization step is an xor-shift or a multiply of 0),
    // which is why this literal alone proves nothing about the algorithm -- CH21/CH22 carry that.
    const ContentHash h = hashBytes(std::span<const std::byte>{});
    CHECK(h.hi == 0);
    CHECK(h.lo == 0);
    CHECK_FALSE(h.valid());
}

TEST_CASE("ContentHash: PINNED LITERAL 1 -- short ASCII input (CH21, AC-6)") {
    // 43 bytes, pure ASCII. Cross-checked against the canonical reference (aappleby/smhasher,
    // SHA-256 30f121ed155ebf336af398aabb7d8d157afdfafc8d981e7b48d2a1ceb4b63e4e) with the word-order
    // conversion applied, AND against the published SMHasher VerificationTest constant 0x6384BA69 via
    // a seeded transliteration of this file's own algorithm -- both legs agreed before this literal
    // was frozen (docs/10-engineering-log.md).
    const std::string input = "The quick brown fox jumps over the lazy dog";
    REQUIRE(input.size() == 43);
    const ContentHash h = hashBytes(bytesOf(input));
    CHECK(formatContentHash(h) == "e34bbc7bbc071b6c7a433ca9c49a9347");
}

TEST_CASE("ContentHash: PINNED LITERAL 2 -- >=1 MiB deterministic pattern (CH22, AC-6)") {
    // Exactly 1024*1024 + 5 bytes -- deliberately NOT block-aligned, so the tail exercises the carry
    // crossing a chunk boundary. Cross-checked the same way as CH21.
    const std::vector<std::byte> buf = makePattern(static_cast<std::size_t>(1024U) * 1024U + 5U);
    const ContentHash h = hashBytes(buf);
    CHECK(formatContentHash(h) == "912d8bc874074f7eb99738f4eaeb2311");
}

TEST_CASE("ContentHash: a one-bit change in a 64-byte buffer changes the hash (CH23, AC-4)") {
    std::vector<std::byte> a = makePattern(64);
    std::vector<std::byte> b = a;
    b[31] ^= std::byte{0x01};  // flip one bit, mid-buffer
    CHECK(hashBytes(a) != hashBytes(b));
}

TEST_CASE("ContentHash: length extension -- \"abc\" vs \"abc\\0\" differ (CH24, AC-4, seed S4)") {
    // Proves h1 ^= totalLength is LIVE: without it, appending a trailing zero byte to a buffer whose
    // tail byte count already accounts for it could hash identically.
    const std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const std::array<std::byte, 4> abcNul{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{0}};
    CHECK(hashBytes(abc) != hashBytes(abcNul));
}

TEST_CASE("ContentHash: the adversarial-chunking battery -- all splits agree (CH25, AC-5, seed S3)") {
    // "The 3 MiB + 5-byte buffer" (plan §S Step 1): large enough for the {1 MiB x3, tail} pattern
    // below, and NOT block-aligned (the +5), so every pattern here exercises the carry differently.
    const std::vector<std::byte> data = makePattern(static_cast<std::size_t>(3U) * 1024U * 1024U + 5U);
    const ContentHash whole = hashBytes(data);

    // Five REPEATING chunk-size patterns.
    CHECK(hashInCyclicChunks(data, {1}) == whole);
    CHECK(hashInCyclicChunks(data, {15, 1}) == whole);
    CHECK(hashInCyclicChunks(data, {16}) == whole);
    CHECK(hashInCyclicChunks(data, {17}) == whole);
    CHECK(hashInCyclicChunks(data, {7, 9, 3, 13, 21, 2}) == whole);

    // {1x64 then the rest}: one 64-byte chunk, then everything else in one call.
    {
        ContentHasher hasher;
        hasher.update(std::span<const std::byte>(data).subspan(0, 64));
        hasher.update(std::span<const std::byte>(data).subspan(64));
        CHECK(hasher.finish() == whole);
    }

    // {1 MiB x3, tail}: three full 1 MiB chunks, then the remainder.
    {
        constexpr std::size_t ONE_MIB = static_cast<std::size_t>(1024U) * 1024U;
        ContentHasher hasher;
        std::size_t offset = 0;
        for (int i = 0; i < 3; ++i) {
            hasher.update(std::span<const std::byte>(data).subspan(offset, ONE_MIB));
            offset += ONE_MIB;
        }
        hasher.update(std::span<const std::byte>(data).subspan(offset));
        CHECK(hasher.finish() == whole);
    }

    // One single call -- the degenerate, non-chunked case.
    {
        ContentHasher hasher;
        hasher.update(data);
        CHECK(hasher.finish() == whole);
    }
}

TEST_CASE("ContentHash: finish() called twice returns the same value (CH26, A12)") {
    ContentHasher hasher;
    hasher.update(bytesOf(std::string("some bytes")));
    const ContentHash first = hasher.finish();
    const ContentHash second = hasher.finish();
    CHECK(first == second);
}
