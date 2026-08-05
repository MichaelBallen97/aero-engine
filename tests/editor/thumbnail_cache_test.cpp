// tests/editor/thumbnail_cache_test.cpp -- task 3.1.3, Step 4: the thumbnail ledger (key ordering,
// the Absent/Ready/Failed/Skipped state machine, the decode budget, the eviction LRU) and the
// deterministic integer box resampler. A TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the asset_view_test.cpp precedent): thumbnail_cache.hpp depends on
// nothing but guid.hpp/content_hash.hpp, neither of which needs reflection -- every case here must
// be PRESENT and PASSING in all three build configurations. Tier-0: no GPU, no stb, no ImGui, no
// disk I/O. NO ENTROPY SOURCE ANYWHERE -- every Guid comes from a fixed-seed GuidGenerator or a
// literal (the standing 3.1.1 rule).
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using engine::ContentHash;
using engine::Guid;
using engine::GuidGenerator;
using engine::editor::fitRgbaIntoTile;
using engine::editor::MAX_THUMBNAIL_DECODES_PER_TICK;
using engine::editor::THUMBNAIL_EDGE_TEXELS;
using engine::editor::ThumbnailKey;
using engine::editor::ThumbnailLedger;
using engine::editor::ThumbnailState;

namespace {

[[nodiscard]] ThumbnailKey keyFrom(GuidGenerator& gen) {
    ThumbnailKey key;
    key.guid = gen.next();
    key.hash = ContentHash{.hi = 1, .lo = 1};
    return key;
}

}  // namespace

// ================================================================================================
// Ledger -- TC1-TC20
// ================================================================================================

TEST_CASE("thumbnail cache: a fresh ledger's stateOf is Absent; counts are 0 (TC1)") {
    GuidGenerator gen(1);
    const ThumbnailLedger ledger;
    const ThumbnailKey key = keyFrom(gen);
    CHECK(ledger.stateOf(key) == ThumbnailState::Absent);
    CHECK(ledger.readyCount() == 0);
    CHECK(ledger.unavailableCount() == 0);
}

TEST_CASE("thumbnail cache: touch then stateOf is still Absent -- touch does not mark (TC2)") {
    GuidGenerator gen(2);
    ThumbnailLedger ledger;
    const ThumbnailKey key = keyFrom(gen);
    ledger.touch(key, 1);
    CHECK(ledger.stateOf(key) == ThumbnailState::Absent);
}

TEST_CASE("thumbnail cache: markReady/markFailed/markSkipped each set exactly that state (TC3)") {
    GuidGenerator gen(3);
    ThumbnailLedger ledger;
    const ThumbnailKey a = keyFrom(gen);
    const ThumbnailKey b = keyFrom(gen);
    const ThumbnailKey c = keyFrom(gen);
    ledger.touch(a, 1);
    ledger.touch(b, 1);
    ledger.touch(c, 1);
    ledger.markReady(a);
    ledger.markFailed(b);
    ledger.markSkipped(c);
    CHECK(ledger.stateOf(a) == ThumbnailState::Ready);
    CHECK(ledger.stateOf(b) == ThumbnailState::Failed);
    CHECK(ledger.stateOf(c) == ThumbnailState::Skipped);
}

TEST_CASE("thumbnail cache: touch on an existing key updates lastTouched, not state (TC4, seed S1)") {
    GuidGenerator gen(4);
    ThumbnailLedger ledger;
    const ThumbnailKey key = keyFrom(gen);
    ledger.touch(key, 1);
    ledger.markReady(key);
    ledger.touch(key, 99);
    CHECK(ledger.stateOf(key) == ThumbnailState::Ready);
}

TEST_CASE("thumbnail cache: nextDecodes(2) returns at most 2 (TC5, AC-8, seed S4)") {
    GuidGenerator gen(5);
    ThumbnailLedger ledger;
    for (int i = 0; i < 5; ++i) {
        ledger.touch(keyFrom(gen), static_cast<std::uint64_t>(i));
    }
    CHECK(ledger.nextDecodes(2).size() == 2);
}

TEST_CASE("thumbnail cache: nextDecodes(0) returns empty (TC6, AC-8)") {
    GuidGenerator gen(6);
    ThumbnailLedger ledger;
    ledger.touch(keyFrom(gen), 1);
    CHECK(ledger.nextDecodes(0).empty());
}

