// tests/texture_cook_test.cpp -- task 3.3.2: the texture cook. A TU of aero_tests, which supplies
// main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk, and NO IMAGE FIXTURE OF ANY KIND -- cookTexture takes RGBA8
// texels, which every case here synthesizes in code. That is the property that keeps the engine tier
// fixture-free while the editor tier needs real files.
//
// THE PREFIX IS `TX`, not `TC`: TC1..TC47 are already taken by tests/editor/thumbnail_cache_test.cpp.
// Two binaries do not collide at link time, but a case id is a GLOBAL identifier in this repo, cited
// from the sabotage matrix, the validation page, docs/10 and CLAUDE.md.
//
// Every hand-computed expectation below is written as a LITERAL with its arithmetic in a comment, so
// it can be re-derived on paper. A case whose expected value came out of the code it is testing is
// not a test.
#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/texture_cook.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies its operands through the
// stdlib's operator<<, which MS STL declares against an INCOMPLETE std::basic_ostream in headers only
// <ostream> completes. libc++ and libstdc++ are self-sufficient, so omitting it builds clean on macOS
// and Linux and fails only on the Windows lane, inside the STL headers rather than at the CHECK.
#include <ostream>
#include <span>
#include <string>
#include <vector>

using engine::assets::detail::downsampleRgba8;
using engine::assets::detail::linearToSrgb;
using engine::assets::detail::mipLevelCount;
using engine::assets::detail::srgbToLinear;

namespace {

// A grey RGBA8 image: R == G == B == the given level, alpha 255 unless overridden.
[[nodiscard]] std::vector<std::byte> greyImage(std::span<const std::uint8_t> levels) {
    std::vector<std::byte> out(levels.size() * 4, std::byte{0});
    for (std::size_t i = 0; i < levels.size(); ++i) {
        out[4 * i + 0] = static_cast<std::byte>(levels[i]);
        out[4 * i + 1] = static_cast<std::byte>(levels[i]);
        out[4 * i + 2] = static_cast<std::byte>(levels[i]);
        out[4 * i + 3] = std::byte{255};
    }
    return out;
}

[[nodiscard]] std::uint8_t channelAt(std::span<const std::byte> image, std::size_t texel, std::size_t channel) {
    return static_cast<std::uint8_t>(image[4 * texel + channel]);
}

// Halve `src` and return the result, sized by the filter's own rule.
[[nodiscard]] std::vector<std::byte> halve(std::span<const std::byte> src, std::uint32_t width, std::uint32_t height,
                                           bool srgb) {
    const std::uint32_t dstWidth = width > 1 ? width >> 1U : 1U;
    const std::uint32_t dstHeight = height > 1 ? height >> 1U : 1U;
    std::vector<std::byte> dst(static_cast<std::size_t>(dstWidth) * dstHeight * 4, std::byte{0});
    downsampleRgba8(src, width, height, dst, srgb);
    return dst;
}

// floor(log2(max(w, h))) + 1, written out LONGHAND here so TX1 never asserts the function against
// itself.
[[nodiscard]] std::uint32_t levelCountRef(std::uint32_t width, std::uint32_t height) {
    std::uint32_t largest = width > height ? width : height;
    std::uint32_t levels = 1;
    while (largest > 1) {
        largest /= 2;
        ++levels;
    }
    return levels;
}

}  // namespace

