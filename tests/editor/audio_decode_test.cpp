// tests/editor/audio_decode_test.cpp -- task 3.7.1: the four audio backends and the two name
// predicates. A TU of aero_editor_shell_test, which supplies main() -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Joins that target UNCONDITIONALLY, on
// editor/mesh_cook_source_test.cpp's terms: no gate, no GPU, no window, no ImGui context.
//
// It reaches tests/fixtures/audio/ through AERO_AUDIO_FIXTURES_DIR -- A PATH, NOT A FLAG, so a
// missing fixture is a REQUIRE failure rather than a silent skip.
//
// AD10/AD11 are the DECODER half of the external anchor (docs/09 section 14): dr_wav and dr_flac
// against libavcodec, on bytes nothing of ours produced. If either reddens it is a FINDING and the
// anchor is doing its job -- understand the difference and record it, never regenerate the golden
// from our own output.
#include <aero/editor/audio_cook_source.hpp>
#include <aero/editor/audio_decode.hpp>

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
// operands of the audioSourceFormatLabel CHECKs below through operator<<(std::ostream&,
// std::string_view), which MS STL defines inline in <string_view> against an INCOMPLETE
// std::basic_ostream. Written when the TU was created rather than after a Windows lane said so.
#include <ostream>
#include <vector>

using engine::editor::AudioSourceFormat;
using engine::editor::audioSourceFormatForName;
using engine::editor::audioSourceFormatLabel;
using engine::editor::decodeAudioFile;
using engine::editor::DecodedAudio;
using engine::editor::isCookableAudioName;