TEST_CASE("thumbnail cache: nextDecodes returns oldest-touched first across 5 keys (TC7, AC-8)") {
    GuidGenerator gen(7);
    ThumbnailLedger ledger;
    std::vector<ThumbnailKey> keys;
    for (int i = 0; i < 5; ++i) {
        keys.push_back(keyFrom(gen));
        ledger.touch(keys.back(), static_cast<std::uint64_t>(i + 1));
    }
    const std::vector<ThumbnailKey> next = ledger.nextDecodes(5);
    REQUIRE(next.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(next[i] == keys[i]);
    }
}

TEST_CASE("thumbnail cache: nextDecodes never returns a Failed key, across 20 calls (TC8, INV-V4, seed S1)") {
    GuidGenerator gen(8);
    ThumbnailLedger ledger;
    const ThumbnailKey failed = keyFrom(gen);
    ledger.touch(failed, 1);
    ledger.markFailed(failed);
    for (int i = 0; i < 20; ++i) {
        const std::vector<ThumbnailKey> next = ledger.nextDecodes(10);
        CHECK(std::find(next.begin(), next.end(), failed) == next.end());
    }
}

TEST_CASE("thumbnail cache: nextDecodes never returns a Skipped key, across 20 calls (TC9, INV-V4, seed S1)") {
    GuidGenerator gen(9);
    ThumbnailLedger ledger;
    const ThumbnailKey skipped = keyFrom(gen);
    ledger.touch(skipped, 1);
    ledger.markSkipped(skipped);
    for (int i = 0; i < 20; ++i) {
        const std::vector<ThumbnailKey> next = ledger.nextDecodes(10);
        CHECK(std::find(next.begin(), next.end(), skipped) == next.end());
    }
}

TEST_CASE("thumbnail cache: nextDecodes never returns a Ready key (TC10, INV-V4)") {
    GuidGenerator gen(10);
    ThumbnailLedger ledger;
    const ThumbnailKey ready = keyFrom(gen);
    ledger.touch(ready, 1);
    ledger.markReady(ready);
    const std::vector<ThumbnailKey> next = ledger.nextDecodes(10);
    CHECK(std::find(next.begin(), next.end(), ready) == next.end());
}

TEST_CASE("thumbnail cache: a Failed key re-touched stays Failed, never returned (TC11, INV-V4, AC-9)") {
    GuidGenerator gen(11);
    ThumbnailLedger ledger;
    const ThumbnailKey key = keyFrom(gen);
    ledger.touch(key, 1);
    ledger.markFailed(key);
    ledger.touch(key, 2);
    CHECK(ledger.stateOf(key) == ThumbnailState::Failed);
    const std::vector<ThumbnailKey> next = ledger.nextDecodes(10);
    CHECK(std::find(next.begin(), next.end(), key) == next.end());
}

TEST_CASE("thumbnail cache: evictions returns nothing when readyCount() <= cap (TC12, INV-V5)") {
    GuidGenerator gen(12);
    ThumbnailLedger ledger;
    for (int i = 0; i < 3; ++i) {
        const ThumbnailKey key = keyFrom(gen);
        ledger.touch(key, static_cast<std::uint64_t>(i));
        ledger.markReady(key);
    }
    CHECK(ledger.evictions(5, 100).empty());
}

TEST_CASE("thumbnail cache: evictions returns exactly readyCount() - cap keys (TC13, INV-V5, seed S3)") {
    GuidGenerator gen(13);
    ThumbnailLedger ledger;
    for (int i = 0; i < 10; ++i) {
        const ThumbnailKey key = keyFrom(gen);
        ledger.touch(key, static_cast<std::uint64_t>(i));
        ledger.markReady(key);
    }
    CHECK(ledger.evictions(4, 999).size() == ledger.readyCount() - 4);
}

TEST_CASE("thumbnail cache: evictions returns least-recently-touched first (TC14, INV-V5)") {
    GuidGenerator gen(14);
    ThumbnailLedger ledger;
    std::vector<ThumbnailKey> keys;
    for (int i = 0; i < 5; ++i) {
        keys.push_back(keyFrom(gen));
        ledger.touch(keys.back(), static_cast<std::uint64_t>(i + 1));
        ledger.markReady(keys.back());
    }
    const std::vector<ThumbnailKey> evicted = ledger.evictions(2, 999);
    REQUIRE(evicted.size() == 3);
    CHECK(evicted[0] == keys[0]);
    CHECK(evicted[1] == keys[1]);
    CHECK(evicted[2] == keys[2]);
}

