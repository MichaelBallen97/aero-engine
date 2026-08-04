// Aero Engine — engine::Guid's text codec and entropy seed (task 3.1.1). splitmix64: pure unsigned
// 64-bit arithmetic, so every operation is defined-wraparound and UBSan-clean, and byte-identical on
// all three platforms. NO LOGGING anywhere in this file -- core services below aero::log do not log.
#include <aero/core/guid.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace engine {
namespace {

constexpr std::uint64_t SPLITMIX_GAMMA = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t SPLITMIX_MIX_A = 0xBF58476D1CE4E5B9ULL;
constexpr std::uint64_t SPLITMIX_MIX_B = 0x94D049BB133111EBULL;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

// The splitmix64 finaliser alone (no state increment). A pure function of its argument: mix64(0) ==
// 0, which is what makes GU21/A12's "a zero HALF is not nil" case constructible and pinned.
constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
    z ^= z >> 30U;
    z *= SPLITMIX_MIX_A;
    z ^= z >> 27U;
    z *= SPLITMIX_MIX_B;
    z ^= z >> 31U;
    return z;
}

// One splitmix64 draw: advance `state` by the golden-ratio increment, then finalise it.
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += SPLITMIX_GAMMA;
    return mix64(state);
}

// -1 when `c` is not an ASCII hex digit. unsigned char in, never char: a UTF-8 continuation byte is
// negative as char, which is UB (project_files.cpp:44-46's rule) -- moot for a 32-hex-digit string,
// kept anyway so the signature can never be misused on a wider input.
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

std::string formatGuid(Guid guid) {
    std::string text(GUID_TEXT_LENGTH, '0');  // zero-padded by construction
    std::size_t index = 0;
    for (const std::uint64_t half : {guid.hi, guid.lo}) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto nibble = static_cast<unsigned>((half >> static_cast<unsigned>(shift)) & 0xFULL);
            text[index] = HEX_DIGITS[nibble];
            ++index;
        }
    }
    return text;
}

std::optional<Guid> parseGuid(std::string_view text) noexcept {
    // Length check FIRST (D3): a dashed or braced value is a parse ERROR, never silently normalized.
    if (text.size() != GUID_TEXT_LENGTH) {
        return std::nullopt;
    }
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (std::size_t i = 0; i < GUID_TEXT_LENGTH; ++i) {
        const int value = hexValue(static_cast<unsigned char>(text[i]));
        if (value < 0) {
            return std::nullopt;  // reject on the first non-hex byte
        }
        std::uint64_t& half = (i < GUID_TEXT_LENGTH / 2U) ? hi : lo;
        half = (half << 4U) | static_cast<std::uint64_t>(value);
    }
    return Guid{hi, lo};
}

Guid GuidGenerator::next() noexcept {
    Guid result;
    do {
        result.hi = splitmix64(state);
        result.lo = splitmix64(state);
        // hi and lo are drawn from two DISTINCT, consecutive generator states, and splitmix64's
        // finaliser (mix64 above) is a bijection on std::uint64_t: each `z ^= z >> k` is an
        // invertible GF(2) triangular map, and each multiplier (SPLITMIX_MIX_A, SPLITMIX_MIX_B) is
        // odd, hence invertible mod 2^64. A bijection cannot map two distinct inputs to zero, so hi
        // and lo can never BOTH be zero -- this loop is provably dead today (plan A12). Kept as
        // defence in depth: a future generator whose two halves share one state, or whose mixer is
        // not a bijection, makes it live again.
    } while (!result.valid());
    return result;
}

GuidGenerator GuidGenerator::fromEntropy() {
    // A function-local monotonic counter (A13): two fromEntropy() calls at the same stack depth take
    // the same `&local` address, and on a machine whose random_device is degenerate (MinGW's is a
    // documented constant) and whose steady_clock is coarse, the other three sources alone could
    // collide. This one cannot -- it makes AC-9 total by construction, not probabilistic.
    static std::atomic<std::uint64_t> callCounter{0};

    std::random_device device;
    const auto r1 = static_cast<std::uint64_t>(device());
    const auto r2 = static_cast<std::uint64_t>(device());
    const std::uint64_t randomBits = r1 | (r2 << 32U);

    const std::uint64_t clockBits =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    int stackProbe = 0;
    const auto addressBits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&stackProbe));

    const std::uint64_t counterBits = callCounter.fetch_add(1, std::memory_order_relaxed);

    std::uint64_t seed = mix64(randomBits);
    seed ^= mix64(clockBits + SPLITMIX_GAMMA);
    seed ^= mix64(addressBits + 2ULL * SPLITMIX_GAMMA);
    seed ^= mix64(counterBits + 3ULL * SPLITMIX_GAMMA);
    return GuidGenerator(seed);
}

}  // namespace engine

std::size_t std::hash<engine::Guid>::operator()(const engine::Guid& g) const noexcept {
    // NOT hi ^ lo (AC-3): that collides for swapped halves, exactly the shape a hand-built
    // duplicate-GUID test would generate. An asymmetric combine (boost::hash_combine's shape),
    // finalised through the same splitmix64 mixer this file already uses -- `engine::mix64` names the
    // anonymous-namespace function above: an unnamed namespace's members are visible through their
    // enclosing namespace within the defining translation unit.
    std::uint64_t h = g.hi;
    h ^= g.lo + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    return static_cast<std::size_t>(engine::mix64(h));
}