namespace {

// The caps every production caller passes, spelled here as literals rather than pulled from
// engine/assets: this target links aero::editor_core, which reaches assets transitively, but the
// point of these two arms is that the DECODER's behaviour is bounded by what it is HANDED, so the
// numbers are the test's own.
constexpr std::uint32_t GENEROUS_FRAMES = 28800000;
constexpr std::uint32_t GENEROUS_CHANNELS = 8;
// The PRODUCT bound, and the number that matters about it is that it is FOUR TIMES SMALLER than
// GENEROUS_FRAMES * GENEROUS_CHANNELS (230 400 000). A decoder bounded on each axis alone accepts the
// larger figure; AD19 is what proves this one bites where the other two do not.
constexpr std::uint64_t GENEROUS_SAMPLES = 2ULL * 28800000ULL;

// The 1.0 s / 8 kHz fixture set's own arithmetic. tone.wav, tone.flac and tone.mp3 are MONO;
// tone.ogg is STEREO, because this ffmpeg build's native Vorbis encoder refuses anything else -- see
// tests/fixtures/audio/README.md, where the refusal is recorded as a measured property of the
// toolchain rather than worked around.
constexpr std::uint32_t FIXTURE_RATE = 8000;
constexpr std::size_t FIXTURE_FRAMES = 8000;
constexpr std::int16_t FIXTURE_PEAK = 3276;
// Four granules. At 8000 frames that is +-58 % of the signal, which is meaningful slack; at the
// earlier 0.25 s length the same window was +-230 % of a 2000-frame clip and could not have failed
// for any decode short of a total refusal.
constexpr std::size_t MP3_FRAME_TOLERANCE = 4608;

[[nodiscard]] std::vector<std::byte> readAudioFixture(const std::string& fileName) {
    std::ifstream file(std::string(AERO_AUDIO_FIXTURES_DIR) + "/" + fileName, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

[[nodiscard]] DecodedAudio decodeFixture(const std::string& fileName, std::uint32_t maxFrames = GENEROUS_FRAMES,
                                         std::uint32_t maxChannels = GENEROUS_CHANNELS,
                                         std::uint64_t maxSamples = GENEROUS_SAMPLES) {
    const std::vector<std::byte> bytes = readAudioFixture(fileName);
    REQUIRE(!bytes.empty());
    return decodeAudioFile(fileName, bytes, maxFrames, maxChannels, maxSamples);
}

[[nodiscard]] std::int16_t peakOf(const std::vector<std::int16_t>& samples) {
    std::int16_t peak = 0;
    for (const std::int16_t s : samples) {
        const int magnitude = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
        if (magnitude > peak) {
            peak = static_cast<std::int16_t>(magnitude);
        }
    }
    return peak;
}

// The comment-stripped text of one editor/src file, for AD18. Comments go FIRST so prose that merely
// NAMES a token can never stand in for code that uses it.
[[nodiscard]] std::string strippedEditorSource(const std::string& fileName) {
    std::ifstream file(std::string(AERO_EDITOR_SRC_DIR) + "/" + fileName, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // ANTI-VACUITY, half one: the raw text is real.
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
    // ANTI-VACUITY, half two: stripping removed something and kept something.
    REQUIRE(stripped.size() < source.size());
    REQUIRE_FALSE(stripped.empty());
    return stripped;
}

}  // namespace

// ---- Group 1: the name predicates (AD1-AD5) -----------------------------------------------------

TEST_CASE("AD1: the four claimed extensions map to the four formats") {
    CHECK(audioSourceFormatForName("a.wav") == AudioSourceFormat::Wav);
    CHECK(audioSourceFormatForName("a.flac") == AudioSourceFormat::Flac);
    CHECK(audioSourceFormatForName("a.mp3") == AudioSourceFormat::Mp3);
    CHECK(audioSourceFormatForName("a.ogg") == AudioSourceFormat::Ogg);
}

TEST_CASE("AD2: the table is ASCII case-insensitive") {
    CHECK(audioSourceFormatForName("a.WAV") == AudioSourceFormat::Wav);
    CHECK(audioSourceFormatForName("a.Ogg") == AudioSourceFormat::Ogg);
    CHECK(audioSourceFormatForName("a.FlAc") == AudioSourceFormat::Flac);
    CHECK(audioSourceFormatForName("a.MP3") == AudioSourceFormat::Mp3);
}

TEST_CASE("AD3: everything outside the table is Unknown") {
    CHECK(audioSourceFormatForName("a.aiff") == AudioSourceFormat::Unknown);
    CHECK(audioSourceFormatForName("a.wave") == AudioSourceFormat::Unknown);
    CHECK(audioSourceFormatForName("wav") == AudioSourceFormat::Unknown);  // no dot at all
    CHECK(audioSourceFormatForName("") == AudioSourceFormat::Unknown);
    CHECK(audioSourceFormatForName("a.") == AudioSourceFormat::Unknown);         // a trailing dot is no extension
    CHECK(audioSourceFormatForName("a.wav.txt") == AudioSourceFormat::Unknown);  // the LAST dot decides
    // ".wav" is a name whose leading dot is at index 0, so rawExtensionOf returns "wav" and this is
    // legitimately Wav. Asserted as what the helper ACTUALLY does rather than as what a reader might
    // expect from a dotfile.
    CHECK(audioSourceFormatForName(".wav") == AudioSourceFormat::Wav);
}

TEST_CASE("AD4: isCookableAudioName is exactly the Unknown test over the same table") {
    const std::array<std::string_view, 11> names{"a.wav",  "a.flac", "a.mp3",     "a.ogg", "a.WAV", "a.Ogg",
                                                 "a.aiff", "wav",    "a.wav.txt", "a.",    ".wav"};
    for (const std::string_view name : names) {
        CHECK(isCookableAudioName(name) == (audioSourceFormatForName(name) != AudioSourceFormat::Unknown));
    }
}

TEST_CASE("AD5: audioSourceFormatLabel is injective, and .flac is claimed") {
    const std::array<AudioSourceFormat, 5> all{AudioSourceFormat::Unknown, AudioSourceFormat::Wav,
                                               AudioSourceFormat::Flac, AudioSourceFormat::Mp3, AudioSourceFormat::Ogg};
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(!audioSourceFormatLabel(all[i]).empty());
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            CHECK(audioSourceFormatLabel(all[i]) != audioSourceFormatLabel(all[j]));
        }
    }
    // Against a LITERAL, so deleting the .flac row from the table reddens here rather than only in
    // the arms that happen to use a flac fixture.
    CHECK(isCookableAudioName("music.flac"));
    CHECK(audioSourceFormatLabel(AudioSourceFormat::Flac) == std::string_view("FLAC"));
}

// ---- Group 2: the four backends against the four committed fixtures (AD6-AD9) --------------------

TEST_CASE("AD6: tone.wav decodes through miniaudio at 8 kHz mono, 8000 frames") {
    const DecodedAudio decoded = decodeFixture("tone.wav");
    CHECK(decoded.error.empty());
    CHECK(decoded.format == AudioSourceFormat::Wav);
    CHECK(decoded.sampleRate == FIXTURE_RATE);
    CHECK(decoded.channels == 1U);
    CHECK(decoded.samples.size() == FIXTURE_FRAMES);
    CHECK(peakOf(decoded.samples) == FIXTURE_PEAK);
}

TEST_CASE("AD7: tone.flac decodes to the same shape through the same path") {
    const DecodedAudio decoded = decodeFixture("tone.flac");
    CHECK(decoded.error.empty());
    CHECK(decoded.format == AudioSourceFormat::Flac);
    CHECK(decoded.sampleRate == FIXTURE_RATE);
    CHECK(decoded.channels == 1U);
    CHECK(decoded.samples.size() == FIXTURE_FRAMES);
    CHECK(peakOf(decoded.samples) == FIXTURE_PEAK);
}

TEST_CASE("AD8: tone.mp3 decodes within the stated frame tolerance") {
    const DecodedAudio decoded = decodeFixture("tone.mp3");
    CHECK(decoded.error.empty());
    CHECK(decoded.format == AudioSourceFormat::Mp3);
    CHECK(decoded.sampleRate == FIXTURE_RATE);
    CHECK(decoded.channels == 1U);
    REQUIRE(decoded.channels > 0);
    const std::size_t frames = decoded.samples.size() / decoded.channels;
    // MP3 carries encoder delay and padding, so a decoder's frame count legitimately differs from the
    // source's. The MEASURED value is printed so docs/10 can record it rather than guess at it.
    MESSAGE("AD8: dr_mp3 decoded " << frames << " frames (source " << FIXTURE_FRAMES << ")");
    const std::size_t difference = frames > FIXTURE_FRAMES ? frames - FIXTURE_FRAMES : FIXTURE_FRAMES - frames;
    CHECK(difference <= MP3_FRAME_TOLERANCE);
    // The peak in the right neighbourhood, never a digest: this is lossy by design.
    const std::int16_t peak = peakOf(decoded.samples);
    MESSAGE("AD8: peak " << peak << " (wav " << FIXTURE_PEAK << ")");
    CHECK(peak > (FIXTURE_PEAK * 85) / 100);
    CHECK(peak < (FIXTURE_PEAK * 115) / 100);
}

TEST_CASE("AD9: tone.ogg decodes through stb_vorbis at 8 kHz STEREO, 8000 frames") {
    const DecodedAudio decoded = decodeFixture("tone.ogg");
    CHECK(decoded.error.empty());
    CHECK(decoded.format == AudioSourceFormat::Ogg);
    CHECK(decoded.sampleRate == FIXTURE_RATE);
    // TWO channels, not one. This ffmpeg build's native Vorbis encoder refuses anything but stereo,
    // which is recorded in the fixture README as a measured property of the toolchain -- and it is the
    // one committed source here whose decode can witness a channel-major interleaving defect at all,
    // since with a single channel frame-major and channel-major are the same byte order.
    //
    // CONSEQUENCE, recorded rather than left to be discovered: this case therefore CANNOT witness a
    // config.channels-forced-to-2 defect on the ogg path -- it would assert 2 and get 2. That seed's
    // witnesses are AD6/AD7/AD8, which drive the miniaudio path, and explicitly not this case. The
    // two backends fail independently, which is itself worth knowing.
    CHECK(decoded.channels == 2U);
    CHECK(decoded.samples.size() == FIXTURE_FRAMES * 2);
    const std::int16_t peak = peakOf(decoded.samples);
    MESSAGE("AD9: stb_vorbis peak " << peak << " (wav " << FIXTURE_PEAK << ")");
    CHECK(peak > (FIXTURE_PEAK * 50) / 100);
    CHECK(peak <= FIXTURE_PEAK);
}

// ---- Group 3: the external anchor, D14's real half (AD10-AD11) -----------------------------------

TEST_CASE("AD10: dr_wav agrees with libavcodec byte for byte") {
    const std::vector<std::byte> anchor = readAudioFixture("tone.s16le.pcm");
    REQUIRE(anchor.size() == FIXTURE_FRAMES * 2);
    const DecodedAudio decoded = decodeFixture("tone.wav");
    REQUIRE(decoded.error.empty());
    REQUIRE(decoded.samples.size() == FIXTURE_FRAMES);

    bool identical = true;
    for (std::size_t i = 0; i < decoded.samples.size(); ++i) {
        const auto lo = static_cast<std::uint16_t>(anchor[i * 2]);
        const auto hi = static_cast<std::uint16_t>(anchor[(i * 2) + 1]);
        const auto expected = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8U)));
        if (decoded.samples[i] != expected) {
            identical = false;
            break;
        }
    }
    CHECK(identical);
}

