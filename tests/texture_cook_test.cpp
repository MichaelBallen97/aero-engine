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
#include <aero/assets/bc_block.hpp>
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
#include <fstream>
#include <iterator>
#include <ostream>
#include <span>
#include <string>
#include <vector>

using engine::assets::cookedTextureBlockBytes;
using engine::assets::CookedTextureFormat;
using engine::assets::cookedTextureLevelAlignment;
using engine::assets::cookTexture;
using engine::assets::encodeBc1Block;
using engine::assets::encodeBc4Block;
using engine::assets::parseCookedTexture;
using engine::assets::TextureCookInput;
using engine::assets::TextureCookResult;
using engine::assets::TextureCookStatus;
using engine::assets::toString;
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

// The comment-stripped text of one engine/assets source file. TWO cases need it -- TX40's
// "the cap is on BYTES, not on dimension" arm and TX48's ordering arm -- and both need the SAME
// stripping, so it lives here rather than being written twice. Comments go first so prose that
// merely NAMES a constant can never stand in for code that uses it.
[[nodiscard]] std::string strippedAssetsSource(const std::string& fileName) {
    std::ifstream file(std::string(AERO_ASSETS_SRC_DIR) + "/" + fileName, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(source.size() > 1000);

    std::string stripped;
    stripped.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }
        }
        if (i < source.size()) {
            stripped.push_back(source[i]);
        }
    }
    // ANTI-VACUITY: stripping must have removed something and kept something.
    REQUIRE(stripped.size() < source.size());
    REQUIRE_FALSE(stripped.empty());
    return stripped;
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
    // THE COUNT IS A LITERAL, NEVER `ROWS.size()`. A guard derived from the very table it guards
    // cannot see a row DELETED -- `checked` and `ROWS.size()` shrink together and the case stays
    // green while testing less. Sabotage seed S48b removed the 1x2 and 1x64 rows (the only two rows
    // that can see mipLevelCount using width alone, which is seed S47) and the whole suite stayed
    // green. Every case-local table in this file, cooked_texture_test.cpp and
    // editor/texture_cook_source_test.cpp pins its count the same way.
    REQUIRE(checked == 10);
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
    REQUIRE(checked == 5);
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

// =================================================================================================
// cookTexture (TX21-TX48)
//
// NEVER WRITE `CHECK(someCookedTextureFormat == CookedTextureFormat::X)` HERE. doctest's
// DOCTEST_STRINGIFY expands to an UNQUALIFIED `toString(...)`, so ADL finds
// engine::assets::toString(CookedTextureFormat) -- a non-template exact match that beats doctest's own
// template -- and the decomposer then tries `std::string_view + const char*`, a hard compile error on
// EVERY lane. Compare through toString() on both sides, as tests/cooked_texture_test.cpp's header
// explains at length.
// =================================================================================================

namespace {

// INV-T3 in both directions, called by every cook case below rather than living in one case of its
// own: `bytes` is non-empty IFF Ok, and `message` is non-empty IFF Refused.
void checkCookInvariant(const TextureCookResult& result) {
    if (result.status == TextureCookStatus::Ok) {
        CHECK_FALSE(result.bytes.empty());
        CHECK(result.message.empty());
    } else {
        CHECK(result.bytes.empty());
        CHECK_FALSE(result.message.empty());
    }
    // v1's cook emits no warning at all, and this is the honest statement of that rather than a
    // synthetic warning written to make the field look used.
    CHECK(result.warnings.empty());
    CHECK(result.warningTotal == 0);
}

[[nodiscard]] TextureCookResult cook(std::span<const std::byte> rgba8, std::uint32_t width, std::uint32_t height,
                                     CookedTextureFormat format, bool generateMips = true) {
    TextureCookInput input;
    input.width = width;
    input.height = height;
    input.rgba8 = rgba8;
    input.format = format;
    input.generateMips = generateMips;
    const TextureCookResult result = cookTexture(input);
    checkCookInvariant(result);
    return result;
}

// A deterministic non-trivial image: a diagonal gradient with a varying alpha.
[[nodiscard]] std::vector<std::byte> testImage(std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> out(static_cast<std::size_t>(width) * height * 4, std::byte{0});
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            out[i + 0] = static_cast<std::byte>((x * 37 + y * 11) & 0xFF);
            out[i + 1] = static_cast<std::byte>((x * 5 + y * 61) & 0xFF);
            out[i + 2] = static_cast<std::byte>((x * 97 + y * 3) & 0xFF);
            out[i + 3] = static_cast<std::byte>((x + y) % 2 == 0 ? 255 : 128);
        }
    }
    return out;
}

constexpr std::array<CookedTextureFormat, 8> COOK_FORMATS = {
    CookedTextureFormat::Rgba8Unorm, CookedTextureFormat::Rgba8Srgb, CookedTextureFormat::Bc1RgbUnorm,
    CookedTextureFormat::Bc1RgbSrgb, CookedTextureFormat::Bc3Unorm,  CookedTextureFormat::Bc3Srgb,
    CookedTextureFormat::Bc4Unorm,   CookedTextureFormat::Bc5Unorm,
};

}  // namespace

TEST_CASE("a zero or over-cap width is Refused with both numbers (TX21)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult zero = cook(pixels, 0, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(zero.status == TextureCookStatus::Refused);
    CHECK(zero.message.find("width") != std::string::npos);
    CHECK(zero.message.find("16384") != std::string::npos);
}

