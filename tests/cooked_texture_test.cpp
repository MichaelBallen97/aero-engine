// tests/cooked_texture_test.cpp -- task 3.3.2: the cooked texture container v1, a strict subset of
// Khronos KTX2. A TU of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 10's own rules -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly one field of something already valid.
//
// THE PREFIX IS `CT`, and `TC` is TAKEN: tests/editor/thumbnail_cache_test.cpp owns TC1..TC47. Two
// binaries do not collide at link time, but in this repo a case id is a GLOBAL identifier cited from
// the sabotage matrix, the validation page, docs/10 and CLAUDE.md, and every one of those is
// grepped.
#include <aero/assets/cooked_texture.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap, a fifth occurrence): doctest stringifies the
// std::string_view operands of the toString and status-label CHECKs below through the stdlib's
// operator<<(std::ostream&, std::string_view), which MS STL defines inline in <string_view> against
// an INCOMPLETE std::basic_ostream -- only <ostream> completes it. libc++ and libstdc++ are
// self-sufficient, so omitting it builds clean on macOS and Linux and fails only on the Windows
// lane, with errors pointing inside the STL headers rather than at the CHECK.
#include <ostream>
#include <span>
#include <string_view>

using engine::assets::cookedTextureBlockBytes;
using engine::assets::cookedTextureBlockHeight;
using engine::assets::cookedTextureBlockWidth;
using engine::assets::cookedTextureDescriptorBytes;
using engine::assets::CookedTextureFormat;
using engine::assets::cookedTextureLevelAlignment;
using engine::assets::CookedTextureStatus;
using engine::assets::cookedTextureStatusLabel;
using engine::assets::isCookedTextureFormat;
using engine::assets::isSrgbCookedFormat;
using engine::assets::toString;

namespace {

// The eight formats in ENUM ORDER. Every table-driven case below walks this and asserts it walked
// all of it -- an anti-vacuity guard, because a table-driven case that silently iterates nothing
// passes with zero assertions.
constexpr std::array<CookedTextureFormat, 8> ALL_FORMATS = {
    CookedTextureFormat::Rgba8Unorm, CookedTextureFormat::Rgba8Srgb, CookedTextureFormat::Bc1RgbUnorm,
    CookedTextureFormat::Bc1RgbSrgb, CookedTextureFormat::Bc3Unorm,  CookedTextureFormat::Bc3Srgb,
    CookedTextureFormat::Bc4Unorm,   CookedTextureFormat::Bc5Unorm,
};

// Euclid, written out here rather than calling std::lcm -- CT6's whole point is to compute the
// alignment rule INDEPENDENTLY of the implementation under test. Asserting
// `levelAlignment == blockBytes` instead would agree with a version that dropped the lcm entirely.
[[nodiscard]] constexpr std::uint32_t gcdRef(std::uint32_t a, std::uint32_t b) noexcept {
    while (b != 0) {
        const std::uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}
[[nodiscard]] constexpr std::uint32_t lcmRef(std::uint32_t a, std::uint32_t b) noexcept { return a / gcdRef(a, b) * b; }

// The KTX2 key/value padding rule, recomputed in the test for CT8.
[[nodiscard]] constexpr std::size_t align4Ref(std::size_t v) noexcept { return ((v + 3) / 4) * 4; }

// A DFD's little-endian u16 at `at`, read by hand. The container's own get* primitives take
// std::span<const std::byte> and the tables are std::uint8_t, and going through a cast to reuse them
// would be a wider change than reading two bytes here.
[[nodiscard]] std::uint32_t dfdU16(std::span<const std::uint8_t> dfd, std::size_t at) {
    return static_cast<std::uint32_t>(dfd[at]) | (static_cast<std::uint32_t>(dfd[at + 1]) << 8U);
}
[[nodiscard]] std::uint32_t dfdU32(std::span<const std::uint8_t> dfd, std::size_t at) {
    return dfdU16(dfd, at) | (dfdU16(dfd, at + 2) << 16U);
}

}  // namespace

TEST_CASE("the eight CookedTextureFormat values ARE Khronos's VkFormat numbers (CT1)") {
    // Asserted one by one as integer literals rather than through a table: these values are a frozen
    // EXTERNAL contract, and a table would let a wrong pair agree with itself.
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Rgba8Unorm) == 37U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Rgba8Srgb) == 43U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc1RgbUnorm) == 131U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc1RgbSrgb) == 132U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc3Unorm) == 137U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc3Srgb) == 138U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc4Unorm) == 139U);
    CHECK(static_cast<std::uint32_t>(CookedTextureFormat::Bc5Unorm) == 141U);
    // There is no Bc4Srgb and no Bc5Srgb because Vulkan defines neither: 140 is BC4_SNORM and 142 is
    // BC5_SNORM, so there is no sRGB value between or beside them to name.
    CHECK(ALL_FORMATS.size() == 8);
}

