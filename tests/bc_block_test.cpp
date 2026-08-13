// tests/bc_block_test.cpp -- task 3.3.2: the BC1 and BC4 integer block encoders. A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. The encoders are PUBLIC rather than src-private precisely so
// this file can drive them directly against per-block byte goldens, which is where their coverage
// belongs: an encoder tested only through a whole-image cook is tested through a filter, a block loop
// and an assembler.
//
// THIS TU OPENS WITH AN INDEPENDENT FIRST-PARTY REFERENCE DECODER, written from the format definition
// and NEVER calling into bc_block.cpp's helpers -- that would be circular, and BB21 exists to prove
// the decoder is not vacuous.
#include <aero/assets/bc_block.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies its operands through the
// stdlib's operator<<, which MS STL declares against an INCOMPLETE std::basic_ostream in headers only
// <ostream> completes. libc++ and libstdc++ are self-sufficient, so omitting it builds clean on macOS
// and Linux and fails only on the Windows lane, inside the STL headers rather than at the CHECK.
#include <ostream>
#include <string>
#include <vector>

using engine::assets::BC1_ERROR_WEIGHT_B;
using engine::assets::BC1_ERROR_WEIGHT_G;
using engine::assets::BC1_ERROR_WEIGHT_R;
using engine::assets::BC1_REFINEMENT_ITERATIONS;
using engine::assets::encodeBc1Block;
using engine::assets::encodeBc4Block;

namespace {

// ---- the independent reference decoder -----------------------------------------------------------
// Written from the format definition: endpoints, palette, packed indices. It shares NO code with the
// encoder, and BB21 pins it against a hand-decoded block so the whole TU cannot pass against a
// decoder that merely echoes its input.

struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

[[nodiscard]] std::uint32_t refU16(std::span<const std::byte, 8> block, std::size_t at) {
    return static_cast<std::uint32_t>(block[at]) | (static_cast<std::uint32_t>(block[at + 1]) << 8U);
}

[[nodiscard]] Rgb8 refDequantize565(std::uint32_t packed) {
    const auto r5 = static_cast<std::int32_t>((packed >> 11U) & 0x1FU);
    const auto g6 = static_cast<std::int32_t>((packed >> 5U) & 0x3FU);
    const auto b5 = static_cast<std::int32_t>(packed & 0x1FU);
    return Rgb8{static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)), static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
                static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2))};
}

// 16 decoded texels, row-major, RGB only -- BC1_RGB carries no alpha.
[[nodiscard]] std::array<Rgb8, 16> decodeBc1Block(std::span<const std::byte, 8> block) {
    const std::uint32_t c0 = refU16(block, 0);
    const std::uint32_t c1 = refU16(block, 2);
    const std::uint32_t indices = refU16(block, 4) | (refU16(block, 6) << 16U);
    std::array<Rgb8, 4> palette{};
    palette[0] = refDequantize565(c0);
    palette[1] = refDequantize565(c1);
    const auto lerp = [](std::uint8_t a, std::uint8_t b, bool fourColour, bool twoThirds) -> std::uint8_t {
        const std::int32_t ia = a;
        const std::int32_t ib = b;
        if (!fourColour) {
            return static_cast<std::uint8_t>(twoThirds ? 0 : (ia + ib + 1) / 2);
        }
        return static_cast<std::uint8_t>(twoThirds ? (ia + 2 * ib + 1) / 3 : (2 * ia + ib + 1) / 3);
    };
    const bool fourColour = c0 > c1;
    palette[2] =
        Rgb8{lerp(palette[0].r, palette[1].r, fourColour, false), lerp(palette[0].g, palette[1].g, fourColour, false),
             lerp(palette[0].b, palette[1].b, fourColour, false)};
    palette[3] =
        Rgb8{lerp(palette[0].r, palette[1].r, fourColour, true), lerp(palette[0].g, palette[1].g, fourColour, true),
             lerp(palette[0].b, palette[1].b, fourColour, true)};
    std::array<Rgb8, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = palette[(indices >> (2U * static_cast<std::uint32_t>(i))) & 3U];
    }
    return out;
}

[[nodiscard]] std::array<std::int32_t, 8> refBc4Palette(std::int32_t r0, std::int32_t r1) {
    std::array<std::int32_t, 8> palette{};
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (std::int32_t k = 1; k <= 6; ++k) {
            palette[static_cast<std::size_t>(k) + 1] = ((7 - k) * r0 + k * r1 + 3) / 7;
        }
    } else {
        for (std::int32_t k = 1; k <= 4; ++k) {
            palette[static_cast<std::size_t>(k) + 1] = ((5 - k) * r0 + k * r1 + 2) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

[[nodiscard]] std::array<std::uint8_t, 16> decodeBc4Block(std::span<const std::byte, 8> block) {
    const auto r0 = static_cast<std::int32_t>(block[0]);
    const auto r1 = static_cast<std::int32_t>(block[1]);
    std::uint64_t field = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        field |= static_cast<std::uint64_t>(block[2 + i]) << (8U * static_cast<std::uint64_t>(i));
    }
    const std::array<std::int32_t, 8> palette = refBc4Palette(r0, r1);
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto index = static_cast<std::size_t>((field >> (3U * static_cast<std::uint64_t>(i))) & 7U);
        out[i] = static_cast<std::uint8_t>(palette[index]);
    }
    return out;
}

