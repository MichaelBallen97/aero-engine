// tests/cooked_audio_test.cpp -- task 3.7.1: the .aerowave container v1. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 14's own tables -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly ONE field of something already valid. There is no cook
// in this TU at all; audio_cook_test.cpp owns the writer, and CA10's hand-built golden is what AK2
// compares the writer's output against.
#include <aero/assets/cooked_audio.hpp>

#include <doctest/doctest.h>

#include <array>
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

using engine::Guid;
using engine::assets::audioSample;
using engine::assets::audioSampleBytes;
using engine::assets::COOKED_AUDIO_ALIGNMENT;
using engine::assets::COOKED_AUDIO_COOKER_VERSION;
using engine::assets::COOKED_AUDIO_FORMAT_VERSION;
using engine::assets::COOKED_AUDIO_HEADER_BYTES;
using engine::assets::COOKED_AUDIO_MAGIC;
using engine::assets::COOKED_AUDIO_SAMPLE_BYTES;
using engine::assets::cookedAudioDurationSeconds;
using engine::assets::CookedAudioParseResult;
using engine::assets::CookedAudioStatus;
using engine::assets::cookedAudioStatusLabel;
using engine::assets::getU32;
using engine::assets::getU64;
using engine::assets::MAX_COOKED_AUDIO_BYTES;
using engine::assets::MAX_COOKED_AUDIO_CHANNELS;
using engine::assets::MAX_COOKED_AUDIO_FRAMES;
using engine::assets::MAX_COOKED_AUDIO_SAMPLE_RATE;
using engine::assets::MAX_COOKED_AUDIO_SAMPLES;
using engine::assets::MIN_COOKED_AUDIO_SAMPLE_RATE;
using engine::assets::parseCookedAudio;
using engine::assets::putU16;
using engine::assets::putU32;
using engine::assets::putU64;

namespace {

// The header size and the sample size are part of the on-disk format, so they are pinned as BUILD
// failures rather than as a case: a change to either cannot be argued with at runtime.
static_assert(COOKED_AUDIO_HEADER_BYTES == 64);
static_assert(COOKED_AUDIO_SAMPLE_BYTES == 2);

// The standard test GUID this whole task uses: hi = 0x0123456789abcdef, lo = 0xfedcba9876543210,
// which is the text form 0123456789abcdeffedcba9876543210 and writes as the byte sequence
// efcdab8967452301 1032547698badcfe -- every byte non-zero, so an assertion against it is a
// statement about byte ORDER and not merely about presence.
constexpr Guid TEST_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

// The header's offsets, spelled here as LITERALS on purpose. This TU is the one place they are not
// taken from detail::H_*: a golden that reads its offsets from the same constants the parser uses
// cannot see either of them move. docs/09 section 14.1 is what these mirror.
constexpr std::size_t AT_MAGIC = 0;
constexpr std::size_t AT_FORMAT_VERSION = 8;
constexpr std::size_t AT_COOKER_VERSION = 12;
constexpr std::size_t AT_GUID_HI = 16;
constexpr std::size_t AT_GUID_LO = 24;
constexpr std::size_t AT_SAMPLE_RATE = 32;
constexpr std::size_t AT_CHANNELS = 36;
constexpr std::size_t AT_FRAME_COUNT = 40;
constexpr std::size_t AT_RESERVED_FLAGS = 44;
constexpr std::size_t AT_SAMPLE_DATA_OFFSET = 48;
constexpr std::size_t AT_TOTAL_BYTES = 56;

// Builds a well-formed .aerowave over `samples`, which must already be interleaved frame-major.
// Every refusal arm below starts from one of these and mutates exactly ONE field.
[[nodiscard]] std::vector<std::byte> buildAudio(std::uint32_t sampleRate, std::uint32_t channels,
                                                std::uint32_t frameCount, Guid guid,
                                                std::span<const std::int16_t> samples) {
    const std::size_t total = COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * samples.size());
    std::vector<std::byte> bytes(total);
    const std::span<std::byte> w(bytes);
    for (std::size_t i = 0; i < COOKED_AUDIO_MAGIC.size(); ++i) {
        w[AT_MAGIC + i] = static_cast<std::byte>(COOKED_AUDIO_MAGIC[i]);
    }
    putU32(w, AT_FORMAT_VERSION, COOKED_AUDIO_FORMAT_VERSION);
    putU32(w, AT_COOKER_VERSION, COOKED_AUDIO_COOKER_VERSION);
    putU64(w, AT_GUID_HI, guid.hi);
    putU64(w, AT_GUID_LO, guid.lo);
    putU32(w, AT_SAMPLE_RATE, sampleRate);
    putU32(w, AT_CHANNELS, channels);
    putU32(w, AT_FRAME_COUNT, frameCount);
    putU32(w, AT_RESERVED_FLAGS, 0);
    putU64(w, AT_SAMPLE_DATA_OFFSET, COOKED_AUDIO_HEADER_BYTES);
    putU64(w, AT_TOTAL_BYTES, total);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        putU16(w, COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * i), static_cast<std::uint16_t>(samples[i]));
    }
    return bytes;
}

