// tests/cooked_animation_test.cpp -- task 3.5.2: the .aeroanim container v1. A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 13's own tables -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly ONE field of something already valid. The two frozen
// goldens (AN1/AN2/AN3) are the one exception, and they are cook OUTPUT verified field by field
// against those same tables before being frozen.
#include <aero/assets/cooked_animation.hpp>

#include "cooked_animation_golden.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies the std::string_view
// operands of the magic and label CHECKs below through operator<<(std::ostream&,
// std::string_view), which MS STL defines inline in <string_view> against an INCOMPLETE
// std::basic_ostream -- only <ostream> completes it. libc++ and libstdc++ are self-sufficient, so
// omitting it builds clean on macOS and Linux and fails only on the Windows lane, with errors
// pointing inside the STL headers rather than at the CHECK. Written when the TU was created.
#include <ostream>
#include <vector>

using engine::assets::animationKeyTime;
using engine::assets::animationKeyValue;
using engine::assets::channelTimeBytes;
using engine::assets::channelValueBytes;
using engine::assets::COOKED_ANIMATION_ALIGNMENT;
using engine::assets::COOKED_ANIMATION_CHANNEL_BYTES;
using engine::assets::COOKED_ANIMATION_FORMAT_VERSION;
using engine::assets::COOKED_ANIMATION_HEADER_BYTES;
using engine::assets::COOKED_ANIMATION_MAGIC;
using engine::assets::CookedAnimationInterpolation;
using engine::assets::cookedAnimationInterpolationLabel;
using engine::assets::CookedAnimationParseResult;
using engine::assets::CookedAnimationPath;
using engine::assets::cookedAnimationPathLabel;
using engine::assets::CookedAnimationStatus;
using engine::assets::cookedAnimationStatusLabel;
using engine::assets::cookedAnimationTimesPadding;
using engine::assets::cookedAnimationValuesPerKey;
using engine::assets::isCookedAnimationInterpolation;
using engine::assets::isCookedAnimationPath;
using engine::assets::MAX_COOKED_ANIMATION_CHANNELS;
using engine::assets::MAX_COOKED_ANIMATION_KEYS;
using engine::assets::MAX_COOKED_ANIMATION_VALUES;
using engine::assets::parseCookedAnimation;