// ---- helpers -------------------------------------------------------------------------------------

[[nodiscard]] std::array<std::uint8_t, 8> encode1(const std::array<std::uint8_t, 64>& src) {
    std::array<std::byte, 8> block{};
    encodeBc1Block(std::span<const std::uint8_t, 64>(src), std::span<std::byte, 8>(block));
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(block[i]);
    }
    return bytes;
}

[[nodiscard]] std::array<std::uint8_t, 8> encode4(const std::array<std::uint8_t, 16>& src) {
    std::array<std::byte, 8> block{};
    encodeBc4Block(std::span<const std::uint8_t, 16>(src), std::span<std::byte, 8>(block));
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(block[i]);
    }
    return bytes;
}

[[nodiscard]] std::array<Rgb8, 16> decode1(const std::array<std::uint8_t, 8>& bytes) {
    std::array<std::byte, 8> block{};
    for (std::size_t i = 0; i < 8; ++i) {
        block[i] = static_cast<std::byte>(bytes[i]);
    }
    return decodeBc1Block(std::span<const std::byte, 8>(block));
}

[[nodiscard]] std::array<std::uint8_t, 16> decode4(const std::array<std::uint8_t, 8>& bytes) {
    std::array<std::byte, 8> block{};
    for (std::size_t i = 0; i < 8; ++i) {
        block[i] = static_cast<std::byte>(bytes[i]);
    }
    return decodeBc4Block(std::span<const std::byte, 8>(block));
}

// A 4x4 block of one repeated opaque colour.
[[nodiscard]] std::array<std::uint8_t, 64> flatRgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::array<std::uint8_t, 64> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        src[4 * i + 0] = r;
        src[4 * i + 1] = g;
        src[4 * i + 2] = b;
        src[4 * i + 3] = 255;
    }
    return src;
}

// The 64-block pseudorandom corpus BB5, BB11, BB14, BB20 and BB22 walk. splitmix64: pure unsigned
// arithmetic, so it is byte-identical on all three lanes and cannot trip UBSan -- the guid.hpp
// generator's argument, reused.
class Corpus {
public:
    [[nodiscard]] std::array<std::uint8_t, 64> nextRgba() {
        std::array<std::uint8_t, 64> src{};
        for (std::size_t i = 0; i < 16; ++i) {
            src[4 * i + 0] = nextByte();
            src[4 * i + 1] = nextByte();
            src[4 * i + 2] = nextByte();
            src[4 * i + 3] = 255;
        }
        return src;
    }
    [[nodiscard]] std::array<std::uint8_t, 16> nextSingle() {
        std::array<std::uint8_t, 16> src{};
        for (std::uint8_t& value : src) {
            value = nextByte();
        }
        return src;
    }

private:
    [[nodiscard]] std::uint8_t nextByte() {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return static_cast<std::uint8_t>((z ^ (z >> 31U)) & 0xFFU);
    }
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
};

constexpr std::size_t CORPUS_BLOCKS = 64;

void checkBytes(const std::array<std::uint8_t, 8>& actual, const std::array<std::uint8_t, 8>& expected) {
    std::size_t compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        INFO("byte ", i);
        CHECK(actual[i] == expected[i]);
        ++compared;
    }
    REQUIRE(compared == 8);
}

}  // namespace

// =================================================================================================
// BC1
// =================================================================================================

TEST_CASE("a flat BC1 block takes the degenerate arm and reproduces its colour (BB1)") {
    // Input: 16 x (90, 140, 210). Equal quantized endpoints put the block in three-colour mode, where
    // index 0 still decodes to exactly c0, so all-zero indices are exact for the mode.
    const std::array<std::uint8_t, 8> bytes = encode1(flatRgba(90, 140, 210));
    checkBytes(bytes, {0x7A, 0x5C, 0x7A, 0x5C, 0x00, 0x00, 0x00, 0x00});
    const std::uint32_t c0 = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U);
    const std::uint32_t c1 = static_cast<std::uint32_t>(bytes[2]) | (static_cast<std::uint32_t>(bytes[3]) << 8U);
    CHECK(c0 == c1);
    for (std::size_t i = 4; i < 8; ++i) {
        CHECK(bytes[i] == 0);
    }
    // Every decoded texel is the SAME colour, and it is the 565 round trip of the input -- exact in
    // red (90 -> 5 bits -> 90), two low in green and four low in blue, which is 565's own resolution
    // and not the encoder's error.
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::size_t checked = 0;
    for (const Rgb8& texel : decoded) {
        CHECK(texel.r == 90);
        CHECK(texel.g == 142);
        CHECK(texel.b == 214);
        ++checked;
    }
    REQUIRE(checked == 16);
}