TEST_CASE("AD11: dr_flac agrees with libavcodec byte for byte") {
    const std::vector<std::byte> anchor = readAudioFixture("tone.s16le.pcm");
    REQUIRE(anchor.size() == FIXTURE_FRAMES * 2);
    const DecodedAudio decoded = decodeFixture("tone.flac");
    REQUIRE(decoded.error.empty());
    REQUIRE(decoded.samples.size() == FIXTURE_FRAMES);

    bool identical = true;
    for (std::size_t i = 0; i < decoded.samples.size(); ++i) {
        const auto lo = static_cast<std::uint16_t>(anchor[i * 2]);
        const auto hi = static_cast<std::uint16_t>(anchor[(i * 2) + 1]);
        const auto expected = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8U)));
        if (decoded.samples[i] != expected) {
            identical = false;
            break;
        }
    }
    CHECK(identical);
}

// ---- Group 4: refusals and hostile input (AD12-AD20) ---------------------------------------------

TEST_CASE("AD12: an unclaimed name is refused, and the message names all four extensions") {
    const std::vector<std::byte> bytes = readAudioFixture("tone.wav");  // REAL bytes, wrong name
    const DecodedAudio decoded = decodeAudioFile("a.aiff", bytes, GENEROUS_FRAMES, GENEROUS_CHANNELS, GENEROUS_SAMPLES);
    CHECK(decoded.format == AudioSourceFormat::Unknown);
    CHECK(!decoded.error.empty());
    CHECK(decoded.samples.empty());
    CHECK(decoded.error.find(".wav") != std::string::npos);
    CHECK(decoded.error.find(".flac") != std::string::npos);
    CHECK(decoded.error.find(".mp3") != std::string::npos);
    CHECK(decoded.error.find(".ogg") != std::string::npos);
}