namespace {

// The two record sizes and the three caps are part of the on-disk format, so they are pinned as
// BUILD failures rather than as a case: a change to any of them cannot be argued with at runtime.
static_assert(COOKED_ANIMATION_HEADER_BYTES == 80);
static_assert(COOKED_ANIMATION_CHANNEL_BYTES == 32);
static_assert(COOKED_ANIMATION_ALIGNMENT == 16);
static_assert(COOKED_ANIMATION_HEADER_BYTES % 16 == 0, "the channel table starts 16-aligned");
static_assert(COOKED_ANIMATION_CHANNEL_BYTES % 16 == 0, "and ends 16-aligned, whatever the count");
static_assert(MAX_COOKED_ANIMATION_CHANNELS == 4096);
static_assert(MAX_COOKED_ANIMATION_KEYS == 2000000);
static_assert(MAX_COOKED_ANIMATION_VALUES == 6000000);

// This file's OWN copy of docs/09 section 13's offsets, spelled independently of the parser's: a
// transposed offset in cooked_animation.cpp must fail here rather than agree with itself.
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_RESERVED_FLAGS = 32;
constexpr std::size_t H_CHANNEL_COUNT = 36;
constexpr std::size_t H_KEY_COUNT = 40;
constexpr std::size_t H_VALUE_COUNT = 44;
constexpr std::size_t H_SOURCE_ANIMATION_INDEX = 48;
constexpr std::size_t H_DURATION_SECONDS = 52;
constexpr std::size_t H_TIMES_DATA_OFFSET = 56;
constexpr std::size_t H_VALUES_DATA_OFFSET = 64;
constexpr std::size_t H_TOTAL_BYTES = 72;

constexpr std::size_t C_TARGET_NODE_LOCAL_ID = 0;
constexpr std::size_t C_PATH = 4;
constexpr std::size_t C_INTERPOLATION = 6;
constexpr std::size_t C_KEY_COUNT = 8;
constexpr std::size_t C_FIRST_KEY = 12;
constexpr std::size_t C_FIRST_VALUE = 16;
constexpr std::size_t C_VALUE_COUNT = 20;
constexpr std::size_t C_RESERVED0 = 24;

void put16(std::vector<std::byte>& b, std::size_t offset, std::uint16_t value) {
    engine::assets::putU16(std::span<std::byte>(b), offset, value);
}
void put32(std::vector<std::byte>& b, std::size_t offset, std::uint32_t value) {
    engine::assets::putU32(std::span<std::byte>(b), offset, value);
}
void put64(std::vector<std::byte>& b, std::size_t offset, std::uint64_t value) {
    engine::assets::putU64(std::span<std::byte>(b), offset, value);
}
void putF(std::vector<std::byte>& b, std::size_t offset, float value) {
    engine::assets::putF32(std::span<std::byte>(b), offset, value);
}

// The offset of channel record `i`, spelled once.
[[nodiscard]] constexpr std::size_t rec(std::uint32_t i) {
    return COOKED_ANIMATION_HEADER_BYTES + (std::size_t{i} * COOKED_ANIMATION_CHANNEL_BYTES);
}

// The codes are written as RAW u16s, never as enumerators, so a case can present a code this format
// does not define -- which is the whole point of half the arms below.
struct ChannelSpec {
    std::uint32_t targetNodeLocalId = 0;
    std::uint16_t path = 0;
    std::uint16_t interpolation = 0;
    std::uint32_t keyCount = 0;
    std::uint32_t firstKey = 0;
    std::uint32_t firstValue = 0;
    std::uint32_t valueCount = 0;
};

struct HeaderSpec {
    std::uint32_t keyCount = 0;
    std::uint32_t valueCount = 0;
    std::uint64_t guidHi = 0;
    std::uint64_t guidLo = 0;
    std::uint32_t sourceAnimationIndex = 0;
    float durationSeconds = 0.0F;
};

// A well-formed buffer for `channels` and `header`, laid out exactly as section 13.1 requires. The
// bulk regions are filled from the GLOBAL element index -- time k is 0.5 * k and value v is
// (v, v + 0.25, -v, 1) -- so a slice returned at the wrong offset is visible in its contents and
// not merely in its length.
[[nodiscard]] std::vector<std::byte> build(std::span<const ChannelSpec> channels, const HeaderSpec& header) {
    const auto channelCount = static_cast<std::uint32_t>(channels.size());
    const std::size_t timesDataOffset =
        COOKED_ANIMATION_HEADER_BYTES + (std::size_t{channelCount} * COOKED_ANIMATION_CHANNEL_BYTES);
    const std::size_t valuesDataOffset =
        timesDataOffset + (4U * std::size_t{header.keyCount}) + cookedAnimationTimesPadding(header.keyCount);
    const std::size_t total = valuesDataOffset + (16U * std::size_t{header.valueCount});

    std::vector<std::byte> b(total);
    for (std::size_t i = 0; i < COOKED_ANIMATION_MAGIC.size(); ++i) {
        b[i] = static_cast<std::byte>(COOKED_ANIMATION_MAGIC[i]);
    }
    put32(b, H_FORMAT_VERSION, COOKED_ANIMATION_FORMAT_VERSION);
    put32(b, H_COOKER_VERSION, 1);
    put64(b, H_GUID_HI, header.guidHi);
    put64(b, H_GUID_LO, header.guidLo);
    put32(b, H_CHANNEL_COUNT, channelCount);
    put32(b, H_KEY_COUNT, header.keyCount);
    put32(b, H_VALUE_COUNT, header.valueCount);
    put32(b, H_SOURCE_ANIMATION_INDEX, header.sourceAnimationIndex);
    putF(b, H_DURATION_SECONDS, header.durationSeconds);
    put64(b, H_TIMES_DATA_OFFSET, timesDataOffset);
    put64(b, H_VALUES_DATA_OFFSET, valuesDataOffset);
    put64(b, H_TOTAL_BYTES, total);

    for (std::uint32_t i = 0; i < channelCount; ++i) {
        const std::size_t o = rec(i);
        put32(b, o + C_TARGET_NODE_LOCAL_ID, channels[i].targetNodeLocalId);
        put16(b, o + C_PATH, channels[i].path);
        put16(b, o + C_INTERPOLATION, channels[i].interpolation);
        put32(b, o + C_KEY_COUNT, channels[i].keyCount);
        put32(b, o + C_FIRST_KEY, channels[i].firstKey);
        put32(b, o + C_FIRST_VALUE, channels[i].firstValue);
        put32(b, o + C_VALUE_COUNT, channels[i].valueCount);
    }
    for (std::uint32_t k = 0; k < header.keyCount; ++k) {
        putF(b, timesDataOffset + (4U * std::size_t{k}), 0.5F * static_cast<float>(k));
    }
    for (std::uint32_t v = 0; v < header.valueCount; ++v) {
        const std::size_t o = valuesDataOffset + (16U * std::size_t{v});
        const auto f = static_cast<float>(v);
        putF(b, o + 0, f);
        putF(b, o + 4, f + 0.25F);
        putF(b, o + 8, -f);
        putF(b, o + 12, 1.0F);
    }
    return b;
}

// The MINIMAL golden's shape, hand-built: one Linear Rotation channel on node 5, two keys. 160
// bytes, with the format's single padding site 8 bytes wide at [120, 128).
[[nodiscard]] std::vector<std::byte> buildOne() {
    const std::array<ChannelSpec, 1> channels = {ChannelSpec{5, 1, 0, 2, 0, 0, 2}};
    return build(std::span<const ChannelSpec>(channels), HeaderSpec{2, 2, 0, 0, 0, 0.5F});
}

// Two channels of DIFFERENT multipliers, so every running-sum and slice arm below has a shape where
// firstKey and firstValue diverge: a 3-key Linear Rotation on node 3 and a 2-key CubicSpline Scale
// on node 7. 320 bytes; the padding site is 12 bytes wide at [164, 176).
[[nodiscard]] std::vector<std::byte> buildTwo() {
    const std::array<ChannelSpec, 2> channels = {ChannelSpec{3, 1, 0, 3, 0, 0, 3}, ChannelSpec{7, 2, 2, 2, 3, 3, 6}};
    return build(std::span<const ChannelSpec>(channels), HeaderSpec{5, 9, 0, 0, 0, 1.0F});
}

[[nodiscard]] CookedAnimationParseResult parse(const std::vector<std::byte>& b) {
    return parseCookedAnimation(std::span<const std::byte>(b));
}

// A frozen golden, read as the bytes it is. The arrays have static storage, so the span the parse
// result retains outlives every case here.
[[nodiscard]] CookedAnimationParseResult parseGolden(std::span<const std::uint8_t> golden) {
    return parseCookedAnimation(std::as_bytes(golden));
}

[[nodiscard]] bool mentions(const std::string& message, std::string_view needle) {
    return message.find(needle) != std::string::npos;
}

[[nodiscard]] std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// Where a returned bulk slice begins, in bytes from the start of the buffer it was cut from.
[[nodiscard]] std::size_t sliceOffset(std::span<const std::byte> slice, const std::vector<std::byte>& whole) {
    return static_cast<std::size_t>(slice.data() - whole.data());
}

}  // namespace