TEST_CASE("a two-colour BC1 block matches its frozen golden (BB2)") {
    // Input: rows 0-1 are (200, 30, 30), rows 2-3 are (30, 30, 200). The two colours sit on a
    // DIAGONAL of the bounding box, so neither box corner is a source colour -- which is exactly the
    // case the least-squares refit exists for.
    std::array<std::uint8_t, 64> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        const bool top = i < 8;
        src[4 * i + 0] = top ? 200 : 30;
        src[4 * i + 1] = 30;
        src[4 * i + 2] = top ? 30 : 200;
        src[4 * i + 3] = 255;
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    checkBytes(bytes, {0xE0, 0xF8, 0xF8, 0x20, 0xAA, 0xAA, 0x55, 0x55});
    // Cross-checked through the independent decoder rather than trusted: the two halves must decode
    // to two DIFFERENT colours, each nearer its own source than the other's.
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    CHECK(decoded[0].r > decoded[15].r);
    CHECK(decoded[0].b < decoded[15].b);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const bool top = i < 8;
        CHECK((decoded[i].r == decoded[top ? 0 : 15].r));
        ++checked;
    }
    REQUIRE(checked == 16);
}

TEST_CASE("a horizontal BC1 gradient matches its frozen golden and is EXACT (BB3)") {
    // Input: grey levels 0, 85, 170, 255 across x, constant down y. Those four values are exactly the
    // BC1 palette of the endpoints (255,255,255) and (0,0,0), so the block round-trips bit for bit
    // and a golden that is off by one index is visible as a wrong grey rather than as a small error.
    std::array<std::uint8_t, 64> src{};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t i = 4 * y + x;
            const auto value = static_cast<std::uint8_t>(x * 85);
            src[4 * i + 0] = value;
            src[4 * i + 1] = value;
            src[4 * i + 2] = value;
            src[4 * i + 3] = 255;
        }
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    checkBytes(bytes, {0xFF, 0xFF, 0x00, 0x00, 0x2D, 0x2D, 0x2D, 0x2D});
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(decoded[i].r == src[4 * i + 0]);
        CHECK(decoded[i].g == src[4 * i + 1]);
        CHECK(decoded[i].b == src[4 * i + 2]);
        ++exact;
    }
    REQUIRE(exact == 16);
}

TEST_CASE("a vertical BC1 gradient gives a DIFFERENT golden from the horizontal one (BB4)") {
    // The same four grey levels, down y instead of across x. A transposed index packing would make
    // this block's bytes equal BB3's, and this case is what says so.
    std::array<std::uint8_t, 64> src{};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t i = 4 * y + x;
            const auto value = static_cast<std::uint8_t>(y * 85);
            src[4 * i + 0] = value;
            src[4 * i + 1] = value;
            src[4 * i + 2] = value;
            src[4 * i + 3] = 255;
        }
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    checkBytes(bytes, {0xFF, 0xFF, 0x00, 0x00, 0x55, 0xFF, 0xAA, 0x00});
    // The endpoints are the SAME as BB3's and the index bytes are NOT -- which is the whole point.
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[1] == 0xFF);
    CHECK(bytes[2] == 0x00);
    CHECK(bytes[3] == 0x00);
    const std::array<std::uint8_t, 4> horizontalIndices = {0x2D, 0x2D, 0x2D, 0x2D};
    bool anyIndexByteDiffers = false;
    for (std::size_t i = 0; i < 4; ++i) {
        anyIndexByteDiffers = anyIndexByteDiffers || bytes[4 + i] != horizontalIndices[i];
    }
    CHECK(anyIndexByteDiffers);
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(decoded[i].r == src[4 * i + 0]);
        ++exact;
    }
    REQUIRE(exact == 16);
}

TEST_CASE("every emitted BC1 block is four-colour unless its endpoints coincide (BB5)") {
    Corpus corpus;
    std::size_t blocks = 0;
    std::size_t degenerate = 0;
    for (std::size_t n = 0; n < CORPUS_BLOCKS; ++n) {
        const std::array<std::uint8_t, 8> bytes = encode1(corpus.nextRgba());
        const std::uint32_t c0 = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U);
        const std::uint32_t c1 = static_cast<std::uint32_t>(bytes[2]) | (static_cast<std::uint32_t>(bytes[3]) << 8U);
        CHECK(c0 >= c1);
        if (c0 == c1) {
            ++degenerate;
            for (std::size_t i = 4; i < 8; ++i) {
                CHECK(bytes[i] == 0);  // the degenerate arm always emits all-zero indices
            }
        }
        ++blocks;
    }
    REQUIRE(blocks == CORPUS_BLOCKS);
    // A random 4x4 of independent colours essentially never quantizes to a single 565 value, so the
    // corpus must exercise the FOUR-COLOUR path throughout. Asserted rather than assumed: a corpus
    // that had gone all-degenerate would make BB5's `c0 >= c1` vacuous.
    CHECK(degenerate == 0);
    // And the flat block still takes the other arm, so both are covered.
    const std::array<std::uint8_t, 8> flat = encode1(flatRgba(17, 17, 17));
    CHECK(flat[0] == flat[2]);
    CHECK(flat[1] == flat[3]);
}

