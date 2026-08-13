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
//
// NEVER WRITE `CHECK(someCookedTextureFormat == CookedTextureFormat::X)` IN THIS TU. doctest's
// DOCTEST_STRINGIFY expands to an UNQUALIFIED `toString(...)`, so ADL finds
// engine::assets::toString(CookedTextureFormat) -- a non-template exact match that beats doctest's
// own template -- and the decomposer then tries `std::string_view + const char*`, which is a hard
// compile error on EVERY lane, inside doctest.h rather than at the CHECK. Compare through toString()
// on both sides (CT7 proves it is injective over the eight) or wrap the comparison in a second pair
// of parentheses, which is what tests/rhi_format_test.cpp does for engine::rhi::toString. The status
// enum is unaffected: its label function is deliberately named cookedTextureStatusLabel, not
// toString.
#include <aero/assets/bc_block.hpp>
#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/core/guid.hpp>

#include "cooked_texture_golden.hpp"  // the four frozen byte goldens, shared with the editor suite

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
#include <string>
#include <string_view>
#include <vector>

using engine::assets::cookedTextureBlockBytes;
using engine::assets::cookedTextureBlockHeight;
using engine::assets::cookedTextureBlockWidth;
using engine::assets::cookedTextureDescriptorBytes;
using engine::assets::CookedTextureFormat;
using engine::assets::cookedTextureLevelAlignment;
using engine::assets::CookedTextureParse;
using engine::assets::CookedTextureStatus;
using engine::assets::cookedTextureStatusLabel;
using engine::assets::isCookedTextureFormat;
using engine::assets::isSrgbCookedFormat;
using engine::assets::parseCookedTexture;
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

// ---- the hand-built valid file, and the knobs the refusal arms need ------------------------------
//
// EVERY buffer the parser cases below feed it is built HERE, from docs/09 section 10's own rules,
// and NOT by the cook -- which does not exist yet at this step and, more importantly, would let a
// cook bug mask a parser bug. Each refusal arm then mutates exactly ONE field of something already
// valid, so the arm's status names the mutation rather than an accident of construction.
struct Ktx2Build {
    CookedTextureFormat format = CookedTextureFormat::Bc1RgbSrgb;
    std::uint32_t width = 4;
    std::uint32_t height = 4;
    std::uint32_t levelCount = 3;  // the full chain for 4x4
    engine::Guid guid{};           // nil unless a case says otherwise
    bool includeGuidKey = true;
    bool useGuidValueOverride = false;
    std::string guidValueOverride;  // written verbatim (plus a NUL) instead of formatGuid(guid)
    bool includeUnknownKey = false;
};

constexpr std::size_t H_VK_FORMAT = 12;
constexpr std::size_t H_TYPE_SIZE = 16;
constexpr std::size_t H_PIXEL_WIDTH = 20;
constexpr std::size_t H_PIXEL_HEIGHT = 24;
constexpr std::size_t H_PIXEL_DEPTH = 28;
constexpr std::size_t H_LAYER_COUNT = 32;
constexpr std::size_t H_FACE_COUNT = 36;
constexpr std::size_t H_LEVEL_COUNT = 40;
constexpr std::size_t H_SUPERCOMPRESSION = 44;
constexpr std::size_t H_DFD_OFFSET = 48;
constexpr std::size_t H_DFD_LENGTH = 52;
constexpr std::size_t H_KVD_OFFSET = 56;
constexpr std::size_t H_KVD_LENGTH = 60;
constexpr std::size_t H_SGD_OFFSET = 64;

[[nodiscard]] std::uint32_t levelExtentRef(std::uint32_t base, std::uint32_t level) {
    const std::uint32_t v = base >> level;
    return v == 0 ? 1U : v;
}

[[nodiscard]] std::uint64_t levelBytesRef(CookedTextureFormat format, std::uint32_t width, std::uint32_t height,
                                          std::uint32_t level) {
    const std::uint32_t bw = cookedTextureBlockWidth(format);
    const std::uint32_t bh = cookedTextureBlockHeight(format);
    const std::uint64_t blocksX = (levelExtentRef(width, level) + bw - 1) / bw;
    const std::uint64_t blocksY = (levelExtentRef(height, level) + bh - 1) / bh;
    return blocksX * blocksY * cookedTextureBlockBytes(format);
}

void appendKvdRecord(std::vector<std::byte>& kvd, std::string_view key, std::string_view value) {
    // keyAndValueByteLength counts BOTH NUL terminators; the payload is then padded to a multiple of
    // four with zero bytes, and that padding is part of the region.
    const std::size_t kv = key.size() + 1 + value.size() + 1;
    std::array<std::byte, 4> lengthBytes{};
    engine::assets::putU32(lengthBytes, 0, static_cast<std::uint32_t>(kv));
    for (const std::byte b : lengthBytes) {
        kvd.push_back(b);
    }
    for (const char c : key) {
        kvd.push_back(static_cast<std::byte>(c));
    }
    kvd.push_back(std::byte{0});
    for (const char c : value) {
        kvd.push_back(static_cast<std::byte>(c));
    }
    kvd.push_back(std::byte{0});
    for (std::size_t i = kv; i < align4Ref(kv); ++i) {
        kvd.push_back(std::byte{0});
    }
}

[[nodiscard]] std::vector<std::byte> buildKvd(const Ktx2Build& build) {
    // Sorted by Unicode code point of the key, which the spec requires of a writer: 'A' (0x41) is
    // below 'K' (0x4B), and within the two KTX keys 'o' (0x6F) is below 'w' (0x77).
    std::vector<std::byte> kvd;
    if (build.includeGuidKey) {
        const std::string value = build.useGuidValueOverride ? build.guidValueOverride : engine::formatGuid(build.guid);
        appendKvdRecord(kvd, "AeroSourceGuid", value);
    }
    appendKvdRecord(kvd, "KTXorientation", "rd");
    appendKvdRecord(kvd, "KTXwriter", engine::assets::COOKED_TEXTURE_WRITER_ID);
    if (build.includeUnknownKey) {
        // Sorts after KTXwriter, so appending it keeps the region sorted. The parser must SKIP it.
        appendKvdRecord(kvd, "ZZAeroFutureKey", "whatever");
    }
    return kvd;
}