TEST_CASE("cooked animation: the frozen minimal golden parses Ok, header word for header word (AN1)") {
    const CookedAnimationParseResult r = parseGolden(aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL);
    REQUIRE((r.status == CookedAnimationStatus::Ok));
    CHECK(r.message.empty());
    CHECK(r.animation.formatVersion == 1);
    CHECK(r.animation.cookerVersion == 1);
    CHECK(!r.animation.sourceGuid.valid());
    CHECK(r.animation.sourceAnimationIndex == 0);
    CHECK(r.animation.durationSeconds == doctest::Approx(0.5F));
    CHECK(r.animation.keyCount == 2);
    CHECK(r.animation.valueCount == 2);
    CHECK(r.animation.timesDataOffset == 112);
    CHECK(r.animation.valuesDataOffset == 128);
    CHECK(r.animation.bytes.size() == 160);
    CHECK(r.animation.channels.size() == 1);
    // The padding site is eight bytes wide here and every byte of it is zero -- read off the golden
    // rather than off the parser, which merely refuses a non-zero one.
    for (std::size_t at = 120; at < 128; ++at) {
        CHECK(aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL[at] == 0);
    }
}

TEST_CASE("cooked animation: the minimal golden's one record and both regions, exactly (AN2)") {
    const CookedAnimationParseResult r = parseGolden(aero_test::COOKED_ANIMATION_GOLDEN_MINIMAL);
    REQUIRE((r.status == CookedAnimationStatus::Ok));
    REQUIRE(r.animation.channels.size() == 1);
    const engine::assets::CookedAnimationChannel& channel = r.animation.channels[0];
    CHECK(channel.targetNodeLocalId == 5);
    CHECK((channel.path == CookedAnimationPath::Rotation));
    CHECK((channel.interpolation == CookedAnimationInterpolation::Linear));
    CHECK(channel.keyCount == 2);
    CHECK(channel.firstKey == 0);
    CHECK(channel.firstValue == 0);
    CHECK(channel.valueCount == 2);

    const std::span<const std::byte> times = channelTimeBytes(r.animation, 0);
    REQUIRE(times.size() == 8);
    CHECK(bits(animationKeyTime(times, 0)) == bits(0.0F));
    CHECK(bits(animationKeyTime(times, 1)) == bits(0.5F));

    const std::span<const std::byte> values = channelValueBytes(r.animation, 0);
    REQUIRE(values.size() == 32);
    const engine::Vec4 first = animationKeyValue(values, 0);
    CHECK(bits(first.x) == bits(0.0F));
    CHECK(bits(first.y) == bits(0.0F));
    CHECK(bits(first.z) == bits(0.0F));
    CHECK(bits(first.w) == bits(1.0F));
    const engine::Vec4 second = animationKeyValue(values, 1);
    CHECK(bits(second.x) == bits(0.0F));
    CHECK(bits(second.y) == bits(0.0F));
    CHECK(bits(second.z) == bits(1.0F));  // 180 degrees about Z
    CHECK(bits(second.w) == bits(0.0F));
}