TEST_CASE("the endpoint swap remaps indices so the decoded colours are unchanged (BB6)") {
    // The bounding box's LOW corner is quantized FIRST, so before the ordering step every
    // non-degenerate block has c0 < c1 and the swap fires on all of them -- a rule with no case that
    // can see it violated is a rule with no cover. What the swap must preserve is the DECODE: it maps
    // palette 0 to 1 and 2 to 3 and back, so `index ^ 1` is exact.
    //
    // BB3's block is where that is checkable by hand: the emitted c0 is 0xFFFF (white), which is the
    // bounding box's HIGH corner, so the swap definitely happened -- and every texel still decodes to
    // exactly its own source grey.
    std::array<std::uint8_t, 64> src{};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t i = 4 * y + x;
            const auto value = static_cast<std::uint8_t>(x * 85);
            src[4 * i + 0] = value;
            src[4 * i + 1] = value;
            src[4 * i + 2] = value;
            src[4 * i + 3] = 255;
        }
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    const std::uint32_t c0 = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U);
    const std::uint32_t c1 = static_cast<std::uint32_t>(bytes[2]) | (static_cast<std::uint32_t>(bytes[3]) << 8U);
    CHECK(c0 == 0xFFFF);  // the HIGH corner, which the encoder quantized SECOND
    CHECK(c1 == 0x0000);
    CHECK(c0 > c1);
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(decoded[i].r == src[4 * i + 0]);
        ++exact;
    }
    REQUIRE(exact == 16);
    // Without the index remap, texel 0 (source 0) would decode through the OTHER endpoint, 255.
    CHECK(decoded[0].r != 255);
}

TEST_CASE("BC1 index bits sit at 2*(4*y+x) of the little-endian word at byte 4 (BB7)") {
    // Grey levels {0, 85, 170, 255} -- exactly the palette of the (255,255,255)/(0,0,0) endpoints --
    // laid out at (x + 2*y) % 4, which is asymmetric under BOTH transposition and reversal. Every
    // texel therefore decodes EXACTLY to its own source, and a transposed, reversed or bit-shifted
    // packing moves at least one of them.
    constexpr std::array<std::uint8_t, 4> LEVELS = {0, 85, 170, 255};
    std::array<std::uint8_t, 64> src{};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t i = 4 * y + x;
            const std::uint8_t value = LEVELS[(x + 2 * y) % 4];
            src[4 * i + 0] = value;
            src[4 * i + 1] = value;
            src[4 * i + 2] = value;
            src[4 * i + 3] = 255;
        }
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    checkBytes(bytes, {0xFF, 0xFF, 0x00, 0x00, 0x2D, 0xD2, 0x2D, 0xD2});

    const std::uint32_t word = static_cast<std::uint32_t>(bytes[4]) | (static_cast<std::uint32_t>(bytes[5]) << 8U) |
                               (static_cast<std::uint32_t>(bytes[6]) << 16U) |
                               (static_cast<std::uint32_t>(bytes[7]) << 24U);
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::array<std::uint32_t, 16> indices{};
    std::size_t exact = 0;
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const std::size_t i = 4 * y + x;
            indices[i] = (word >> (2U * static_cast<std::uint32_t>(i))) & 3U;
            INFO("texel x=", x, " y=", y);
            CHECK(decoded[i].r == src[4 * i + 0]);
            ++exact;
        }
    }
    REQUIRE(exact == 16);

    // ANTI-VACUITY: the pattern must actually be able to SEE a transposition and a reversal.
    bool differsUnderTranspose = false;
    bool differsUnderReversal = false;
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            differsUnderTranspose = differsUnderTranspose || indices[4 * y + x] != indices[4 * x + y];
        }
    }
    for (std::size_t i = 0; i < 16; ++i) {
        differsUnderReversal = differsUnderReversal || indices[i] != indices[15 - i];
    }
    CHECK(differsUnderTranspose);
    CHECK(differsUnderReversal);
}

TEST_CASE("the frozen 3/6/1 error weights change the output bytes (BB8)") {
    CHECK(BC1_ERROR_WEIGHT_R == 3);
    CHECK(BC1_ERROR_WEIGHT_G == 6);
    CHECK(BC1_ERROR_WEIGHT_B == 1);
    // BB2's diagonal two-colour block, whose golden this case re-pins. MEASURED by rebuilding with
    // the weights seeded to 1/1/1: that produces
    //     F8 C0 E4 20 FF FF FF FF
    // instead, whose sum of squared error against the source is 254 592 rather than 13 424. So the
    // weights are not decorative, they are better, and this case can see them change.
    std::array<std::uint8_t, 64> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        const bool top = i < 8;
        src[4 * i + 0] = top ? 200 : 30;
        src[4 * i + 1] = 30;
        src[4 * i + 2] = top ? 30 : 200;
        src[4 * i + 3] = 255;
    }
    const std::array<std::uint8_t, 8> bytes = encode1(src);
    checkBytes(bytes, {0xE0, 0xF8, 0xF8, 0x20, 0xAA, 0xAA, 0x55, 0x55});
    constexpr std::array<std::uint8_t, 8> UNWEIGHTED = {0xF8, 0xC0, 0xE4, 0x20, 0xFF, 0xFF, 0xFF, 0xFF};
    bool differs = false;
    for (std::size_t i = 0; i < 8; ++i) {
        differs = differs || bytes[i] != UNWEIGHTED[i];
    }
    CHECK(differs);
}