TEST_CASE("mipLevelCount is floor(log2(max(w, h))) + 1 across POT, NPOT and degenerate axes (TX1)") {
    struct Row {
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t expected;
    };
    // The expected values are LITERALS, and every one is also cross-checked against the longhand
    // reference so a wrong literal and a wrong function cannot agree.
    //
    // 1x2 and 2x1 are here deliberately: a version using WIDTH alone instead of max(w, h) agrees with
    // every square fixture and with every wider-than-tall one, and 1x2 is the smallest input that can
    // see it. 640x480 is the plan's own worked example RE-DERIVED here: 640 -> 320 -> 160 -> 80 -> 40
    // -> 20 -> 10 -> 5 -> 2 -> 1 is TEN levels, not eleven.
    constexpr std::array<Row, 10> ROWS = {
        Row{1, 1, 1},   Row{2, 1, 2}, Row{1, 2, 2},      Row{2, 2, 2},  Row{4, 4, 3},
        Row{16, 16, 5}, Row{5, 3, 3}, Row{640, 480, 10}, Row{1, 64, 7}, Row{16384, 16384, 15},
    };
    std::size_t checked = 0;
    for (const Row& row : ROWS) {
        INFO("dimensions ", row.width, "x", row.height);
        CHECK(mipLevelCount(row.width, row.height) == row.expected);
        CHECK(levelCountRef(row.width, row.height) == row.expected);
        ++checked;
    }
    REQUIRE(checked == ROWS.size());
    // A zero extent has no chain at all. The cook refuses such an input long before this is reached,
    // but the function is total anyway.
    CHECK(mipLevelCount(0, 0) == 0);
}

TEST_CASE("the sRGB forward table spans the full 16-bit range exactly (TX2)") {
    CHECK(srgbToLinear(0) == 0);
    CHECK(srgbToLinear(255) == 65535);
}

TEST_CASE("the sRGB forward table is strictly increasing across all 256 entries (TX3)") {
    // A flat or reversed pair is the shape a transcription slip in 256 committed literals produces.
    // The .cpp turns this into a static_assert as well, which is strictly stronger; this case is what
    // catches it if the assertion is ever weakened.
    std::size_t checked = 0;
    for (std::uint32_t v = 0; v + 1 < 256; ++v) {
        INFO("entry ", v);
        CHECK(srgbToLinear(static_cast<std::uint8_t>(v)) < srgbToLinear(static_cast<std::uint8_t>(v + 1)));
        ++checked;
    }
    REQUIRE(checked == 255);
}

TEST_CASE("five hand-recomputed anchors of the sRGB forward table (TX4)") {
    // THE ONLY NON-CIRCULAR CHECK ON THE TABLE. Every value below was recomputed independently while
    // writing this case -- not copied out of the array and not out of the generator's output -- with
    // the arithmetic spelled out so it can be redone on paper or with `python3 -c`. If one disagrees
    // with the committed table, the TABLE is wrong, not the anchor.
    //
    // eotf(c) = c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ^ 2.4, and the entry is
    // round(65535 * eotf(v / 255)).
    //
    //  v =   0  ->  0                                                          exactly 0
    //  v =  10  ->  10/255 = 0.039215686 <= 0.04045, so the LINEAR segment:
    //               0.039215686 / 12.92 = 0.003035270; * 65535 = 198.9164  ->     199
    //  v =  11  ->  11/255 = 0.043137255 >  0.04045, the FIRST value on the POWER segment:
    //               ((0.043137255 + 0.055) / 1.055) ^ 2.4 = 0.003346536; * 65535 = 219.3152 -> 219
    //  v = 128  ->  ((0.501960784 + 0.055) / 1.055) ^ 2.4 = 0.215860500; * 65535 = 14146.4179 -> 14146
    //  v = 186  ->  ((0.729411765 + 0.055) / 1.055) ^ 2.4 = 0.491020850; * 65535 = 32179.0514 -> 32179
    //               (49.1% of full intensity -- note that TRUE half-intensity is at v = 188, 32957,
    //                so "186 is half" is an approximation and is recorded as one)
    CHECK(srgbToLinear(0) == 0);
    CHECK(srgbToLinear(10) == 199);
    CHECK(srgbToLinear(11) == 219);
    CHECK(srgbToLinear(128) == 14146);
    CHECK(srgbToLinear(186) == 32179);
    // The knee itself: 10 is the last value on the linear segment and 11 the first on the power one.
    // 10 * (1/255) = 0.0392 <= 0.04045 < 0.0431 = 11 * (1/255).
    CHECK(srgbToLinear(11) - srgbToLinear(10) == 20);
    CHECK(srgbToLinear(10) - srgbToLinear(9) == 20);
}