TEST_CASE("AD13: an empty byte span is refused before either backend opens") {
    const std::array<std::string_view, 4> names{"a.wav", "a.flac", "a.mp3", "a.ogg"};
    for (const std::string_view name : names) {
        const DecodedAudio decoded =
            decodeAudioFile(name, std::span<const std::byte>(), GENEROUS_FRAMES, GENEROUS_CHANNELS, GENEROUS_SAMPLES);
        CHECK(!decoded.error.empty());
        CHECK(decoded.samples.empty());
        CHECK(decoded.error.find("empty") != std::string::npos);
        // The FORMAT is still reported: the name claimed a backend even though no byte reached it.
        CHECK(decoded.format != AudioSourceFormat::Unknown);
    }
}

TEST_CASE("AD14: a truncated fixture refuses or shortens, never crashes and never leaks") {
    // The first 40 % of each of the four. Under ASan on every Debug lane, and under LeakSanitizer on
    // the Linux Debug lane, which is the one tier that can see a decoder handle escape an error path.
    const std::array<std::string_view, 4> names{"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"};
    for (const std::string_view name : names) {
        const std::vector<std::byte> full = readAudioFixture(std::string(name));
        REQUIRE(full.size() > 100);
        const std::span<const std::byte> truncated(full.data(), (full.size() * 40) / 100);
        const DecodedAudio decoded =
            decodeAudioFile(name, truncated, GENEROUS_FRAMES, GENEROUS_CHANNELS, GENEROUS_SAMPLES);
        if (decoded.error.empty()) {
            // A shorter stream is a legitimate outcome for a truncated container.
            REQUIRE(decoded.channels > 0);
            CHECK(decoded.samples.size() / decoded.channels <= FIXTURE_FRAMES);
        } else {
            CHECK(decoded.samples.empty());
        }
    }
}