TEST_CASE("EXACTLY two refinement iterations, and one or three would differ (BB9)") {
    CHECK(BC1_REFINEMENT_ITERATIONS == 2);
    // A noisy block from the corpus, written out as literals so this case stands alone. MEASURED by
    // rebuilding with BC1_REFINEMENT_ITERATIONS seeded to 1 and to 3:
    //     1 iteration   F7 9F 46 38 23 ED AD F8   (sum of squared error 218 584)
    //     2 iterations  EF 77 8E 60 23 CD AD D8   (179 854)  <- the frozen answer
    //     3 iterations  8E 7F EE 58 23 CD AD D8   (178 073)
    // All three are distinct, so the count is a real output-byte decision. It is FIXED rather than
    // error-driven because an "iterate until the error stops improving" rule would be a second
    // determinism surface -- one whose behaviour depends on an epsilon -- for a third of a percent.
    constexpr std::array<std::uint8_t, 64> SRC = {
        244, 79,  236, 255, 155, 234, 225, 255, 60,  195, 166, 255, 9,  246, 123, 255, 47,  25,  171, 255, 85, 134,
        100, 255, 172, 215, 13,  255, 106, 76,  105, 255, 123, 0,   16, 255, 22,  114, 79,  255, 37,  176, 41, 255,
        247, 195, 131, 255, 161, 223, 41,  255, 42,  206, 168, 255, 55, 56,  55,  255, 173, 73,  225, 255,
    };
    const std::array<std::uint8_t, 8> bytes = encode1(SRC);
    checkBytes(bytes, {0xEF, 0x77, 0x8E, 0x60, 0x23, 0xCD, 0xAD, 0xD8});
    constexpr std::array<std::uint8_t, 8> ONE_ITERATION = {0xF7, 0x9F, 0x46, 0x38, 0x23, 0xED, 0xAD, 0xF8};
    constexpr std::array<std::uint8_t, 8> THREE_ITERATIONS = {0x8E, 0x7F, 0xEE, 0x58, 0x23, 0xCD, 0xAD, 0xD8};
    bool differsFromOne = false;
    bool differsFromThree = false;
    for (std::size_t i = 0; i < 8; ++i) {
        differsFromOne = differsFromOne || bytes[i] != ONE_ITERATION[i];
        differsFromThree = differsFromThree || bytes[i] != THREE_ITERATIONS[i];
    }
    CHECK(differsFromOne);
    CHECK(differsFromThree);
}

TEST_CASE("encodeBc1Block ignores alpha entirely (BB10)") {
    // VK_FORMAT_BC1_RGB_* carries no alpha, and this is the case that says the encoder never lets it
    // reach the output. Two inputs differing ONLY in alpha, one of them fully transparent.
    Corpus corpus;
    std::size_t blocks = 0;
    for (std::size_t n = 0; n < 8; ++n) {
        const std::array<std::uint8_t, 64> opaque = corpus.nextRgba();
        std::array<std::uint8_t, 64> transparent = opaque;
        for (std::size_t i = 0; i < 16; ++i) {
            transparent[4 * i + 3] = static_cast<std::uint8_t>(i * 3);
        }
        checkBytes(encode1(transparent), encode1(opaque));
        ++blocks;
    }
    REQUIRE(blocks == 8);
}

TEST_CASE("BC1 round-trip peak error over the corpus, reported as a NUMBER (BB11)") {
    // The bound this case states is a RECORD, not a target: BC1's 5:6:5 endpoints plus two
    // interpolants cannot represent 16 independent random colours, so a large peak here is the
    // format's, not the encoder's. The point is that the number is printed and pinned, so a
    // regression that doubles it is visible.
    Corpus corpus;
    std::int32_t peak = 0;
    std::int64_t totalSquaredError = 0;
    std::size_t blocks = 0;
    for (std::size_t n = 0; n < CORPUS_BLOCKS; ++n) {
        const std::array<std::uint8_t, 64> src = corpus.nextRgba();
        const std::array<Rgb8, 16> decoded = decode1(encode1(src));
        for (std::size_t i = 0; i < 16; ++i) {
            const std::array<std::int32_t, 3> difference = {
                decoded[i].r - src[4 * i + 0], decoded[i].g - src[4 * i + 1], decoded[i].b - src[4 * i + 2]};
            for (const std::int32_t d : difference) {
                const std::int32_t magnitude = d < 0 ? -d : d;
                peak = magnitude > peak ? magnitude : peak;
                totalSquaredError += static_cast<std::int64_t>(d) * d;
            }
        }
        ++blocks;
    }
    REQUIRE(blocks == CORPUS_BLOCKS);
    MESSAGE("BC1 over " << CORPUS_BLOCKS << " random blocks: peak per-channel error " << peak
                        << ", total squared error " << totalSquaredError);
    // MEASURED at 179 (peak) and 9 714 098 (total) on this corpus. The bound below is a RECORD with
    // headroom, not a target: 64 blocks of 16 INDEPENDENT random colours is the pathological case for
    // a four-entry palette on a line. Moving it is a deliberate act, and R2's PSNR comparison against
    // stb_dxt on real images -- not this number -- is what says whether the encoder is good enough.
    CHECK(peak <= 200);
    CHECK(totalSquaredError > 0);  // anti-vacuity: the corpus really was encoded
}