[[nodiscard]] std::vector<std::byte> makeKtx2(const Ktx2Build& build) {
    const std::span<const std::uint8_t> dfd = cookedTextureDescriptorBytes(build.format);
    const std::vector<std::byte> kvd = buildKvd(build);
    const std::size_t levelIndexBytes = std::size_t{24} * build.levelCount;
    const std::size_t dfdOffset = 80 + levelIndexBytes;
    const std::size_t kvdOffset = dfdOffset + dfd.size();
    const std::size_t kvdEnd = kvdOffset + kvd.size();
    const std::uint32_t alignment = cookedTextureLevelAlignment(build.format);
    const std::size_t levelDataStart = ((kvdEnd + alignment - 1) / alignment) * alignment;

    // Level DATA is written smallest-first while the level INDEX is filled level-0-first, so the
    // offsets are computed in reverse and stored forward. That inversion is the single most likely
    // place for an off-by-one in this whole format.
    std::vector<std::uint64_t> offsets(build.levelCount, 0);
    std::size_t cursor = levelDataStart;
    for (std::uint32_t level = build.levelCount; level-- > 0;) {
        offsets[level] = cursor;
        cursor += static_cast<std::size_t>(levelBytesRef(build.format, build.width, build.height, level));
    }

    std::vector<std::byte> file(cursor, std::byte{0});
    const std::span<std::byte> out(file);
    for (std::size_t i = 0; i < engine::assets::KTX2_IDENTIFIER.size(); ++i) {
        out[i] = static_cast<std::byte>(engine::assets::KTX2_IDENTIFIER[i]);
    }
    engine::assets::putU32(out, H_VK_FORMAT, static_cast<std::uint32_t>(build.format));
    engine::assets::putU32(out, H_TYPE_SIZE, 1);
    engine::assets::putU32(out, H_PIXEL_WIDTH, build.width);
    engine::assets::putU32(out, H_PIXEL_HEIGHT, build.height);
    engine::assets::putU32(out, H_PIXEL_DEPTH, 0);
    engine::assets::putU32(out, H_LAYER_COUNT, 0);
    engine::assets::putU32(out, H_FACE_COUNT, 1);
    engine::assets::putU32(out, H_LEVEL_COUNT, build.levelCount);
    engine::assets::putU32(out, H_SUPERCOMPRESSION, 0);
    engine::assets::putU32(out, H_DFD_OFFSET, static_cast<std::uint32_t>(dfdOffset));
    engine::assets::putU32(out, H_DFD_LENGTH, static_cast<std::uint32_t>(dfd.size()));
    engine::assets::putU32(out, H_KVD_OFFSET, static_cast<std::uint32_t>(kvdOffset));
    engine::assets::putU32(out, H_KVD_LENGTH, static_cast<std::uint32_t>(kvd.size()));
    // sgdByteOffset and sgdByteLength stay zero, which is what the zero-initialized buffer gives.

    for (std::uint32_t level = 0; level < build.levelCount; ++level) {
        const std::size_t record = 80 + std::size_t{24} * level;
        const std::uint64_t bytes = levelBytesRef(build.format, build.width, build.height, level);
        engine::assets::putU64(out, record + 0, offsets[level]);
        engine::assets::putU64(out, record + 8, bytes);
        engine::assets::putU64(out, record + 16, bytes);  // uncompressedByteLength == byteLength
    }
    for (std::size_t i = 0; i < dfd.size(); ++i) {
        out[dfdOffset + i] = static_cast<std::byte>(dfd[i]);
    }
    for (std::size_t i = 0; i < kvd.size(); ++i) {
        out[kvdOffset + i] = kvd[i];
    }
    // The level payloads are arbitrary: the parser deliberately does not check block CONTENTS,
    // because there is no such thing as an invalid BCn block.
    for (std::size_t i = levelDataStart; i < file.size(); ++i) {
        out[i] = static_cast<std::byte>(0xA5);
    }
    return file;
}

// AC-21's biconditional, asserted by every parser case rather than in one case of its own: the
// message is non-empty IFF the status is not Ok.
void expectRefusal(const CookedTextureParse& parse, CookedTextureStatus expected) {
    CHECK(parse.status == expected);
    CHECK_FALSE(parse.message.empty());
}
void expectOk(const CookedTextureParse& parse) {
    CHECK(parse.status == CookedTextureStatus::Ok);
    CHECK(parse.message.empty());
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

// =================================================================================================
// The parser (CT13-CT44). Every arm mutates exactly ONE field of a buffer makeKtx2() built valid.
// =================================================================================================

TEST_CASE("a buffer shorter than the 80-byte header is TooSmall (CT13)") {
    const std::vector<std::byte> empty;
    expectRefusal(parseCookedTexture(empty), CookedTextureStatus::TooSmall);

    const std::vector<std::byte> almost(engine::assets::KTX2_HEADER_BYTES - 1, std::byte{0});
    expectRefusal(parseCookedTexture(almost), CookedTextureStatus::TooSmall);
}

TEST_CASE("a valid file parses Ok and its view reports what was written (CT14)") {
    // The positive control. Without it every refusal case below could pass against a parser that
    // refuses everything.
    Ktx2Build build;
    build.guid = engine::Guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const std::vector<std::byte> file = makeKtx2(build);
    const CookedTextureParse parse = parseCookedTexture(file);
    expectOk(parse);
    // Compared through toString rather than as raw enumerators -- see the header comment on the
    // DOCTEST_STRINGIFY / ADL collision. CT7 proved toString is injective over the eight, so this is
    // the same assertion with a readable failure message.
    CHECK(toString(parse.view.format()) == toString(CookedTextureFormat::Bc1RgbSrgb));
    CHECK(parse.view.width() == 4);
    CHECK(parse.view.height() == 4);
    CHECK(parse.view.levelCount() == 3);
    CHECK(parse.view.sourceGuid() == build.guid);
    CHECK(parse.view.levelWidth(0) == 4);
    CHECK(parse.view.levelWidth(1) == 2);
    CHECK(parse.view.levelWidth(2) == 1);
    CHECK(parse.view.levelHeight(2) == 1);
    // Out of range is an answer, never a read.
    CHECK(parse.view.levelWidth(3) == 0);
    CHECK(parse.view.levelHeight(3) == 0);
    CHECK(parse.view.levelBytes(3).empty());
    CHECK(parse.view.levelBytes(0).size() == 8);  // one 4x4 BC1 block
}

TEST_CASE("a buffer over MAX_COOKED_TEXTURE_BYTES is CapExceeded before a field is read (CT15)") {
    // The check is the parser's SECOND step, before the identifier is even compared, so proving it
    // needs a span whose size() is over the cap. Allocating 512 MiB for real would cost every lane
    // half a gigabyte on every run, so the span is formed over a genuinely VALID small file with a
    // faked size instead. That is deliberate and it is safe in both directions: with the check
    // present nothing is dereferenced at all, and with the check REMOVED (the sabotage direction)
    // the parser walks a real, well-formed 344-byte file and touches no byte outside it -- so the
    // seed shows up as a different STATUS rather than as an out-of-bounds read.
    const std::vector<std::byte> valid = makeKtx2(Ktx2Build{});
    REQUIRE(parseCookedTexture(valid).status == CookedTextureStatus::Ok);
    const std::span<const std::byte> oversized(valid.data(),
                                               static_cast<std::size_t>(engine::assets::MAX_COOKED_TEXTURE_BYTES) + 1);
    expectRefusal(parseCookedTexture(oversized), CookedTextureStatus::CapExceeded);
}

TEST_CASE("every one of the 12 identifier bytes is checked (CT16)") {
    std::size_t flipped = 0;
    for (std::size_t i = 0; i < engine::assets::KTX2_IDENTIFIER.size(); ++i) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        file[i] ^= std::byte{0xFF};
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadIdentifier);
        ++flipped;
    }
    REQUIRE(flipped == 12);
}

