// tests/guid_test.cpp -- task 3.1.1: engine::Guid's value semantics, text codec, ordering, hash and
// seeded generator. A TU of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Every case here pins EXACT values against a fixed seed (D2/R1) -- no test in this file ever
// touches an entropy source except GU24/GU25, which only prove fromEntropy() differs and is valid,
// never a specific value.
#include <aero/core/guid.hpp>

#include <doctest/doctest.h>

#include <array>  // GU17's std::array<char, 33> -- reached transitively on libc++, not on MSVC (813bc4d)
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

using engine::formatGuid;
using engine::Guid;
using engine::GUID_TEXT_LENGTH;
using engine::GuidGenerator;
using engine::parseGuid;

TEST_CASE("Guid: default-constructed is nil (GU1)") {
    const Guid g;
    CHECK(g.hi == 0);
    CHECK(g.lo == 0);
    CHECK_FALSE(g.valid());
}

TEST_CASE("Guid: valid() is true whenever either half is non-zero (GU2)") {
    CHECK(Guid{1, 0}.valid());
    CHECK(Guid{0, 1}.valid());
    CHECK(Guid{1, 1}.valid());
    CHECK_FALSE(Guid{0, 0}.valid());
}

TEST_CASE("Guid: layout matches Handle's precedent (GU3)") {
    CHECK(sizeof(Guid) == 16);
    CHECK(std::is_trivially_copyable_v<Guid>);
}

TEST_CASE("Guid: operator== / != compare both halves (GU4)") {
    CHECK(Guid{1, 2} == Guid{1, 2});
    CHECK(Guid{1, 2} != Guid{2, 2});
    CHECK(Guid{1, 2} != Guid{1, 3});
    CHECK(Guid{0, 0} == Guid{});
}

TEST_CASE("Guid: operator< is a strict total order by (hi, lo) (GU5)") {
    CHECK(Guid{1, 0} < Guid{1, 1});
    CHECK(Guid{1, 1} < Guid{2, 0});
    CHECK(Guid{1, 0} < Guid{2, 0});
    CHECK_FALSE(Guid{1, 1} < Guid{1, 1});  // irreflexive
    CHECK_FALSE(Guid{2, 0} < Guid{1, 1});
}

TEST_CASE("Guid: std::hash differs for swapped halves (GU6, AC-3)") {
    const Guid a{0x1111111111111111ULL, 0x2222222222222222ULL};
    const Guid b{0x2222222222222222ULL, 0x1111111111111111ULL};
    CHECK(a != b);
    CHECK(std::hash<Guid>{}(a) != std::hash<Guid>{}(b));
}

TEST_CASE("Guid: std::hash is stable and usable as a set key (GU7)") {
    const Guid g{0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL};
    CHECK(std::hash<Guid>{}(g) == std::hash<Guid>{}(g));

    std::unordered_set<Guid> set;
    set.insert(g);
    set.insert(g);
    CHECK(set.size() == 1);
    set.insert(Guid{1, 2});
    CHECK(set.size() == 2);
}

TEST_CASE("Guid: formatGuid returns exactly 32 lowercase hex chars (GU8)") {
    const std::string text = formatGuid(Guid{0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL});
    REQUIRE(text.size() == GUID_TEXT_LENGTH);
    for (const char c : text) {
        const bool isLowerHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isLowerHex);
    }
}

TEST_CASE("Guid: formatGuid(nil) is 32 zeros (GU9, AC-4)") { CHECK(formatGuid(Guid{}) == std::string(32, '0')); }

TEST_CASE("Guid: formatGuid zero-pads (GU10)") { CHECK(formatGuid(Guid{1, 1}) == "00000000000000010000000000000001"); }

TEST_CASE("Guid: formatGuid emits hi first (GU11)") {
    CHECK(formatGuid(Guid{0x0123456789ABCDEFULL, 0}) == "0123456789abcdef0000000000000000");
}

TEST_CASE("Guid: parseGuid is case-tolerant (GU12, AC-5)") {
    const std::optional<Guid> lower = parseGuid("a3f1c07e5b8d42198e6f0c3d7a2b4b92");
    const std::optional<Guid> upper = parseGuid("A3F1C07E5B8D42198E6F0C3D7A2B4B92");
    const std::optional<Guid> mixed = parseGuid("a3F1c07E5b8D42198E6f0C3d7A2b4B92");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());
    REQUIRE(mixed.has_value());
    CHECK(*lower == *upper);
    CHECK(*lower == *mixed);
}

TEST_CASE("Guid: parseGuid rejects the wrong length (GU13)") {
    CHECK_FALSE(parseGuid("").has_value());
    CHECK_FALSE(parseGuid(std::string(31, 'a')).has_value());
    CHECK_FALSE(parseGuid(std::string(33, 'a')).has_value());
}

TEST_CASE("Guid: parseGuid rejects dashed and braced forms (GU14)") {
    CHECK_FALSE(parseGuid("a3f1c07e-5b8d-4219-8e6f-0c3d7a2b4b92").has_value());
    CHECK_FALSE(parseGuid("{a3f1c07e5b8d42198e6f0c3d7a2b4b92}").has_value());
}