// A HEADER-ONLY buffer: 64 bytes claiming counts whose sample region is absent. Used by every arm
// that must be refused BEFORE anything sizes a region -- the point of checking totalBytes against
// bytes.size() is that these cost no allocation at all.
[[nodiscard]] std::vector<std::byte> buildHeaderOnly(std::uint32_t sampleRate, std::uint32_t channels,
                                                     std::uint32_t frameCount) {
    std::vector<std::byte> bytes(COOKED_AUDIO_HEADER_BYTES);
    const std::span<std::byte> w(bytes);
    for (std::size_t i = 0; i < COOKED_AUDIO_MAGIC.size(); ++i) {
        w[AT_MAGIC + i] = static_cast<std::byte>(COOKED_AUDIO_MAGIC[i]);
    }
    putU32(w, AT_FORMAT_VERSION, COOKED_AUDIO_FORMAT_VERSION);
    putU32(w, AT_COOKER_VERSION, COOKED_AUDIO_COOKER_VERSION);
    putU32(w, AT_SAMPLE_RATE, sampleRate);
    putU32(w, AT_CHANNELS, channels);
    putU32(w, AT_FRAME_COUNT, frameCount);
    putU64(w, AT_SAMPLE_DATA_OFFSET, COOKED_AUDIO_HEADER_BYTES);
    putU64(w, AT_TOTAL_BYTES, COOKED_AUDIO_HEADER_BYTES);
    return bytes;
}

// The two-frame mono reference used by CA10-CA12 and by most refusal arms.
constexpr std::array<std::int16_t, 2> REF_SAMPLES{0x0201, -2};

[[nodiscard]] std::vector<std::byte> buildReference() { return buildAudio(8000, 1, 2, TEST_GUID, REF_SAMPLES); }

}  // namespace

// ---- Group 1: the constants and the static_asserts' premises (CA1-CA5) -------------------------

TEST_CASE("CA1: the container's identity constants are pinned against literals") {
    CHECK(COOKED_AUDIO_MAGIC == std::string_view("AEROWAVE"));
    CHECK(COOKED_AUDIO_MAGIC.size() == 8);
    // 8 ASCII bytes, NO NUL: a string_view over a literal does not carry the terminator, and the
    // writer copies exactly size() bytes.
    for (const char c : COOKED_AUDIO_MAGIC) {
        CHECK(c != '\0');
    }
    CHECK(COOKED_AUDIO_FORMAT_VERSION == 1);
    CHECK(COOKED_AUDIO_COOKER_VERSION == 1);
}

TEST_CASE("CA2: the header size, its alignment and the sample size") {
    CHECK(COOKED_AUDIO_HEADER_BYTES == 64);
    CHECK(COOKED_AUDIO_HEADER_BYTES % COOKED_AUDIO_ALIGNMENT == 0);
    CHECK(COOKED_AUDIO_ALIGNMENT == 16);
    CHECK(COOKED_AUDIO_SAMPLE_BYTES == 2);
}

TEST_CASE("CA3: the five caps, against literals") {
    CHECK(MIN_COOKED_AUDIO_SAMPLE_RATE == 8000);
    CHECK(MAX_COOKED_AUDIO_SAMPLE_RATE == 384000);
    CHECK(MAX_COOKED_AUDIO_CHANNELS == 8);
    CHECK(MAX_COOKED_AUDIO_FRAMES == 28800000);
    CHECK(MAX_COOKED_AUDIO_SAMPLES == 57600000);
}