// =================================================================================================
// BC4
// =================================================================================================

TEST_CASE("a flat BC4 block is exact and takes the degenerate arm (BB12)") {
    std::array<std::uint8_t, 16> src{};
    src.fill(137);
    const std::array<std::uint8_t, 8> bytes = encode4(src);
    checkBytes(bytes, {0x89, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    CHECK(bytes[0] == bytes[1]);  // r0 == r1 selects six-value mode, where index 0 is still exactly r0
    const std::array<std::uint8_t, 16> decoded = decode4(bytes);
    std::size_t exact = 0;
    for (const std::uint8_t value : decoded) {
        CHECK(value == 137);
        ++exact;
    }
    REQUIRE(exact == 16);
}

TEST_CASE("a BC4 ramp matches its frozen golden (BB13)") {
    // Input: 0, 17, 34, ... 255. Sixteen source levels onto eight palette entries, so the decode is a
    // staircase and the peak error is the step, 17.
    std::array<std::uint8_t, 16> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        src[i] = static_cast<std::uint8_t>(i * 17);
    }
    const std::array<std::uint8_t, 8> bytes = encode4(src);
    checkBytes(bytes, {0xFF, 0x00, 0xC9, 0x6F, 0xB7, 0xE4, 0x26, 0x01});
    const std::array<std::uint8_t, 16> decoded = decode4(bytes);
    constexpr std::array<std::uint8_t, 16> EXPECTED = {0,   0,   36,  36,  73,  73,  109, 109,
                                                       146, 146, 182, 182, 219, 219, 255, 255};
    std::size_t checked = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(decoded[i] == EXPECTED[i]);
        ++checked;
    }
    REQUIRE(checked == 16);
}

TEST_CASE("BC4 stores r0 == max and r1 == min, over the whole corpus (BB14)") {
    // A reversed pair would put the block in six-value mode and change every interpolant, so this is
    // the case a swapped max/min reddens.
    Corpus corpus;
    std::size_t blocks = 0;
    std::size_t eightValue = 0;
    for (std::size_t n = 0; n < CORPUS_BLOCKS; ++n) {
        const std::array<std::uint8_t, 16> src = corpus.nextSingle();
        std::uint8_t maximum = src[0];
        std::uint8_t minimum = src[0];
        for (const std::uint8_t value : src) {
            maximum = value > maximum ? value : maximum;
            minimum = value < minimum ? value : minimum;
        }
        const std::array<std::uint8_t, 8> bytes = encode4(src);
        CHECK(bytes[0] == maximum);
        CHECK(bytes[1] == minimum);
        if (bytes[0] > bytes[1]) {
            ++eightValue;
        }
        ++blocks;
    }
    REQUIRE(blocks == CORPUS_BLOCKS);
    CHECK(eightValue == CORPUS_BLOCKS);  // anti-vacuity: the corpus really does exercise eight-value
}

TEST_CASE("BC4's eight-value palette is ((7-k)*r0 + k*r1 + 3)/7 (BB15)") {
    // Checked against the reference decoder, which builds the palette from the format definition
    // rather than by calling into the encoder. Driven by a block whose 16 values ARE the eight
    // palette entries, so every entry is reached.
    constexpr std::array<std::int32_t, 8> EXPECTED = {255, 0, 219, 182, 146, 109, 73, 36};
    std::size_t checked = 0;
    for (std::int32_t k = 1; k <= 6; ++k) {
        CHECK(EXPECTED[static_cast<std::size_t>(k) + 1] == ((7 - k) * 255 + k * 0 + 3) / 7);
        ++checked;
    }
    REQUIRE(checked == 6);

    std::array<std::uint8_t, 16> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        src[i] = static_cast<std::uint8_t>(EXPECTED[i % 8]);
    }
    const std::array<std::uint8_t, 16> decoded = decode4(encode4(src));
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(decoded[i] == src[i]);
        ++exact;
    }
    REQUIRE(exact == 16);
}