TEST_CASE("isCookedTextureFormat accepts exactly the eight and nothing else (CT2)") {
    std::size_t accepted = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        CHECK(isCookedTextureFormat(static_cast<std::uint32_t>(f)));
        ++accepted;
    }
    REQUIRE(accepted == ALL_FORMATS.size());  // anti-vacuity

    // 133/134 are BC1_RGBA (punch-through alpha) and 140/142 are the SNORM siblings: all four are
    // real Vulkan formats, deliberately outside our subset, and all four must be refused.
    constexpr std::array<std::uint32_t, 7> REJECTED = {
        0U, 36U, 133U, 134U, 140U, 142U, std::numeric_limits<std::uint32_t>::max()};
    std::size_t rejected = 0;
    for (const std::uint32_t v : REJECTED) {
        CHECK_FALSE(isCookedTextureFormat(v));
        ++rejected;
    }
    REQUIRE(rejected == REJECTED.size());
}

TEST_CASE("isSrgbCookedFormat is true for exactly the three sRGB formats (CT3)") {
    CHECK(isSrgbCookedFormat(CookedTextureFormat::Rgba8Srgb));
    CHECK(isSrgbCookedFormat(CookedTextureFormat::Bc1RgbSrgb));
    CHECK(isSrgbCookedFormat(CookedTextureFormat::Bc3Srgb));
    CHECK_FALSE(isSrgbCookedFormat(CookedTextureFormat::Rgba8Unorm));
    CHECK_FALSE(isSrgbCookedFormat(CookedTextureFormat::Bc1RgbUnorm));
    CHECK_FALSE(isSrgbCookedFormat(CookedTextureFormat::Bc3Unorm));
    CHECK_FALSE(isSrgbCookedFormat(CookedTextureFormat::Bc4Unorm));
    CHECK_FALSE(isSrgbCookedFormat(CookedTextureFormat::Bc5Unorm));

    std::size_t srgbCount = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        if (isSrgbCookedFormat(f)) {
            ++srgbCount;
        }
    }
    CHECK(srgbCount == 3);
}

TEST_CASE("block extents are 1x1 for the uncompressed pair and 4x4 for the six BCn formats (CT4)") {
    std::size_t checked = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        const bool uncompressed = f == CookedTextureFormat::Rgba8Unorm || f == CookedTextureFormat::Rgba8Srgb;
        const std::uint32_t expected = uncompressed ? 1U : 4U;
        CHECK(cookedTextureBlockWidth(f) == expected);
        CHECK(cookedTextureBlockHeight(f) == expected);
        ++checked;
    }
    REQUIRE(checked == ALL_FORMATS.size());
}

TEST_CASE("cookedTextureBlockBytes is 4/4/8/8/16/16/8/16 in enum order (CT5)") {
    // Written out in enum order as literals, NOT derived from the format: the byte size of a block is
    // the number every level's byteLength is computed from, so it is pinned rather than restated.
    constexpr std::array<std::uint32_t, 8> EXPECTED = {4, 4, 8, 8, 16, 16, 8, 16};
    std::size_t checked = 0;
    for (std::size_t i = 0; i < ALL_FORMATS.size(); ++i) {
        CHECK(cookedTextureBlockBytes(ALL_FORMATS[i]) == EXPECTED[i]);
        ++checked;
    }
    REQUIRE(checked == ALL_FORMATS.size());
}

TEST_CASE("cookedTextureLevelAlignment is lcm(blockBytes, 4), computed independently (CT6)") {
    // The two agree for all eight of today's formats -- which is exactly why this case computes the
    // lcm with its own Euclid rather than asserting the coincidence. A version that returned
    // blockBytes directly would pass an `== blockBytes` assertion and be silently wrong for the
    // first format whose block size is not a multiple of 4.
    std::size_t checked = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        const std::uint32_t blockBytes = cookedTextureBlockBytes(f);
        CHECK(cookedTextureLevelAlignment(f) == lcmRef(blockBytes, 4U));
        CHECK(cookedTextureLevelAlignment(f) == blockBytes);  // the coincidence, recorded as such
        ++checked;
    }
    REQUIRE(checked == ALL_FORMATS.size());

    // The reference lcm is not vacuous: it must disagree with `a` for a block size that is not a
    // multiple of 4, which is the case the assertion above exists to survive.
    CHECK(lcmRef(12U, 4U) == 12U);
    CHECK(lcmRef(6U, 4U) == 12U);
    CHECK(lcmRef(1U, 4U) == 4U);
}