TEST_CASE("CA4: the two derived caps are derived, and their values are pinned") {
    CHECK(MAX_COOKED_AUDIO_SAMPLES == 2ULL * MAX_COOKED_AUDIO_FRAMES);
    // 64 + 2 * 57 600 000. The runtime half of the static_assert tripwire in the header: if someone
    // deletes that assert AND replaces the definition with a literal, this is what still catches it.
    CHECK(MAX_COOKED_AUDIO_BYTES == 115200064ULL);
    CHECK(MAX_COOKED_AUDIO_BYTES == COOKED_AUDIO_HEADER_BYTES + (COOKED_AUDIO_SAMPLE_BYTES * MAX_COOKED_AUDIO_SAMPLES));
}

TEST_CASE("CA5: the two rate bounds are miniaudio's own standard-rate bounds") {
    // ma_standard_sample_rate_min == ma_standard_sample_rate_8000 and ma_standard_sample_rate_max ==
    // ma_standard_sample_rate_384000, miniaudio.h:4342-4343 in the pinned 0.11.25 tree. Pinned here
    // against the LITERALS rather than against the miniaudio enumerators, because engine/assets links
    // no vcpkg package at all and must never see that header (D10) -- so the correspondence is a
    // documented fact checked against numbers, which is the only form available at this tier.
    CHECK(MIN_COOKED_AUDIO_SAMPLE_RATE == 8000);
    CHECK(MAX_COOKED_AUDIO_SAMPLE_RATE == 384000);
    CHECK(MIN_COOKED_AUDIO_SAMPLE_RATE < MAX_COOKED_AUDIO_SAMPLE_RATE);
}

// ---- Group 2: cookedAudioDurationSeconds and audioSample (CA6-CA9) -----------------------------

TEST_CASE("CA6: the duration is frameCount / sampleRate, exactly") {
    // All three are exactly representable in fp32, so these are equality comparisons on purpose.
    CHECK(cookedAudioDurationSeconds(8000, 2000) == 0.25F);
    CHECK(cookedAudioDurationSeconds(8000, 8000) == 1.0F);
    CHECK(cookedAudioDurationSeconds(48000, 48000) == 1.0F);
    CHECK(cookedAudioDurationSeconds(44100, 22050) == 0.5F);
}

TEST_CASE("CA7: a zero sample rate answers 0.0F rather than dividing") {
    // THE TOTAL. parseCookedAudio refuses a zero rate and this function does not, deliberately: it
    // is the one spelling of the duration and a caller holding an unparsed header must be able to
    // ask without checking first.
    CHECK(cookedAudioDurationSeconds(0, 1000) == 0.0F);
    CHECK(cookedAudioDurationSeconds(0, 0) == 0.0F);
}

TEST_CASE("CA8: audioSample reads little-endian s16 out of a byte span") {
    const std::array<std::byte, 6> raw{std::byte{0x01}, std::byte{0x02}, std::byte{0xFF},
                                       std::byte{0xFF}, std::byte{0x00}, std::byte{0x80}};
    const std::span<const std::byte> s(raw);
    CHECK(audioSample(s, 0) == static_cast<std::int16_t>(0x0201));  // LE: low byte first
    CHECK(audioSample(s, 1) == static_cast<std::int16_t>(-1));      // a negative value round-trips
    CHECK(audioSample(s, 2) == static_cast<std::int16_t>(-32768));  // INT16_MIN
}

TEST_CASE("CA9: audioSample is TOTAL -- an out-of-range index answers 0 and never reads") {
    const std::array<std::byte, 4> raw{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const std::span<const std::byte> s(raw);
    CHECK(audioSample(s, 1) == static_cast<std::int16_t>(0x4433));
    CHECK(audioSample(s, 2) == 0);  // index == count
    CHECK(audioSample(s, 3) == 0);  // index == count + 1
    CHECK(audioSample(s, 0xFFFFFFFFFFFFFFFFULL) == 0);
    CHECK(audioSample(std::span<const std::byte>(), 0) == 0);  // an EMPTY span
}

// ---- Group 3: the hand-built byte golden, field by field at LITERAL offsets (CA10-CA12) ---------

TEST_CASE("CA10: the byte layout, every field at its literal offset") {
    const std::vector<std::byte> bytes = buildReference();
    REQUIRE(bytes.size() == 68);  // 64 + 2 * 1 * 2
    const std::span<const std::byte> r(bytes);

    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<char>(r[i]) == COOKED_AUDIO_MAGIC[i]);
    }
    CHECK(getU32(r, 8) == 1U);             // formatVersion
    CHECK(getU32(r, 12) == 1U);            // cookerVersion
    CHECK(getU64(r, 16) == TEST_GUID.hi);  // sourceGuid.hi
    CHECK(getU64(r, 24) == TEST_GUID.lo);  // sourceGuid.lo
    CHECK(getU32(r, 32) == 8000U);         // sampleRate
    CHECK(getU32(r, 36) == 1U);            // channels
    CHECK(getU32(r, 40) == 2U);            // frameCount
    CHECK(getU32(r, 44) == 0U);            // reservedFlags
    CHECK(getU64(r, 48) == 64ULL);         // sampleDataOffset
    CHECK(getU64(r, 56) == 68ULL);         // totalBytes
    CHECK(audioSample(r.subspan(64), 0) == REF_SAMPLES[0]);
    CHECK(audioSample(r.subspan(64), 1) == REF_SAMPLES[1]);

    // The GUID's byte ORDER, spelled out: hi then lo, each little-endian, every byte non-zero.
    const std::array<std::uint8_t, 16> expectedGuidBytes{0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                                                         0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(static_cast<std::uint8_t>(r[16 + i]) == expectedGuidBytes[i]);
    }
}