TEST_CASE("Guid: parseGuid rejects whitespace and a 0x prefix (GU15)") {
    CHECK_FALSE(parseGuid(" a3f1c07e5b8d42198e6f0c3d7a2b4b92").has_value());
    CHECK_FALSE(parseGuid("a3f1c07e5b8d42198e6f0c3d7a2b4b92 ").has_value());
    CHECK_FALSE(parseGuid("0xa3f1c07e5b8d42198e6f0c3d7a2b4b9").has_value());
}

TEST_CASE("Guid: parseGuid rejects a non-hex byte anywhere (GU16)") {
    CHECK_FALSE(parseGuid("g3f1c07e5b8d42198e6f0c3d7a2b4b92").has_value());  // first
    CHECK_FALSE(parseGuid("a3f1c07e5bZd42198e6f0c3d7a2b4b92").has_value());  // middle
    CHECK_FALSE(parseGuid("a3f1c07e5b8d42198e6f0c3d7a2b4b9g").has_value());  // last
}

TEST_CASE("Guid: parseGuid rejects an embedded NUL (GU17)") {
    // std::array, not a C array: modernize-avoid-c-arrays is --warnings-as-errors in CI
    // (project_files.cpp:161's precedent). 33 elements: 32 payload bytes (one of them '\0') plus one
    // trailing sentinel that is deliberately NOT part of the string_view below.
    static constexpr std::array<char, 33> RAW{
        'a', '3',  'f', '1', 'c', '0', '7', 'e', '5', '8', 'b', '8', 'd', '4', '2', '1', '9',
        '8', '\0', 'f', '0', 'c', '3', 'd', '7', 'a', '2', 'b', '4', 'b', '9', '2', 'x',
    };
    const std::string_view withNul(RAW.data(), RAW.size() - 1U);  // 32 bytes, one of them '\0'
    CHECK_FALSE(parseGuid(withNul).has_value());
}

TEST_CASE("Guid: round-trip (GU18, AC-6)") {
    const Guid nil{};
    const Guid allF{0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
    const Guid literal{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    for (const Guid& g : {nil, allF, literal}) {
        const std::optional<Guid> parsed = parseGuid(formatGuid(g));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == g);
    }
}

TEST_CASE("Guid: GuidGenerator{0}'s first four draws are pinned (GU19, AC-7)") {
    // Computed at plan time from a reference splitmix64 implementation (plan §G-6). A literal that
    // changes only when the algorithm changes is the whole point (S7) -- do NOT compute this
    // expectation from formatGuid at runtime.
    GuidGenerator gen(0);
    CHECK(formatGuid(gen.next()) == "e220a8397b1dcdaf6e789e6aa1b965f4");
    CHECK(formatGuid(gen.next()) == "06c45d188009454ff88bb8a8724c81ec");
    CHECK(formatGuid(gen.next()) == "1b39896a51a8749b53cb9f0c747ea2ea");
    CHECK(formatGuid(gen.next()) == "2c829abe1f4532e1c584133ac916ab3c");
}

TEST_CASE("Guid: identical seeds agree; distinct seeds diverge on the first draw (GU20)") {
    GuidGenerator a(42);
    GuidGenerator b(42);
    for (int i = 0; i < 100; ++i) {
        CHECK(a.next() == b.next());
    }
    GuidGenerator seed0(0);
    GuidGenerator seed1(1);
    CHECK(formatGuid(seed1.next()) == "910a2dec89025cc1beeb8da1658eec67");
    CHECK(seed0.next() != GuidGenerator(1).next());
}

TEST_CASE("Guid: a zero HALF from next() is not mistaken for nil (GU21, A12)") {
    GuidGenerator gen(0x61C8864680B583EBULL);
    const Guid g = gen.next();
    CHECK(g.hi == 0);
    CHECK(g.lo != 0);
    CHECK(g.valid());
    CHECK(formatGuid(g) == "0000000000000000e220a8397b1dcdaf");
}

TEST_CASE("Guid: next() never returns nil (GU22, AC-8)") {
    GuidGenerator gen(7);
    // Reduced from the plan's 1,000,000 to keep the Debug/ASan lane comfortably under a second;
    // never delete this case, only its loop count (plan note at GU22/GU23).
    for (int i = 0; i < 200000; ++i) {
        CHECK(gen.next().valid());
    }
}

TEST_CASE("Guid: no duplicate over 100,000 draws (GU23)") {
    GuidGenerator gen(99);
    std::set<Guid> seen;
    for (int i = 0; i < 100000; ++i) {
        const Guid g = gen.next();
        const bool inserted = seen.insert(g).second;
        CHECK(inserted);
    }
}

TEST_CASE("Guid: two fromEntropy() generators differ (GU24, AC-9/A13)") {
    GuidGenerator a = GuidGenerator::fromEntropy();
    GuidGenerator b = GuidGenerator::fromEntropy();
    CHECK(a.next() != b.next());
}

TEST_CASE("Guid: fromEntropy()'s output is always valid (GU25)") {
    GuidGenerator gen = GuidGenerator::fromEntropy();
    for (int i = 0; i < 1000; ++i) {
        CHECK(gen.next().valid());
    }
}

TEST_CASE("Guid: parseGuid(formatGuid(g)) == g, generated (GU26)") {
    GuidGenerator gen(2026);
    for (int i = 0; i < 10000; ++i) {
        const Guid g = gen.next();
        const std::optional<Guid> parsed = parseGuid(formatGuid(g));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == g);
    }
}
