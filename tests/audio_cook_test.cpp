// tests/audio_cook_test.cpp -- task 3.7.1: cookAudio, the .aerowave producer. A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window. It reads exactly one file -- tests/fixtures/audio/tone.s16le.pcm,
// through AERO_AUDIO_FIXTURES_DIR -- and that file is ffmpeg's OWN decode of tone.wav, committed. AK17
// is the cook's half of the external anchor: it proves the cook is byte-transparent and frame-major
// against bytes nothing of ours produced. The decoder half (dr_wav and dr_flac against libavcodec)
// lives in tests/editor/audio_decode_test.cpp, which is where the decoders are reachable.
#include <aero/assets/audio_cook.hpp>
#include <aero/assets/cooked_audio.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies the std::string_view
// operands of the CHECKs below through operator<<(std::ostream&, std::string_view), which MS STL
// defines inline in <string_view> against an INCOMPLETE std::basic_ostream. Written when the TU was
// created rather than after a Windows lane said so.
#include <ostream>
#include <vector>

using engine::Guid;
using engine::assets::AudioCookInput;
using engine::assets::AudioCookResult;
using engine::assets::AudioCookStatus;
using engine::assets::audioSample;
using engine::assets::audioSampleBytes;
using engine::assets::cookAudio;
using engine::assets::COOKED_AUDIO_HEADER_BYTES;
using engine::assets::COOKED_AUDIO_SAMPLE_BYTES;
using engine::assets::cookedAudioDurationSeconds;
using engine::assets::CookedAudioParseResult;
using engine::assets::CookedAudioStatus;
using engine::assets::getU16;
using engine::assets::getU32;
using engine::assets::getU64;
using engine::assets::MAX_COOK_WARNINGS;
using engine::assets::MAX_COOKED_AUDIO_FRAMES;
using engine::assets::parseCookedAudio;