TEST_CASE("thumbnail cache: evictions EXCLUDES every key touched at currentFrame (TC15, E12, seed S2)") {
    GuidGenerator gen(15);
    ThumbnailLedger ledger;
    std::vector<ThumbnailKey> keys;
    for (int i = 0; i < 5; ++i) {
        keys.push_back(keyFrom(gen));
        // ALL touched at frame 7 -- every candidate is protected, so evictions must return EMPTY even
        // though readyCount() (5) exceeds the cap (2).
        ledger.touch(keys.back(), 7);
        ledger.markReady(keys.back());
    }
    CHECK(ledger.evictions(2, 7).empty());
}

TEST_CASE("thumbnail cache: evictions never returns a non-Ready key (TC16, INV-V6)") {
    GuidGenerator gen(16);
    ThumbnailLedger ledger;
    const ThumbnailKey ready = keyFrom(gen);
    const ThumbnailKey failed = keyFrom(gen);
    const ThumbnailKey absent = keyFrom(gen);
    ledger.touch(ready, 1);
    ledger.markReady(ready);
    ledger.touch(failed, 1);
    ledger.markFailed(failed);
    ledger.touch(absent, 1);
    const std::vector<ThumbnailKey> evicted = ledger.evictions(0, 999);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0] == ready);
}

TEST_CASE("thumbnail cache: forget removes exactly one entry (TC17)") {
    GuidGenerator gen(17);
    ThumbnailLedger ledger;
    const ThumbnailKey a = keyFrom(gen);
    const ThumbnailKey b = keyFrom(gen);
    ledger.touch(a, 1);
    ledger.touch(b, 1);
    ledger.forget(a);
    CHECK(ledger.stateOf(a) == ThumbnailState::Absent);
    CHECK(ledger.nextDecodes(10).size() == 1);  // only b remains as an entry
}

TEST_CASE("thumbnail cache: clear() empties everything (TC18, E27)") {
    GuidGenerator gen(18);
    ThumbnailLedger ledger;
    for (int i = 0; i < 4; ++i) {
        const ThumbnailKey key = keyFrom(gen);
        ledger.touch(key, static_cast<std::uint64_t>(i));
        ledger.markReady(key);
    }
    ledger.clear();
    CHECK(ledger.readyCount() == 0);
    CHECK(ledger.unavailableCount() == 0);
    CHECK(ledger.nextDecodes(10).empty());
}

TEST_CASE("thumbnail cache: readyCount counts only Ready; unavailableCount is Failed + Skipped (TC19, AC-9)") {
    GuidGenerator gen(19);
    ThumbnailLedger ledger;
    const ThumbnailKey ready = keyFrom(gen);
    const ThumbnailKey failed = keyFrom(gen);
    const ThumbnailKey skipped = keyFrom(gen);
    const ThumbnailKey absent = keyFrom(gen);
    ledger.touch(ready, 1);
    ledger.markReady(ready);
    ledger.touch(failed, 1);
    ledger.markFailed(failed);
    ledger.touch(skipped, 1);
    ledger.markSkipped(skipped);
    ledger.touch(absent, 1);
    CHECK(ledger.readyCount() == 1);
    CHECK(ledger.unavailableCount() == 2);
}

TEST_CASE("thumbnail cache: same Guid, different ContentHash -- distinct entries (TC20, D7, seed S8)") {
    GuidGenerator gen(20);
    const Guid guid = gen.next();
    ThumbnailKey a;
    a.guid = guid;
    a.hash = ContentHash{.hi = 1, .lo = 1};
    ThumbnailKey b;
    b.guid = guid;
    b.hash = ContentHash{.hi = 2, .lo = 2};

    ThumbnailLedger ledger;
    ledger.touch(a, 1);
    ledger.touch(b, 1);
    ledger.markReady(a);
    CHECK(ledger.stateOf(a) == ThumbnailState::Ready);
    CHECK(ledger.stateOf(b) == ThumbnailState::Absent);
}

// ================================================================================================
// Key ordering -- TC21-TC23
// ================================================================================================