TEST_CASE("AD15: ASCII garbage under each claimed name is refused") {
    std::vector<std::byte> garbage(64);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<std::byte>('A' + static_cast<char>(i % 26));
    }
    const std::array<std::string_view, 4> names{"a.wav", "a.flac", "a.mp3", "a.ogg"};
    for (const std::string_view name : names) {
        const DecodedAudio decoded =
            decodeAudioFile(name, garbage, GENEROUS_FRAMES, GENEROUS_CHANNELS, GENEROUS_SAMPLES);
        CHECK(!decoded.error.empty());
        CHECK(decoded.samples.empty());
    }
}

TEST_CASE("AD16: maxFrames == 1 is refused, and the message names the source's own frame count") {
    // The PRE-ALLOCATION check is the only place the source's true length is knowable, so asserting
    // the MESSAGE -- not merely the status -- is what makes this case able to see that check deleted.
    // With only the in-loop check the refusal still happens, and the message says "more than 1".
    const std::array<std::string_view, 4> names{"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"};
    for (const std::string_view name : names) {
        const DecodedAudio full = decodeFixture(std::string(name));
        REQUIRE(full.error.empty());
        REQUIRE(full.channels > 0);
        const std::size_t frames = full.samples.size() / full.channels;

        const DecodedAudio capped = decodeFixture(std::string(name), 1, GENEROUS_CHANNELS);
        CHECK(!capped.error.empty());
        CHECK(capped.samples.empty());
        MESSAGE(name << " capped message: " << capped.error);
        CHECK(capped.error.find(std::to_string(frames)) != std::string::npos);
        CHECK(capped.error.find("cap of 1") != std::string::npos);
    }
}

TEST_CASE("AD17: maxChannels == 0 is refused, naming the source's channel count and the bound") {
    const std::array<std::string_view, 4> names{"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"};
    for (const std::string_view name : names) {
        const DecodedAudio full = decodeFixture(std::string(name));
        REQUIRE(full.error.empty());
        const DecodedAudio capped = decodeFixture(std::string(name), GENEROUS_FRAMES, 0);
        CHECK(!capped.error.empty());
        CHECK(capped.samples.empty());
        CHECK(capped.error.find(std::to_string(full.channels) + " channels") != std::string::npos);
        CHECK(capped.error.find("cap of 0") != std::string::npos);
    }
}

TEST_CASE("AD19: the PRODUCT is capped, before anything is allocated, where neither axis alone bites") {
    // The code-review round's finding: bounding `frames` and `channels` independently and never their
    // product lets a caller passing the cook's own per-axis caps accept 28 800 000 x 8 = 230 400 000
    // s16 samples -- ~440 MiB, four times MAX_COOKED_AUDIO_BYTES -- which cookAudio then refuses on
    // its sample cap, so the whole allocation was guaranteed waste.
    //
    // Both axes are left GENEROUS here, so neither per-axis check can fire; only the product can. And
    // the assertion is on the MESSAGE, AD16's technique: the pre-allocation check is the only place
    // the source's own frame count is knowable, so a message naming it is what distinguishes "refused
    // before the reserve" from "refused inside the loop". A status-only arm cannot see the difference.
    const std::array<std::string_view, 4> names{"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"};
    for (const std::string_view name : names) {
        const DecodedAudio full = decodeFixture(std::string(name));
        REQUIRE(full.error.empty());
        REQUIRE(full.channels > 0);
        const std::uint64_t frames = full.samples.size() / full.channels;
        const std::uint64_t samples = full.samples.size();
        REQUIRE(samples > 1);

        // ANTI-VACUITY: the bound under test is strictly below the source's own sample count AND the
        // per-axis product the two other caps would have permitted. Without the second half this case
        // would still pass against a decoder that had no product check at all but a tighter frame cap.
        const std::uint64_t tightSamples = samples - 1;
        REQUIRE(tightSamples < static_cast<std::uint64_t>(GENEROUS_FRAMES) * GENEROUS_CHANNELS);
        REQUIRE(frames <= GENEROUS_FRAMES);
        REQUIRE(full.channels <= GENEROUS_CHANNELS);

        const DecodedAudio capped = decodeFixture(std::string(name), GENEROUS_FRAMES, GENEROUS_CHANNELS, tightSamples);
        CHECK(!capped.error.empty());
        CHECK(capped.samples.empty());
        MESSAGE(name << " product-capped message: " << capped.error);
        // The message names the PRODUCT and the BOUND, plus both factors it was formed from.
        CHECK(capped.error.find(std::to_string(samples) + " samples") != std::string::npos);
        CHECK(capped.error.find("cap of " + std::to_string(tightSamples)) != std::string::npos);
        CHECK(capped.error.find(std::to_string(full.channels) + " channels") != std::string::npos);
        CHECK(capped.error.find(std::to_string(frames) + " frames") != std::string::npos);
        // ...and it is the DECLARED form, so the refusal came before the reserve rather than from the
        // loop, whose wording ("decodes to more than") names no source-derived count at all.
        CHECK(capped.error.find("declares") != std::string::npos);
        CHECK(capped.error.find("decodes to more than") == std::string::npos);

        // The complement: the exact product is accepted, so the comparison is `>` and not `>=`. A cap
        // that refused its own boundary would refuse a legal file at the container's stated maximum.
        const DecodedAudio exact = decodeFixture(std::string(name), GENEROUS_FRAMES, GENEROUS_CHANNELS, samples);
        CHECK(exact.error.empty());
        CHECK(exact.samples.size() == samples);
    }
}