TEST_CASE("every inverse threshold IS the midpoint of its two forward neighbours (TX5)") {
    // The threshold table is DERIVED from the forward one, and the .cpp static_asserts that
    // derivation, so only one array can be independently wrong. This case checks it through the
    // public seam instead of exposing the array: linearToSrgb(x) counts the thresholds <= x, so the
    // midpoint T[i] must be the FIRST value that answers i + 1, and T[i] - 1 must still answer i.
    std::size_t checked = 0;
    for (std::uint32_t i = 0; i + 1 < 256; ++i) {
        const std::uint32_t low = srgbToLinear(static_cast<std::uint8_t>(i));
        const std::uint32_t high = srgbToLinear(static_cast<std::uint8_t>(i + 1));
        const auto midpoint = static_cast<std::uint16_t>((low + high + 1) / 2);
        INFO("threshold ", i, " between ", low, " and ", high);
        CHECK(linearToSrgb(midpoint) == i + 1);
        REQUIRE(midpoint > 0);
        CHECK(linearToSrgb(static_cast<std::uint16_t>(midpoint - 1)) == i);
        ++checked;
    }
    REQUIRE(checked == 255);
}

TEST_CASE("the gamma pair round-trips all 256 values exhaustively (TX6)") {
    // The property the mip filter actually depends on: decoding to linear, averaging and re-encoding
    // must be the identity for a CONSTANT input, which is exactly this.
    std::size_t checked = 0;
    for (std::uint32_t v = 0; v < 256; ++v) {
        INFO("value ", v);
        CHECK(linearToSrgb(srgbToLinear(static_cast<std::uint8_t>(v))) == v);
        ++checked;
    }
    REQUIRE(checked == 256);
}

TEST_CASE("linearToSrgb is total at both ends of the 16-bit range (TX7)") {
    CHECK(linearToSrgb(0) == 0);
    CHECK(linearToSrgb(65535) == 255);
    CHECK(linearToSrgb(1) == 0);   // below the first threshold, which is (0 + 20 + 1) / 2 == 10
    CHECK(linearToSrgb(10) == 1);  // exactly the first threshold
    CHECK(linearToSrgb(65534) == 255);
}

TEST_CASE("a hand-computed 4x2 -> 2x1 downsample, both axes even (TX8)") {
    // Both axes even, so both take the two-tap branch: weights {1, 1}, denominators 2 and 2, and the
    // fused denominator is 4.
    //   source  row 0:  10  20  30  40
    //           row 1:  50  60  70  80
    //   out[0] = (10 + 20 + 50 + 60 + 2) / 4 = 142 / 4 = 35     (the true mean is exactly 35)
    //   out[1] = (30 + 40 + 70 + 80 + 2) / 4 = 222 / 4 = 55     (the true mean is exactly 55)
    constexpr std::array<std::uint8_t, 8> LEVELS = {10, 20, 30, 40, 50, 60, 70, 80};
    const std::vector<std::byte> src = greyImage(LEVELS);
    const std::vector<std::byte> dst = halve(src, 4, 2, false);
    REQUIRE(dst.size() == 8);  // 2 x 1 x RGBA
    CHECK(channelAt(dst, 0, 0) == 35);
    CHECK(channelAt(dst, 1, 0) == 55);
    CHECK(channelAt(dst, 0, 3) == 255);  // alpha is 255 throughout and must stay so
    CHECK(channelAt(dst, 1, 3) == 255);
}

TEST_CASE("a hand-computed 3x3 -> 1x1 downsample, both axes odd (TX9)") {
    // Both axes odd with S = 3, so D = 1 and the weights are {D - i, D, i + 1} = {1, 1, 1} over a
    // denominator of 2D + 1 = 3. The fused denominator is 9 and every one of the nine texels
    // contributes equally.
    //   10  20  30
    //   40  50  60
    //   70  80  91          sum = 451
    //   out = (451 + 4) / 9 = 455 / 9 = 50          (the true mean is 50.111)
    //
    // The alternatives this input is chosen to REFUTE, both of which shift the image by half a texel
    // per level and therefore compound down the chain:
    //   a truncated 2x2 box (drop the last row and column) = (10 + 20 + 40 + 50) / 4  = 30
    //   a clamped 2x2 box   (same four texels)                                        = 30
    constexpr std::array<std::uint8_t, 9> LEVELS = {10, 20, 30, 40, 50, 60, 70, 80, 91};
    const std::vector<std::byte> src = greyImage(LEVELS);
    const std::vector<std::byte> dst = halve(src, 3, 3, false);
    REQUIRE(dst.size() == 4);  // 1 x 1 x RGBA
    CHECK(channelAt(dst, 0, 0) == 50);
    CHECK(channelAt(dst, 0, 0) != 30);
}