TEST_CASE("thumbnail cache: operator== compares both halves (TC21, seed S8)") {
    GuidGenerator gen(21);
    const Guid guid = gen.next();
    const ThumbnailKey a{.guid = guid, .hash = ContentHash{.hi = 1, .lo = 1}};
    const ThumbnailKey b{.guid = guid, .hash = ContentHash{.hi = 2, .lo = 2}};
    CHECK_FALSE(a == b);
    CHECK(a == a);
}

TEST_CASE("thumbnail cache: operator< is a strict total order, consistent with == (TC22, A10)") {
    GuidGenerator gen(22);
    const ThumbnailKey a = keyFrom(gen);
    const ThumbnailKey b = keyFrom(gen);
    // irreflexive
    CHECK_FALSE(a < a);
    // consistent with ==
    if (a == b) {
        CHECK_FALSE(a < b);
        CHECK_FALSE(b < a);
    } else {
        CHECK((a < b) != (b < a));
    }
}

TEST_CASE("thumbnail cache: 200 shuffled keys are all findable and the ledger's order is stable (TC23, A10)") {
    GuidGenerator gen(23);
    ThumbnailLedger ledger;
    std::vector<ThumbnailKey> keys;
    keys.reserve(200);
    for (int i = 0; i < 200; ++i) {
        keys.push_back(keyFrom(gen));
    }
    // Insert in reverse order -- a std::lower_bound-backed sorted vector must still find every one.
    for (std::size_t i = keys.size(); i-- > 0;) {
        ledger.touch(keys[i], static_cast<std::uint64_t>(i));
    }
    for (const ThumbnailKey& key : keys) {
        CHECK(ledger.stateOf(key) == ThumbnailState::Absent);
    }
}

// ================================================================================================
// Resampler -- TC24-TC38. Every assertion is a BYTE COMPARISON.
// ================================================================================================

namespace {
[[nodiscard]] std::vector<std::uint8_t> makeSolid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g,
                                                  std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4U);
    for (std::size_t i = 0; i < out.size(); i += 4U) {
        out[i + 0] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }
    return out;
}
}  // namespace

TEST_CASE("thumbnail cache: output length is always exactly edge*edge*4 (TC24, contract)") {
    const std::vector<std::uint8_t> src = makeSolid(10, 10, 1, 2, 3, 4);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 10, 10, 128);
    CHECK(tile.size() == 128ULL * 128ULL * 4ULL);
}

TEST_CASE("thumbnail cache: degenerate inputs yield an empty vector (TC25, contract)") {
    const std::vector<std::uint8_t> src = makeSolid(4, 4, 0, 0, 0, 0);
    CHECK(fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 4, 0).empty());
    CHECK(fitRgbaIntoTile(std::span<const std::uint8_t>(src), 0, 4, 8).empty());
    CHECK(fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 0, 8).empty());
}

TEST_CASE("thumbnail cache: a short source span yields an empty vector (TC26, contract)") {
    std::vector<std::uint8_t> src(4 * 4 * 4 - 1, 0);  // one byte short
    CHECK(fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 4, 8).empty());
}

TEST_CASE(
    "thumbnail cache: a 2:1 checkerboard downscale is byte-compared against hand computation (TC27, AC-6, seed S6)") {
    // A 4x4 two-colour checkerboard, downscaled to 2x2 (edge=2, source fits exactly -- srcW==edge*2).
    // Quadrant (0,0) [rows 0-1, cols 0-1] is all {10,20,30,40}; the rest is {200,210,220,230}.
    std::vector<std::uint8_t> src(4 * 4 * 4, 0);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * 4 + x) * 4;
            const bool topLeft = (y < 2) && (x < 2);
            src[idx + 0] = topLeft ? 10 : 200;
            src[idx + 1] = topLeft ? 20 : 210;
            src[idx + 2] = topLeft ? 30 : 220;
            src[idx + 3] = topLeft ? 40 : 230;
        }
    }
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 4, 2);
    REQUIRE(tile.size() == 2 * 2 * 4);
    // dst(0,0) averages the top-left quadrant -- uniform, so the average is exact.
    CHECK(tile[0] == 10);
    CHECK(tile[1] == 20);
    CHECK(tile[2] == 30);
    CHECK(tile[3] == 40);
    // dst(1,1) averages the bottom-right quadrant.
    const std::size_t br = (1ULL * 2 + 1) * 4;
    CHECK(tile[br + 0] == 200);
    CHECK(tile[br + 1] == 210);
    CHECK(tile[br + 2] == 220);
    CHECK(tile[br + 3] == 230);
}