TEST_CASE("a height one past MAX_TEXTURE_DIMENSION is Refused (TX22)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult over =
        cook(pixels, 4, engine::assets::MAX_TEXTURE_DIMENSION + 1, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(over.status == TextureCookStatus::Refused);
    CHECK(over.message.find("height") != std::string::npos);
}

TEST_CASE("a pixel span one byte short of the dimensions is Refused (TX23)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult shortSpan =
        cook(std::span<const std::byte>(pixels).first(pixels.size() - 1), 4, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(shortSpan.status == TextureCookStatus::Refused);
    CHECK(shortSpan.message.find("63") != std::string::npos);  // the span's own size
    CHECK(shortSpan.message.find("64") != std::string::npos);  // and what 4x4 RGBA8 must be
}

TEST_CASE("a pixel span one byte long is Refused too (TX24)") {
    // The over-long direction matters as much as the short one: a span that is merely BIG ENOUGH
    // would let a caller cook a 4x4 out of the first 64 bytes of something else entirely.
    std::vector<std::byte> pixels = testImage(4, 4);
    pixels.push_back(std::byte{0});
    const TextureCookResult longSpan = cook(pixels, 4, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(longSpan.status == TextureCookStatus::Refused);
    CHECK(longSpan.message.find("65") != std::string::npos);
}

TEST_CASE("bytes are non-empty IFF Ok, across every format (TX25)") {
    // checkCookInvariant asserts the biconditional on every call, so this case is where it is driven
    // over BOTH sides deliberately rather than incidentally.
    const std::vector<std::byte> pixels = testImage(5, 3);
    std::size_t accepted = 0;
    for (const CookedTextureFormat format : COOK_FORMATS) {
        INFO("format ", toString(format));
        const TextureCookResult result = cook(pixels, 5, 3, format);
        CHECK(result.status == TextureCookStatus::Ok);
        ++accepted;
    }
    // COOK_FORMATS is shared by several cases, so its SIZE is pinned here with a literal: the guards
    // that read `== COOK_FORMATS.size()` cannot see the table itself shrink, and this is the one
    // assertion that can (the ALL_FORMATS/CT1 arrangement one file over, restated).
    CHECK(COOK_FORMATS.size() == 8);
    REQUIRE(accepted == COOK_FORMATS.size());
    const TextureCookResult refused = cook(pixels, 0, 3, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(refused.status == TextureCookStatus::Refused);
}

TEST_CASE("Rgba8Unorm level 0 is BIT-EXACT with its input (TX26)") {
    const std::vector<std::byte> pixels = testImage(5, 3);
    const TextureCookResult result = cook(pixels, 5, 3, CookedTextureFormat::Rgba8Unorm);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == pixels.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        CHECK(level0[i] == pixels[i]);
        ++compared;
    }
    REQUIRE(compared == pixels.size());
}

TEST_CASE("Rgba8Srgb level 0 is bit-exact too -- the colour space never touches the bytes (TX27)") {
    // The colour space changes the vkFormat and the descriptor's transferFunction, and NOTHING else
    // about level 0. It does change the MIP levels, which is TX17's business.
    const std::vector<std::byte> pixels = testImage(5, 3);
    const TextureCookResult unorm = cook(pixels, 5, 3, CookedTextureFormat::Rgba8Unorm);
    const TextureCookResult srgb = cook(pixels, 5, 3, CookedTextureFormat::Rgba8Srgb);
    REQUIRE(unorm.status == TextureCookStatus::Ok);
    REQUIRE(srgb.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(srgb.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == pixels.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        CHECK(level0[i] == pixels[i]);
        ++compared;
    }
    REQUIRE(compared == pixels.size());
    // Same total size, different bytes: the two files differ in vkFormat and in the descriptor.
    CHECK(unorm.bytes.size() == srgb.bytes.size());
    bool anyDiffers = false;
    for (std::size_t i = 0; i < unorm.bytes.size(); ++i) {
        anyDiffers = anyDiffers || unorm.bytes[i] != srgb.bytes[i];
    }
    CHECK(anyDiffers);
}

TEST_CASE("every level's byteLength is blocksX * blocksY * blockBytes, for all four BCn (TX28)") {
    // Driven at 5x3 (partial blocks in both axes, and a level chain that hits 1x1) and at 16x16 (whole
    // blocks throughout). A version that dropped the blocksY term would agree with every level that is
    // one block tall -- which is all of the 5x3 chain except level 0 -- so 16x16 is here to see it.
    constexpr std::array<CookedTextureFormat, 4> BLOCK_FORMATS = {
        CookedTextureFormat::Bc1RgbUnorm, CookedTextureFormat::Bc3Unorm, CookedTextureFormat::Bc4Unorm,
        CookedTextureFormat::Bc5Unorm};
    struct Shape {
        std::uint32_t width;
        std::uint32_t height;
    };
    constexpr std::array<Shape, 2> SHAPES = {Shape{5, 3}, Shape{16, 16}};
    std::size_t levelsChecked = 0;
    for (const Shape& shape : SHAPES) {
        const std::vector<std::byte> pixels = testImage(shape.width, shape.height);
        for (const CookedTextureFormat format : BLOCK_FORMATS) {
            INFO("format ", toString(format), " at ", shape.width, "x", shape.height);
            const TextureCookResult result = cook(pixels, shape.width, shape.height, format);
            REQUIRE(result.status == TextureCookStatus::Ok);
            const auto parse = parseCookedTexture(result.bytes);
            REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
            for (std::uint32_t level = 0; level < parse.view.levelCount(); ++level) {
                const std::uint32_t levelWidth = parse.view.levelWidth(level);
                const std::uint32_t levelHeight = parse.view.levelHeight(level);
                const std::uint64_t blocksX = (levelWidth + 3) / 4;
                const std::uint64_t blocksY = (levelHeight + 3) / 4;
                const std::uint64_t expected = blocksX * blocksY * cookedTextureBlockBytes(format);
                INFO("level ", level, " is ", levelWidth, "x", levelHeight);
                CHECK(parse.view.levelBytes(level).size() == expected);
                ++levelsChecked;
            }
        }
    }
    // 5x3 has 3 levels and 16x16 has 5, over four formats each.
    REQUIRE(levelsChecked == (3 + 5) * 4);
}

TEST_CASE("a BC1 level is exactly one 8-byte block per 4x4 footprint (TX29)") {
    const std::vector<std::byte> pixels = testImage(8, 8);
    const TextureCookResult result = cook(pixels, 8, 8, CookedTextureFormat::Bc1RgbSrgb);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(parse.view.levelBytes(0).size() == 2 * 2 * 8);
    CHECK(parse.view.levelBytes(1).size() == 1 * 1 * 8);  // 4x4
    CHECK(parse.view.levelBytes(2).size() == 8);          // 2x2, still one block
    CHECK(parse.view.levelBytes(3).size() == 8);          // 1x1, still one block
    CHECK(parse.view.levelCount() == 4);
}

TEST_CASE("a BC5 level is 16 bytes per block and a BC4 level is 8 (TX30)") {
    const std::vector<std::byte> pixels = testImage(8, 8);
    const auto bc4 = parseCookedTexture(cook(pixels, 8, 8, CookedTextureFormat::Bc4Unorm).bytes);
    const auto bc5 = parseCookedTexture(cook(pixels, 8, 8, CookedTextureFormat::Bc5Unorm).bytes);
    REQUIRE(bc4.status == engine::assets::CookedTextureStatus::Ok);
    REQUIRE(bc5.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(bc4.view.levelBytes(0).size() == 4 * 8);
    CHECK(bc5.view.levelBytes(0).size() == 4 * 16);
    CHECK(bc5.view.levelBytes(0).size() == 2 * bc4.view.levelBytes(0).size());
}

TEST_CASE("an uncompressed level is width * height * 4 at every level (TX31)") {
    const std::vector<std::byte> pixels = testImage(5, 3);
    const auto parse = parseCookedTexture(cook(pixels, 5, 3, CookedTextureFormat::Rgba8Unorm).bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    REQUIRE(parse.view.levelCount() == 3);
    CHECK(parse.view.levelBytes(0).size() == 5 * 3 * 4);
    CHECK(parse.view.levelBytes(1).size() == 2 * 1 * 4);
    CHECK(parse.view.levelBytes(2).size() == 1 * 1 * 4);
}

TEST_CASE("BC3 is ALPHA-then-colour, computed here by calling the encoders directly (TX32)") {
    // An output-byte decision, and one that produces a plausible image rather than an obviously broken
    // one when it is wrong -- which is exactly why it gets its own case.
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult result = cook(pixels, 4, 4, CookedTextureFormat::Bc3Unorm, false);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == 16);

    std::array<std::uint8_t, 64> texels{};
    std::array<std::uint8_t, 16> alpha{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            texels[4 * i + channel] = static_cast<std::uint8_t>(pixels[4 * i + channel]);
        }
        alpha[i] = texels[4 * i + 3];
    }
    std::array<std::byte, 8> expectedAlpha{};
    std::array<std::byte, 8> expectedColour{};
    encodeBc4Block(alpha, expectedAlpha);
    encodeBc1Block(texels, expectedColour);

    std::size_t compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        INFO("byte ", i);
        CHECK(level0[i] == expectedAlpha[i]);       // bytes 0..7 are the ALPHA block
        CHECK(level0[i + 8] == expectedColour[i]);  // bytes 8..15 are the COLOUR block
        ++compared;
    }
    REQUIRE(compared == 8);
    // ANTI-VACUITY: the two halves must actually DIFFER, or "alpha first" and "colour first" would be
    // indistinguishable on this input.
    bool halvesDiffer = false;
    for (std::size_t i = 0; i < 8; ++i) {
        halvesDiffer = halvesDiffer || expectedAlpha[i] != expectedColour[i];
    }
    CHECK(halvesDiffer);
}

TEST_CASE("BC5 is RED-then-green, by the same construction (TX33)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult result = cook(pixels, 4, 4, CookedTextureFormat::Bc5Unorm, false);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == 16);

    std::array<std::uint8_t, 16> red{};
    std::array<std::uint8_t, 16> green{};
    for (std::size_t i = 0; i < 16; ++i) {
        red[i] = static_cast<std::uint8_t>(pixels[4 * i + 0]);
        green[i] = static_cast<std::uint8_t>(pixels[4 * i + 1]);
    }
    std::array<std::byte, 8> expectedRed{};
    std::array<std::byte, 8> expectedGreen{};
    encodeBc4Block(red, expectedRed);
    encodeBc4Block(green, expectedGreen);

    std::size_t compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        INFO("byte ", i);
        CHECK(level0[i] == expectedRed[i]);
        CHECK(level0[i + 8] == expectedGreen[i]);
        ++compared;
    }
    REQUIRE(compared == 8);
    bool halvesDiffer = false;
    for (std::size_t i = 0; i < 8; ++i) {
        halvesDiffer = halvesDiffer || expectedRed[i] != expectedGreen[i];
    }
    CHECK(halvesDiffer);
}

TEST_CASE("a partial edge block CLAMPS the sample coordinate, never zero-fills (TX34)") {
    // A 5x3 image whose column 4 differs sharply from column 3. The block at bx = 1 covers columns
    // 4..7 and rows 0..3, of which columns 5..7 and row 3 do not exist -- so every one of them is a
    // clamped copy of the nearest real texel. Zero-fill would drag the block's endpoints toward black
    // and darken the image's right and bottom edges, and this case is what says so: the zero-filled
    // gather is encoded too and asserted DIFFERENT.
    std::vector<std::byte> pixels(std::size_t{5} * 3 * 4, std::byte{0});
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 5; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 5 + x) * 4;
            const auto value = static_cast<std::byte>(x == 4 ? 250 : 20 + 10 * x);
            pixels[i + 0] = value;
            pixels[i + 1] = value;
            pixels[i + 2] = value;
            pixels[i + 3] = std::byte{255};
        }
    }
    const TextureCookResult result = cook(pixels, 5, 3, CookedTextureFormat::Bc1RgbUnorm, false);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == 2 * 8);  // ceil(5/4) x ceil(3/4) == 2 x 1 blocks

    std::array<std::uint8_t, 64> clamped{};
    std::array<std::uint8_t, 64> zeroFilled{};
    for (std::uint32_t ty = 0; ty < 4; ++ty) {
        for (std::uint32_t tx = 0; tx < 4; ++tx) {
            const std::uint32_t x = 4 + tx;
            const std::uint32_t y = ty;
            const std::size_t to = (static_cast<std::size_t>(4 * ty + tx)) * 4;
            const std::uint32_t clampedX = x < 5 ? x : 4;
            const std::uint32_t clampedY = y < 3 ? y : 2;
            const std::size_t from = (static_cast<std::size_t>(clampedY) * 5 + clampedX) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                clamped[to + channel] = static_cast<std::uint8_t>(pixels[from + channel]);
                zeroFilled[to + channel] = (x < 5 && y < 3) ? clamped[to + channel] : 0;
            }
        }
    }
    std::array<std::byte, 8> expected{};
    std::array<std::byte, 8> zeroed{};
    encodeBc1Block(clamped, expected);
    encodeBc1Block(zeroFilled, zeroed);

    std::size_t compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        INFO("byte ", i);
        CHECK(level0[8 + i] == expected[i]);  // the block at bx = 1
        ++compared;
    }
    REQUIRE(compared == 8);
    bool differsFromZeroFill = false;
    for (std::size_t i = 0; i < 8; ++i) {
        differsFromZeroFill = differsFromZeroFill || expected[i] != zeroed[i];
    }
    CHECK(differsFromZeroFill);
}