TEST_CASE("a hand-computed 5x3 -> 2x1 downsample: the polyphase weights are ASYMMETRIC (TX10)") {
    // THE CASE A SWAPPED (D - i) / (i + 1) REDDENS AND TX9 STRUCTURALLY CANNOT. For S = 5, D = 2 and
    // the two destination taps get DIFFERENT weight vectors:
    //   i = 0: {D - 0, D, 0 + 1} = {2, 2, 1} over source columns {0, 1, 2}
    //   i = 1: {D - 1, D, 1 + 1} = {1, 2, 2} over source columns {2, 3, 4}
    // Both sum to 2D + 1 = 5, and they are the exact overlaps of the continuous boxes [0, 2.5] and
    // [2.5, 5] with the five source columns -- which is what "polyphase" means here and why the
    // weights are asymmetric between taps rather than within one.
    // The row axis is S = 3, weights {1, 1, 1}, denominator 3, so the fused denominator is 15.
    //
    //   source  row 0:   10   20   30   40   50
    //           row 1:   60   70   80   90  100
    //           row 2:  110  120  130  140  150
    //
    //   out[0]:  row0   2*10 +  2*20 + 1*30  =  90
    //            row1   2*60 +  2*70 + 1*80  = 340
    //            row2  2*110 + 2*120 + 1*130 = 590     sum 1020 -> (1020 + 7) / 15 = 68
    //   out[1]:  row0   1*30 +  2*40 + 2*50  = 210
    //            row1   1*80 +  2*90 + 2*100 = 460
    //            row2  1*130 + 2*140 + 2*150 = 710     sum 1380 -> (1380 + 7) / 15 = 92
    //
    // With the weight vectors SWAPPED -- {1, 2, 2} for tap 0 and {2, 2, 1} for tap 1, which is what
    // exchanging (D - i) and (i + 1) produces -- the same input gives 1080 / 15 = 72 and
    // 1320 / 15 = 88. Both are written in below, so the case names what it refutes.
    constexpr std::array<std::uint8_t, 15> LEVELS = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150};
    const std::vector<std::byte> src = greyImage(LEVELS);
    const std::vector<std::byte> dst = halve(src, 5, 3, false);
    REQUIRE(dst.size() == 8);  // 2 x 1 x RGBA
    CHECK(channelAt(dst, 0, 0) == 68);
    CHECK(channelAt(dst, 1, 0) == 92);
    CHECK(channelAt(dst, 0, 0) != 72);  // the swapped-weight answer for tap 0
    CHECK(channelAt(dst, 1, 0) != 88);  // and for tap 1
}

TEST_CASE("the polyphase weights sum to their denominator, for every odd extent (TX11)") {
    // Checked through the filter rather than by reaching into it: the weights sum to the denominator
    // IFF a CONSTANT row downsamples to exactly that constant. Any other weight sum scales the
    // result, which is what makes an energy-losing filter visible as a brightness drift down the mip
    // chain -- the classic symptom this rule exists to prevent.
    constexpr std::array<std::uint32_t, 5> EXTENTS = {3, 5, 7, 9, 4097};
    constexpr std::uint8_t CONSTANT = 200;
    std::size_t checked = 0;
    for (const std::uint32_t extent : EXTENTS) {
        const std::vector<std::uint8_t> levels(extent, CONSTANT);
        const std::vector<std::byte> src = greyImage(levels);
        const std::vector<std::byte> dst = halve(src, extent, 1, false);
        REQUIRE(dst.size() == static_cast<std::size_t>(extent >> 1U) * 4);
        for (std::size_t i = 0; i < dst.size() / 4; ++i) {
            INFO("extent ", extent, " texel ", i);
            CHECK(channelAt(dst, i, 0) == CONSTANT);
        }
        ++checked;
    }
    REQUIRE(checked == EXTENTS.size());
}