TEST_CASE("BC4 index bits sit at 3*(4*y+x) of the 48-bit little-endian field at byte 2 (BB16)") {
    // Sixteen values drawn from the eight palette entries at (3*i) % 8 -- asymmetric under both
    // transposition and reversal, and every value lands exactly on a palette entry, so a correct
    // packing decodes bit for bit.
    constexpr std::array<std::uint8_t, 8> PALETTE = {255, 0, 219, 182, 146, 109, 73, 36};
    std::array<std::uint8_t, 16> src{};
    for (std::size_t i = 0; i < 16; ++i) {
        src[i] = PALETTE[(3 * i) % 8];
    }
    const std::array<std::uint8_t, 8> bytes = encode4(src);
    checkBytes(bytes, {0xFF, 0x00, 0x98, 0xC3, 0xAB, 0x98, 0xC3, 0xAB});

    std::uint64_t field = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        field |= static_cast<std::uint64_t>(bytes[2 + i]) << (8U * static_cast<std::uint64_t>(i));
    }
    CHECK((field >> 48U) == 0);  // 16 texels x 3 bits == 48; nothing may spill past the block

    const std::array<std::uint8_t, 16> decoded = decode4(bytes);
    std::array<std::uint32_t, 16> indices{};
    std::size_t exact = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        indices[i] = static_cast<std::uint32_t>((field >> (3U * static_cast<std::uint64_t>(i))) & 7U);
        CHECK(decoded[i] == src[i]);
        ++exact;
    }
    REQUIRE(exact == 16);

    bool differsUnderTranspose = false;
    bool differsUnderReversal = false;
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            differsUnderTranspose = differsUnderTranspose || indices[4 * y + x] != indices[4 * x + y];
        }
    }
    for (std::size_t i = 0; i < 16; ++i) {
        differsUnderReversal = differsUnderReversal || indices[i] != indices[15 - i];
    }
    CHECK(differsUnderTranspose);
    CHECK(differsUnderReversal);
}

TEST_CASE("a BC4 value exactly between two palette entries takes the LOWER index (BB17)") {
    // r0 = 254, r1 = 0 gives the palette 254, 0, 218, 181, 145, 109, 73, 36. The value 127 is 18 from
    // BOTH 145 (index 4) and 109 (index 5) -- an EXACT tie, constructed rather than stumbled on. The
    // lower index wins, so it must decode to 145 and never to 109. This is an output-byte decision.
    std::array<std::uint8_t, 16> src{};
    src[1] = 127;
    src[15] = 254;
    const std::array<std::uint8_t, 8> bytes = encode4(src);
    CHECK(bytes[0] == 254);
    CHECK(bytes[1] == 0);
    const std::array<std::int32_t, 8> palette = refBc4Palette(254, 0);
    CHECK(palette[4] == 145);
    CHECK(palette[5] == 109);
    CHECK((145 - 127) == (127 - 109));  // the tie, asserted rather than asserted-about
    const std::array<std::uint8_t, 16> decoded = decode4(bytes);
    CHECK(decoded[1] == 145);
    CHECK(decoded[1] != 109);
}

TEST_CASE("BC4 reproduces a constant exactly, at every value (BB18)") {
    // The property TX45 leans on when it cooks a fully transparent image to BC3: the alpha block must
    // come back bit-exact, not merely close.
    std::size_t checked = 0;
    for (std::uint32_t value = 0; value <= 255; value += 17) {
        std::array<std::uint8_t, 16> src{};
        src.fill(static_cast<std::uint8_t>(value));
        const std::array<std::uint8_t, 16> decoded = decode4(encode4(src));
        for (const std::uint8_t decodedValue : decoded) {
            CHECK(decodedValue == value);
        }
        ++checked;
    }
    REQUIRE(checked == 16);
}

TEST_CASE("both encoders write EXACTLY eight bytes and touch nothing else (BB19)") {
    // Driven with a ten-byte buffer whose outer bytes are poisoned. The fixed-extent span parameters
    // make a wrongly-sized call a compile error, but they do not stop an off-by-one INSIDE the
    // function -- a putU64 at BC4's byte 2 would write two bytes past the block, which is exactly the
    // mistake the encoder's own comment warns against.
    std::array<std::byte, 10> buffer{};
    buffer.fill(std::byte{0x5A});
    const std::array<std::uint8_t, 64> rgba = flatRgba(12, 200, 77);
    encodeBc1Block(std::span<const std::uint8_t, 64>(rgba), std::span<std::byte, 8>(buffer.data() + 1, 8));
    CHECK(buffer[0] == std::byte{0x5A});
    CHECK(buffer[9] == std::byte{0x5A});

    buffer.fill(std::byte{0x5A});
    std::array<std::uint8_t, 16> single{};
    for (std::size_t i = 0; i < 16; ++i) {
        single[i] = static_cast<std::uint8_t>(255 - 15 * i);
    }
    encodeBc4Block(std::span<const std::uint8_t, 16>(single), std::span<std::byte, 8>(buffer.data() + 1, 8));
    CHECK(buffer[0] == std::byte{0x5A});
    CHECK(buffer[9] == std::byte{0x5A});
    // ANTI-VACUITY: the eight bytes in between really were written.
    bool anyWritten = false;
    for (std::size_t i = 1; i < 9; ++i) {
        anyWritten = anyWritten || buffer[i] != std::byte{0x5A};
    }
    CHECK(anyWritten);
}

TEST_CASE("both encoders are deterministic across repeated calls (BB20)") {
    Corpus corpus;
    std::size_t blocks = 0;
    for (std::size_t n = 0; n < CORPUS_BLOCKS; ++n) {
        const std::array<std::uint8_t, 64> rgba = corpus.nextRgba();
        checkBytes(encode1(rgba), encode1(rgba));
        const std::array<std::uint8_t, 16> single = corpus.nextSingle();
        checkBytes(encode4(single), encode4(single));
        ++blocks;
    }
    REQUIRE(blocks == CORPUS_BLOCKS);
}