TEST_CASE("toString is total and distinct across the eight formats (CT7)") {
    std::array<std::string_view, 8> labels{};
    std::size_t checked = 0;
    for (std::size_t i = 0; i < ALL_FORMATS.size(); ++i) {
        labels[i] = toString(ALL_FORMATS[i]);
        CHECK_FALSE(labels[i].empty());
        CHECK(labels[i] != std::string_view{"Unknown"});
        ++checked;
    }
    REQUIRE(checked == ALL_FORMATS.size());

    std::size_t pairs = 0;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        for (std::size_t j = i + 1; j < labels.size(); ++j) {
            CHECK(labels[i] != labels[j]);
            ++pairs;
        }
    }
    REQUIRE(pairs == 28);  // 8 choose 2

    // The status labels get the same treatment in one place rather than a case of their own.
    CHECK(cookedTextureStatusLabel(CookedTextureStatus::Ok) == std::string_view{"Ok"});
    CHECK(cookedTextureStatusLabel(CookedTextureStatus::BadDescriptor) == std::string_view{"Bad descriptor"});
    CHECK(cookedTextureStatusLabel(CookedTextureStatus::Supercompressed) == std::string_view{"Supercompressed"});
}

TEST_CASE("the record sizes are 80/24/120 and the KVD total is a closed form (CT8)") {
    CHECK(engine::assets::KTX2_HEADER_BYTES == 80);
    CHECK(engine::assets::KTX2_LEVEL_RECORD_BYTES == 24);
    CHECK(engine::assets::KTX2_KVD_BYTES == 120);
    CHECK(engine::assets::KTX2_DFD_BYTES_1_SAMPLE == 44);
    CHECK(engine::assets::KTX2_DFD_BYTES_2_SAMPLE == 60);
    CHECK(engine::assets::KTX2_DFD_BYTES_4_SAMPLE == 92);

    // Recomputed from the three keys' own spellings, so a future rename of a key -- or a bump of
    // COOKED_TEXTURE_COOKER_VERSION that lengthens the writer string -- moves this case rather than
    // silently desynchronising kvdByteLength from the bytes after it.
    constexpr std::string_view GUID_KEY = "AeroSourceGuid";
    constexpr std::string_view ORIENTATION_KEY = "KTXorientation";
    constexpr std::string_view WRITER_KEY = "KTXwriter";
    const std::size_t guidRecord = 4 + align4Ref(GUID_KEY.size() + 1 + engine::GUID_TEXT_LENGTH + 1);
    const std::size_t orientationRecord = 4 + align4Ref(ORIENTATION_KEY.size() + 1 + 2 + 1);
    const std::size_t writerRecord =
        4 + align4Ref(WRITER_KEY.size() + 1 + engine::assets::COOKED_TEXTURE_WRITER_ID.size() + 1);
    CHECK(guidRecord == 52);
    CHECK(orientationRecord == 24);
    CHECK(writerRecord == 44);
    CHECK(guidRecord + orientationRecord + writerRecord == engine::assets::KTX2_KVD_BYTES);

    CHECK(engine::assets::COOKED_TEXTURE_WRITER_ID.size() == 26);
    CHECK(engine::assets::COOKED_TEXTURE_COOKER_VERSION == 1U);
    CHECK(engine::assets::KTX2_IDENTIFIER.size() == 12);
    // «KTX 20» plus the CR LF SUB LF trailer, which is what makes a text-mode write or a 7-bit
    // transport corrupt the file at byte 0 rather than silently deeper in.
    constexpr std::array<std::uint8_t, 12> EXPECTED_IDENTIFIER = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                                  0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    std::size_t identifierBytes = 0;
    for (std::size_t i = 0; i < EXPECTED_IDENTIFIER.size(); ++i) {
        CHECK(engine::assets::KTX2_IDENTIFIER[i] == EXPECTED_IDENTIFIER[i]);
        ++identifierBytes;
    }
    REQUIRE(identifierBytes == 12);
}