TEST_CASE("a degenerate axis stays 1 and takes the one-tap branch at every level (TX12)") {
    // 1xN and Nx1. The extent-1 axis must neither halve to 0 nor read a second, non-existent tap.
    constexpr std::array<std::uint8_t, 8> LEVELS = {0, 16, 32, 48, 64, 80, 96, 112};
    const std::vector<std::byte> column = greyImage(LEVELS);

    const std::vector<std::byte> down = halve(column, 1, 8, false);
    REQUIRE(down.size() == 16);  // 1 x 4 x RGBA
    // Each output is the two-tap mean of a vertical pair: (0+16+1)/2 = 8, (32+48+1)/2 = 40,
    // (64+80+1)/2 = 72, (96+112+1)/2 = 104.
    CHECK(channelAt(down, 0, 0) == 8);
    CHECK(channelAt(down, 1, 0) == 40);
    CHECK(channelAt(down, 2, 0) == 72);
    CHECK(channelAt(down, 3, 0) == 104);

    const std::vector<std::byte> across = halve(column, 8, 1, false);
    REQUIRE(across.size() == 16);  // 4 x 1 x RGBA
    CHECK(channelAt(across, 0, 0) == 8);
    CHECK(channelAt(across, 3, 0) == 104);

    // And the whole chain down to 1x1 keeps the degenerate axis at 1.
    std::vector<std::byte> current = column;
    std::uint32_t height = 8;
    std::uint32_t levels = 1;
    while (height > 1) {
        current = halve(current, 1, height, false);
        height = height >> 1U;
        ++levels;
    }
    CHECK(levels == mipLevelCount(1, 8));
    CHECK(current.size() == 4);  // 1 x 1 x RGBA
}

TEST_CASE("a weighted sum landing exactly on .5 rounds UP (TX13)") {
    // 2x1 -> 1x1: one two-tap axis (denominator 2) and one one-tap axis (denominator 1), so the fused
    // denominator is 2 and 100 + 101 = 201 is exactly 100.5.
    //   round half UP   -> (201 + 1) / 2 = 101
    //   round half DOWN ->  201      / 2 = 100
    constexpr std::array<std::uint8_t, 2> LEVELS = {100, 101};
    const std::vector<std::byte> dst = halve(greyImage(LEVELS), 2, 1, false);
    REQUIRE(dst.size() == 4);
    CHECK(channelAt(dst, 0, 0) == 101);
    CHECK(channelAt(dst, 0, 0) != 100);
}

TEST_CASE("an sRGB downsample averages in LINEAR light, not in stored bytes (TX14)") {
    // Two texels, 0 and 255, halved to one.
    //   BYTE space:   (0 + 255 + 1) / 2 = 128
    //   LINEAR space: srgbToLinear(0) = 0 and srgbToLinear(255) = 65535, so
    //                 (0 + 65535 + 1) / 2 = 32768, and linearToSrgb(32768) = 188, because the
    //                 thresholds around it are T[187] = (32567 + 32957 + 1) / 2 = 32762 and
    //                 T[188] = (32957 + 33350 + 1) / 2 = 33154, so exactly 188 thresholds are <= it.
    // 188 is the correct answer: mid-grey in linear light really is about 0.74 in sRGB. Averaging
    // sRGB-encoded bytes directly gives 128 and is the classic mip-chain brightness drift.
    constexpr std::array<std::uint8_t, 2> LEVELS = {0, 255};
    const std::vector<std::byte> dst = halve(greyImage(LEVELS), 2, 1, true);
    REQUIRE(dst.size() == 4);
    CHECK(channelAt(dst, 0, 0) == 188);
    CHECK(channelAt(dst, 0, 0) != 128);
    // The two thresholds the answer sits between, re-derived here.
    CHECK(linearToSrgb(32768) == 188);
    CHECK(srgbToLinear(187) == 32567);
    CHECK(srgbToLinear(188) == 32957);
}