TEST_CASE("a non-zero supercompressionScheme is Supercompressed, not UnsupportedFormat (CT17)") {
    constexpr std::array<std::uint32_t, 3> SCHEMES = {1U, 2U, std::numeric_limits<std::uint32_t>::max()};
    std::size_t checked = 0;
    for (const std::uint32_t scheme : SCHEMES) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_SUPERCOMPRESSION, scheme);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::Supercompressed);
        ++checked;
    }
    REQUIRE(checked == SCHEMES.size());
}

TEST_CASE("supercompression is diagnosed BEFORE vkFormat, which pins the ladder's order (CT18)") {
    // A Basis file's vkFormat is legitimately VK_FORMAT_UNDEFINED (0). Answering "unsupported format
    // 0" instead of "this file is supercompressed" is the wrong diagnosis, and this is the case that
    // says so: both fields are wrong at once and the scheme must win.
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    engine::assets::putU32(file, H_SUPERCOMPRESSION, 1);
    engine::assets::putU32(file, H_VK_FORMAT, 0);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::Supercompressed);
}

TEST_CASE("a vkFormat outside the eight is UnsupportedFormat (CT19)") {
    // 133/134 are BC1_RGBA, 140/142 are the SNORM siblings: real Vulkan formats, deliberately out.
    constexpr std::array<std::uint32_t, 5> FORMATS = {133U, 134U, 140U, 142U, 0U};
    std::size_t checked = 0;
    for (const std::uint32_t vk : FORMATS) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_VK_FORMAT, vk);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::UnsupportedFormat);
        ++checked;
    }
    REQUIRE(checked == FORMATS.size());
}

TEST_CASE("a typeSize other than 1 is UnsupportedFormat (CT20)") {
    constexpr std::array<std::uint32_t, 3> SIZES = {0U, 2U, 4U};
    std::size_t checked = 0;
    for (const std::uint32_t size : SIZES) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_TYPE_SIZE, size);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::UnsupportedFormat);
        ++checked;
    }
    REQUIRE(checked == SIZES.size());
}

TEST_CASE("a non-zero pixelDepth is UnsupportedShape and the message names the field (CT21)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    engine::assets::putU32(file, H_PIXEL_DEPTH, 1);
    const CookedTextureParse parse = parseCookedTexture(file);
    expectRefusal(parse, CookedTextureStatus::UnsupportedShape);
    CHECK(parse.message.find("pixelDepth") != std::string::npos);
}

TEST_CASE("a non-zero layerCount is UnsupportedShape and the message names the field (CT22)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    engine::assets::putU32(file, H_LAYER_COUNT, 1);
    const CookedTextureParse parse = parseCookedTexture(file);
    expectRefusal(parse, CookedTextureStatus::UnsupportedShape);
    CHECK(parse.message.find("layerCount") != std::string::npos);
}

TEST_CASE("a faceCount other than 1 is UnsupportedShape and the message names the field (CT23)") {
    constexpr std::array<std::uint32_t, 2> FACES = {0U, 6U};  // 6 is a cubemap, the plausible one
    std::size_t checked = 0;
    for (const std::uint32_t faces : FACES) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_FACE_COUNT, faces);
        const CookedTextureParse parse = parseCookedTexture(file);
        expectRefusal(parse, CookedTextureStatus::UnsupportedShape);
        CHECK(parse.message.find("faceCount") != std::string::npos);
        ++checked;
    }
    REQUIRE(checked == FACES.size());
}

TEST_CASE("pixelWidth outside 1..MAX_TEXTURE_DIMENSION is CapExceeded (CT24)") {
    constexpr std::array<std::uint32_t, 3> WIDTHS = {0U, engine::assets::MAX_TEXTURE_DIMENSION + 1,
                                                     std::numeric_limits<std::uint32_t>::max()};
    std::size_t checked = 0;
    for (const std::uint32_t width : WIDTHS) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_PIXEL_WIDTH, width);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::CapExceeded);
        ++checked;
    }
    REQUIRE(checked == WIDTHS.size());
}

TEST_CASE("pixelHeight outside 1..MAX_TEXTURE_DIMENSION is CapExceeded (CT25)") {
    constexpr std::array<std::uint32_t, 2> HEIGHTS = {0U, engine::assets::MAX_TEXTURE_DIMENSION + 1};
    std::size_t checked = 0;
    for (const std::uint32_t height : HEIGHTS) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_PIXEL_HEIGHT, height);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::CapExceeded);
        ++checked;
    }
    REQUIRE(checked == HEIGHTS.size());
}

TEST_CASE("levelCount 0 is refused outright and so is one past the cap (CT26)") {
    // KTX2 itself allows levelCount == 0 (a hint that the reader should generate mips) EXCEPT for
    // block-compressed formats. This container refuses it for every format: the chain is complete or
    // it is one level, and there is no third state.
    constexpr std::array<std::uint32_t, 2> COUNTS = {0U, engine::assets::MAX_TEXTURE_LEVELS + 1};
    std::size_t checked = 0;
    for (const std::uint32_t count : COUNTS) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU32(file, H_LEVEL_COUNT, count);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::CapExceeded);
        ++checked;
    }
    REQUIRE(checked == COUNTS.size());
}

