#pragma once
// Aero Engine — Guid: the stable, cross-machine asset identity (task 3.1.1). The Handle<Tag>
// precedent (handle.hpp): a small, trivially-copyable value type that every layer above needs and
// that depends on NOTHING. It lives in core, NOT in engine/assets, because engine/scene sits BELOW
// assets in docs/03's layer order and 3.4.x's asset-referencing components must be able to hold one.
//
// This is NOT an RFC 4122 UUID: no version or variant bits are set, so all 128 bits are random. The
// canonical text form is 32 LOWERCASE hex digits, no dashes, high half first -- so lexicographic
// string order equals numeric order. Readers accept any case; writers always emit lowercase (the
// docs/09 §2.4 tolerant-read/canonical-write rule).
//
// NIL (all-zero) is the reserved none sentinel: never generated, never valid in a .meta. That is
// Handle's `generation == 0` and scene v1's `parent: 0`, a third time.
//
// LINKAGE NOTE (plan A14): std::hash<engine::Guid>::operator() is DECLARED here but DEFINED in
// guid.cpp, matching formatGuid/parseGuid and keeping the mixing constants in one TU. So this header
// is not header-only -- anything that hashes a Guid must link aero::core. Every consumer in this
// tree already does (aero_tests, aero_editor_core -> aero::core PUBLIC, and /tools when 3.3 needs
// it) -- but the first reader who wants a Guid in a header-only utility will otherwise find out at
// link time, not at include time.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>  // the two static_asserts below; libstdc++ does NOT supply it transitively

namespace engine {

struct Guid {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return hi != 0 || lo != 0; }
    constexpr bool operator==(const Guid&) const noexcept = default;
    // Total order (hi, then lo) so a Guid is a legal std::map key and sort key. Deliberately NOT
    // <=>-only: an explicit < keeps the ordering readable at the point it matters.
    [[nodiscard]] constexpr bool operator<(const Guid& other) const noexcept {
        return hi != other.hi ? hi < other.hi : lo < other.lo;
    }
};

static_assert(sizeof(Guid) == 16);
static_assert(std::is_trivially_copyable_v<Guid>);

inline constexpr std::size_t GUID_TEXT_LENGTH = 32;

// Exactly GUID_TEXT_LENGTH lowercase hex digits, zero-padded, `hi` first. Nil formats as 32 '0's.
[[nodiscard]] std::string formatGuid(Guid guid);
// EXACTLY 32 hex digits, any case, and nothing else -- no dashes, no braces, no whitespace, no 0x,
// no other length. A dashed value is a parse ERROR, never a silently normalized success (D3).
[[nodiscard]] std::optional<Guid> parseGuid(std::string_view text) noexcept;

// The ONLY source of new GUIDs. Deterministic given its seed -- which is what makes every test in
// this tree pin exact values and touch no entropy source at all (D2). splitmix64: pure unsigned
// 64-bit arithmetic, so it is byte-identical on all three platforms and cannot trip UBSan.
class GuidGenerator {
public:
    explicit constexpr GuidGenerator(std::uint64_t seed) noexcept : state(seed) {}
    // Production seeding. Mixes std::random_device (twice), steady_clock, a stack address and a
    // monotonic call counter, so a DEGENERATE random_device -- MinGW's is a documented constant --
    // cannot produce a fixed stream (AC-9/A13).
    [[nodiscard]] static GuidGenerator fromEntropy();
    // NEVER returns nil: it loops. hi and lo are drawn from two distinct generator states and
    // splitmix64's finaliser is a bijection on std::uint64_t, so a bijection cannot map two distinct
    // inputs to zero -- both halves can never be zero at once, and this loop is provably dead today
    // (plan A12). Kept as defence in depth for a future generator whose halves could share a state or
    // whose mixer is not a bijection; total by construction, not by probability (AC-8).
    [[nodiscard]] Guid next() noexcept;

private:
    std::uint64_t state;
};

}  // namespace engine

template <>
struct std::hash<engine::Guid> {
    // NOT hi ^ lo: that collides for swapped halves, which is exactly the shape a duplicate-GUID
    // test would generate by hand (AC-3). A 64-bit finalizer mix instead. DEFINED in guid.cpp (A14).
    [[nodiscard]] std::size_t operator()(const engine::Guid& g) const noexcept;
};