TEST_CASE("the same input under a linear format takes the BYTE-space answer (TX15)") {
    constexpr std::array<std::uint8_t, 2> LEVELS = {0, 255};
    const std::vector<std::byte> dst = halve(greyImage(LEVELS), 2, 1, false);
    REQUIRE(dst.size() == 4);
    CHECK(channelAt(dst, 0, 0) == 128);
    CHECK(channelAt(dst, 0, 0) != 188);
}

TEST_CASE("ALPHA takes the byte-space answer in BOTH colour spaces (TX16)") {
    // Alpha is COVERAGE, never a gamma-encoded colour, so gamma-correcting it is a real and subtle
    // bug: it would make a soft edge's mip chain drift in opacity while its colour stayed right.
    // Two texels with alpha 0 and 255 must average to 128 whether or not the format is sRGB, while
    // the colour channels of the very same call answer 188 and 128 respectively.
    std::vector<std::byte> src(8, std::byte{0});
    for (std::size_t i = 0; i < 2; ++i) {
        const auto value = static_cast<std::byte>(i == 0 ? 0 : 255);
        src[4 * i + 0] = value;
        src[4 * i + 1] = value;
        src[4 * i + 2] = value;
        src[4 * i + 3] = value;  // alpha follows the colour, so a shared code path would be visible
    }
    const std::vector<std::byte> srgb = halve(src, 2, 1, true);
    REQUIRE(srgb.size() == 4);
    CHECK(channelAt(srgb, 0, 0) == 188);  // colour: linear light
    CHECK(channelAt(srgb, 0, 3) == 128);  // alpha: stored bytes
    CHECK(channelAt(srgb, 0, 3) != 188);

    const std::vector<std::byte> unorm = halve(src, 2, 1, false);
    REQUIRE(unorm.size() == 4);
    CHECK(channelAt(unorm, 0, 0) == 128);
    CHECK(channelAt(unorm, 0, 3) == 128);
}

TEST_CASE("a grey ramp gives DIFFERENT level-1 bytes under sRGB and under linear (TX17)") {
    // The whole-image restatement of TX14/TX15, and the case that ties the colour space to the FORMAT
    // rather than to a caller-supplied bool: the cook derives `srgb` from isSrgbCookedFormat, which is
    // the only place the colour space is carried.
    constexpr std::array<std::uint8_t, 16> LEVELS = {0,   17,  34,  51,  68,  85,  102, 119,
                                                     136, 153, 170, 187, 204, 221, 238, 255};
    const std::vector<std::byte> src = greyImage(LEVELS);
    const std::vector<std::byte> asSrgb = halve(src, 4, 4, true);
    const std::vector<std::byte> asUnorm = halve(src, 4, 4, false);
    REQUIRE(asSrgb.size() == asUnorm.size());
    bool anyDiffers = false;
    for (std::size_t i = 0; i < asSrgb.size(); ++i) {
        anyDiffers = anyDiffers || asSrgb[i] != asUnorm[i];
    }
    CHECK(anyDiffers);
    // And the sRGB result is BRIGHTER everywhere it differs, which is the direction that says the
    // linear round trip is being done rather than merely something different.
    std::size_t brighter = 0;
    for (std::size_t i = 0; i < asSrgb.size() / 4; ++i) {
        if (channelAt(asSrgb, i, 0) > channelAt(asUnorm, i, 0)) {
            ++brighter;
        }
    }
    CHECK(brighter > 0);

    CHECK(engine::assets::isSrgbCookedFormat(engine::assets::CookedTextureFormat::Bc1RgbSrgb));
    CHECK_FALSE(engine::assets::isSrgbCookedFormat(engine::assets::CookedTextureFormat::Bc1RgbUnorm));
}