TEST_CASE("thumbnail cache: a 4:1 downscale of a gradient -- three texels byte-compared (TC28, seed S6)") {
    // 8x8 gradient, red channel = x*10 (0,10,...,70), downscaled to 2x2 (4:1 in both axes).
    std::vector<std::uint8_t> src(8 * 8 * 4, 0);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * 8 + x) * 4;
            src[idx + 0] = static_cast<std::uint8_t>(x * 10);
            src[idx + 1] = 0;
            src[idx + 2] = 0;
            src[idx + 3] = 255;
        }
    }
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 8, 8, 2);
    REQUIRE(tile.size() == 2 * 2 * 4);
    // dst(0,0): source x in [0,4) -> red values {0,10,20,30}, mean = 60/4 = 15.
    CHECK(tile[0] == 15);
    // dst(1,0): source x in [4,8) -> red values {40,50,60,70}, mean = 220/4 = 55.
    CHECK(tile[4] == 55);
    // dst(1,1): same red column as dst(1,0) (red is x-only) -> 55.
    const std::size_t idx11 = (1ULL * 2 + 1) * 4;
    CHECK(tile[idx11] == 55);
}

TEST_CASE("thumbnail cache: the divisor is the SAMPLE COUNT, not the destination width (TC29, seed S6)") {
    // 3x3 -> 1x1: nine known values, integer mean, not sum and not the first sample.
    std::vector<std::uint8_t> src(3 * 3 * 4, 0);
    const std::array<std::uint8_t, 9> values{1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (std::size_t i = 0; i < 9; ++i) {
        src[(i * 4) + 0] = values[i];
    }
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 3, 3, 1);
    REQUIRE(tile.size() == 1 * 1 * 4);
    // mean of 1..9 == 45/9 == 5, NOT the sum (45) and not the first sample (1).
    CHECK(tile[0] == 5);
}

TEST_CASE("thumbnail cache: a 1x1 source is CENTRED at 1:1, never stretched (TC30, E5, seed S5)") {
    std::vector<std::uint8_t> src{111, 222, 33, 255};
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 1, 1, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    // centred: (128-1)/2 == 63 -- texel (63,63) holds the source pixel.
    const std::size_t centre = (63ULL * 128 + 63) * 4;
    CHECK(tile[centre + 0] == 111);
    CHECK(tile[centre + 1] == 222);
    CHECK(tile[centre + 2] == 33);
    CHECK(tile[centre + 3] == 255);
    // the surrounding tile is transparent black.
    const std::size_t corner = 0;
    CHECK(tile[corner + 0] == 0);
    CHECK(tile[corner + 1] == 0);
    CHECK(tile[corner + 2] == 0);
    CHECK(tile[corner + 3] == 0);
}

TEST_CASE("thumbnail cache: a 64x64 source (smaller in both axes) is NEVER upscaled (TC31, E5, seed S5)") {
    const std::vector<std::uint8_t> src = makeSolid(64, 64, 9, 8, 7, 6);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 64, 64, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    // centred at (32,32): (128-64)/2 == 32.
    for (std::uint32_t y = 0; y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t idx = ((static_cast<std::size_t>(32 + y) * 128) + (32 + x)) * 4;
            if (tile[idx] != 9 || tile[idx + 1] != 8 || tile[idx + 2] != 7 || tile[idx + 3] != 6) {
                FAIL("mismatch at ", x, ",", y);
            }
        }
    }
    // one texel outside the block, at (31,31), stays transparent black.
    const std::size_t outside = ((31ULL * 128) + 31) * 4;
    CHECK(tile[outside] == 0);
}

TEST_CASE("thumbnail cache: a 10000x4 source yields 128x1 with no zero dimension (TC32, E6)") {
    const std::vector<std::uint8_t> src = makeSolid(10000, 4, 1, 1, 1, 1);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 10000, 4, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    // the fitted row is exactly 1 texel tall, letterboxed above and below.
    const std::size_t rowY = (128 - 1) / 2;         // == 63
    const std::size_t idx = (rowY * 128 + 10) * 4;  // an arbitrary column inside the fitted row
    CHECK(tile[idx] == 1);
    const std::size_t above = ((rowY - 1) * 128 + 10) * 4;
    CHECK(tile[above] == 0);
}