TEST_CASE("a levelCount legal in itself but impossible for the dimensions is CapExceeded (CT27)") {
    // 8x8 has exactly four levels (8, 4, 2, 1). Five is inside 1..15 and still impossible, and this
    // is the clause that makes it a refusal rather than one empty level.
    Ktx2Build build;
    build.width = 8;
    build.height = 8;
    build.levelCount = 4;
    std::vector<std::byte> file = makeKtx2(build);
    REQUIRE(parseCookedTexture(file).status == CookedTextureStatus::Ok);
    engine::assets::putU32(file, H_LEVEL_COUNT, 5);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::CapExceeded);
}

TEST_CASE("a buffer too short for its own level index is TooSmall (CT28)") {
    const std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    // 80 + 24*3 = 152 bytes of header plus index; one byte short of that cannot hold the index, and
    // the check is written as a subtraction so it cannot be defeated by a huge levelCount.
    const std::span<const std::byte> truncated(file.data(), 151);
    expectRefusal(parseCookedTexture(truncated), CookedTextureStatus::TooSmall);
}

TEST_CASE("a dfdByteOffset that is not exactly past the level index is BadTable (CT29)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t dfdOffset = engine::assets::getU32(file, H_DFD_OFFSET);
    CHECK(dfdOffset == 152);  // 80 + 24*3, pinned so a layout change is visible here
    engine::assets::putU32(file, H_DFD_OFFSET, dfdOffset + 1);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("a dfdByteLength that is wrong for the vkFormat is BadTable (CT30)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});  // BC1: a 44-byte descriptor
    engine::assets::putU32(file, H_DFD_LENGTH, 60);       // BC3's length, on a BC1 format
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("a descriptor whose own dfdTotalSize disagrees with the header is BadTable (CT31)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t dfdOffset = engine::assets::getU32(file, H_DFD_OFFSET);
    engine::assets::putU32(file, dfdOffset, 60);  // the descriptor claims 60; the header says 44
    // BadTable, not BadDescriptor: the self-consistency of the region's own size is a STRUCTURAL
    // fact, checked before a single descriptor byte is compared. The ladder's order is the contract.
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("an sRGB format carrying its UNORM sibling's descriptor is BadDescriptor (CT32)") {
    // THE CASE THAT MAKES THE C1/C2 BYTES LOAD-BEARING AT PARSE TIME. The two tables in each pair
    // have the same LENGTH, so every structural check passes and only the byte comparison can tell
    // them apart.
    struct Pair {
        CookedTextureFormat srgb;
        CookedTextureFormat unorm;
    };
    constexpr std::array<Pair, 3> PAIRS = {
        Pair{CookedTextureFormat::Bc1RgbSrgb, CookedTextureFormat::Bc1RgbUnorm},
        Pair{CookedTextureFormat::Bc3Srgb, CookedTextureFormat::Bc3Unorm},
        Pair{CookedTextureFormat::Rgba8Srgb, CookedTextureFormat::Rgba8Unorm},
    };
    std::size_t checked = 0;
    for (const Pair& pair : PAIRS) {
        Ktx2Build build;
        build.format = pair.srgb;
        std::vector<std::byte> file = makeKtx2(build);
        REQUIRE(parseCookedTexture(file).status == CookedTextureStatus::Ok);
        const std::uint32_t dfdOffset = engine::assets::getU32(file, H_DFD_OFFSET);
        const std::span<const std::uint8_t> sibling = cookedTextureDescriptorBytes(pair.unorm);
        REQUIRE(sibling.size() == cookedTextureDescriptorBytes(pair.srgb).size());
        for (std::size_t i = 0; i < sibling.size(); ++i) {
            file[dfdOffset + i] = static_cast<std::byte>(sibling[i]);
        }
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadDescriptor);
        ++checked;
    }
    REQUIRE(checked == PAIRS.size());
}

TEST_CASE("a single flipped descriptor byte is BadDescriptor, at each of four positions (CT33)") {
    // colorModel, transferFunction, bytesPlane0 and a sample's channel id. All four are past
    // dfdTotalSize, so the structural checks pass and the byte comparison is what fires.
    constexpr std::array<std::size_t, 4> POSITIONS = {12, 14, 20, 31};
    std::size_t checked = 0;
    for (const std::size_t position : POSITIONS) {
        Ktx2Build build;
        build.format = CookedTextureFormat::Bc3Srgb;  // 60 bytes, two samples: all four exist
        std::vector<std::byte> file = makeKtx2(build);
        const std::uint32_t dfdOffset = engine::assets::getU32(file, H_DFD_OFFSET);
        file[dfdOffset + position] ^= std::byte{0x40};
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadDescriptor);
        ++checked;
    }
    REQUIRE(checked == POSITIONS.size());
}

TEST_CASE("a kvdByteOffset that is not exactly past the descriptor is BadTable (CT34)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t kvdOffset = engine::assets::getU32(file, H_KVD_OFFSET);
    engine::assets::putU32(file, H_KVD_OFFSET, kvdOffset + 1);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("a key/value record declaring length 0 is BadTable -- the infinite-loop guard (CT35)") {
    // Without this guard the walk advances by 4 forever on a region whose length is a multiple of 4,
    // which is every region this format can produce.
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t kvdOffset = engine::assets::getU32(file, H_KVD_OFFSET);
    engine::assets::putU32(file, kvdOffset, 0);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("a key/value record whose length overruns its region is BadTable (CT36)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t kvdOffset = engine::assets::getU32(file, H_KVD_OFFSET);
    const std::uint32_t kvdLength = engine::assets::getU32(file, H_KVD_LENGTH);
    engine::assets::putU32(file, kvdOffset, kvdLength);  // the whole region, with no room for its own header
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);

    // And the wrapping variant: a length near UINT32_MAX must be refused by subtraction, never
    // accepted because `offset + length` wrapped.
    std::vector<std::byte> wrapped = makeKtx2(Ktx2Build{});
    engine::assets::putU32(wrapped, kvdOffset, std::numeric_limits<std::uint32_t>::max());
    expectRefusal(parseCookedTexture(wrapped), CookedTextureStatus::BadTable);
}

TEST_CASE("records that do not tile the region exactly are BadTable (CT37)") {
    // Shrinking kvdByteLength by one leaves the first two records intact and makes the third overrun
    // -- which is also what catches a walk that advances by `4 + kv` instead of `4 + align4(kv)`,
    // since AeroSourceGuid's own kv is already a multiple of four and hides the bug on record one.
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::uint32_t kvdLength = engine::assets::getU32(file, H_KVD_LENGTH);
    CHECK(kvdLength == engine::assets::KTX2_KVD_BYTES);
    engine::assets::putU32(file, H_KVD_LENGTH, kvdLength - 1);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
}

TEST_CASE("an unknown key/value key is SKIPPED, not refused (CT38)") {
    // Key/value data is the format's one additive extension point, and that does NOT contradict
    // docs/09 section 9.11's reserved-field refusal: a KTX2 key is NAMED and LENGTH-PREFIXED, so it
    // can genuinely be skipped and every one of its bytes is accounted for.
    Ktx2Build build;
    build.guid = engine::Guid{0xAAAABBBBCCCCDDDDULL, 0x1111222233334444ULL};
    build.includeUnknownKey = true;
    const std::vector<std::byte> file = makeKtx2(build);
    const CookedTextureParse parse = parseCookedTexture(file);
    expectOk(parse);
    CHECK(parse.view.sourceGuid() == build.guid);
    // The region really did grow -- otherwise this case proves nothing.
    CHECK(engine::assets::getU32(file, H_KVD_LENGTH) > engine::assets::KTX2_KVD_BYTES);
}

TEST_CASE("a missing AeroSourceGuid yields the nil GUID rather than a refusal (CT39)") {
    Ktx2Build build;
    build.includeGuidKey = false;
    const std::vector<std::byte> file = makeKtx2(build);
    const CookedTextureParse parse = parseCookedTexture(file);
    expectOk(parse);
    CHECK_FALSE(parse.view.sourceGuid().valid());
    CHECK(engine::assets::getU32(file, H_KVD_LENGTH) < engine::assets::KTX2_KVD_BYTES);
}

TEST_CASE("a malformed AeroSourceGuid value yields the nil GUID rather than a refusal (CT40)") {
    // A wrong length and a non-hex digit, both at the right length boundary. Provenance that cannot
    // be read is not a corrupt image.
    constexpr std::array<std::string_view, 3> VALUES = {
        "0123456789abcdef0123456789abcde",    // 31 digits
        "0123456789abcdef0123456789abcdefg",  // 33 characters
        "0123456789abcdef0123456789abcdeZ",   // 32 characters, one not hex
    };
    std::size_t checked = 0;
    for (const std::string_view value : VALUES) {
        Ktx2Build build;
        build.guid = engine::Guid{1, 2};
        build.useGuidValueOverride = true;
        build.guidValueOverride = std::string(value);
        const CookedTextureParse parse = parseCookedTexture(makeKtx2(build));
        expectOk(parse);
        CHECK_FALSE(parse.view.sourceGuid().valid());
        ++checked;
    }
    REQUIRE(checked == VALUES.size());
}

TEST_CASE("a level byteLength that is wrong for its dimensions is BadRange (CT41)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    const std::size_t record = 80;  // level 0
    const std::uint64_t declared = engine::assets::getU64(file, record + 8);
    CHECK(declared == 8);  // one 4x4 BC1 block
    engine::assets::putU64(file, record + 8, declared + 8);
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadRange);
}

TEST_CASE("a level whose uncompressedByteLength disagrees with its byteLength is BadRange (CT42)") {
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    engine::assets::putU64(file, 80 + 16, 0);  // level 0's uncompressedByteLength
    expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadRange);
}