namespace {

// The same standard test GUID cooked_audio_test.cpp uses: hi = 0x0123456789abcdef,
// lo = 0xfedcba9876543210, which writes as efcdab8967452301 1032547698badcfe -- every byte non-zero,
// so AK13 is a statement about byte ORDER and not merely about presence.
constexpr Guid TEST_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

// The 1.0 s / 8 kHz / mono fixture set's own arithmetic, as literals. tone.s16le.pcm is exactly
// 16000 bytes = 8000 frames x 1 channel x 2 B, and the artifact it cooks to is 64 + 16000 = 16064.
// See tests/fixtures/audio/README.md for how the numbers were measured and why the source is 1.0 s.
constexpr std::size_t ANCHOR_BYTES = 16000;
constexpr std::size_t ANCHOR_FRAMES = 8000;
constexpr std::size_t ANCHOR_ARTIFACT_BYTES = 16064;

[[nodiscard]] std::vector<std::byte> readAudioFixture(const std::string& fileName) {
    // A PATH, not a flag (the AERO_MATERIAL_FIXTURES_DIR precedent): a missing fixture is a REQUIRE
    // failure rather than a silent skip.
    std::ifstream file(std::string(AERO_AUDIO_FIXTURES_DIR) + "/" + fileName, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

[[nodiscard]] AudioCookResult cookSamples(std::uint32_t rate, std::uint32_t channels,
                                          std::span<const std::int16_t> samples, Guid guid = Guid{}) {
    AudioCookInput input;
    input.sourceGuid = guid;
    input.sampleRate = rate;
    input.channels = channels;
    input.samples = samples;
    return cookAudio(input);
}

constexpr std::array<std::int16_t, 2> REF_SAMPLES{0x0201, -2};

}  // namespace

TEST_CASE("AK1: a minimal happy cook reports every statistic") {
    const AudioCookResult cooked = cookSamples(8000, 1, REF_SAMPLES, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    CHECK(cooked.message.empty());
    CHECK(cooked.bytes.size() == 68);  // 64 + 2 * 1 * 2
    CHECK(cooked.stats.frameCount == 2U);
    CHECK(cooked.stats.sampleCount == 2ULL);
    CHECK(cooked.stats.byteSize == 68ULL);
    CHECK(cooked.stats.durationSeconds == 0.00025F);
}

TEST_CASE("AK2: the cook's bytes are byte-identical to the hand-built golden") {
    // cooked_audio_test.cpp's CA10 builds the same 68-byte buffer by hand from docs/09 section 14's
    // tables. This case is what ties the WRITER to it: CA10 alone cannot see the writer move, because
    // it builds its own buffer.
    const AudioCookResult cooked = cookSamples(8000, 1, REF_SAMPLES, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    REQUIRE(cooked.bytes.size() == 68);
    const std::span<const std::byte> r(cooked.bytes);

    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<char>(r[i]) == std::string_view("AEROWAVE")[i]);
    }
    CHECK(getU32(r, 8) == 1U);
    CHECK(getU32(r, 12) == 1U);
    CHECK(getU64(r, 16) == TEST_GUID.hi);
    CHECK(getU64(r, 24) == TEST_GUID.lo);
    CHECK(getU32(r, 32) == 8000U);
    CHECK(getU32(r, 36) == 1U);
    CHECK(getU32(r, 40) == 2U);
    CHECK(getU32(r, 44) == 0U);
    CHECK(getU64(r, 48) == 64ULL);
    CHECK(getU64(r, 56) == 68ULL);
    CHECK(getU16(r, 64) == 0x0201U);
    CHECK(getU16(r, 66) == 0xFFFEU);  // -2 as two little-endian bytes
}

TEST_CASE("AK3: the artifact round-trips through the parser") {
    const AudioCookResult cooked = cookSamples(44100, 2, REF_SAMPLES, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    const CookedAudioParseResult parsed = parseCookedAudio(cooked.bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    CHECK(parsed.audio.sampleRate == 44100U);
    CHECK(parsed.audio.channels == 2U);
    CHECK(parsed.audio.frameCount == 1U);  // 2 samples over 2 channels
    CHECK(parsed.audio.sourceGuid == TEST_GUID);
    CHECK(parsed.audio.sampleDataOffset == COOKED_AUDIO_HEADER_BYTES);
    CHECK(parsed.audio.totalBytes == cooked.bytes.size());
    CHECK(audioSampleBytes(parsed.audio).size() == 4);
}

TEST_CASE("AK4: a sample rate below the minimum is refused, naming the value and the bound") {
    const AudioCookResult cooked = cookSamples(7999, 1, REF_SAMPLES);
    CHECK(cooked.status == AudioCookStatus::Refused);
    CHECK(cooked.message.find("7999") != std::string::npos);
    CHECK(cooked.message.find("8000") != std::string::npos);
}

TEST_CASE("AK5: a sample rate above the maximum is refused, naming the value and the bound") {
    const AudioCookResult cooked = cookSamples(384001, 1, REF_SAMPLES);
    CHECK(cooked.status == AudioCookStatus::Refused);
    CHECK(cooked.message.find("384001") != std::string::npos);
    CHECK(cooked.message.find("384000") != std::string::npos);
}

TEST_CASE("AK6: zero channels and over-cap channels are both refused") {
    {
        const AudioCookResult cooked = cookSamples(8000, 0, REF_SAMPLES);
        CHECK(cooked.status == AudioCookStatus::Refused);
        CHECK(cooked.message.find("zero channels") != std::string::npos);
    }
    {
        const std::vector<std::int16_t> samples(9, 1);
        const AudioCookResult cooked = cookSamples(8000, 9, samples);
        CHECK(cooked.status == AudioCookStatus::Refused);
        CHECK(cooked.message.find("9 channels") != std::string::npos);
        CHECK(cooked.message.find("cap of 8") != std::string::npos);
    }
}

TEST_CASE("AK7: a sample count that is not a whole number of frames is refused") {
    // 7 samples over 2 channels. Integer division would floor this to 3 frames and silently drop the
    // ragged sample, shifting nothing visibly and everything audibly.
    const std::vector<std::int16_t> samples(7, 5);
    const AudioCookResult cooked = cookSamples(8000, 2, samples);
    CHECK(cooked.status == AudioCookStatus::Refused);
    CHECK(cooked.message.find("7 samples") != std::string::npos);
    CHECK(cooked.message.find("2-channel") != std::string::npos);
    CHECK(cooked.bytes.empty());
}

TEST_CASE("AK8: an empty sample span is refused -- a .aerowave is never empty") {
    const AudioCookResult cooked = cookSamples(8000, 1, std::span<const std::int16_t>());
    CHECK(cooked.status == AudioCookStatus::Refused);
    CHECK(cooked.message.find("zero frames") != std::string::npos);
    CHECK(cooked.bytes.empty());
}

TEST_CASE("AK9: bytes is empty IFF Refused -- asserted in BOTH directions") {
    // One direction alone is satisfied by a cook that always returns empty, and the other alone by
    // one that always returns bytes. Both arms, over every refusal this TU can reach plus a success.
    const std::vector<std::int16_t> nine(9, 1);
    const std::vector<std::int16_t> seven(7, 1);
    const std::vector<AudioCookResult> refusals{
        cookSamples(7999, 1, REF_SAMPLES), cookSamples(384001, 1, REF_SAMPLES),
        cookSamples(8000, 0, REF_SAMPLES), cookSamples(8000, 9, nine),
        cookSamples(8000, 2, seven),       cookSamples(8000, 1, std::span<const std::int16_t>())};
    for (const AudioCookResult& cooked : refusals) {
        CHECK(cooked.status == AudioCookStatus::Refused);
        CHECK(cooked.bytes.empty());
        CHECK(!cooked.message.empty());
    }
    const AudioCookResult ok = cookSamples(8000, 1, REF_SAMPLES, TEST_GUID);
    CHECK(ok.status == AudioCookStatus::Ok);
    CHECK(!ok.bytes.empty());
    CHECK(ok.message.empty());
}

TEST_CASE("AK10: v1 emits NO warning on any path") {
    const std::vector<std::int16_t> seven(7, 1);
    const std::vector<AudioCookResult> results{cookSamples(8000, 1, REF_SAMPLES, TEST_GUID),
                                               cookSamples(7999, 1, REF_SAMPLES), cookSamples(8000, 2, seven),
                                               cookSamples(8000, 1, std::span<const std::int16_t>())};
    for (const AudioCookResult& cooked : results) {
        CHECK(cooked.warnings.empty());
        CHECK(cooked.warningTotal == 0);
    }
}

TEST_CASE("AK11: two cooks of the same input produce byte-identical vectors") {
    const AudioCookResult a = cookSamples(48000, 2, REF_SAMPLES, TEST_GUID);
    const AudioCookResult b = cookSamples(48000, 2, REF_SAMPLES, TEST_GUID);
    REQUIRE(a.status == AudioCookStatus::Ok);
    REQUIRE(b.status == AudioCookStatus::Ok);
    CHECK(a.bytes == b.bytes);
}

TEST_CASE("AK12: a nil sourceGuid is legal and writes sixteen zero bytes") {
    const AudioCookResult cooked = cookSamples(8000, 1, REF_SAMPLES);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    for (std::size_t i = 16; i < 32; ++i) {
        CHECK(cooked.bytes[i] == std::byte{0});
    }
    const CookedAudioParseResult parsed = parseCookedAudio(cooked.bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    CHECK_FALSE(parsed.audio.sourceGuid.valid());
}

TEST_CASE("AK13: the source GUID is written hi then lo, each little-endian") {
    const AudioCookResult cooked = cookSamples(8000, 1, REF_SAMPLES, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    const std::array<std::uint8_t, 16> expected{0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                                                0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(static_cast<std::uint8_t>(cooked.bytes[16 + i]) == expected[i]);
    }
}

TEST_CASE("AK14: an 8-channel cook is FRAME-MAJOR, read back sample by sample") {
    // 8 channels x 3 frames = 24 samples, each carrying its own (frame, channel) identity so a
    // channel-major writer produces a DIFFERENT value at every position but two. A 1-channel fixture
    // cannot see this defect at all: with one channel the two layouts are the same byte order.
    std::vector<std::int16_t> samples(24);
    for (std::uint32_t f = 0; f < 3; ++f) {
        for (std::uint32_t c = 0; c < 8; ++c) {
            samples[(f * 8) + c] = static_cast<std::int16_t>((f * 100) + c);
        }
    }
    const AudioCookResult cooked = cookSamples(48000, 8, samples, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    CHECK(cooked.bytes.size() == COOKED_AUDIO_HEADER_BYTES + 48);

    const CookedAudioParseResult parsed = parseCookedAudio(cooked.bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    const std::span<const std::byte> region = audioSampleBytes(parsed.audio);
    for (std::uint32_t f = 0; f < 3; ++f) {
        for (std::uint32_t c = 0; c < 8; ++c) {
            CHECK(audioSample(region, (static_cast<std::uint64_t>(f) * 8) + c) ==
                  static_cast<std::int16_t>((f * 100) + c));
        }
    }
}

TEST_CASE("AK15: the extreme s16 values round-trip bit-exactly") {
    const std::array<std::int16_t, 4> samples{-32768, 32767, 0, -1};
    const AudioCookResult cooked = cookSamples(8000, 1, samples, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    const CookedAudioParseResult parsed = parseCookedAudio(cooked.bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);
    const std::span<const std::byte> region = audioSampleBytes(parsed.audio);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(audioSample(region, i) == samples[i]);
    }
}

TEST_CASE("AK16: stats.durationSeconds is cookedAudioDurationSeconds, used rather than restated") {
    const AudioCookResult cooked = cookSamples(44100, 1, REF_SAMPLES, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    CHECK(cooked.stats.durationSeconds == cookedAudioDurationSeconds(44100, cooked.stats.frameCount));
}

TEST_CASE("AK17: the cook is byte-transparent against ffmpeg's own decode") {
    // THE ANCHOR'S COOK HALF (D14). tone.s16le.pcm is ffmpeg's decode of tone.wav -- raw interleaved
    // s16 LE, no container, produced by libavcodec and by nothing of ours. The bytes are reassembled
    // into int16_t through getU16 and NEVER through a reinterpret_cast, so this case makes no
    // alignment or aliasing assumption about the file it read.
    //
    // If this ever reddens, the resolution is to understand the difference and record it -- never to
    // regenerate the golden from our own output.
    const std::vector<std::byte> anchor = readAudioFixture("tone.s16le.pcm");
    REQUIRE(anchor.size() == ANCHOR_BYTES);

    std::vector<std::int16_t> samples(anchor.size() / COOKED_AUDIO_SAMPLE_BYTES);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(getU16(anchor, i * COOKED_AUDIO_SAMPLE_BYTES));
    }
    REQUIRE(samples.size() == ANCHOR_FRAMES);

    const AudioCookResult cooked = cookSamples(8000, 1, samples, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    const CookedAudioParseResult parsed = parseCookedAudio(cooked.bytes);
    REQUIRE(parsed.status == CookedAudioStatus::Ok);

    const std::span<const std::byte> region = audioSampleBytes(parsed.audio);
    REQUIRE(region.size() == anchor.size());
    bool identical = true;
    for (std::size_t i = 0; i < region.size(); ++i) {
        if (region[i] != anchor[i]) {
            identical = false;
            break;
        }
    }
    CHECK(identical);
}

TEST_CASE("AK18: the anchor's artifact is 16064 bytes over 8000 frames") {
    const std::vector<std::byte> anchor = readAudioFixture("tone.s16le.pcm");
    REQUIRE(anchor.size() == ANCHOR_BYTES);
    std::vector<std::int16_t> samples(anchor.size() / COOKED_AUDIO_SAMPLE_BYTES);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(getU16(anchor, i * COOKED_AUDIO_SAMPLE_BYTES));
    }
    const AudioCookResult cooked = cookSamples(8000, 1, samples, TEST_GUID);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    // 64 + 2 x 1 x 8000, the whole format's arithmetic in one number.
    CHECK(cooked.bytes.size() == ANCHOR_ARTIFACT_BYTES);
    CHECK(cooked.stats.frameCount == ANCHOR_FRAMES);
    CHECK(cooked.stats.durationSeconds == 1.0F);
}

TEST_CASE("AK19: the frame cap is checked BEFORE the output buffer is sized") {
    // 28 800 001 int16_t is one 57.6 MB allocation on the TEST's side, and it is the honest way to
    // exercise a cap whose whole point is "refuse before you allocate the output". A source-text pin
    // would observe the intention rather than the effect.
    const std::vector<std::int16_t> samples(static_cast<std::size_t>(MAX_COOKED_AUDIO_FRAMES) + 1, 0);
    const AudioCookResult cooked = cookSamples(48000, 1, samples);
    CHECK(cooked.status == AudioCookStatus::Refused);
    CHECK(cooked.message.find("28800001") != std::string::npos);
    CHECK(cooked.message.find("28800000") != std::string::npos);
    CHECK(cooked.bytes.empty());
}

TEST_CASE("AK20: MAX_COOK_WARNINGS reaches this header through cooked_mesh.hpp") {
    // AC-1's include list, proven by USE rather than by reading the file: the eight byte primitives
    // and this constant both arrive through <aero/assets/cooked_mesh.hpp>, which is the recorded
    // reconciliation this format inherits for the fourth time.
    CHECK(MAX_COOK_WARNINGS == 20);
}