TEST_CASE("CA11: parsing the golden reads back every field") {
    const std::vector<std::byte> bytes = buildReference();
    const CookedAudioParseResult parsed = parseCookedAudio(bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    CHECK(parsed.message.empty());
    CHECK(parsed.audio.formatVersion == 1U);
    CHECK(parsed.audio.cookerVersion == 1U);
    CHECK(parsed.audio.sourceGuid == TEST_GUID);
    CHECK(parsed.audio.sampleRate == 8000U);
    CHECK(parsed.audio.channels == 1U);
    CHECK(parsed.audio.frameCount == 2U);
    CHECK(parsed.audio.sampleDataOffset == 64ULL);
    CHECK(parsed.audio.totalBytes == 68ULL);

    const std::span<const std::byte> samples = audioSampleBytes(parsed.audio);
    CHECK(samples.size() == 4);
    CHECK(audioSample(samples, 0) == REF_SAMPLES[0]);
    CHECK(audioSample(samples, 1) == REF_SAMPLES[1]);
    CHECK(cookedAudioDurationSeconds(parsed.audio.sampleRate, parsed.audio.frameCount) == 0.00025F);
}

TEST_CASE("CA12: the parsed span IS the caller's buffer -- never a copy") {
    const std::vector<std::byte> bytes = buildReference();
    const CookedAudioParseResult parsed = parseCookedAudio(bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    CHECK(parsed.audio.bytes.data() == bytes.data());
    CHECK(parsed.audio.bytes.size() == bytes.size());
    // And the sample accessor is a subspan of that same buffer, offset by the header.
    CHECK(audioSampleBytes(parsed.audio).data() == bytes.data() + COOKED_AUDIO_HEADER_BYTES);
}

// ---- Group 4: the hostile-input battery (CA13-CA26) ---------------------------------------------

TEST_CASE("CA13: a buffer shorter than the header is TooSmall") {
    CHECK(parseCookedAudio(std::span<const std::byte>()).status == CookedAudioStatus::TooSmall);
    const std::vector<std::byte> one(1);
    CHECK(parseCookedAudio(one).status == CookedAudioStatus::TooSmall);
    const std::vector<std::byte> almost(COOKED_AUDIO_HEADER_BYTES - 1);
    const CookedAudioParseResult parsed = parseCookedAudio(almost);
    CHECK(parsed.status == CookedAudioStatus::TooSmall);
    CHECK(parsed.message.find("63") != std::string::npos);
    CHECK(parsed.message.find("64") != std::string::npos);
}

TEST_CASE("CA14: a wrong magic is BadMagic, including a one-byte difference in the last byte") {
    std::vector<std::byte> bytes = buildReference();
    bytes[7] = static_cast<std::byte>('F');  // AEROWAVF -- the LAST byte, never a prefix check
    CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::BadMagic);

    std::vector<std::byte> zeroed = buildReference();
    for (std::size_t i = 0; i < 8; ++i) {
        zeroed[i] = std::byte{0};
    }
    CHECK(parseCookedAudio(zeroed).status == CookedAudioStatus::BadMagic);
}

TEST_CASE("CA15: any formatVersion other than 1 is UnsupportedVersion") {
    for (const std::uint32_t version : {0U, 2U, 0xFFFFFFFFU}) {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_FORMAT_VERSION, version);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::UnsupportedVersion);
        CHECK(!parsed.message.empty());
    }
}