TEST_CASE("a misaligned level byteOffset is BadRange, at every level (CT43)") {
    // The writer can only ever misalign the FIRST level it emits (the smallest), because every
    // level's byteLength is a multiple of its own alignment. The parser checks all of them anyway: a
    // hostile file is not obliged to share our arithmetic.
    std::size_t checked = 0;
    for (std::uint32_t level = 0; level < 3; ++level) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        const std::size_t record = 80 + std::size_t{24} * level;
        const std::uint64_t offset = engine::assets::getU64(file, record);
        CHECK(offset % 8 == 0);
        engine::assets::putU64(file, record, offset + 4);  // BC1 aligns to 8, so +4 misaligns
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadRange);
        ++checked;
    }
    REQUIRE(checked == 3);
}

TEST_CASE("a level offset near UINT64_MAX is BadRange by SUBTRACTION, never a read (CT44)") {
    // THE CASE THE SUBTRACTION IDIOM EXISTS FOR. 0xFFFFFFFFFFFFFFF8 + 8 wraps to 0, so an
    // addition-based bounds check (`offset + length > size`) computes 0 > 344, decides the range is
    // fine, and hands levelBytes() an offset eighteen exabytes past the buffer. Written as
    // `length <= size && offset <= size - length` it cannot wrap. ASan must stay clean here.
    std::vector<std::byte> file = makeKtx2(Ktx2Build{});
    engine::assets::putU64(file, 80, 0xFFFFFFFFFFFFFFF8ULL);  // 8-aligned, so the alignment check passes
    const CookedTextureParse parse = parseCookedTexture(file);
    expectRefusal(parse, CookedTextureStatus::BadRange);
    // And the view it did not build cannot be read through either.
    CHECK(parse.view.levelBytes(0).empty());
}

// The ladder's step 13. Numbered CT44a rather than CT45 because CT45-CT52 are reserved for step 5's
// cook round-trip battery, and a case id is a global identifier in this repo -- the tree's own prefix
// grep already accepts a trailing letter.
TEST_CASE("a non-zero supercompression global-data pair with scheme 0 is BadTable (CT44a)") {
    // v1 emits no global data at all, so a non-zero pair alongside scheme 0 is malformed rather than
    // an extension: the two fields describe a section that, by the scheme, does not exist.
    std::size_t checked = 0;
    for (const std::size_t field : {H_SGD_OFFSET, H_SGD_OFFSET + 8}) {
        std::vector<std::byte> file = makeKtx2(Ktx2Build{});
        engine::assets::putU64(file, field, 1);
        expectRefusal(parseCookedTexture(file), CookedTextureStatus::BadTable);
        ++checked;
    }
    REQUIRE(checked == 2);
}

// =================================================================================================
// The round trip (CT45-CT52). THE PROPERTY THE REST OF THE TASK LEANS ON: every file the cook accepts
// parses Ok through this container's own reader, for every one of the eight formats. It is also what
// catches a divergence between the cook's level-byte arithmetic and the parser's, which are
// deliberately separate implementations of the same rule.
// =================================================================================================