TEST_CASE("cooked animation: the frozen mixed golden, all three records in emitted order (AN3)") {
    const CookedAnimationParseResult r = parseGolden(aero_test::COOKED_ANIMATION_GOLDEN_MIXED);
    REQUIRE((r.status == CookedAnimationStatus::Ok));
    CHECK(r.message.empty());
    CHECK(r.animation.formatVersion == 1);
    CHECK(r.animation.cookerVersion == 1);
    CHECK(r.animation.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(r.animation.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    CHECK(r.animation.sourceAnimationIndex == 2);
    CHECK(r.animation.durationSeconds == doctest::Approx(2.0F));
    CHECK(r.animation.keyCount == 7);
    CHECK(r.animation.valueCount == 11);
    CHECK(r.animation.timesDataOffset == 176);
    CHECK(r.animation.valuesDataOffset == 208);
    CHECK(r.animation.bytes.size() == 384);
    REQUIRE(r.animation.channels.size() == 3);

    // Emitted ascending by (targetNodeLocalId, path), which for the two node-7 channels means
    // Translation (0) before Scale (2) -- the property a node-only sort key cannot produce.
    CHECK(r.animation.channels[0].targetNodeLocalId == 3);
    CHECK((r.animation.channels[0].path == CookedAnimationPath::Rotation));
    CHECK((r.animation.channels[0].interpolation == CookedAnimationInterpolation::Linear));
    CHECK(r.animation.channels[0].keyCount == 3);
    CHECK(r.animation.channels[0].valueCount == 3);
    CHECK(r.animation.channels[1].targetNodeLocalId == 7);
    CHECK((r.animation.channels[1].path == CookedAnimationPath::Translation));
    CHECK((r.animation.channels[1].interpolation == CookedAnimationInterpolation::Step));
    CHECK(r.animation.channels[1].keyCount == 2);
    CHECK(r.animation.channels[1].valueCount == 2);
    CHECK(r.animation.channels[2].targetNodeLocalId == 7);
    CHECK((r.animation.channels[2].path == CookedAnimationPath::Scale));
    CHECK((r.animation.channels[2].interpolation == CookedAnimationInterpolation::CubicSpline));
    CHECK(r.animation.channels[2].keyCount == 2);
    CHECK(r.animation.channels[2].valueCount == 6);

    // Both running sums, which diverge from each other only at the cubic channel.
    CHECK(r.animation.channels[0].firstKey == 0);
    CHECK(r.animation.channels[1].firstKey == 3);
    CHECK(r.animation.channels[2].firstKey == 5);
    CHECK(r.animation.channels[0].firstValue == 0);
    CHECK(r.animation.channels[1].firstValue == 3);
    CHECK(r.animation.channels[2].firstValue == 5);

    // w is ZERO on both Translation values, though the cook was handed 7 and -8.
    const std::span<const std::byte> translation = channelValueBytes(r.animation, 1);
    REQUIRE(translation.size() == 32);
    CHECK(animationKeyValue(translation, 0).x == doctest::Approx(1.0F));
    CHECK(bits(animationKeyValue(translation, 0).w) == bits(0.0F));
    CHECK(animationKeyValue(translation, 1).x == doctest::Approx(4.0F));
    CHECK(bits(animationKeyValue(translation, 1).w) == bits(0.0F));
    // The Rotation channel's w is NOT zeroed -- the anti-vacuity twin, in the same array.
    CHECK(animationKeyValue(channelValueBytes(r.animation, 0), 0).w == doctest::Approx(1.0F));
    // The four-byte padding site, zero in the golden itself.
    for (std::size_t at = 204; at < 208; ++at) {
        CHECK(aero_test::COOKED_ANIMATION_GOLDEN_MIXED[at] == 0);
    }
}

TEST_CASE("cooked animation: buffers shorter than a whole file are refused at every boundary (AN4)") {
    const std::vector<std::byte> whole = buildOne();
    REQUIRE(whole.size() == 160);
    // Below the header: TooSmall, before a single field is interpreted.
    const std::array<std::size_t, 2> tooSmall = {0, 79};
    for (const std::size_t n : tooSmall) {
        const std::vector<std::byte> truncated(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        const CookedAnimationParseResult r = parse(truncated);
        CHECK((r.status == CookedAnimationStatus::TooSmall));
    }
    // At and past the header: the header's own totalBytes no longer equals the buffer, which is a
    // SizeMismatch and NOT a TooSmall -- the file says how long it is and the buffer disagrees. 80
    // is the header-only case, which still declares channelCount 1; 111 is one byte short of the
    // whole channel table.
    const std::array<std::size_t, 3> sizeMismatch = {80, 111, 159};
    for (const std::size_t n : sizeMismatch) {
        const std::vector<std::byte> truncated(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
        const CookedAnimationParseResult r = parse(truncated);
        CHECK((r.status == CookedAnimationStatus::SizeMismatch));
    }
    CHECK((parse(whole).status == CookedAnimationStatus::Ok));
}

TEST_CASE("cooked animation: the magic is compared over all eight bytes (AN5)") {
    CHECK(COOKED_ANIMATION_MAGIC == std::string_view{"AEROANIM"});
    CHECK(COOKED_ANIMATION_MAGIC.size() == 8);
    // Byte 7 ALONE is wrong -- the case a seven-byte comparison cannot see.
    std::vector<std::byte> b = buildOne();
    b[7] = static_cast<std::byte>('X');
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::BadMagic));
    CHECK(!r.message.empty());
}

TEST_CASE("cooked animation: an unreadable formatVersion is refused in both directions (AN6)") {
    const std::array<std::uint32_t, 2> versions = {0, 2};
    for (const std::uint32_t version : versions) {
        std::vector<std::byte> b = buildOne();
        put32(b, H_FORMAT_VERSION, version);
        const CookedAnimationParseResult r = parse(b);
        CHECK((r.status == CookedAnimationStatus::UnsupportedVersion));
        CHECK(mentions(r.message, "format version"));
    }
    // cookerVersion is INFORMATIONAL and never gates a parse -- the anti-vacuity twin.
    std::vector<std::byte> b = buildOne();
    put32(b, H_COOKER_VERSION, 99);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::Ok));
    CHECK(r.animation.cookerVersion == 99);
}