TEST_CASE("CA16: a non-zero reservedFlags is a REFUSAL, never an ignore") {
    for (const std::uint32_t flags : {1U, 0x80000000U}) {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_RESERVED_FLAGS, flags);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::ReservedNotZero);
    }
}

TEST_CASE("CA17: totalBytes disagreeing with the buffer is SizeMismatch") {
    // Both arms keep totalBytes CONSISTENT with channels x frames -- what moves is the buffer, so it
    // is the totalBytes == bytes.size() identity that fires and CA18's stays green.
    {
        std::vector<std::byte> bytes = buildReference();
        bytes.pop_back();  // 67 bytes, header still says 68
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::SizeMismatch);
        CHECK(parsed.message.find("67") != std::string::npos);
    }
    {
        std::vector<std::byte> bytes = buildReference();
        bytes.push_back(std::byte{0});  // 69 bytes, header still says 68
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::SizeMismatch);
        CHECK(parsed.message.find("69") != std::string::npos);
    }
}

TEST_CASE("CA18: totalBytes agreeing with the buffer but NOT with channels x frames is SizeMismatch") {
    // THE ARM CA17 CANNOT SEE. The buffer is resized so totalBytes == bytes.size() holds and only the
    // format's own arithmetic disagrees -- which is what proves the two identities are independent
    // rather than one of them being dead code.
    std::vector<std::byte> bytes = buildReference();
    bytes.resize(70);
    putU64(std::span<std::byte>(bytes), AT_TOTAL_BYTES, 70);
    const CookedAudioParseResult parsed = parseCookedAudio(bytes);
    CHECK(parsed.status == CookedAudioStatus::SizeMismatch);
    CHECK(parsed.message.find("70") != std::string::npos);
    CHECK(parsed.message.find("68") != std::string::npos);  // what 1 channel x 2 frames actually needs
}

TEST_CASE("CA19: sampleDataOffset is EXACT -- anything but 64 is BadRange") {
    for (const std::uint64_t offset : {std::uint64_t{63}, std::uint64_t{65}, std::uint64_t{0}, std::uint64_t{66}}) {
        std::vector<std::byte> bytes = buildReference();
        putU64(std::span<std::byte>(bytes), AT_SAMPLE_DATA_OFFSET, offset);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        // Assert the STATUS, not merely "refused": with the exact check deleted, two of these four
        // reach the size check and refuse for a DIFFERENT reason, and a refused-is-refused assertion
        // could not tell the difference.
        CHECK(parsed.status == CookedAudioStatus::BadRange);
    }
}

TEST_CASE("CA20: a sample rate outside [8000, 384000] is BadTable, naming the value and the bound") {
    {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_SAMPLE_RATE, 7999);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::BadTable);
        CHECK(parsed.message.find("7999") != std::string::npos);
        CHECK(parsed.message.find("8000") != std::string::npos);
    }
    {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_SAMPLE_RATE, 384001);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::BadTable);
        CHECK(parsed.message.find("384001") != std::string::npos);
        CHECK(parsed.message.find("384000") != std::string::npos);
    }
}

TEST_CASE("CA21: both rate bounds are INCLUSIVE") {
    // The half that makes CA20 a BOUND rather than merely a refusal: an upper-bound arm that only
    // tests "above is refused" cannot see the bound move DOWN.
    for (const std::uint32_t rate : {8000U, 384000U}) {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_SAMPLE_RATE, rate);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::Ok);
    }
}

TEST_CASE("CA22: zero channels is BadTable, over-cap channels is CapExceeded, 8 is Ok") {
    {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_CHANNELS, 0);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::BadTable);
    }
    {
        std::vector<std::byte> bytes = buildHeaderOnly(8000, 9, 2);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::CapExceeded);
        // The value AND its bound, as phrases rather than as bare digits: a single-character find
        // would be satisfied by any message containing a 9 or an 8 anywhere.
        CHECK(parsed.message.find("9 channels") != std::string::npos);
        CHECK(parsed.message.find("cap of 8") != std::string::npos);
    }
    {
        // 8 channels x 2 frames = 16 samples, a real 96-byte artifact.
        const std::vector<std::int16_t> samples(16, 7);
        const std::vector<std::byte> bytes = buildAudio(8000, 8, 2, TEST_GUID, samples);
        REQUIRE(bytes.size() == 96);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::Ok);
    }
}