namespace {

// One case body, run once per format, so the eight cases below are eight distinct ids over one
// checked property rather than one table-driven case that could iterate nothing.
void checkRoundTrip(CookedTextureFormat format, std::uint32_t width, std::uint32_t height) {
    const engine::Guid guid{0x0F1E2D3C4B5A6978ULL, 0x8796A5B4C3D2E1F0ULL};
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4, std::byte{0});
    for (std::size_t i = 0; i < pixels.size() / 4; ++i) {
        pixels[4 * i + 0] = static_cast<std::byte>((i * 29) & 0xFF);
        pixels[4 * i + 1] = static_cast<std::byte>((i * 71 + 13) & 0xFF);
        pixels[4 * i + 2] = static_cast<std::byte>((i * 137 + 200) & 0xFF);
        pixels[4 * i + 3] = static_cast<std::byte>(i % 3 == 0 ? 255 : 90);
    }
    engine::assets::TextureCookInput input;
    input.sourceGuid = guid;
    input.width = width;
    input.height = height;
    input.rgba8 = pixels;
    input.format = format;
    const engine::assets::TextureCookResult cooked = engine::assets::cookTexture(input);
    REQUIRE(cooked.status == engine::assets::TextureCookStatus::Ok);
    REQUIRE_FALSE(cooked.bytes.empty());

    const CookedTextureParse parse = parseCookedTexture(cooked.bytes);
    expectOk(parse);
    CHECK(toString(parse.view.format()) == toString(format));
    CHECK(parse.view.width() == width);
    CHECK(parse.view.height() == height);
    CHECK(parse.view.levelCount() == cooked.stats.levelCount);
    CHECK(parse.view.sourceGuid() == guid);
    // Every level is reachable and non-empty, and the one past the end is an EMPTY span rather than a
    // read.
    std::size_t levels = 0;
    for (std::uint32_t level = 0; level < parse.view.levelCount(); ++level) {
        CHECK_FALSE(parse.view.levelBytes(level).empty());
        ++levels;
    }
    REQUIRE(levels == parse.view.levelCount());
    CHECK(parse.view.levelBytes(parse.view.levelCount()).empty());
    // The nil GUID is legal too, and the file's SIZE must not depend on which was supplied -- the
    // AeroSourceGuid record is written unconditionally.
    input.sourceGuid = engine::Guid{};
    const engine::assets::TextureCookResult nilGuid = engine::assets::cookTexture(input);
    REQUIRE(nilGuid.status == engine::assets::TextureCookStatus::Ok);
    CHECK(nilGuid.bytes.size() == cooked.bytes.size());
    const CookedTextureParse nilParse = parseCookedTexture(nilGuid.bytes);
    expectOk(nilParse);
    CHECK_FALSE(nilParse.view.sourceGuid().valid());
}

}  // namespace

TEST_CASE("a cooked Rgba8Unorm texture parses Ok through this container's reader (CT45)") {
    checkRoundTrip(CookedTextureFormat::Rgba8Unorm, 5, 3);
}
TEST_CASE("a cooked Rgba8Srgb texture parses Ok through this container's reader (CT46)") {
    checkRoundTrip(CookedTextureFormat::Rgba8Srgb, 5, 3);
}
TEST_CASE("a cooked Bc1RgbUnorm texture parses Ok through this container's reader (CT47)") {
    checkRoundTrip(CookedTextureFormat::Bc1RgbUnorm, 9, 7);
}
TEST_CASE("a cooked Bc1RgbSrgb texture parses Ok through this container's reader (CT48)") {
    checkRoundTrip(CookedTextureFormat::Bc1RgbSrgb, 9, 7);
}
TEST_CASE("a cooked Bc3Unorm texture parses Ok through this container's reader (CT49)") {
    checkRoundTrip(CookedTextureFormat::Bc3Unorm, 16, 16);
}
TEST_CASE("a cooked Bc3Srgb texture parses Ok through this container's reader (CT50)") {
    checkRoundTrip(CookedTextureFormat::Bc3Srgb, 16, 16);
}
TEST_CASE("a cooked Bc4Unorm texture parses Ok through this container's reader (CT51)") {
    checkRoundTrip(CookedTextureFormat::Bc4Unorm, 1, 1);
}
TEST_CASE("a cooked Bc5Unorm texture parses Ok through this container's reader (CT52)") {
    checkRoundTrip(CookedTextureFormat::Bc5Unorm, 5, 3);
}

// =================================================================================================
// The FOUR FROZEN BYTE GOLDENS (CT53-CT58). Each array in tests/cooked_texture_golden.hpp carries the
// exact RGBA8 texels it was cooked from, so every case below RE-COOKS that input and compares the
// whole file byte for byte -- a golden that is only a captured blob proves nothing about the
// transform that produced it.
//
// They are FROZEN. A change to any layout, ordering, padding, filter or encoder rule fails them by
// construction; if one has to change, the cook changed, and that is a COOKED_TEXTURE_COOKER_VERSION
// decision rather than a test edit.
// =================================================================================================