TEST_CASE("cooked animation: a non-zero reservedFlags is a refusal, not a field to ignore (AN7)") {
    std::vector<std::byte> b = buildOne();
    put32(b, H_RESERVED_FLAGS, 1);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::ReservedNotZero));
    CHECK(mentions(r.message, "reserved flags"));
}

TEST_CASE("cooked animation: a channel's non-zero reserved0 is refused and names the record (AN8)") {
    std::vector<std::byte> b = buildTwo();
    put64(b, rec(1) + C_RESERVED0, 0x0000000100000000ULL);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::ReservedNotZero));
    CHECK(mentions(r.message, "channel record 1"));
    // The high half alone is enough: the field is a whole u64 and the read must cover all of it.
    std::vector<std::byte> low = buildTwo();
    put64(low, rec(0) + C_RESERVED0, 1);
    const CookedAnimationParseResult r2 = parse(low);
    CHECK((r2.status == CookedAnimationStatus::ReservedNotZero));
    CHECK(mentions(r2.message, "channel record 0"));
}

TEST_CASE("cooked animation: the single padding site is CHECKED, at both of its ends (AN9)") {
    // buildOne's padding site is [120, 128) -- eight bytes, because keyCount % 4 == 2.
    const std::array<std::size_t, 2> ends = {120, 127};
    for (const std::size_t at : ends) {
        std::vector<std::byte> b = buildOne();
        b[at] = static_cast<std::byte>(0xFF);
        const CookedAnimationParseResult r = parse(b);
        CHECK((r.status == CookedAnimationStatus::ReservedNotZero));
        CHECK(mentions(r.message, "padding byte"));
    }
    // Anti-vacuity: the bytes on either SIDE of the site are real data and are not policed here.
    CHECK((parse(buildOne()).status == CookedAnimationStatus::Ok));
}

TEST_CASE("cooked animation: totalBytes is compared against the buffer AND against the layout (AN10)") {
    // Arm 1 -- off by one against the buffer's own size.
    std::vector<std::byte> b = buildOne();
    put64(b, H_TOTAL_BYTES, 161);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::SizeMismatch));
    CHECK(mentions(r.message, "total bytes"));

    // Arm 2 -- totalBytes agrees with the buffer and disagrees with valuesDataOffset + 16 * valueCount.
    std::vector<std::byte> inconsistent = buildOne();
    put32(inconsistent, H_VALUE_COUNT, 1);
    const CookedAnimationParseResult r2 = parse(inconsistent);
    CHECK((r2.status == CookedAnimationStatus::SizeMismatch));
    CHECK(mentions(r2.message, "values at offset"));
}

TEST_CASE("cooked animation: each of the three header counts is capped on its own (AN11)") {
    std::vector<std::byte> channels = buildOne();
    put32(channels, H_CHANNEL_COUNT, MAX_COOKED_ANIMATION_CHANNELS + 1);
    const CookedAnimationParseResult rc = parse(channels);
    CHECK((rc.status == CookedAnimationStatus::CapExceeded));
    CHECK(mentions(rc.message, "channels"));

    std::vector<std::byte> keys = buildOne();
    put32(keys, H_KEY_COUNT, MAX_COOKED_ANIMATION_KEYS + 1);
    const CookedAnimationParseResult rk = parse(keys);
    CHECK((rk.status == CookedAnimationStatus::CapExceeded));
    CHECK(mentions(rk.message, "keys"));

    std::vector<std::byte> values = buildOne();
    put32(values, H_VALUE_COUNT, MAX_COOKED_ANIMATION_VALUES + 1);
    const CookedAnimationParseResult rv = parse(values);
    CHECK((rv.status == CookedAnimationStatus::CapExceeded));
    CHECK(mentions(rv.message, "values"));
}