TEST_CASE("CA23: zero frames is BadTable, over-cap frames is CapExceeded") {
    {
        std::vector<std::byte> bytes = buildReference();
        putU32(std::span<std::byte>(bytes), AT_FRAME_COUNT, 0);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::BadTable);
    }
    {
        std::vector<std::byte> bytes = buildHeaderOnly(8000, 1, MAX_COOKED_AUDIO_FRAMES + 1);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::CapExceeded);
        CHECK(parsed.message.find("28800001") != std::string::npos);
        CHECK(parsed.message.find("28800000") != std::string::npos);
    }
}

TEST_CASE("CA24: the SAMPLE cap is the binding one for multi-channel audio") {
    // Every arm here is a 64-byte HEADER ONLY: the point of checking totalBytes against bytes.size()
    // is that a header claiming 28.8 M frames costs no allocation to refuse, so these must not size
    // a region.
    //
    // 1 x 28 800 000 = 28 800 000 samples and 2 x 28 800 000 = 57 600 000 samples are both AT or
    // under the sample cap, so both pass the caps and are refused by the SIZE check instead.
    // 3 x 28 800 000 = 86 400 000 is over it, and that is the multi-channel cap biting.
    {
        const std::vector<std::byte> bytes = buildHeaderOnly(48000, 1, MAX_COOKED_AUDIO_FRAMES);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::SizeMismatch);
    }
    {
        const std::vector<std::byte> bytes = buildHeaderOnly(48000, 2, MAX_COOKED_AUDIO_FRAMES);
        CHECK(parseCookedAudio(bytes).status == CookedAudioStatus::SizeMismatch);
    }
    {
        const std::vector<std::byte> bytes = buildHeaderOnly(48000, 3, MAX_COOKED_AUDIO_FRAMES);
        const CookedAudioParseResult parsed = parseCookedAudio(bytes);
        CHECK(parsed.status == CookedAudioStatus::CapExceeded);
        CHECK(parsed.message.find("86400000") != std::string::npos);
        CHECK(parsed.message.find("57600000") != std::string::npos);
    }
}

TEST_CASE("CA25: a header claiming 0xFFFFFFFF channels and 0xFFFFFFFF frames is refused") {
    // The u32 product of those two values is 1, so a parser that multiplied in u32 and sized a
    // region from the result would accept a 66-byte buffer as a complete file. It cannot get that
    // far here: channels is checked against its own cap FIRST, so the refusal is CapExceeded from
    // the CHANNEL cap and the multiply is never reached.
    //
    // RECORDED, because the plan predicted otherwise: with the caps ordered before the product, NO
    // input can make channels x frameCount overflow a u32 (8 x 28 800 000 = 230 400 000), so the u64
    // cast in the parser has NO runtime witness and is defence in depth. It stays because it is what
    // makes the sample-cap comparison against a u64 constant well-typed, and because the ordering is
    // a property of this parser rather than of the format.
    const std::vector<std::byte> bytes = buildHeaderOnly(8000, 0xFFFFFFFFU, 0xFFFFFFFFU);
    const CookedAudioParseResult parsed = parseCookedAudio(bytes);
    CHECK(parsed.status == CookedAudioStatus::CapExceeded);
    CHECK(parsed.message.find("4294967295") != std::string::npos);
}

TEST_CASE("CA26: cookedAudioStatusLabel is injective over all nine enumerators") {
    // A LITERAL width of 9. A deleted row does not shrink the array -- it leaves a value-initialised
    // Ok in the gap, which the injectivity loop below then catches as a duplicate label. The table
    // cannot silently lose an enumerator.
    const std::array<CookedAudioStatus, 9> all{CookedAudioStatus::Ok,
                                               CookedAudioStatus::TooSmall,
                                               CookedAudioStatus::BadMagic,
                                               CookedAudioStatus::UnsupportedVersion,
                                               CookedAudioStatus::ReservedNotZero,
                                               CookedAudioStatus::SizeMismatch,
                                               CookedAudioStatus::CapExceeded,
                                               CookedAudioStatus::BadTable,
                                               CookedAudioStatus::BadRange};
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(!cookedAudioStatusLabel(all[i]).empty());
        for (std::size_t j = i + 1; j < 9; ++j) {
            CHECK(cookedAudioStatusLabel(all[i]) != cookedAudioStatusLabel(all[j]));
        }
    }
}