namespace {

// A golden and its input are both std::uint8_t arrays (reviewable as decimal texels and hex bytes in
// the header); cookTexture and parseCookedTexture both take std::byte. One conversion, here, rather
// than at a dozen call sites.
[[nodiscard]] std::vector<std::byte> goldenPixels(std::span<const std::uint8_t> texels) {
    std::vector<std::byte> out;
    out.reserve(texels.size());
    for (const std::uint8_t v : texels) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Cooks a golden's own input and asserts the result is that golden, byte for byte. The mismatch is
// reported by INDEX, because a 400-byte CHECK that merely says "not equal" is unreadable.
struct GoldenCook {
    engine::assets::TextureCookResult result;
    CookedTextureParse parse;
};

template <std::size_t PIXELS, std::size_t BYTES>
[[nodiscard]] GoldenCook checkGolden(const std::array<std::uint8_t, PIXELS>& input,
                                     const std::array<std::uint8_t, BYTES>& golden, CookedTextureFormat format,
                                     std::uint32_t width, std::uint32_t height, engine::Guid guid) {
    const std::vector<std::byte> pixels = goldenPixels(input);
    REQUIRE(pixels.size() == static_cast<std::size_t>(width) * height * 4);

    engine::assets::TextureCookInput in;
    in.sourceGuid = guid;
    in.width = width;
    in.height = height;
    in.rgba8 = pixels;
    in.format = format;
    in.generateMips = true;

    GoldenCook cooked{engine::assets::cookTexture(in), CookedTextureParse{}};
    REQUIRE(cooked.result.status == engine::assets::TextureCookStatus::Ok);
    REQUIRE(cooked.result.bytes.size() == BYTES);

    std::size_t firstMismatch = BYTES;
    for (std::size_t i = 0; i < BYTES; ++i) {
        if (cooked.result.bytes[i] != static_cast<std::byte>(golden[i])) {
            firstMismatch = i;
            break;
        }
    }
    CHECK(firstMismatch == BYTES);  // the index of the first differing byte, or BYTES for "identical"

    // A golden that does not parse would be a golden of a broken file.
    cooked.parse = parseCookedTexture(cooked.result.bytes);
    expectOk(cooked.parse);
    CHECK(toString(cooked.parse.view.format()) == toString(format));
    CHECK(cooked.parse.view.width() == width);
    CHECK(cooked.parse.view.height() == height);
    CHECK(cooked.parse.view.sourceGuid() == guid);
    return cooked;
}

// The non-nil GUID Golden C carries, and the one tests/cooked_mesh_golden.hpp's Golden C uses: one
// value pins the provenance field of BOTH containers.
constexpr engine::Guid GOLDEN_C_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

}  // namespace

TEST_CASE("Golden A -- 4x4 BC1 sRGB with a full chain and a nil GUID -- is 344 frozen bytes (CT53)") {
    const GoldenCook cooked =
        checkGolden(aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4_INPUT, aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4,
                    CookedTextureFormat::Bc1RgbSrgb, 4, 4, engine::Guid{});
    CHECK(cooked.parse.view.levelCount() == 3);
    // The transferFunction byte of the BC1 sRGB descriptor, at descriptor offset 14 and therefore at
    // file offset 152 + 14. CT12 pins the TABLE; this pins that the WRITER emitted that table.
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4[152 + 14] == 0x02);
    // The nil GUID's value is 32 '0' characters, not a shorter record: the layout must not depend on
    // whether a GUID was supplied.
    std::size_t zeros = 0;
    for (std::size_t i = 215; i < 247; ++i) {
        if (aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4[i] == 0x30) {
            ++zeros;
        }
    }
    CHECK(zeros == 32);
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4[247] == 0x00);
}

TEST_CASE("Golden B -- 1x1 RGBA8, the smallest legal file, with ZERO padding -- is 320 bytes (CT54)") {
    const GoldenCook cooked =
        checkGolden(aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1_INPUT, aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1,
                    CookedTextureFormat::Rgba8Unorm, 1, 1, engine::Guid{});
    CHECK(cooked.parse.view.levelCount() == 1);

    // THE POINT OF THIS GOLDEN: 316 is already a multiple of 4, so there is NO padding at all. A cook
    // that padded unconditionally reddens this one and no other.
    const std::span<const std::byte> level0 = cooked.parse.view.levelBytes(0);
    REQUIRE(level0.size() == 4);
    CHECK(cooked.parse.view.levelRecord(0).byteOffset == 316);
    // AC-29: an Rgba8* cook's level 0 is the input verbatim.
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(std::to_integer<std::uint32_t>(level0[i]) == aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1_INPUT[i]);
    }

    // levelCount is 1 either way at 1x1, so BOTH spellings of generateMips must produce IDENTICAL
    // bytes -- not merely an identical level count.
    const std::vector<std::byte> pixels = goldenPixels(aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1_INPUT);
    engine::assets::TextureCookInput in;
    in.width = 1;
    in.height = 1;
    in.rgba8 = pixels;
    in.format = CookedTextureFormat::Rgba8Unorm;
    in.generateMips = false;
    const engine::assets::TextureCookResult noMips = engine::assets::cookTexture(in);
    REQUIRE(noMips.status == engine::assets::TextureCookStatus::Ok);
    CHECK(noMips.bytes == cooked.result.bytes);
}

TEST_CASE("Golden C -- 5x3 BC5 linear, odd in both axes, with a non-nil GUID -- is 400 bytes (CT55)") {
    const GoldenCook cooked =
        checkGolden(aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3_INPUT, aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3,
                    CookedTextureFormat::Bc5Unorm, 5, 3, GOLDEN_C_GUID);
    CHECK(cooked.parse.view.levelCount() == 3);
    // The two BC5 sample channel ids, at descriptor offsets 31 and 47 -- the descriptor starts at 152.
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3[152 + 31] == 0x00);  // RED
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3[152 + 47] == 0x01);  // GREEN
    // The GUID's 32 lowercase hex characters, high half first, byte-visible at 231..262.
    std::string spelled;
    for (std::size_t i = 231; i < 263; ++i) {
        spelled.push_back(static_cast<char>(aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3[i]));
    }
    CHECK(spelled == engine::formatGuid(GOLDEN_C_GUID));
}

TEST_CASE("Golden D -- 2x2 BC3 sRGB -- is 352 bytes and carries the 1F alpha qualifier (CT56)") {
    const GoldenCook cooked =
        checkGolden(aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2_INPUT, aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2,
                    CookedTextureFormat::Bc3Srgb, 2, 2, engine::Guid{});
    CHECK(cooked.parse.view.levelCount() == 2);

    // THE REASON THIS GOLDEN EXISTS. BC3_SRGB's descriptor is NOT a one-byte edit of BC3_UNORM's:
    // setChannelFlags sets KHR_DF_SAMPLE_DATATYPE_LINEAR (0x10) on an sRGB format's alpha sample, and
    // both KHR_DF_CHANNEL_BC3_ALPHA and KHR_DF_CHANNEL_RGBSDA_ALPHA are 15, so the first sample's
    // channel byte is 0x1F. The descriptor starts at 128, so that byte is at file offset 159. Without
    // this golden the corrected byte was covered by exactly one case, over the TABLE rather than over
    // a file the writer actually emitted.
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2[128 + 14] == 0x02);  // transferFunction = KHR_DF_TRANSFER_SRGB
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2[128 + 31] == 0x1F);  // alpha sample: channel 15 | LINEAR
    CHECK(aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2[128 + 47] == 0x00);  // colour sample: channel 0, no qualifier

    // BC3's ALPHA-THEN-COLOUR composition, in bytes rather than only behaviourally: level 0's first
    // eight bytes are the BC4 alpha block over the four alphas 255/128/64/0, whose endpoints are max
    // then min, and its second eight are the BC1 colour block.
    const std::span<const std::byte> level0 = cooked.parse.view.levelBytes(0);
    REQUIRE(level0.size() == 16);
    CHECK(std::to_integer<std::uint32_t>(level0[0]) == 255);  // r0 = max alpha
    CHECK(std::to_integer<std::uint32_t>(level0[1]) == 0);    // r1 = min alpha
    std::array<std::uint8_t, 16> alphaTexels{};
    for (std::size_t i = 0; i < 16; ++i) {
        // The 2x2 image clamped into a 4x4 block: every texel outside the image repeats the nearest
        // one inside it, which is what TX34 proves for the colour half.
        const std::size_t x = (i % 4) >= 2 ? 1 : (i % 4);
        const std::size_t y = (i / 4) >= 2 ? 1 : (i / 4);
        alphaTexels[i] = aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2_INPUT[(y * 2 + x) * 4 + 3];
    }
    std::array<std::byte, 8> alphaBlock{};
    engine::assets::encodeBc4Block(alphaTexels, alphaBlock);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(std::to_integer<std::uint32_t>(level0[i]) == std::to_integer<std::uint32_t>(alphaBlock[i]));
    }
}