TEST_CASE("MAX_TEXTURE_LEVELS is floor(log2(MAX_TEXTURE_DIMENSION)) + 1, computed here (CT9)") {
    // Computed with a shift loop rather than restated, so a future change to MAX_TEXTURE_DIMENSION
    // that forgets the level cap reddens this case.
    std::uint32_t levels = 1;
    for (std::uint32_t v = engine::assets::MAX_TEXTURE_DIMENSION; v > 1; v >>= 1U) {
        ++levels;
    }
    CHECK(levels == engine::assets::MAX_TEXTURE_LEVELS);
    CHECK(engine::assets::MAX_TEXTURE_DIMENSION == 16384U);
    CHECK(engine::assets::MAX_TEXTURE_LEVELS == 15U);
    CHECK(engine::assets::MAX_COOKED_TEXTURE_BYTES == 512ULL * 1024 * 1024);
}

TEST_CASE("every DFD table's length matches its sample count and its own dfdTotalSize (CT10)") {
    // sampleCount is derived from the DFD itself: the basic block is six words (24 bytes) and every
    // sample is 16, so (descriptorBlockSize - 24) / 16 is the count the file declares. Checking it
    // against the count the FORMAT demands is what catches a table with the right length and the
    // wrong number of samples.
    struct Row {
        CookedTextureFormat format;
        std::size_t length;
        std::uint32_t samples;
    };
    constexpr std::array<Row, 8> ROWS = {
        Row{CookedTextureFormat::Rgba8Unorm, 92, 4},  Row{CookedTextureFormat::Rgba8Srgb, 92, 4},
        Row{CookedTextureFormat::Bc1RgbUnorm, 44, 1}, Row{CookedTextureFormat::Bc1RgbSrgb, 44, 1},
        Row{CookedTextureFormat::Bc3Unorm, 60, 2},    Row{CookedTextureFormat::Bc3Srgb, 60, 2},
        Row{CookedTextureFormat::Bc4Unorm, 44, 1},    Row{CookedTextureFormat::Bc5Unorm, 60, 2},
    };
    std::size_t checked = 0;
    for (const Row& row : ROWS) {
        const std::span<const std::uint8_t> dfd = cookedTextureDescriptorBytes(row.format);
        REQUIRE(dfd.size() == row.length);
        CHECK(dfdU32(dfd, 0) == static_cast<std::uint32_t>(row.length));
        const std::uint32_t descriptorBlockSize = dfdU16(dfd, 10);
        REQUIRE(descriptorBlockSize >= 24);
        CHECK((descriptorBlockSize - 24U) / 16U == row.samples);
        CHECK((descriptorBlockSize - 24U) % 16U == 0U);
        ++checked;
    }
    REQUIRE(checked == ROWS.size());
}

TEST_CASE("every DFD's descriptorBlockSize, versionNumber and bytesPlane0 agree with the format (CT11)") {
    std::size_t checked = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        const std::span<const std::uint8_t> dfd = cookedTextureDescriptorBytes(f);
        REQUIRE(dfd.size() >= 28);
        // word 1 is versionNumber in the low half, descriptorBlockSize in the high half.
        CHECK(dfdU16(dfd, 8) == 2U);  // KHR_DF_VERSIONNUMBER_1_4, which did not bump from 1.3
        CHECK(dfdU16(dfd, 10) == dfd.size() - 4);
        // word 4's low byte is bytesPlane0 -- the block byte size, which must equal ours exactly, or
        // a consumer sizing its upload from the DFD disagrees with one sizing it from the vkFormat.
        CHECK(dfd[20] == cookedTextureBlockBytes(f));
        // word 3 holds each texel-block extent MINUS ONE.
        CHECK(dfd[16] == cookedTextureBlockWidth(f) - 1U);
        CHECK(dfd[17] == cookedTextureBlockHeight(f) - 1U);
        CHECK(dfd[18] == 0U);  // depth 1
        CHECK(dfd[19] == 0U);  // the fourth extent, 1
        // word 2's transferFunction byte IS the colour space, and it is the only place in the file
        // besides vkFormat where it appears. KTX2 makes SRGB a MUST for any *_SRGB* format.
        CHECK(dfd[14] == (isSrgbCookedFormat(f) ? 2U : 1U));
        CHECK(dfd[13] == 1U);  // primaries BT709
        CHECK(dfd[15] == 0U);  // flags: straight (not premultiplied) alpha
        ++checked;
    }
    REQUIRE(checked == ALL_FORMATS.size());
}