TEST_CASE("cooked animation: a .aeroanim is never empty -- all three counts are >= 1 (AN12)") {
    std::vector<std::byte> channels = buildOne();
    put32(channels, H_CHANNEL_COUNT, 0);
    const CookedAnimationParseResult rc = parse(channels);
    CHECK((rc.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(rc.message, "zero channels"));

    std::vector<std::byte> keys = buildOne();
    put32(keys, H_KEY_COUNT, 0);
    const CookedAnimationParseResult rk = parse(keys);
    CHECK((rk.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(rk.message, "zero keys"));

    std::vector<std::byte> values = buildOne();
    put32(values, H_VALUE_COUNT, 0);
    const CookedAnimationParseResult rv = parse(values);
    CHECK((rv.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(rv.message, "zero values"));
}

TEST_CASE("cooked animation: a channel declaring zero keys is refused and names the record (AN13)") {
    std::vector<std::byte> b = buildTwo();
    put32(b, rec(1) + C_KEY_COUNT, 0);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(r.message, "channel record 1"));
    CHECK(mentions(r.message, "zero keys"));
}

TEST_CASE("cooked animation: an undefined path code is a refusal, never a reinterpretation (AN14)") {
    std::vector<std::byte> b = buildTwo();
    put16(b, rec(1) + C_PATH, 3);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(r.message, "channel record 1"));
    CHECK(mentions(r.message, "path code 3"));
    // 65535 is as undefined as 3, and the field is a whole u16.
    std::vector<std::byte> wide = buildTwo();
    put16(wide, rec(0) + C_PATH, 65535);
    CHECK((parse(wide).status == CookedAnimationStatus::BadTable));
}

TEST_CASE("cooked animation: an undefined interpolation code is a refusal (AN15)") {
    std::vector<std::byte> b = buildTwo();
    put16(b, rec(0) + C_INTERPOLATION, 3);
    const CookedAnimationParseResult r = parse(b);
    CHECK((r.status == CookedAnimationStatus::BadTable));
    CHECK(mentions(r.message, "channel record 0"));
    CHECK(mentions(r.message, "interpolation code 3"));
    std::vector<std::byte> wide = buildTwo();
    put16(wide, rec(1) + C_INTERPOLATION, 65535);
    CHECK((parse(wide).status == CookedAnimationStatus::BadTable));
}

TEST_CASE("cooked animation: valueCount must be keyCount times THE multiplier, both ways (AN16)") {
    // A Linear channel claiming three values per key.
    {
        const std::array<ChannelSpec, 1> channels = {ChannelSpec{5, 1, 0, 2, 0, 0, 6}};
        const std::vector<std::byte> b = build(std::span<const ChannelSpec>(channels), HeaderSpec{2, 6, 0, 0, 0, 0.5F});
        const CookedAnimationParseResult r = parse(b);
        CHECK((r.status == CookedAnimationStatus::BadTable));
        CHECK(mentions(r.message, "channel record 0"));
        CHECK(mentions(r.message, "Linear"));
    }
    // A CubicSpline channel claiming one.
    {
        const std::array<ChannelSpec, 1> channels = {ChannelSpec{5, 2, 2, 2, 0, 0, 2}};
        const std::vector<std::byte> b = build(std::span<const ChannelSpec>(channels), HeaderSpec{2, 2, 0, 0, 0, 0.5F});
        const CookedAnimationParseResult r = parse(b);
        CHECK((r.status == CookedAnimationStatus::BadTable));
        CHECK(mentions(r.message, "Cubic spline"));
    }
}

TEST_CASE("cooked animation: the times region offset is enforced EXACTLY, with two messages (AN17)") {
    // Unaligned.
    std::vector<std::byte> unaligned = buildOne();
    put64(unaligned, H_TIMES_DATA_OFFSET, 113);
    const CookedAnimationParseResult ru = parse(unaligned);
    CHECK((ru.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(ru.message, "times region offset is not 16-aligned"));

    // Aligned, and not immediately after the channel table.
    std::vector<std::byte> moved = buildOne();
    put64(moved, H_TIMES_DATA_OFFSET, 128);
    const CookedAnimationParseResult rm = parse(moved);
    CHECK((rm.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(rm.message, "does not begin immediately after the channel table"));
    CHECK(ru.message != rm.message);
}

TEST_CASE("cooked animation: the values region offset is enforced EXACTLY, with two messages (AN18)") {
    std::vector<std::byte> unaligned = buildOne();
    put64(unaligned, H_VALUES_DATA_OFFSET, 129);
    const CookedAnimationParseResult ru = parse(unaligned);
    CHECK((ru.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(ru.message, "values region offset is not 16-aligned"));

    std::vector<std::byte> moved = buildOne();
    put64(moved, H_VALUES_DATA_OFFSET, 160);
    const CookedAnimationParseResult rm = parse(moved);
    CHECK((rm.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(rm.message, "single padding site"));
    CHECK(ru.message != rm.message);
}

TEST_CASE("cooked animation: a MISPOSITIONED padding site is what exact offsets exist to see (AN19)") {
    // buildOne's one legal values offset is 128. Sixteen too far and sixteen too near are both files
    // whose padding site is the wrong width -- and equality is the ONLY check that can see either.
    const std::array<std::uint64_t, 2> wrong = {144, 112};
    for (const std::uint64_t offset : wrong) {
        std::vector<std::byte> b = buildOne();
        put64(b, H_VALUES_DATA_OFFSET, offset);
        const CookedAnimationParseResult r = parse(b);
        CHECK((r.status == CookedAnimationStatus::BadRange));
        CHECK(mentions(r.message, "single padding site"));
    }
}

TEST_CASE("cooked animation: a channel slice outside its region is refused, by subtraction (AN20)") {
    // buildTwo's times region holds 5 keys and its values region 9 values.
    std::vector<std::byte> pastEnd = buildTwo();
    put32(pastEnd, rec(1) + C_FIRST_KEY, 4);  // 4 + 2 > 5
    const CookedAnimationParseResult rk = parse(pastEnd);
    CHECK((rk.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(rk.message, "channel record 1"));
    CHECK(mentions(rk.message, "times region"));

    std::vector<std::byte> wayPast = buildTwo();
    put32(wayPast, rec(1) + C_FIRST_KEY, 6);  // firstKey alone is already past the region
    CHECK((parse(wayPast).status == CookedAnimationStatus::BadRange));

    std::vector<std::byte> values = buildTwo();
    put32(values, rec(1) + C_FIRST_VALUE, 4);  // 4 + 6 > 9
    const CookedAnimationParseResult rv = parse(values);
    CHECK((rv.status == CookedAnimationStatus::BadRange));
    CHECK(mentions(rv.message, "channel record 1"));
    CHECK(mentions(rv.message, "values region"));

    // The near miss on either side: the LAST legal placement of each parses Ok.
    std::vector<std::byte> exact = buildTwo();
    put32(exact, rec(1) + C_FIRST_KEY, 3);
    put32(exact, rec(1) + C_FIRST_VALUE, 3);
    CHECK((parse(exact).status == CookedAnimationStatus::Ok));
}

TEST_CASE("cooked animation: both bulk accessors are total and cut exactly the right slice (AN21)") {
    const std::vector<std::byte> whole = buildTwo();
    const CookedAnimationParseResult r = parse(whole);
    REQUIRE((r.status == CookedAnimationStatus::Ok));
    REQUIRE(r.animation.channels.size() == 2);

    // Channel 0: 3 keys at times offset 144, 3 values at values offset 176.
    const std::span<const std::byte> t0 = channelTimeBytes(r.animation, 0);
    CHECK(t0.size() == 12);
    CHECK(sliceOffset(t0, whole) == 144);
    CHECK(animationKeyTime(t0, 0) == doctest::Approx(0.0F));
    CHECK(animationKeyTime(t0, 2) == doctest::Approx(1.0F));
    const std::span<const std::byte> v0 = channelValueBytes(r.animation, 0);
    CHECK(v0.size() == 48);
    CHECK(sliceOffset(v0, whole) == 176);
    CHECK(animationKeyValue(v0, 0).x == doctest::Approx(0.0F));

    // Channel 1: firstKey 3 and firstValue 3, so both slices are OFFSET -- and their contents prove
    // it, since the fill derives from the GLOBAL element index.
    const std::span<const std::byte> t1 = channelTimeBytes(r.animation, 1);
    CHECK(t1.size() == 8);
    CHECK(sliceOffset(t1, whole) == 156);
    CHECK(animationKeyTime(t1, 0) == doctest::Approx(1.5F));
    const std::span<const std::byte> v1 = channelValueBytes(r.animation, 1);
    CHECK(v1.size() == 96);
    CHECK(sliceOffset(v1, whole) == 224);
    CHECK(animationKeyValue(v1, 0).x == doctest::Approx(3.0F));
    CHECK(animationKeyValue(v1, 5).x == doctest::Approx(8.0F));

    // An out-of-range channel index is an EMPTY span, never a read.
    CHECK(channelTimeBytes(r.animation, 2).empty());
    CHECK(channelValueBytes(r.animation, 2).empty());
    CHECK(channelTimeBytes(r.animation, 0xFFFFFFFFU).empty());
    CHECK(channelValueBytes(r.animation, 0xFFFFFFFFU).empty());
}

TEST_CASE("cooked animation: the two typed readers move bits, not values (AN22)") {
    // A hand-built region: -0.0f and the signalling-NaN pattern 0x7FA00000 both survive a round trip
    // BIT FOR BIT, which doctest::Approx cannot see and == cannot even express for a NaN.
    std::vector<std::byte> region(32);
    putF(region, 0, -0.0F);
    put32(region, 4, 0x7FA00000U);
    put32(region, 8, 0x7FA00000U);
    putF(region, 12, -0.0F);
    put32(region, 16, 0x7FA00000U);
    putF(region, 20, -0.0F);
    put32(region, 24, 0x7FA00000U);
    putF(region, 28, -0.0F);

    const std::span<const std::byte> bulk(region);
    CHECK(bits(animationKeyTime(bulk, 0)) == bits(-0.0F));
    CHECK(bits(animationKeyTime(bulk, 0)) != bits(0.0F));  // the whole point: +0 and -0 are distinct
    CHECK(bits(animationKeyTime(bulk, 1)) == 0x7FA00000U);

    const engine::Vec4 value = animationKeyValue(bulk, 1);
    CHECK(bits(value.x) == 0x7FA00000U);
    CHECK(bits(value.y) == bits(-0.0F));
    CHECK(bits(value.z) == 0x7FA00000U);
    CHECK(bits(value.w) == bits(-0.0F));
}

TEST_CASE("cooked animation: the three labels are total and distinct, and message is empty iff Ok (AN23)") {
    const std::array<CookedAnimationPath, 3> paths = {CookedAnimationPath::Translation, CookedAnimationPath::Rotation,
                                                      CookedAnimationPath::Scale};
    CHECK(paths.size() == 3);  // literal row count
    for (std::size_t i = 0; i < paths.size(); ++i) {
        CHECK(!cookedAnimationPathLabel(paths[i]).empty());
        for (std::size_t j = i + 1; j < paths.size(); ++j) {
            CHECK(cookedAnimationPathLabel(paths[i]) != cookedAnimationPathLabel(paths[j]));
        }
    }
    const std::array<CookedAnimationInterpolation, 3> modes = {CookedAnimationInterpolation::Linear,
                                                               CookedAnimationInterpolation::Step,
                                                               CookedAnimationInterpolation::CubicSpline};
    CHECK(modes.size() == 3);  // literal row count
    for (std::size_t i = 0; i < modes.size(); ++i) {
        CHECK(!cookedAnimationInterpolationLabel(modes[i]).empty());
        for (std::size_t j = i + 1; j < modes.size(); ++j) {
            CHECK(cookedAnimationInterpolationLabel(modes[i]) != cookedAnimationInterpolationLabel(modes[j]));
        }
    }
    const std::array<CookedAnimationStatus, 9> statuses = {CookedAnimationStatus::Ok,
                                                           CookedAnimationStatus::TooSmall,
                                                           CookedAnimationStatus::BadMagic,
                                                           CookedAnimationStatus::UnsupportedVersion,
                                                           CookedAnimationStatus::ReservedNotZero,
                                                           CookedAnimationStatus::SizeMismatch,
                                                           CookedAnimationStatus::CapExceeded,
                                                           CookedAnimationStatus::BadTable,
                                                           CookedAnimationStatus::BadRange};
    CHECK(statuses.size() == 9);  // literal row count
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        CHECK(!cookedAnimationStatusLabel(statuses[i]).empty());
        for (std::size_t j = i + 1; j < statuses.size(); ++j) {
            CHECK(cookedAnimationStatusLabel(statuses[i]) != cookedAnimationStatusLabel(statuses[j]));
        }
    }
    // A code this format does not define is TOTAL rather than undefined behaviour.
    CHECK(cookedAnimationPathLabel(static_cast<CookedAnimationPath>(7)) == std::string_view{"Unknown"});
    CHECK(cookedAnimationInterpolationLabel(static_cast<CookedAnimationInterpolation>(7)) ==
          std::string_view{"Unknown"});

    // message is "" IFF Ok, over one arm of every refusal this file can reach.
    std::vector<std::vector<std::byte>> arms;
    arms.push_back(buildOne());
    arms.emplace_back(8);
    arms.push_back(buildOne());
    arms.back()[3] = static_cast<std::byte>('X');
    arms.push_back(buildOne());
    put32(arms.back(), H_FORMAT_VERSION, 7);
    arms.push_back(buildOne());
    put32(arms.back(), H_RESERVED_FLAGS, 3);
    arms.push_back(buildOne());
    put64(arms.back(), H_TOTAL_BYTES, 1);
    arms.push_back(buildOne());
    put32(arms.back(), H_KEY_COUNT, MAX_COOKED_ANIMATION_KEYS + 1);
    arms.push_back(buildOne());
    put32(arms.back(), H_CHANNEL_COUNT, 0);
    arms.push_back(buildOne());
    put64(arms.back(), H_TIMES_DATA_OFFSET, 113);
    CHECK(arms.size() == 9);  // literal arm count
    for (const std::vector<std::byte>& arm : arms) {
        const CookedAnimationParseResult r = parse(arm);
        CHECK(r.message.empty() == (r.status == CookedAnimationStatus::Ok));
    }
}

TEST_CASE("cooked animation: the multiplier, the two predicates and the padding formula (AN24)") {
    // Pinned against LITERALS, never against the formula applied to itself.
    CHECK(cookedAnimationValuesPerKey(CookedAnimationInterpolation::Linear) == 1);
    CHECK(cookedAnimationValuesPerKey(CookedAnimationInterpolation::Step) == 1);
    CHECK(cookedAnimationValuesPerKey(CookedAnimationInterpolation::CubicSpline) == 3);
    // Total even for a code this format does not define.
    CHECK(cookedAnimationValuesPerKey(static_cast<CookedAnimationInterpolation>(9)) == 1);

    CHECK(isCookedAnimationPath(CookedAnimationPath::Translation));
    CHECK(isCookedAnimationPath(CookedAnimationPath::Rotation));
    CHECK(isCookedAnimationPath(CookedAnimationPath::Scale));
    CHECK(!isCookedAnimationPath(static_cast<CookedAnimationPath>(3)));
    CHECK(!isCookedAnimationPath(static_cast<CookedAnimationPath>(65535)));
    CHECK(isCookedAnimationInterpolation(CookedAnimationInterpolation::Linear));
    CHECK(isCookedAnimationInterpolation(CookedAnimationInterpolation::Step));
    CHECK(isCookedAnimationInterpolation(CookedAnimationInterpolation::CubicSpline));
    CHECK(!isCookedAnimationInterpolation(static_cast<CookedAnimationInterpolation>(3)));
    CHECK(!isCookedAnimationInterpolation(static_cast<CookedAnimationInterpolation>(65535)));

    // 12 / 8 / 4 / 0 for keyCount % 4 == 1 / 2 / 3 / 0, as literals.
    CHECK(cookedAnimationTimesPadding(1) == 12);
    CHECK(cookedAnimationTimesPadding(2) == 8);
    CHECK(cookedAnimationTimesPadding(3) == 4);
    CHECK(cookedAnimationTimesPadding(4) == 0);
    CHECK(cookedAnimationTimesPadding(5) == 12);
    CHECK(cookedAnimationTimesPadding(6) == 8);
    CHECK(cookedAnimationTimesPadding(7) == 4);
    CHECK(cookedAnimationTimesPadding(8) == 0);
    CHECK(cookedAnimationTimesPadding(0) == 0);
    CHECK(cookedAnimationTimesPadding(MAX_COOKED_ANIMATION_KEYS) == 0);
}