TEST_CASE("AD20: the sample cap survives a channel count the frame cap cannot see") {
    // tone.ogg is the STEREO fixture, which is what makes it the one that can separate the two caps
    // by construction: at 8000 frames it carries 16 000 samples, so a frame cap of 8000 passes while
    // a sample cap of 8000 does not. Against a MONO fixture the two numbers coincide and this arm
    // could not tell a product check from a frame check at all.
    const DecodedAudio stereo = decodeFixture("tone.ogg");
    REQUIRE(stereo.error.empty());
    REQUIRE(stereo.channels == 2);
    const std::uint64_t frames = stereo.samples.size() / stereo.channels;

    const DecodedAudio capped =
        decodeFixture("tone.ogg", static_cast<std::uint32_t>(frames), GENEROUS_CHANNELS, frames);
    CHECK(!capped.error.empty());
    CHECK(capped.samples.empty());
    CHECK(capped.error.find(std::to_string(frames * 2) + " samples") != std::string::npos);
    // The FRAME cap it was handed is satisfied exactly, so nothing but the product could have refused.
    CHECK(capped.error.find("over the cap of " + std::to_string(frames)) != std::string::npos);
    CHECK(capped.error.find("frames, over the cap of") == std::string::npos);
}

// ---- Group 5: the source-text pin (AD18) ---------------------------------------------------------

TEST_CASE("AD18: audio_decode.cpp's decoder-configuration invariants, pinned in source text") {
    const std::string src = strippedEditorSource("audio_decode.cpp");

    // 1. Dither is set EXPLICITLY to none. No behavioural case in this tree can see this on a 16-bit
    //    source -- dr_wav copies rather than converts -- which is exactly why the pin exists.
    CHECK(src.find("config.ditherMode = ma_dither_mode_none") != std::string::npos);

    // 2. The decoder config takes BOTH of miniaudio's "use the stream's value" sentinels. Any other
    //    value silently engages ma_data_converter and resamples or remixes.
    CHECK(src.find("ma_decoder_config_init(ma_format_s16, 0, 0)") != std::string::npos);

    // 3. The negative half: no other dither mode appears anywhere.
    CHECK(src.find("ma_dither_mode_triangle") == std::string::npos);
    CHECK(src.find("ma_dither_mode_rectangle") == std::string::npos);

    // 4. AC-11's second clause, which is a PROHIBITION rather than a preprocessor guarantee:
    //    STB_VORBIS_NO_STDIO does NOT compile stb_vorbis_decode_memory out (measured against the
    //    pinned source -- it sits under a single !NO_INTEGER_CONVERSION guard). Both one-call
    //    convenience entry points must appear nowhere outside a comment.
    CHECK(src.find("stb_vorbis_decode_memory") == std::string::npos);
    CHECK(src.find("stb_vorbis_decode_filename") == std::string::npos);

    // 5. The pull API this file DOES use, asserted so clause 4 cannot pass on a file that decodes
    //    nothing at all -- a pin that is green on a correct tree AND on an empty one proves nothing.
    CHECK(src.find("stb_vorbis_open_memory") != std::string::npos);
    CHECK(src.find("stb_vorbis_get_samples_short_interleaved") != std::string::npos);
    CHECK(src.find("stb_vorbis_close") != std::string::npos);
}