TEST_CASE("generateMips false gives levelCount 1 and an identical level 0 (TX35)") {
    const std::vector<std::byte> pixels = testImage(8, 8);
    const TextureCookResult mipped = cook(pixels, 8, 8, CookedTextureFormat::Bc1RgbSrgb, true);
    const TextureCookResult flat = cook(pixels, 8, 8, CookedTextureFormat::Bc1RgbSrgb, false);
    REQUIRE(mipped.status == TextureCookStatus::Ok);
    REQUIRE(flat.status == TextureCookStatus::Ok);
    const auto mippedParse = parseCookedTexture(mipped.bytes);
    const auto flatParse = parseCookedTexture(flat.bytes);
    REQUIRE(mippedParse.status == engine::assets::CookedTextureStatus::Ok);
    REQUIRE(flatParse.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(mippedParse.view.levelCount() == 4);
    CHECK(flatParse.view.levelCount() == 1);
    CHECK(flat.bytes.size() < mipped.bytes.size());

    const std::span<const std::byte> a = mippedParse.view.levelBytes(0);
    const std::span<const std::byte> b = flatParse.view.levelBytes(0);
    REQUIRE(a.size() == b.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i] == b[i]);
        ++compared;
    }
    REQUIRE(compared == a.size());
}

TEST_CASE("the COOK's own chain filters level p from level p-1, not from level 0 (TX35a)") {
    // TX19 proves the FILTER composes -- it halves twice by hand -- which is a DIFFERENT statement
    // from "the cook chains its own levels", and the difference is not academic: sabotage seed S16
    // pointed cookTexture's filter at level 0 for every level and TX19 stayed green. Only the byte
    // goldens caught it, and a golden catches every change equally, so nothing named the rule that
    // had broken. This case names it.
    //
    // Rgba8Unorm is what makes the chain OBSERVABLE: its level bytes ARE the filter's output,
    // verbatim, with no block encoder in between.
    std::array<std::uint8_t, 64> levels{};
    constexpr std::array<std::uint8_t, 16> QUADRANT = {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0};
    for (std::size_t y = 0; y < 4; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            levels[8 * y + x] = QUADRANT[4 * y + x];
        }
    }
    const std::vector<std::byte> base = greyImage(levels);
    const std::vector<std::byte> level1 = halve(base, 8, 8, false);
    const std::vector<std::byte> level2 = halve(level1, 4, 4, false);

    const TextureCookResult cooked = cook(base, 8, 8, CookedTextureFormat::Rgba8Unorm);
    REQUIRE(cooked.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(cooked.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    REQUIRE(parse.view.levelCount() == 4);  // 8 -> 4 -> 2 -> 1

    const std::span<const std::byte> cookedLevel1 = parse.view.levelBytes(1);
    REQUIRE(cookedLevel1.size() == level1.size());
    const std::span<const std::byte> cookedLevel2 = parse.view.levelBytes(2);
    REQUIRE(cookedLevel2.size() == level2.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < level2.size(); ++i) {
        CHECK(cookedLevel2[i] == level2[i]);
        ++compared;
    }
    REQUIRE(compared == 16);  // 2 x 2 texels x 4 channels, as a literal

    // And the two answers genuinely differ, which is what lets this case see the rule violated:
    // level 2 folded from level 1 is (1 + 1 + 1 + 0 + 2) / 4 = 1, while a resample straight from
    // level 0 would have given (2 + 2 + 2 + 1 + 8) / 16 = 0. TX19 carries the same arithmetic.
    CHECK(static_cast<std::uint8_t>(cookedLevel2[0]) == 1);
    CHECK(static_cast<std::uint8_t>(cookedLevel2[0]) != 0);
    // Level 1 is asserted too, but only for completeness: it is the ONE level a "from level 0"
    // version gets right by construction, so a case that stopped here would discriminate nothing.
    CHECK(static_cast<std::uint8_t>(cookedLevel1[0]) == 1);
}

TEST_CASE("a 1x1 image cooks identically with and without mips (TX36)") {
    // levelCount is 1 either way, so the two spellings must produce the SAME BYTES rather than merely
    // the same picture -- there is no second level for the flag to change.
    const std::vector<std::byte> pixels = testImage(1, 1);
    std::size_t checked = 0;
    for (const CookedTextureFormat format : COOK_FORMATS) {
        INFO("format ", toString(format));
        const TextureCookResult mipped = cook(pixels, 1, 1, format, true);
        const TextureCookResult flat = cook(pixels, 1, 1, format, false);
        REQUIRE(mipped.status == TextureCookStatus::Ok);
        REQUIRE(flat.status == TextureCookStatus::Ok);
        REQUIRE(mipped.bytes.size() == flat.bytes.size());
        for (std::size_t i = 0; i < mipped.bytes.size(); ++i) {
            CHECK(mipped.bytes[i] == flat.bytes[i]);
        }
        ++checked;
    }
    REQUIRE(checked == COOK_FORMATS.size());
}

TEST_CASE("a dimension over the cap is refused before anything is allocated (TX37)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult result =
        cook(pixels, engine::assets::MAX_TEXTURE_DIMENSION + 1, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(result.status == TextureCookStatus::Refused);
    CHECK(result.stats.byteSize == 0);
}

TEST_CASE("a size mismatch is refused before the level count is computed (TX38)") {
    const std::vector<std::byte> pixels = testImage(4, 4);
    const TextureCookResult result =
        cook(std::span<const std::byte>(pixels).first(4), 4, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(result.status == TextureCookStatus::Refused);
    CHECK(result.stats.levelCount == 0);
}

TEST_CASE("an artifact over MAX_COOKED_TEXTURE_BYTES is refused, naming both numbers (TX39)") {
    // 12000 x 12000 Rgba8Unorm is 576 000 000 bytes at level 0 alone, against a 536 870 912-byte cap.
    //
    // The pixel span's SIZE is what the cook validates and its CONTENTS are never read before this
    // refusal -- dimensions, then span size, then level count, then the byte cap, and only after all
    // four is a single texel touched. So the span is formed over a real but much smaller allocation
    // with a faked size, exactly as CT15 does, rather than by allocating 576 MB on every lane on every
    // run. Nothing is dereferenced in either direction.
    const std::vector<std::byte> backing(4096, std::byte{0});
    constexpr std::uint64_t DIMENSION = 12000;
    constexpr auto FAKED = static_cast<std::size_t>(DIMENSION * DIMENSION * 4);
    const std::span<const std::byte> oversized(backing.data(), FAKED);

    TextureCookInput input;
    input.width = static_cast<std::uint32_t>(DIMENSION);
    input.height = static_cast<std::uint32_t>(DIMENSION);
    input.rgba8 = oversized;
    input.format = CookedTextureFormat::Rgba8Unorm;
    const TextureCookResult result = cookTexture(input);
    checkCookInvariant(result);
    CHECK(result.status == TextureCookStatus::Refused);
    CHECK(result.message.find("536870912") != std::string::npos);  // the cap

    // The total the message must name, computed HERE from the level rule rather than copied from the
    // cook. Note that it is 767 998 156 and NOT 768 000 000: the chain's tail is
    // 12000, 6000, 3000, 1500, 750, 375, 187, 93, 46, 23, 11, 5, 2, 1, and the odd extents floor,
    // so the geometric 4/3 estimate is a few thousand bytes high.
    std::uint64_t expectedTotal = 0;
    for (std::uint32_t level = 0; level < mipLevelCount(12000, 12000); ++level) {
        const std::uint64_t extent = std::max<std::uint64_t>(1, DIMENSION >> level);
        expectedTotal += extent * extent * 4;
    }
    CHECK(expectedTotal == 767998156);
    CHECK(expectedTotal > engine::assets::MAX_COOKED_TEXTURE_BYTES);
    // The ARTIFACT is that plus its prefix: an 80-byte header, a 14-level index at 24 bytes each, the
    // 92-byte descriptor for an unpacked format and the fixed 120-byte key/value region, which is 628
    // and is already 4-aligned, so this format's single padding site is empty.
    CHECK(mipLevelCount(12000, 12000) == 14);
    const std::uint64_t prefix = engine::assets::KTX2_HEADER_BYTES + 14 * engine::assets::KTX2_LEVEL_RECORD_BYTES +
                                 engine::assets::KTX2_DFD_BYTES_4_SAMPLE + engine::assets::KTX2_KVD_BYTES;
    CHECK(prefix == 628);
    CHECK(prefix % cookedTextureLevelAlignment(CookedTextureFormat::Rgba8Unorm) == 0);
    CHECK(result.message.find(std::to_string(prefix + expectedTotal)) != std::string::npos);
}

TEST_CASE("the cap is on BYTES, not on dimension (TX40)") {
    // At one fixed size the two format families produce wildly different artifacts, which is what
    // makes "the cap is on bytes" mean something: Rgba8 is 4 bytes per texel and BC1 is half a byte,
    // so the same dimensions give an eightfold difference.
    const std::vector<std::byte> pixels = testImage(16, 16);
    const TextureCookResult rgba = cook(pixels, 16, 16, CookedTextureFormat::Rgba8Unorm);
    const TextureCookResult bc1 = cook(pixels, 16, 16, CookedTextureFormat::Bc1RgbUnorm);
    REQUIRE(rgba.status == TextureCookStatus::Ok);
    REQUIRE(bc1.status == TextureCookStatus::Ok);
    // Level data only -- the 80-byte header, the index, the descriptor and the 120-byte key/value
    // region are the same order of magnitude as a small image's payload and would drown the ratio.
    const auto rgbaParse = parseCookedTexture(rgba.bytes);
    const auto bc1Parse = parseCookedTexture(bc1.bytes);
    REQUIRE(rgbaParse.status == engine::assets::CookedTextureStatus::Ok);
    REQUIRE(bc1Parse.status == engine::assets::CookedTextureStatus::Ok);
    // LEVEL 0 is where the ratio is exact: 16 x 16 x 4 = 1024 against 4 x 4 blocks x 8 = 128.
    CHECK(rgbaParse.view.levelBytes(0).size() == 1024);
    CHECK(bc1Parse.view.levelBytes(0).size() == 128);
    CHECK(rgbaParse.view.levelBytes(0).size() == 8 * bc1Parse.view.levelBytes(0).size());

    std::uint64_t rgbaLevels = 0;
    std::uint64_t bc1Levels = 0;
    for (std::uint32_t level = 0; level < 5; ++level) {
        rgbaLevels += rgbaParse.view.levelBytes(level).size();
        bc1Levels += bc1Parse.view.levelBytes(level).size();
    }
    CHECK(rgbaLevels == 1364);  // (256 + 64 + 16 + 4 + 1) texels x 4
    CHECK(bc1Levels == 184);    // (16 + 4 + 1 + 1 + 1) blocks x 8
    // Over the WHOLE chain the ratio is only 7.4x rather than 8x, because a 2x2 and a 1x1 level each
    // still pay for a full 4x4 block. That is the format's, not the cook's, and it is recorded rather
    // than asserted away.
    CHECK(rgbaLevels > 7 * bc1Levels);
    CHECK(rgbaLevels < 8 * bc1Levels);

    // AND the arithmetic at the dimension TX39 refuses, computed here from the format's own block
    // rule rather than by cooking: 12000 x 12000 is over the cap for Rgba8 and comfortably under it
    // for BC1. THE BC1 COOK ITSELF IS DELIBERATELY NOT RUN -- it would need a 576 MB input buffer and
    // twelve million block encodes on every lane on every run, which is not a unit test. TX39 is the
    // behavioural half; this is the arithmetic half, and the two together are what AC-37 asks for.
    constexpr std::uint64_t DIMENSION = 12000;
    std::uint64_t rgbaTotal = 0;
    std::uint64_t bc1Total = 0;
    for (std::uint32_t level = 0; level < mipLevelCount(DIMENSION, DIMENSION); ++level) {
        const std::uint64_t levelWidth = std::max<std::uint64_t>(1, DIMENSION >> level);
        const std::uint64_t levelHeight = levelWidth;
        rgbaTotal += levelWidth * levelHeight * 4;
        bc1Total += ((levelWidth + 3) / 4) * ((levelHeight + 3) / 4) * 8;
    }
    CHECK(rgbaTotal > engine::assets::MAX_COOKED_TEXTURE_BYTES);
    CHECK(bc1Total < engine::assets::MAX_COOKED_TEXTURE_BYTES);
    MESSAGE("at 12000x12000 with a full chain: Rgba8Unorm " << rgbaTotal << " B, Bc1RgbUnorm " << bc1Total << " B, cap "
                                                            << engine::assets::MAX_COOKED_TEXTURE_BYTES << " B");

    // AND THE SOURCE-TEXT HALF, which is the only thing in this tree that can see the rule violated.
    // Everything above is arithmetic the TEST computes: nothing here ever calls cookTexture at a
    // dimension where the two families disagree, because doing so needs a 576 MB input and twelve
    // million block encodes on every lane on every run, and the RGBA8/BC1 crossover cannot be lowered
    // (RGBA8 only passes the 512 MiB cap above ~10 033 texels a side). So a cap check rewritten as a
    // DIMENSION test -- or one that merely gains a dimension clause beside the byte comparison --
    // refuses exactly what TX39 expects it to refuse, with the same message, and every case in this
    // tree stays green. Sabotage seeds S53 and S53b are those two edits and S53b reddened NOTHING
    // before this assertion existed.
    //
    // The comment-stripped source text is the answer, the CM50/TX48 precedent: assert the cap check
    // compares the computed byte TOTAL and names neither axis.
    const std::string stripped = strippedAssetsSource("texture_cook.cpp");
    const std::size_t at = stripped.find("if (totalBytes");
    REQUIRE(at != std::string::npos);
    CHECK(stripped.find("if (totalBytes", at + 1) == std::string::npos);  // exactly one such check
    const std::size_t brace = stripped.find('{', at);
    REQUIRE(brace != std::string::npos);
    const std::string condition = stripped.substr(at, brace - at);
    CHECK(condition.find("MAX_COOKED_TEXTURE_BYTES") != std::string::npos);
    CHECK(condition.find("width") == std::string::npos);
    CHECK(condition.find("height") == std::string::npos);
}

TEST_CASE("the same input cooked twice is byte-identical, for all eight formats (TX41)") {
    const std::vector<std::byte> pixels = testImage(9, 7);
    std::size_t checked = 0;
    for (const CookedTextureFormat format : COOK_FORMATS) {
        INFO("format ", toString(format));
        const TextureCookResult first = cook(pixels, 9, 7, format);
        const TextureCookResult second = cook(pixels, 9, 7, format);
        REQUIRE(first.status == TextureCookStatus::Ok);
        REQUIRE(first.bytes.size() == second.bytes.size());
        for (std::size_t i = 0; i < first.bytes.size(); ++i) {
            CHECK(first.bytes[i] == second.bytes[i]);
        }
        ++checked;
    }
    REQUIRE(checked == COOK_FORMATS.size());
}

TEST_CASE("the output does not depend on the source buffer's address (TX42)") {
    // Cheap, and it catches an accidental dependence on the source pointer -- an alignment-dependent
    // fast path, a hash of an address, an uninitialised read that happens to be stable in one buffer.
    const std::vector<std::byte> pixels = testImage(6, 6);
    std::vector<std::byte> shifted(pixels.size() + 1, std::byte{0});
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        shifted[i + 1] = pixels[i];
    }
    const TextureCookResult a = cook(pixels, 6, 6, CookedTextureFormat::Bc3Srgb);
    const TextureCookResult b =
        cook(std::span<const std::byte>(shifted).subspan(1), 6, 6, CookedTextureFormat::Bc3Srgb);
    REQUIRE(a.status == TextureCookStatus::Ok);
    REQUIRE(a.bytes.size() == b.bytes.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < a.bytes.size(); ++i) {
        CHECK(a.bytes[i] == b.bytes[i]);
        ++compared;
    }
    REQUIRE(compared == a.bytes.size());
}

TEST_CASE("the stats match independently computed values (TX43)") {
    const std::vector<std::byte> pixels = testImage(5, 3);
    const TextureCookResult result = cook(pixels, 5, 3, CookedTextureFormat::Bc5Unorm);
    REQUIRE(result.status == TextureCookStatus::Ok);
    CHECK(result.stats.levelCount == 3);
    CHECK(result.stats.sourceByteSize == 5 * 3 * 4);
    CHECK(result.stats.byteSize == result.bytes.size());
    // Levels are 5x3, 2x1 and 1x1, so the block counts are 2 x 1, 1 x 1 and 1 x 1.
    CHECK(result.stats.blockCount == 2 + 1 + 1);
    // And the whole file is header + index + descriptor + key/value + padding + level data:
    //   80 + 24*3 = 152; BC5's descriptor is 60, so the key/value region starts at 212 and ends at
    //   332; BC5 aligns to 16, so align(332, 16) = 336 and the single padding site is 4 bytes; the
    //   levels are 16, 16 and 32 bytes, smallest first, so the total is 336 + 64 = 400.
    CHECK(result.bytes.size() == 400);
}

TEST_CASE("v1's cook emits no warning on any path (TX44)") {
    // The honest statement of the field's current state. checkCookInvariant already asserts it on
    // every call in this TU; this case says it deliberately, so the day a warning IS emitted this is
    // the case that has to be changed on purpose.
    const std::vector<std::byte> pixels = testImage(4, 4);
    std::size_t checked = 0;
    for (const CookedTextureFormat format : COOK_FORMATS) {
        const TextureCookResult result = cook(pixels, 4, 4, format);
        CHECK(result.warnings.empty());
        CHECK(result.warningTotal == 0);
        ++checked;
    }
    REQUIRE(checked == COOK_FORMATS.size());
    const TextureCookResult refused = cook(pixels, 0, 4, CookedTextureFormat::Bc1RgbUnorm);
    CHECK(refused.warnings.empty());
    CHECK(refused.warningTotal == 0);
}

TEST_CASE("a constant alpha channel survives BC3 exactly (TX45)") {
    // BC4's six-value mode reproduces a constant EXACTLY: r0 == r1 == the constant with all-zero
    // indices, and index 0 still decodes to r0. Asserted on the emitted bytes rather than through a
    // decoder, because the emitted form is unambiguous and hand-checkable.
    std::size_t checked = 0;
    for (const std::uint8_t alpha : {std::uint8_t{0}, std::uint8_t{128}, std::uint8_t{255}}) {
        std::vector<std::byte> pixels(std::size_t{4} * 4 * 4, std::byte{0});
        for (std::size_t i = 0; i < 16; ++i) {
            pixels[4 * i + 0] = static_cast<std::byte>(i * 16);
            pixels[4 * i + 1] = static_cast<std::byte>(255 - i * 16);
            pixels[4 * i + 2] = static_cast<std::byte>(i * 7);
            pixels[4 * i + 3] = static_cast<std::byte>(alpha);
        }
        const TextureCookResult result = cook(pixels, 4, 4, CookedTextureFormat::Bc3Unorm, false);
        REQUIRE(result.status == TextureCookStatus::Ok);
        const auto parse = parseCookedTexture(result.bytes);
        REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
        const std::span<const std::byte> level0 = parse.view.levelBytes(0);
        REQUIRE(level0.size() == 16);
        INFO("alpha ", alpha);
        CHECK(level0[0] == static_cast<std::byte>(alpha));  // r0
        CHECK(level0[1] == static_cast<std::byte>(alpha));  // r1 -- equal, so six-value mode
        for (std::size_t i = 2; i < 8; ++i) {
            CHECK(level0[i] == std::byte{0});  // all-zero indices, every texel exactly r0
        }
        ++checked;
    }
    REQUIRE(checked == 3);
}

TEST_CASE("a single-colour image takes the degenerate arm at every level (TX46)") {
    std::vector<std::byte> pixels(std::size_t{8} * 8 * 4, std::byte{0});
    for (std::size_t i = 0; i < 64; ++i) {
        pixels[4 * i + 0] = std::byte{200};
        pixels[4 * i + 1] = std::byte{100};
        pixels[4 * i + 2] = std::byte{50};
        pixels[4 * i + 3] = std::byte{255};
    }
    const TextureCookResult result = cook(pixels, 8, 8, CookedTextureFormat::Bc1RgbUnorm);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    std::size_t blocksChecked = 0;
    for (std::uint32_t level = 0; level < parse.view.levelCount(); ++level) {
        const std::span<const std::byte> data = parse.view.levelBytes(level);
        for (std::size_t block = 0; block < data.size(); block += 8) {
            INFO("level ", level, " block at ", block);
            CHECK(data[block + 0] == data[block + 2]);  // c0 == c1
            CHECK(data[block + 1] == data[block + 3]);
            for (std::size_t i = 4; i < 8; ++i) {
                CHECK(data[block + i] == std::byte{0});  // all-zero indices
            }
            ++blocksChecked;
        }
    }
    // 4 + 1 + 1 + 1 blocks across the four levels of an 8x8 chain.
    REQUIRE(blocksChecked == 7);
    // And every level is the SAME block, because a filtered constant is that constant.
    CHECK(parse.view.levelBytes(0)[0] == parse.view.levelBytes(3)[0]);
    CHECK(parse.view.levelBytes(0)[1] == parse.view.levelBytes(3)[1]);
}

TEST_CASE("a 1x1 BC1 cook is one block of fifteen clamped copies (TX47)") {
    std::vector<std::byte> pixels(4, std::byte{0});
    pixels[0] = std::byte{200};
    pixels[1] = std::byte{100};
    pixels[2] = std::byte{50};
    pixels[3] = std::byte{255};
    const TextureCookResult result = cook(pixels, 1, 1, CookedTextureFormat::Bc1RgbUnorm);
    REQUIRE(result.status == TextureCookStatus::Ok);
    const auto parse = parseCookedTexture(result.bytes);
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(parse.view.levelCount() == 1);
    const std::span<const std::byte> level0 = parse.view.levelBytes(0);
    REQUIRE(level0.size() == 8);
    // All sixteen texels are the same colour after clamping, so this is the degenerate arm and the
    // block must equal a flat block of that colour, computed here by calling the encoder directly.
    std::array<std::uint8_t, 64> flat{};
    for (std::size_t i = 0; i < 16; ++i) {
        flat[4 * i + 0] = 200;
        flat[4 * i + 1] = 100;
        flat[4 * i + 2] = 50;
        flat[4 * i + 3] = 255;
    }
    std::array<std::byte, 8> expected{};
    encodeBc1Block(flat, expected);
    std::size_t compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(level0[i] == expected[i]);
        ++compared;
    }
    REQUIRE(compared == 8);
    CHECK(level0[0] == level0[2]);  // c0 == c1
    CHECK(level0[1] == level0[3]);
}

TEST_CASE("the byte-cap check comes BEFORE the allocation, asserted in source text (TX48)") {
    // A SOURCE-TEXT case, and it has to be. A 512 MB allocation of virtual address space simply
    // succeeds on a 64-bit host -- task 3.3.1 measured exactly that at 274 GB with the reserves moved
    // deliberately above their checks -- so NO RUNTIME CASE IN THIS TREE CAN SEE THIS ORDERING
    // VIOLATED. Reversing the two lines is a real defect that a fully green suite would ship.
    //
    // Comments are stripped first, so the prose above the check (which names the constant) cannot
    // stand in for the check itself.
    const std::string stripped = strippedAssetsSource("texture_cook.cpp");
    // ANTI-VACUITY: the helper already asserts the strip removed something and kept something; this
    // is the one that says the text is the file we meant.
    CHECK(stripped.find("cookTexture") != std::string::npos);

    const std::size_t check = stripped.find("> MAX_COOKED_TEXTURE_BYTES");
    const std::size_t allocation = stripped.find("std::vector<std::byte> artifact(");
    REQUIRE(check != std::string::npos);
    REQUIRE(allocation != std::string::npos);
    CHECK(check < allocation);
    // Exactly one allocation site, so "before the allocation" is unambiguous.
    CHECK(stripped.find("std::vector<std::byte> artifact(", allocation + 1) == std::string::npos);
}