TEST_CASE("downsampleRgba8 reads nothing outside its source, and refuses a mis-sized span (TX18)") {
    // The source is allocated EXACTLY srcWidth * srcHeight * 4, so an over-read is an ASan report on
    // the Debug lanes rather than a silent wrong pixel. Odd in both axes, which is where the
    // three-tap branch reaches furthest.
    constexpr std::array<std::uint8_t, 35> LEVELS = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
        18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    };
    const std::vector<std::byte> src = greyImage(LEVELS);  // 7 x 5
    REQUIRE(src.size() == 7 * 5 * 4);
    const std::vector<std::byte> dst = halve(src, 7, 5, true);
    CHECK(dst.size() == 3 * 2 * 4);

    // A mis-sized destination writes NOTHING rather than a partial result or a read.
    std::vector<std::byte> tooSmall(4, std::byte{0xAB});
    downsampleRgba8(src, 7, 5, tooSmall, false);
    CHECK(tooSmall[0] == std::byte{0xAB});
    // A mis-sized SOURCE likewise.
    std::vector<std::byte> properDst(std::size_t{3} * 2 * 4, std::byte{0xCD});
    downsampleRgba8(std::span<const std::byte>(src).first(16), 7, 5, properDst, false);
    CHECK(properDst[0] == std::byte{0xCD});
}

TEST_CASE("each level comes from level p-1, NEVER resampled from level 0 (TX19)") {
    // Resampling every level from the base would make each level a different filter's output and
    // would cost O(levels x base area). It also gives DIFFERENT bytes, and this input is built so the
    // difference is visible at level 2 with one-bit values and no gamma involved.
    //
    // The top-left 4x4 of an 8x8, laid out so its four 2x2 blocks sum to 2, 2, 2 and 1:
    //       0 1 0 1
    //       1 0 1 0
    //       0 1 1 0
    //       1 0 0 0
    //   level 1's four texels there are (2+2)/4 = 1, (2+2)/4 = 1, (2+2)/4 = 1, (1+2)/4 = 0
    //   level 2 texel 0, FROM LEVEL 1  = (1 + 1 + 1 + 0 + 2) / 4 = 5 / 4 = 1
    //   level 2 texel 0, FROM LEVEL 0  = (2 + 2 + 2 + 1 + 8) / 16 = 15 / 16 = 0
    // Two different answers from the same base, which is what makes this case able to see the rule.
    std::array<std::uint8_t, 64> levels{};
    constexpr std::array<std::uint8_t, 16> QUADRANT = {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            levels[8 * y + x] = QUADRANT[4 * y + x];
        }
    }
    const std::vector<std::byte> base = greyImage(levels);
    const std::vector<std::byte> level1 = halve(base, 8, 8, false);
    REQUIRE(level1.size() == 4 * 4 * 4);
    CHECK(channelAt(level1, 0, 0) == 1);
    CHECK(channelAt(level1, 1, 0) == 1);
    CHECK(channelAt(level1, 4, 0) == 1);  // texel (0, 1) of a 4-wide level
    CHECK(channelAt(level1, 5, 0) == 0);  // texel (1, 1)

    const std::vector<std::byte> level2 = halve(level1, 4, 4, false);
    REQUIRE(level2.size() == 2 * 2 * 4);
    CHECK(channelAt(level2, 0, 0) == 1);  // from level 1
    CHECK(channelAt(level2, 0, 0) != 0);  // what a resample from level 0 would have given
}

TEST_CASE("the maximum dimension's chain is exactly MAX_TEXTURE_LEVELS (TX20)") {
    CHECK(mipLevelCount(engine::assets::MAX_TEXTURE_DIMENSION, engine::assets::MAX_TEXTURE_DIMENSION) ==
          engine::assets::MAX_TEXTURE_LEVELS);
    CHECK(mipLevelCount(engine::assets::MAX_TEXTURE_DIMENSION, 1) == engine::assets::MAX_TEXTURE_LEVELS);
    CHECK(mipLevelCount(1, engine::assets::MAX_TEXTURE_DIMENSION) == engine::assets::MAX_TEXTURE_LEVELS);
    // One below the cap is one level shorter, so the cap is not accidentally a plateau.
    CHECK(mipLevelCount(engine::assets::MAX_TEXTURE_DIMENSION / 2, 1) == engine::assets::MAX_TEXTURE_LEVELS - 1);
}