TEST_CASE("levels[0] holds the LARGEST offset and the last level starts at the aligned KVD end (CT56a)") {
    // The level-order inversion, on every golden that has more than one level. The level INDEX is
    // level-0-first while the level DATA is smallest-first, so the stored offsets must be STRICTLY
    // DECREASING in index order -- the single most likely place for an off-by-one in this format, and
    // one a same-sized file would otherwise hide.
    struct Case {
        std::span<const std::uint8_t> golden;
        CookedTextureFormat format;
    };
    const std::array<Case, 3> cases = {
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4, CookedTextureFormat::Bc1RgbSrgb},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3, CookedTextureFormat::Bc5Unorm},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2, CookedTextureFormat::Bc3Srgb},
    };
    std::size_t checked = 0;
    for (const Case& c : cases) {
        const std::vector<std::byte> file = goldenPixels(c.golden);
        const CookedTextureParse parse = parseCookedTexture(file);
        expectOk(parse);
        const std::uint32_t levels = parse.view.levelCount();
        REQUIRE(levels >= 2);
        for (std::uint32_t level = 1; level < levels; ++level) {
            CHECK(parse.view.levelRecord(level).byteOffset < parse.view.levelRecord(level - 1).byteOffset);
        }
        // The smallest level starts exactly at the aligned end of the key/value data, and the largest
        // ends exactly at the end of the file.
        const std::uint64_t kvdEnd =
            engine::assets::getU32(file, H_KVD_OFFSET) + engine::assets::getU32(file, H_KVD_LENGTH);
        const std::uint32_t alignment = cookedTextureLevelAlignment(c.format);
        CHECK(parse.view.levelRecord(levels - 1).byteOffset == ((kvdEnd + alignment - 1) / alignment) * alignment);
        CHECK(parse.view.levelRecord(0).byteOffset + parse.view.levelRecord(0).byteLength == file.size());
        ++checked;
    }
    REQUIRE(checked == cases.size());
}

TEST_CASE("there is EXACTLY ONE padding site in each golden and no gap between levels (CT57)") {
    // mipPadding can occur at exactly one place in the whole file -- between the end of the KVD and
    // the start of the smallest level -- because every level's byteLength is a multiple of the
    // alignment. That is a property of the FORMAT, so it is asserted over the goldens' real bytes
    // rather than over the writer's own arithmetic.
    struct Case {
        std::span<const std::uint8_t> golden;
        CookedTextureFormat format;
        std::size_t expectedPadding;
    };
    const std::array<Case, 4> cases = {
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4, CookedTextureFormat::Bc1RgbSrgb, 4},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1, CookedTextureFormat::Rgba8Unorm, 0},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3, CookedTextureFormat::Bc5Unorm, 4},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2, CookedTextureFormat::Bc3Srgb, 12},
    };
    std::size_t checked = 0;
    for (const Case& c : cases) {
        const std::vector<std::byte> file = goldenPixels(c.golden);
        const CookedTextureParse parse = parseCookedTexture(file);
        expectOk(parse);
        const std::uint64_t kvdEnd =
            engine::assets::getU32(file, H_KVD_OFFSET) + engine::assets::getU32(file, H_KVD_LENGTH);
        const std::uint32_t alignment = cookedTextureLevelAlignment(c.format);
        const std::uint32_t levels = parse.view.levelCount();
        const std::uint64_t smallest = parse.view.levelRecord(levels - 1).byteOffset;

        // The gap is exactly (A - kvdEnd % A) % A bytes, and every one of them is 0x00.
        CHECK(smallest - kvdEnd == (alignment - (kvdEnd % alignment)) % alignment);
        CHECK(smallest - kvdEnd == c.expectedPadding);
        for (std::uint64_t at = kvdEnd; at < smallest; ++at) {
            CHECK(c.golden[static_cast<std::size_t>(at)] == 0x00);
        }
        // And there is NO gap anywhere else: each level abuts the next one down the file.
        for (std::uint32_t level = levels - 1; level > 0; --level) {
            const engine::assets::CookedTextureLevel& smaller = parse.view.levelRecord(level);
            CHECK(smaller.byteOffset + smaller.byteLength == parse.view.levelRecord(level - 1).byteOffset);
        }
        ++checked;
    }
    REQUIRE(checked == cases.size());
}

TEST_CASE("the four goldens total 344, 320, 400 and 352 bytes, with NO trailing bytes (CT58)") {
    // AC-10: the file ends exactly where the largest level ends. A writer that appended anything --
    // padding, a footer, a stray alignment run -- would pass every other case in this TU.
    struct Case {
        std::span<const std::uint8_t> golden;
        std::size_t expectedSize;
    };
    const std::array<Case, 4> cases = {
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4, 344},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1, 320},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3, 400},
        Case{aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2, 352},
    };
    std::size_t checked = 0;
    for (const Case& c : cases) {
        CHECK(c.golden.size() == c.expectedSize);
        const std::vector<std::byte> file = goldenPixels(c.golden);
        const CookedTextureParse parse = parseCookedTexture(file);
        expectOk(parse);
        const engine::assets::CookedTextureLevel& largest = parse.view.levelRecord(0);
        CHECK(largest.byteOffset + largest.byteLength == c.expectedSize);
        ++checked;
    }
    REQUIRE(checked == cases.size());
}