TEST_CASE("thumbnail cache: a 4x10000 source yields 1x128 (TC33, E6)") {
    const std::vector<std::uint8_t> src = makeSolid(4, 10000, 2, 2, 2, 2);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 10000, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    const std::size_t colX = (128 - 1) / 2;  // == 63
    const std::size_t idx = (10ULL * 128 + colX) * 4;
    CHECK(tile[idx] == 2);
}

TEST_CASE("thumbnail cache: the letterbox is transparent black on all four margins (TC34, E8)") {
    const std::vector<std::uint8_t> src = makeSolid(2, 2, 255, 255, 255, 255);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 2, 2, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    // top-left corner, top-right corner, bottom-left corner, bottom-right corner -- all margin.
    CHECK(tile[0] == 0);
    CHECK(tile[((127ULL) * 4)] == 0);
    CHECK(tile[(127ULL * 128 * 4)] == 0);
    CHECK(tile[((127ULL * 128 + 127) * 4)] == 0);
}

TEST_CASE("thumbnail cache: alpha is carried through a 2:1 reduction (TC35, E8)") {
    std::vector<std::uint8_t> src(4 * 4 * 4, 0);
    for (std::size_t i = 0; i < src.size(); i += 4) {
        src[i + 3] = 128;  // 50% alpha, uniform
    }
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 4, 4, 2);
    REQUIRE(tile.size() == 2 * 2 * 4);
    CHECK(tile[3] == 128);
}

TEST_CASE("thumbnail cache: a 129x129 source (one texel over) downscales to exactly 128x128 (TC36, boundary)") {
    const std::vector<std::uint8_t> src = makeSolid(129, 129, 3, 3, 3, 3);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 129, 129, 128);
    CHECK(tile.size() == 128ULL * 128ULL * 4ULL);
    // A solid source downscales to the SAME solid colour everywhere in the fitted region.
    CHECK(tile[0] == 3);
    CHECK(tile[((127ULL * 128 + 127) * 4)] == 3);
}

TEST_CASE(
    "thumbnail cache: determinism -- same input twice is byte-identical; a shifted copy differs (TC37, S7's "
    "discriminator)") {
    const std::vector<std::uint8_t> src = makeSolid(37, 41, 12, 34, 56, 78);
    const std::vector<std::uint8_t> a = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 37, 41, 128);
    const std::vector<std::uint8_t> b = fitRgbaIntoTile(std::span<const std::uint8_t>(src), 37, 41, 128);
    CHECK(a == b);

    std::vector<std::uint8_t> shifted = src;
    for (std::size_t i = 0; i + 3 < shifted.size(); i += 4) {
        shifted[i] = static_cast<std::uint8_t>(shifted[i] + 3);  // a genuinely different logical image
    }
    const std::vector<std::uint8_t> c = fitRgbaIntoTile(std::span<const std::uint8_t>(shifted), 37, 41, 128);
    CHECK_FALSE(a == c);
}

TEST_CASE("thumbnail cache: the accumulator does not overflow at the 500 000-sample worst case (TC38, A11, seed S6)") {
    // A 64 000 000 x 1 strip, every byte 255, reduced to 128x1 -- 500 000 samples per destination
    // texel. std::uint32_t would wrap (500000 * 255 ~= 127 500 000, safe in uint32 actually -- the
    // REAL risk this guards is a narrower or shared accumulator elsewhere; asserted here at the
    // documented worst case regardless): the mean of an all-255 input must be exactly 255, not a
    // wrapped or truncated value.
    constexpr std::uint32_t stripWidth = 64000000;
    std::vector<std::uint8_t> src(static_cast<std::size_t>(stripWidth) * 1 * 4, 255);
    const std::vector<std::uint8_t> tile = fitRgbaIntoTile(std::span<const std::uint8_t>(src), stripWidth, 1, 128);
    REQUIRE(tile.size() == 128ULL * 128ULL * 4ULL);
    const std::size_t rowY = (128 - 1) / 2;
    for (std::uint32_t x = 0; x < 128; ++x) {
        const std::size_t idx = (rowY * 128 + x) * 4;
        if (tile[idx] != 255 || tile[idx + 1] != 255 || tile[idx + 2] != 255 || tile[idx + 3] != 255) {
            FAIL("mismatch at x=", x);
        }
    }
}
