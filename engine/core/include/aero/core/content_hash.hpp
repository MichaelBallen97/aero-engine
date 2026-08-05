#pragma once
// Aero Engine — ContentHash: the 128-bit fingerprint of a file's BYTES (task 3.1.2). The guid.hpp
// precedent, one file over: a small trivially-copyable value type that depends on NOTHING, living in
// core because /tools (3.3's cooker) and /runtime (Phase 5's packager) will both need it.
//
// THIS IS NOT A Guid, AND THE DISTINCTION IS THE POINT (3.1.1's A3). A Guid is IDENTITY: minted once,
// frozen in a committed sidecar, deliberately independent of both path and content. A ContentHash is
// the opposite by design: it changes every time the bytes change, is never committed, and is only
// ever compared against a value this same build wrote. A `using ContentHash = Guid` would compile,
// read naturally, and make findByGuid(hash) a silent no-op forever.
//
// MurmurHash3 x64_128, seed 0, EXPLICIT little-endian byte loads (never a reinterpret_cast to
// uint64_t*, which is both an alignment UB and an endianness bug): byte-identical on all three
// platforms, pure uint64 arithmetic with defined wraparound, one pass. NOT CRYPTOGRAPHIC -- an
// adversary who can place chosen files in the assets tree can construct a collision, and has strictly
// better options than confusing a cache. The algorithm NAME is recorded in the cache index, so a
// future swap is a cache discard rather than a silent comparison of incompatible digests.
//
// LINKAGE NOTE (guid.hpp's A14, second application): std::hash<engine::ContentHash>::operator() is
// DECLARED here and DEFINED in content_hash.cpp, so anything that hashes a ContentHash must link
// aero::core. Every consumer in this tree already does.

#include <array>  // ContentHasher::carry -- NEVER a C array (modernize-avoid-c-arrays is live)
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine {

struct ContentHash {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    // TRAP (measured against the canonical reference, plan A4): MurmurHash3 x64_128 of the EMPTY
    // input with seed 0 is ALL ZEROS. So valid() is FALSE for the digest of a zero-byte file, which
    // is a perfectly legitimate value. This is NOT a "was this hashed?" flag -- the only such flag is
    // engagement of std::optional<ContentHash>. Read it as "not the empty-input digest".
    [[nodiscard]] constexpr bool valid() const noexcept { return hi != 0 || lo != 0; }
    constexpr bool operator==(const ContentHash&) const noexcept = default;
    [[nodiscard]] constexpr bool operator<(const ContentHash& other) const noexcept {
        return hi != other.hi ? hi < other.hi : lo < other.lo;
    }
};

static_assert(sizeof(ContentHash) == 16);
static_assert(std::is_trivially_copyable_v<ContentHash>);

inline constexpr std::size_t CONTENT_HASH_TEXT_LENGTH = 32;
inline constexpr std::string_view CONTENT_HASH_ALGORITHM = "murmur3-x64-128";

// Exactly 32 lowercase hex digits, zero-padded, `hi` FIRST. The empty-input digest formats as 32 '0's.
[[nodiscard]] std::string formatContentHash(ContentHash hash);
// EXACTLY 32 hex digits, any case, and nothing else -- parseGuid's contract verbatim: no dashes, no
// braces, no whitespace, no 0x, no other length, no embedded NUL. ALL-ZERO IS ACCEPTED (plan A4).
[[nodiscard]] std::optional<ContentHash> parseContentHash(std::string_view text) noexcept;

// One-shot. IMPLEMENTED IN TERMS OF ContentHasher (plan A11), so there is exactly ONE MurmurHash3 in
// this tree and AC-5's "any chunking gives the same value" holds by construction, not by testing.
[[nodiscard]] ContentHash hashBytes(std::span<const std::byte> bytes) noexcept;

// Incremental: a 16-byte carry buffer holds the partial block between calls, so update() accepts ANY
// chunk sizes. This is what lets a 2 GB file be hashed in 1 MiB reads without ever existing in memory
// (D8) -- and it is why AC-5's adversarial-split battery is not decoration.
class ContentHasher {
public:
    void update(std::span<const std::byte> bytes) noexcept;
    // const, and therefore idempotent (plan A12): it computes the tail and the finalization on LOCAL
    // copies of h1/h2. Calling it twice gives the same answer; calling update() afterwards simply
    // continues the stream, which is what an incremental hasher should do.
    [[nodiscard]] ContentHash finish() const noexcept;

private:
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;
    std::uint64_t totalLength = 0;
    std::array<std::byte, 16> carry{};
    std::size_t carryLength = 0;
};

}  // namespace engine

template <>
struct std::hash<engine::ContentHash> {
    // NOT hi ^ lo: that collides for swapped halves. A 64-bit finalizer mix instead (AC-2).
    // DEFINED in content_hash.cpp.
    [[nodiscard]] std::size_t operator()(const engine::ContentHash& h) const noexcept;
};