TEST_CASE("the UNORM/SRGB DFD pairs differ in ONE byte for BC1 and TWO for BC3 and RGBA8 (CT12)") {
    // THE CASE THAT PINS THE TWO CORRECTIONS. dfdutils' setChannelFlags sets
    // KHR_DF_SAMPLE_DATATYPE_LINEAR (0x10) on an sRGB format's ALPHA sample, and both
    // KHR_DF_CHANNEL_BC3_ALPHA and KHR_DF_CHANNEL_RGBSDA_ALPHA are 15 -- so 15 | 0x10 = 0x1F. The
    // KTX2 spec makes it a MUST. Our own parser compares the DFD against the same table our writer
    // emits, so getting this wrong ships files `ktx validate` rejects WITH THE WHOLE SUITE GREEN.
    // This case must fail if either sRGB table is ever "simplified" into a one-byte patch.
    struct Pair {
        CookedTextureFormat unorm;
        CookedTextureFormat srgb;
        std::size_t differences;
    };
    // THREE pairs, not four: BC4 and BC5 have no sRGB sibling at any Vulkan value, so there is no
    // fourth diff to assert. The count is asserted below so that stays visible.
    constexpr std::array<Pair, 3> PAIRS = {
        Pair{CookedTextureFormat::Bc1RgbUnorm, CookedTextureFormat::Bc1RgbSrgb, 1},
        Pair{CookedTextureFormat::Bc3Unorm, CookedTextureFormat::Bc3Srgb, 2},
        Pair{CookedTextureFormat::Rgba8Unorm, CookedTextureFormat::Rgba8Srgb, 2},
    };
    std::size_t checked = 0;
    for (const Pair& pair : PAIRS) {
        const std::span<const std::uint8_t> a = cookedTextureDescriptorBytes(pair.unorm);
        const std::span<const std::uint8_t> b = cookedTextureDescriptorBytes(pair.srgb);
        REQUIRE(a.size() == b.size());
        std::size_t differing = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                ++differing;
            }
        }
        CHECK(differing == pair.differences);
        ++checked;
    }
    REQUIRE(checked == PAIRS.size());

    // The exact indices and values, spelled out, because "two bytes differ" would also be true of
    // two WRONG bytes.
    const std::span<const std::uint8_t> bc1U = cookedTextureDescriptorBytes(CookedTextureFormat::Bc1RgbUnorm);
    const std::span<const std::uint8_t> bc1S = cookedTextureDescriptorBytes(CookedTextureFormat::Bc1RgbSrgb);
    CHECK(bc1U[14] == 0x01);
    CHECK(bc1S[14] == 0x02);

    const std::span<const std::uint8_t> bc3U = cookedTextureDescriptorBytes(CookedTextureFormat::Bc3Unorm);
    const std::span<const std::uint8_t> bc3S = cookedTextureDescriptorBytes(CookedTextureFormat::Bc3Srgb);
    CHECK(bc3U[14] == 0x01);
    CHECK(bc3S[14] == 0x02);
    CHECK(bc3U[31] == 0x0F);  // BC3_ALPHA, no qualifier
    CHECK(bc3S[31] == 0x1F);  // BC3_ALPHA | KHR_DF_SAMPLE_DATATYPE_LINEAR -- CORRECTION C1

    const std::span<const std::uint8_t> rgbaU = cookedTextureDescriptorBytes(CookedTextureFormat::Rgba8Unorm);
    const std::span<const std::uint8_t> rgbaS = cookedTextureDescriptorBytes(CookedTextureFormat::Rgba8Srgb);
    CHECK(rgbaU[14] == 0x01);
    CHECK(rgbaS[14] == 0x02);
    CHECK(rgbaU[79] == 0x0F);  // RGBSDA_ALPHA, sample 3 starts at 28 + 3*16 = 76
    CHECK(rgbaS[79] == 0x1F);  // RGBSDA_ALPHA | KHR_DF_SAMPLE_DATATYPE_LINEAR -- CORRECTION C2

    // BC4 and BC5 have no sRGB sibling, so nothing in the eight carries their colour model with
    // transferFunction 2. Asserted rather than assumed, because "complete the set" is the obvious
    // wrong instinct and this is where it would be caught.
    std::size_t srgbBlockFormats = 0;
    for (const CookedTextureFormat f : ALL_FORMATS) {
        if (isSrgbCookedFormat(f)) {
            const std::span<const std::uint8_t> dfd = cookedTextureDescriptorBytes(f);
            CHECK(dfd[12] != 0x83);  // never BC4's colour model
            CHECK(dfd[12] != 0x84);  // never BC5's
            ++srgbBlockFormats;
        }
    }
    REQUIRE(srgbBlockFormats == 3);
}