TEST_CASE("the reference decoder is NOT vacuous -- hand-decoded blocks (BB21)") {
    // Without this the whole TU could pass against a decoder that returned the encoder's input. Both
    // blocks are written by hand and both decodes are computed by hand, with the arithmetic in the
    // comments, and neither ever goes near the encoder.
    //
    // BC1: c0 = 0xF800 (r5 = 31 -> 255, g = 0, b = 0) and c1 = 0x001F (b5 = 31 -> 255). c0 > c1, so
    // four-colour: palette[2] = (2*255 + 0 + 1)/3 = 170 red, (0 + 255 + 1)/3 = 85 blue; palette[3] =
    // (255 + 0 + 1)/3 = 85 red, (0 + 510 + 1)/3 = 170 blue. The index byte 0xE4 is 11 10 01 00, so
    // texels 0..3 take indices 0, 1, 2, 3 and the remaining twelve take 0.
    constexpr std::array<std::uint8_t, 8> BC1_BLOCK = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
    const std::array<Rgb8, 16> bc1 = decode1(BC1_BLOCK);
    CHECK(bc1[0].r == 255);
    CHECK(bc1[0].g == 0);
    CHECK(bc1[0].b == 0);
    CHECK(bc1[1].r == 0);
    CHECK(bc1[1].b == 255);
    CHECK(bc1[2].r == 170);
    CHECK(bc1[2].b == 85);
    CHECK(bc1[3].r == 85);
    CHECK(bc1[3].b == 170);
    std::size_t tail = 0;
    for (std::size_t i = 4; i < 16; ++i) {
        CHECK(bc1[i].r == 255);
        CHECK(bc1[i].b == 0);
        ++tail;
    }
    REQUIRE(tail == 12);

    // BC4: r0 = 200, r1 = 100, so the eight-value palette is
    //   200, 100, (6*200+100+3)/7 = 186, (5*200+200+3)/7 = 171, (4*200+300+3)/7 = 157,
    //   (3*200+400+3)/7 = 143, (2*200+500+3)/7 = 129, (200+600+3)/7 = 114
    // The 48-bit field 0o... is built here as index i = i % 8, i.e. 0,1,2,3,4,5,6,7 repeated: bits
    // 0..23 hold 0,1,2,3,4,5,6,7 as 3-bit groups -> 0b111'110'101'100'011'010'001'000 == 0xFAC688.
    constexpr std::array<std::uint8_t, 8> BC4_BLOCK = {200, 100, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA};
    const std::array<std::uint8_t, 16> bc4 = decode4(BC4_BLOCK);
    constexpr std::array<std::uint8_t, 8> BC4_EXPECTED = {200, 100, 186, 171, 157, 143, 129, 114};
    std::size_t checked = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(bc4[i] == BC4_EXPECTED[i % 8]);
        ++checked;
    }
    REQUIRE(checked == 16);
}

TEST_CASE("sixteen distinct colours -- the worst case for a four-entry palette (BB22)") {
    // Deliberately the pathological input: sixteen widely separated colours that no line through RGB
    // can approximate. The encoder must still produce a well-formed four-colour block and stay inside
    // the bound BB11 prints -- this case exists so "it fell over on hard input" is a red test rather
    // than a discovery in the field.
    constexpr std::array<std::uint8_t, 64> SRC = {
        0,   0,   0, 255, 255, 0,   0,   255, 0,   255, 0,   255, 0,   0,   255, 255, 255, 255, 0,  255, 255, 0,
        255, 255, 0, 255, 255, 255, 255, 255, 255, 255, 128, 0,   0,   255, 0,   128, 0,   255, 0,  0,   128, 255,
        128, 128, 0, 255, 128, 0,   128, 255, 0,   128, 128, 255, 128, 128, 128, 255, 64,  192, 32, 255,
    };
    const std::array<std::uint8_t, 8> bytes = encode1(SRC);
    const std::uint32_t c0 = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U);
    const std::uint32_t c1 = static_cast<std::uint32_t>(bytes[2]) | (static_cast<std::uint32_t>(bytes[3]) << 8U);
    CHECK(c0 >= c1);
    const std::array<Rgb8, 16> decoded = decode1(bytes);
    std::int32_t peak = 0;
    std::size_t checked = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const std::array<std::int32_t, 3> difference = {decoded[i].r - SRC[4 * i + 0], decoded[i].g - SRC[4 * i + 1],
                                                        decoded[i].b - SRC[4 * i + 2]};
        for (const std::int32_t d : difference) {
            const std::int32_t magnitude = d < 0 ? -d : d;
            peak = magnitude > peak ? magnitude : peak;
        }
        ++checked;
    }
    REQUIRE(checked == 16);
    MESSAGE("BC1 on sixteen distinct colours: peak per-channel error " << peak);
    // MEASURED at 198. A RECORD with headroom, exactly like BB11's.
    CHECK(peak <= 220);
    CHECK(peak > 0);  // anti-vacuity: this input CANNOT be represented exactly, so a zero peak is a bug
}
