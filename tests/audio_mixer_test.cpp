// tests/audio_mixer_test.cpp -- task 3.7.2: engine::audio::AudioMixer, MX1-MX22. A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// TIER-0 BY CONSTRUCTION: the mixer is a plain object with no device, no thread and no timing
// anywhere in its interface (D18), so every case here builds one, hands it commands and calls
// render(). Every case therefore runs in EVERY configuration this project builds, with
// AERO_REQUIRE_GPU set and unset.
//
// <ostream> is included preventively (the 0.4.1 trap: a doctest CHECK over a std::string_view fails
// the Windows lane ALONE, inside the MS STL headers). There is no #if of any kind in this file and
// there must never be one -- 3.6.3 shipped four cases inside a file-level #if with everything green
// while the one arm that mattered never ran.

#include <aero/assets/audio_cook.hpp>
#include <aero/audio/audio.hpp>
#include <aero/core/vfs.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::ByteBuffer;
using engine::Guid;
using engine::audio::AudioClip;
using engine::audio::AudioCommand;
using engine::audio::AudioMixer;
using engine::audio::ClipHandle;
using engine::audio::loadAudioClip;
using engine::audio::MAX_VOICES;
using engine::audio::VoiceParams;

constexpr std::string_view CLIP_PATH = "res://clip.aerowave";

// A backend serving ONE path out of memory -- the audio_clip_test.cpp shape, duplicated into this TU
// on purpose. Two small duplications beat a shared test header that three translation units must
// agree about, and this project has no tests/support/ convention to hang one on.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    explicit MemoryBackend(ByteBuffer bytes) : content(std::move(bytes)) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override { return relPath == "clip.aerowave"; }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        if (relPath != "clip.aerowave") {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(content.size());
    }

    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view relPath) const override {
        if (relPath != "clip.aerowave") {
            return std::nullopt;
        }
        return content;
    }

private:
    ByteBuffer content;
};

// AudioClip has no public constructor from bytes -- only loadAudioClip. So a test cooks the samples
// IN MEMORY with the real cookAudio, mounts them through a real VirtualFileSystem, and loads them
// back. The whole res:// path runs with bytes that never touched a disk, and no fixture is committed
// for any MX case.
class ClipSource {
public:
    ClipSource(std::uint32_t rate, std::uint32_t channels, const std::vector<std::int16_t>& samples) {
        engine::assets::AudioCookInput input;
        input.sourceGuid = Guid{};
        input.sampleRate = rate;
        input.channels = channels;
        input.samples = std::span<const std::int16_t>{samples};
        engine::assets::AudioCookResult cooked = engine::assets::cookAudio(input);
        REQUIRE(cooked.status == engine::assets::AudioCookStatus::Ok);

        vfs.mount("res://", std::make_unique<MemoryBackend>(std::move(cooked.bytes)));
        engine::audio::AudioClipLoadResult loaded = loadAudioClip(vfs, CLIP_PATH);
        REQUIRE(loaded.status == engine::audio::AudioClipLoadStatus::Ok);
        clip = std::move(loaded.clip);
    }

    [[nodiscard]] const AudioClip* get() const noexcept { return &clip; }

private:
    engine::VirtualFileSystem vfs;
    AudioClip clip;
};

// A ramp 0, 1, 2, ... so a test can tell WHICH frame it is hearing from the sample value alone.
[[nodiscard]] std::vector<std::int16_t> rampSamples(std::uint32_t frames, std::uint32_t channels) {
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::size_t index = (static_cast<std::size_t>(f) * channels) + c;
            samples[index] = static_cast<std::int16_t>((f * 10) + (c * 1000));
        }
    }
    return samples;
}

[[nodiscard]] AudioCommand startCommand(std::uint32_t slot, std::uint32_t generation, std::uint32_t clipIndex,
                                        const VoiceParams& params) {
    AudioCommand command;
    command.kind = AudioCommand::Kind::Start;
    command.slot = slot;
    command.generation = generation;
    command.clip = ClipHandle{.index = clipIndex, .generation = 1};
    command.params = params;
    return command;
}

// The 2D default every non-spatialized case starts from: spatialize false, loop false, unit gains.
[[nodiscard]] VoiceParams flatParams() {
    VoiceParams params;
    params.spatialize = false;
    return params;
}

// Fills a buffer with a recognisable non-zero pattern, so "render wrote every element" can FAIL.
void poison(std::span<float> buffer) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = -12345.5F + static_cast<float>(i);
    }
}

constexpr float S16_SCALE = 1.0F / 32768.0F;

}  // namespace

TEST_CASE("MX1: render with NO voices writes EVERY element and every one is exactly 0") {
    AudioMixer mixer;
    std::array<float, 64> buffer{};
    poison(buffer);  // without this the case cannot fail

    mixer.render(buffer, 2, 48000);
    for (const float sample : buffer) {
        CHECK(sample == 0.0F);
    }
    CHECK(mixer.activeVoices() == 0U);
    CHECK(mixer.callbacksCompleted() == 1U);
    CHECK(mixer.framesRendered() == 32U);
}

TEST_CASE("MX2: a matched-rate, unit-gain voice reproduces the clip's samples EXACTLY") {
    const ClipSource source(48000, 2, rampSamples(64, 2));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));

    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

    // Asserted on the SECOND block, which is where the value is stable once the per-block gain ramp
    // arrives. The first block is MX3's business.
    std::array<float, 16> first{};
    mixer.render(first, 2, 48000);
    std::array<float, 16> second{};
    mixer.render(second, 2, 48000);

    // Block 2 starts at frame 8 (16 samples / 2 channels).
    for (std::uint32_t f = 0; f < 8; ++f) {
        const std::uint32_t clipFrame = 8 + f;
        const float expectedLeft = static_cast<float>(clipFrame * 10) * S16_SCALE;
        const float expectedRight = static_cast<float>((clipFrame * 10) + 1000) * S16_SCALE;
        CHECK(second[(f * 2) + 0] == expectedLeft);
        CHECK(second[(f * 2) + 1] == expectedRight);
    }
}

TEST_CASE("MX3: the FIRST block RAMPS 0 -> target and the SECOND is flat") {
    // ONE SEQUENCE CASE, never two independent ones -- two independent cases both pass under the very
    // defect they exist to catch (the CD5 rule). It witnesses BOTH halves of the ramp at once:
    //   * t == (f + 1) / frames rather than f / frames, so the LAST frame of block 1 reaches the
    //     target EXACTLY. With f / frames it never does, and the shortfall COMPOUNDS across blocks.
    //   * currentGain is committed to the target at the END of the block, never before the frame
    //     loop. Committing first would make block 1 flat and delete the ramp entirely.
    const ClipSource source(48000, 1, std::vector<std::int16_t>(64, 16384));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));

    VoiceParams params = flatParams();
    params.volume = 0.5F;
    mixer.applyCommand(startCommand(0, 1, 0, params));

    const float target = 16384.0F * S16_SCALE * 0.5F;

    constexpr std::size_t BLOCK = 8;
    std::array<float, BLOCK> first{};
    mixer.render(first, 1, 48000);
    std::array<float, BLOCK> second{};
    mixer.render(second, 1, 48000);

    CHECK(first[0] < target);  // strictly below target -- the ramp starts from silence
    CHECK(first[0] > 0.0F);    // and is not itself zero: t(0) is 1/8, not 0
    CHECK(first[0] == target * (1.0F / 8.0F));

    for (std::size_t f = 0; f < BLOCK; ++f) {
        const float t = static_cast<float>(f + 1) / static_cast<float>(BLOCK);
        CHECK(first[f] == target * t);  // exact, and MONOTONE by construction
    }
    CHECK(first[BLOCK - 1] == target);  // THE LAST FRAME REACHES THE TARGET EXACTLY

    for (const float sample : second) {
        CHECK(sample == target);  // block 2 is flat: currentGain was committed at block 1's end
    }
}

TEST_CASE("MX4: s16 scaling maps -32768 to exactly -1 and 32767 to exactly 32767/32768") {
    const std::vector<std::int16_t> samples = {std::numeric_limits<std::int16_t>::min(),
                                               std::numeric_limits<std::int16_t>::max()};
    const ClipSource source(48000, 1, samples);
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    VoiceParams params = flatParams();
    params.loop = true;  // so the voice survives the warm-up block and repeats the same two frames
    mixer.applyCommand(startCommand(0, 1, 0, params));

    // The gain ramps from silence across the FIRST block, so the scaling is asserted on the second,
    // where currentGain has been committed to the target and the ramp is a no-op.
    std::array<float, 2> warmUp{};
    mixer.render(warmUp, 1, 48000);
    std::array<float, 2> buffer{};
    mixer.render(buffer, 1, 48000);

    CHECK(buffer[0] == -1.0F);                // EXACT: 1/32768 is a power of two
    CHECK(buffer[1] == 32767.0F / 32768.0F);  // EXACT, and deliberately NOT 1.0
}

TEST_CASE("MX12: the voice cap refuses beyond MAX_VOICES and activeVoices never exceeds it") {
    const ClipSource source(48000, 1, std::vector<std::int16_t>(1024, 1000));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));

    // 200 starts across slots 0..199. Everything at or past MAX_VOICES is refused by the mixer.
    for (std::uint32_t slot = 0; slot < 200; ++slot) {
        mixer.applyCommand(startCommand(slot, 1, 0, flatParams()));
        std::array<float, 4> buffer{};
        mixer.render(buffer, 1, 48000);
        CHECK(mixer.activeVoices() <= MAX_VOICES);
    }
    CHECK(mixer.activeVoices() == MAX_VOICES);  // the 65th and every one after it was refused
}

TEST_CASE("MX14: clipChannels == channels plays straight through, PER CHANNEL") {
    // The clip's L and R differ by 1000 at every frame; they must come out differing the same way.
    const ClipSource source(48000, 2, rampSamples(32, 2));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

    std::array<float, 8> warmUp{};
    mixer.render(warmUp, 2, 48000);  // let the gain ramp complete; block 2 is flat at the target
    std::array<float, 8> buffer{};
    mixer.render(buffer, 2, 48000);

    for (std::uint32_t f = 0; f < 4; ++f) {
        const std::uint32_t clipFrame = 4 + f;  // block 2 starts at frame 4
        const float left = buffer[(f * 2) + 0];
        const float right = buffer[(f * 2) + 1];
        CHECK(left == static_cast<float>(clipFrame * 10) * S16_SCALE);
        CHECK(right == static_cast<float>((clipFrame * 10) + 1000) * S16_SCALE);
        CHECK(right != left);
    }
}

TEST_CASE("MX15: a channel-count mismatch downmixes to the MEAN and fans out") {
    // A 2-channel clip whose L is 1000 and R is 3000 at every frame: the mean is 2000. The clip is
    // CONSTANT, so the only thing that varies across the second block is the gain -- which the
    // warm-up block has already brought to the target.
    std::vector<std::int16_t> samples(64);
    for (std::size_t f = 0; f < 32; ++f) {
        samples[(f * 2) + 0] = 1000;
        samples[(f * 2) + 1] = 3000;
    }
    const ClipSource source(48000, 2, samples);
    const float mono = 2000.0F * S16_SCALE;

    SUBCASE("into 1 channel") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));
        std::array<float, 4> warmUp{};
        mixer.render(warmUp, 1, 48000);
        std::array<float, 4> buffer{};
        mixer.render(buffer, 1, 48000);
        for (const float sample : buffer) {
            CHECK(sample == doctest::Approx(mono).epsilon(1e-6));
        }
    }

    SUBCASE("into 4 channels -- the SAME value four times") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));
        std::array<float, 16> warmUp{};
        mixer.render(warmUp, 4, 48000);
        std::array<float, 16> buffer{};
        mixer.render(buffer, 4, 48000);
        for (const float sample : buffer) {
            CHECK(sample == doctest::Approx(mono).epsilon(1e-6));
        }
    }
}

TEST_CASE("MX17: an unusable clip handle retires the voice and writes silence, never a read") {
    const ClipSource source(48000, 1, std::vector<std::int16_t>(16, 5000));

    SUBCASE("an index past the published clip count") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, /*clipIndex=*/7, flatParams()));

        std::array<float, 4> buffer{};
        poison(buffer);
        mixer.render(buffer, 1, 48000);
        for (const float sample : buffer) {
            CHECK(sample == 0.0F);
        }
        CHECK(mixer.activeVoices() == 0U);
    }

    SUBCASE("a PUBLISHED null pointer") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, nullptr));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

        std::array<float, 4> buffer{};
        poison(buffer);
        mixer.render(buffer, 1, 48000);
        for (const float sample : buffer) {
            CHECK(sample == 0.0F);
        }
        CHECK(mixer.activeVoices() == 0U);
    }

    SUBCASE("a DEFAULT-CONSTRUCTED clip, whose frameCount is 0") {
        const AudioClip empty;
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, &empty));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

        std::array<float, 4> buffer{};
        poison(buffer);
        mixer.render(buffer, 1, 48000);
        for (const float sample : buffer) {
            CHECK(sample == 0.0F);
        }
        CHECK(mixer.activeVoices() == 0U);
    }
}

TEST_CASE("MX21: a buffer that is not a whole number of frames leaves its remainder EXACTLY 0") {
    const ClipSource source(48000, 2, rampSamples(32, 2));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

    std::array<float, 8> warmUp{};
    mixer.render(warmUp, 2, 48000);  // 4 whole frames, and the gain ramp completes

    // 7 elements over 2 channels: 3 whole frames plus one trailing element.
    std::array<float, 7> buffer{};
    poison(buffer);
    mixer.render(buffer, 2, 48000);

    CHECK(buffer[0] == static_cast<float>(4 * 10) * S16_SCALE);  // block 2 starts at frame 4
    CHECK(buffer[1] == static_cast<float>((4 * 10) + 1000) * S16_SCALE);
    CHECK(buffer[6] == 0.0F);             // THE TRAILING PARTIAL FRAME, zeroed and never written past
    CHECK(mixer.framesRendered() == 7U);  // 4 from the warm-up plus 3 whole frames here
}

TEST_CASE("MX22: a zero rate, zero channels or an empty span all write silence and never divide") {
    const ClipSource source(48000, 1, std::vector<std::int16_t>(16, 5000));

    SUBCASE("sampleRate == 0") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));
        std::array<float, 8> buffer{};
        poison(buffer);
        mixer.render(buffer, 2, 0);
        for (const float sample : buffer) {
            CHECK(sample == 0.0F);
        }
        CHECK(mixer.framesRendered() == 0U);
        CHECK(mixer.callbacksCompleted() == 1U);  // the counters are published on EVERY exit
    }

    SUBCASE("channels == 0") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));
        std::array<float, 8> buffer{};
        poison(buffer);
        mixer.render(buffer, 0, 48000);
        for (const float sample : buffer) {
            CHECK(sample == 0.0F);
        }
        CHECK(mixer.framesRendered() == 0U);
        CHECK(mixer.callbacksCompleted() == 1U);
    }

    SUBCASE("an EMPTY output span") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));
        mixer.render(std::span<float>{}, 2, 48000);
        CHECK(mixer.framesRendered() == 0U);
        CHECK(mixer.callbacksCompleted() == 1U);
    }
}

// ============================================================================================
// Step 5: the 32.32 fixed-point cursor, linear interpolation, the loop seam and retirement.
// ============================================================================================

TEST_CASE("MX5: a 44100 -> 48000 resample reaches the closed-form fixed-point frame index EXACTLY") {
    // Asserted through OBSERVABLE OUTPUT (which frame the mixer read) rather than by exposing the
    // cursor: the clip is a ramp whose value IS its frame index, so the sample names the frame. The
    // FIRST block is the gain ramp's, so the closed form is checked on the second.
    const ClipSource source(44100, 1, rampSamples(512, 1));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

    constexpr std::size_t BLOCK = 64;
    std::array<float, BLOCK> first{};
    mixer.render(first, 1, 48000);
    std::array<float, BLOCK> second{};
    mixer.render(second, 1, 48000);

    // The closed form, recomputed here in the SAME integer arithmetic the mixer uses -- one divide,
    // one multiply, one convert, NO libm call anywhere in the chain, which is exactly what makes an
    // exact assertion legitimate.
    const auto increment = static_cast<std::uint64_t>((44100.0 / 48000.0) * 1.0 * 4294967296.0);
    std::uint64_t cursor = 0;
    for (std::size_t f = 0; f < BLOCK; ++f) {
        cursor += increment;  // walk past the warm-up block
    }
    for (std::size_t f = 0; f < BLOCK; ++f) {
        const auto index = static_cast<std::uint32_t>(cursor >> 32U);
        const auto next = static_cast<std::uint32_t>(index + 1);
        const float frac = static_cast<float>(cursor & 0xFFFFFFFFULL) * (1.0F / 4294967296.0F);
        const float a = static_cast<float>(index * 10) * S16_SCALE;
        const float b = static_cast<float>(next * 10) * S16_SCALE;
        CHECK(second[f] == (a + ((b - a) * frac)));
        cursor += increment;
    }
    // A downsampling ratio: fewer clip frames are consumed than output frames produced.
    CHECK((cursor >> 32U) < 2 * BLOCK);
}

TEST_CASE("MX6: pitch 2 consumes twice the frames per block and pitch 0.5 consumes half") {
    const ClipSource source(48000, 1, rampSamples(2048, 1));

    // The clip is a ramp of 10 per frame, so the FIRST sample of the second block names the frame the
    // cursor reached, which is exactly what "how many frames did this block consume" means.
    auto firstSampleOfSecondBlock = [&source](float pitch) {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        VoiceParams params = flatParams();
        params.pitch = pitch;
        mixer.applyCommand(startCommand(0, 1, 0, params));
        std::array<float, 32> block{};
        mixer.render(block, 1, 48000);
        mixer.render(block, 1, 48000);
        return block[0];
    };

    CHECK(firstSampleOfSecondBlock(1.0F) == static_cast<float>(32 * 10) * S16_SCALE);
    CHECK(firstSampleOfSecondBlock(2.0F) == static_cast<float>(64 * 10) * S16_SCALE);
    CHECK(firstSampleOfSecondBlock(0.5F) == static_cast<float>(16 * 10) * S16_SCALE);
}

TEST_CASE("MX7: pitch 0 parks the voice ALIVE, and pitch out of range clamps at both ends") {
    const ClipSource source(48000, 1, rampSamples(2048, 1));

    SUBCASE("pitch 0 keeps the voice alive and emits the frame it is parked on") {
        // Mirrors AnimationPlayer::speed == 0 being pause WITHOUT clearing `playing`.
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        VoiceParams params = flatParams();
        params.pitch = 0.0F;
        mixer.applyCommand(startCommand(0, 1, 0, params));

        std::array<float, 16> block{};
        mixer.render(block, 1, 48000);
        mixer.render(block, 1, 48000);
        CHECK(mixer.activeVoices() == 1U);  // ALIVE, not retired
        for (const float sample : block) {
            CHECK(sample == 0.0F);  // frame 0 of the ramp is 0, and the cursor never moved
        }
    }

    SUBCASE("a NEGATIVE pitch clamps to 0 and behaves identically to pitch 0") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        VoiceParams params = flatParams();
        params.pitch = -3.0F;
        mixer.applyCommand(startCommand(0, 1, 0, params));

        std::array<float, 16> block{};
        mixer.render(block, 1, 48000);
        mixer.render(block, 1, 48000);
        CHECK(mixer.activeVoices() == 1U);
        CHECK(block[0] == 0.0F);
    }

    SUBCASE("a pitch ABOVE MAX_PITCH clamps to MAX_PITCH") {
        auto firstOfSecondBlock = [&source](float pitch) {
            AudioMixer mixer;
            REQUIRE(mixer.publishClip(0, source.get()));
            VoiceParams params = flatParams();
            params.pitch = pitch;
            mixer.applyCommand(startCommand(0, 1, 0, params));
            std::array<float, 32> block{};
            mixer.render(block, 1, 48000);
            mixer.render(block, 1, 48000);
            return block[0];
        };
        CHECK(firstOfSecondBlock(100.0F) == firstOfSecondBlock(engine::audio::MAX_PITCH));
        CHECK(firstOfSecondBlock(100.0F) == static_cast<float>(32 * 4 * 10) * S16_SCALE);
    }
}

TEST_CASE("MX8: looping WRAPS the second index, so the seam interpolates INTO frame 0") {
    // THE PLAN'S ORIGINAL FIXTURE CANNOT DISCRIMINATE AND THE CORRECTION IS RECORDED HERE. It asked
    // for "a clip whose last and first samples are EQUAL", on the theory that a clamping
    // implementation would then show a discontinuity. It cannot: with s[N-1] == s[0], wrapping
    // interpolates s[N-1] -> s[0] and clamping interpolates s[N-1] -> s[N-1], and those are THE SAME
    // NUMBER. The two implementations are byte-identical on that fixture.
    //
    // What actually separates them is the seam SAMPLE VALUE on a fixture whose endpoints DIFFER while
    // the CYCLE is complete -- one whole period of a sine over N frames, where the ideal frame N is
    // frame 0. This case computes the wrap's answer and the clamp's answer in closed form and asserts
    // the output is the first and not the second. Witnessed by arithmetic, never by listening.
    constexpr std::uint32_t FRAMES = 64;
    std::vector<std::int16_t> samples(FRAMES);
    for (std::uint32_t f = 0; f < FRAMES; ++f) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(f) / static_cast<double>(FRAMES);
        samples[f] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(phase)));
    }
    REQUIRE(samples[FRAMES - 1] != samples[0]);  // the endpoints DIFFER -- that is the whole point

    const ClipSource source(44100, 1, samples);
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    VoiceParams params = flatParams();
    params.loop = true;
    mixer.applyCommand(startCommand(0, 1, 0, params));

    constexpr std::size_t BLOCK = 256;
    std::array<float, BLOCK> warmUp{};
    mixer.render(warmUp, 1, 48000);  // the gain ramp completes here; the seam is read from block 2
    std::array<float, BLOCK> buffer{};
    mixer.render(buffer, 1, 48000);

    // Replicate the cursor in the same integer arithmetic the mixer uses, walk past the warm-up
    // block, and find the first output frame in block 2 that sits on the LAST clip frame with a
    // NON-ZERO fraction -- the one frame whose value depends on which second index was chosen.
    const auto increment = static_cast<std::uint64_t>((44100.0 / 48000.0) * 1.0 * 4294967296.0);
    const std::uint64_t clipSpan = static_cast<std::uint64_t>(FRAMES) << 32U;
    std::uint64_t cursor = 0;
    for (std::size_t f = 0; f < BLOCK; ++f) {
        if ((cursor >> 32U) >= FRAMES) {
            cursor %= clipSpan;
        }
        cursor += increment;
    }
    std::size_t seamIndex = BLOCK;
    float seamFrac = 0.0F;
    for (std::size_t f = 0; f < BLOCK; ++f) {
        if ((cursor >> 32U) >= FRAMES) {
            cursor %= clipSpan;
        }
        const auto index = static_cast<std::uint32_t>(cursor >> 32U);
        const float frac = static_cast<float>(cursor & 0xFFFFFFFFULL) * (1.0F / 4294967296.0F);
        if (index == FRAMES - 1 && frac > 0.0F) {
            seamIndex = f;
            seamFrac = frac;
            break;
        }
        cursor += increment;
    }
    REQUIRE(seamIndex < BLOCK);

    const float last = static_cast<float>(samples[FRAMES - 1]) * S16_SCALE;
    const float first = static_cast<float>(samples[0]) * S16_SCALE;
    const float wrapped = last + ((first - last) * seamFrac);  // the CORRECT answer
    const float clamped = last;                                // what a CLAMPING second index gives

    CHECK(buffer[seamIndex] == wrapped);  // exact: the same three float operations, in the same order
    CHECK(buffer[seamIndex] != clamped);  // and provably NOT the clamp's answer
    CHECK(mixer.activeVoices() == 1U);    // a LOOPING voice never retires, even after eight wraps
    CHECK(mixer.framesRendered() == 2 * BLOCK);
}

TEST_CASE("MX9: a NON-looping clip CLAMPS its second index, retires, and is EXACTLY 0 after the end") {
    // THE FIXTURE IS THE WITNESS AND IT HAD TO BE STRENGTHENED. Its first form used a CONSTANT clip
    // at a MATCHED rate, and neither half of it could see the clamp: with every sample equal, the
    // clamp's s[N-1] and a wrap's s[0] are THE SAME NUMBER, and with a matched rate and pitch 1 the
    // fractional cursor is always 0 so the second index is never read at all. Seeding the non-looping
    // arm to wrap left the whole suite GREEN. It now uses a RAMP (so the endpoints differ) at a
    // NON-INTEGER rate ratio (so the seam frame is genuinely interpolated), and it asserts the
    // clamp's answer against the wrap's explicitly.
    constexpr std::uint32_t FRAMES = 8;
    std::vector<std::int16_t> samples(FRAMES);
    for (std::uint32_t f = 0; f < FRAMES; ++f) {
        samples[f] = static_cast<std::int16_t>((f + 1) * 1000);  // 1000 .. 8000, endpoints DIFFER
    }
    const ClipSource source(44100, 1, samples);
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

    constexpr std::size_t BLOCK = 32;
    std::array<float, BLOCK> buffer{};
    poison(buffer);
    mixer.render(buffer, 1, 48000);

    // Replicate the cursor in the mixer's own integer arithmetic and predict every frame, with the
    // gain ramp folded in -- the voice retires inside this one block, so there is no warmed-up block
    // to read and the ramp is asserted here rather than waited out.
    const auto increment = static_cast<std::uint64_t>((44100.0 / 48000.0) * 1.0 * 4294967296.0);
    std::uint64_t cursor = 0;
    std::size_t lastLive = 0;
    float seamFrac = 0.0F;
    for (std::size_t f = 0; f < BLOCK; ++f) {
        const std::uint64_t index = cursor >> 32U;
        if (index >= FRAMES) {
            CHECK(buffer[f] == 0.0F);  // EXACTLY 0 after the end
            continue;
        }
        const auto i = static_cast<std::uint32_t>(index);
        const std::uint32_t next = i + 1 < FRAMES ? i + 1 : FRAMES - 1U;  // THE CLAMP
        const float frac = static_cast<float>(cursor & 0xFFFFFFFFULL) * (1.0F / 4294967296.0F);
        const float a = static_cast<float>(samples[i]) * S16_SCALE;
        const float b = static_cast<float>(samples[next]) * S16_SCALE;
        const float t = static_cast<float>(f + 1) / static_cast<float>(BLOCK);
        CHECK(buffer[f] == ((a + ((b - a) * frac)) * t));
        if (i == FRAMES - 1 && frac > 0.0F) {
            lastLive = f;
            seamFrac = frac;
        }
        cursor += increment;
    }

    // THE DISCRIMINATING ASSERTION: on the clip's LAST frame with a non-zero fraction, the clamp's
    // answer is s[N-1] flat and a WRAP would interpolate toward s[0]. They differ, and the output is
    // the first.
    REQUIRE(lastLive != 0);
    REQUIRE(seamFrac > 0.0F);
    const float last = static_cast<float>(samples[FRAMES - 1]) * S16_SCALE;
    const float first = static_cast<float>(samples[0]) * S16_SCALE;
    const float t = static_cast<float>(lastLive + 1) / static_cast<float>(BLOCK);
    CHECK(buffer[lastLive] == last * t);                                    // the CLAMP's answer
    CHECK(buffer[lastLive] != ((last + ((first - last) * seamFrac)) * t));  // and NOT a wrap's

    CHECK(mixer.activeVoices() == 0U);  // retired at the block's end
}

TEST_CASE("MX10: a retired voice reaches the retire ring EXACTLY once, with ITS OWN generation") {
    const ClipSource source(48000, 1, std::vector<std::int16_t>(4, 1000));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(/*slot=*/5, /*generation=*/9, 0, flatParams()));

    std::array<float, 16> buffer{};
    mixer.render(buffer, 1, 48000);

    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    REQUIRE(mixer.popRetired(slot, generation));
    CHECK(slot == 5U);
    CHECK(generation == 9U);  // ITS OWN generation, not the slot's current one

    CHECK_FALSE(mixer.popRetired(slot, generation));  // exactly once
    mixer.render(buffer, 1, 48000);
    CHECK_FALSE(mixer.popRetired(slot, generation));  // and still exactly once after another block
}

TEST_CASE("MX19: a cursor at MAX_COOKED_AUDIO_FRAMES is exact in u64 and COLLAPSES through float") {
    // THE ARM THAT JUSTIFIES THE TYPE, and it is easy to write vacuously. The assertion is not "a u64
    // holds a big number". It is that two sub-sample positions one increment apart are
    // DISTINGUISHABLE in 32.32 fixed point and INDISTINGUISHABLE once round-tripped through a float
    // frame position -- because 28 800 000 > 2^24 == 16 777 216.
    constexpr std::uint32_t MAX_FRAMES = engine::assets::MAX_COOKED_AUDIO_FRAMES;
    CHECK(MAX_FRAMES > (1U << 24U));

    const std::uint64_t c0 = static_cast<std::uint64_t>(MAX_FRAMES) << 32U;
    const std::uint64_t c1 = c0 + 1;
    CHECK(c0 != c1);
    CHECK((c1 >> 32U) == MAX_FRAMES);  // one fixed-point tick does not move the frame index

    // Two ADJACENT frame positions -- not sub-sample, whole frames -- already collapse in float.
    const auto f0 = static_cast<float>(MAX_FRAMES);
    const auto f1 = static_cast<float>(MAX_FRAMES + 1U);
    CHECK(f0 == f1);  // a float frame cursor cannot even count frames at this magnitude

    // And the u64 form can, which is the whole point.
    CHECK((static_cast<std::uint64_t>(MAX_FRAMES) != static_cast<std::uint64_t>(MAX_FRAMES) + 1U));

    // The worst legal cursor is comfortably inside u64: 28 800 000 * 2^32 ~= 1.24e17 against 1.8e19.
    CHECK(c0 / 4294967296ULL == MAX_FRAMES);
}

// ============================================================================================
// Step 6: per-block gain ramps, spatialization, the mix-wide scalars and the finiteness-guarded
// output clamp.
// ============================================================================================

TEST_CASE("MX11: stop() ramps to zero WITHIN the block and retires at its end") {
    // ONE SEQUENCE, asserting both halves: the ramp reaches 0 inside the block in which the Stop was
    // drained, AND the retirement lands on the ring at that block's end. D17's "stop REUSES the ramp
    // instead of adding a path" is only worth anything if both are true together.
    const ClipSource source(48000, 1, std::vector<std::int16_t>(4096, 20000));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(/*slot=*/3, /*generation=*/7, 0, flatParams()));

    constexpr std::size_t BLOCK = 16;
    std::array<float, BLOCK> warmUp{};
    mixer.render(warmUp, 1, 48000);  // the gain reaches its target here
    const float target = 20000.0F * S16_SCALE;
    CHECK(warmUp[BLOCK - 1] == target);

    AudioCommand stop;
    stop.kind = AudioCommand::Kind::Stop;
    stop.slot = 3;
    stop.generation = 7;
    mixer.applyCommand(stop);

    std::array<float, BLOCK> buffer{};
    mixer.render(buffer, 1, 48000);

    CHECK(buffer[0] < target);          // ramping DOWN from the target
    CHECK(buffer[0] > 0.0F);            // and not instantly silent
    CHECK(buffer[BLOCK - 1] == 0.0F);   // EXACTLY zero by the block's last frame
    CHECK(mixer.activeVoices() == 0U);  // and gone

    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    REQUIRE(mixer.popRetired(slot, generation));
    CHECK(slot == 3U);
    CHECK(generation == 7U);
    CHECK_FALSE(mixer.popRetired(slot, generation));
}

TEST_CASE("MX13: voices SUM, and a pile-up CLAMPS to +-1 rather than exceeding it") {
    const ClipSource loud(48000, 1, std::vector<std::int16_t>(4096, 20000));
    const ClipSource negative(48000, 1, std::vector<std::int16_t>(4096, -20000));

    SUBCASE("two voices sum") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, loud.get()));
        VoiceParams params = flatParams();
        params.volume = 0.25F;
        mixer.applyCommand(startCommand(0, 1, 0, params));
        mixer.applyCommand(startCommand(1, 1, 0, params));

        std::array<float, 8> warmUp{};
        mixer.render(warmUp, 1, 48000);
        std::array<float, 8> buffer{};
        mixer.render(buffer, 1, 48000);

        const float one = 20000.0F * S16_SCALE * 0.25F;
        for (const float sample : buffer) {
            CHECK(sample == doctest::Approx(2.0F * one).epsilon(1e-6));
        }
    }

    SUBCASE("three full-gain voices CLAMP and never exceed 1 on either sign") {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, loud.get()));
        REQUIRE(mixer.publishClip(1, negative.get()));
        for (std::uint32_t slot = 0; slot < 3; ++slot) {
            mixer.applyCommand(startCommand(slot, 1, 0, flatParams()));
        }
        std::array<float, 8> warmUp{};
        mixer.render(warmUp, 1, 48000);
        std::array<float, 8> buffer{};
        mixer.render(buffer, 1, 48000);
        for (const float sample : buffer) {
            CHECK(sample <= 1.0F);
            CHECK(sample >= -1.0F);
        }
        CHECK(buffer[0] == 1.0F);  // 3 x 0.61 saturates, and the clamp is what stops it

        AudioMixer downward;
        REQUIRE(downward.publishClip(0, negative.get()));
        for (std::uint32_t slot = 0; slot < 3; ++slot) {
            downward.applyCommand(startCommand(slot, 1, 0, flatParams()));
        }
        std::array<float, 8> negWarm{};
        downward.render(negWarm, 1, 48000);
        std::array<float, 8> negBuffer{};
        downward.render(negBuffer, 1, 48000);
        for (const float sample : negBuffer) {
            CHECK(sample >= -1.0F);
        }
        CHECK(negBuffer[0] == -1.0F);
    }
}

TEST_CASE("MX16: a SPATIALIZED stereo clip is downmixed to the MEAN before panning") {
    // Distinguishes the MEAN from "play channel 0", which is what a naive downmix would leave
    // standing. L is 1000 and R is 3000, so the mean is 2000 and channel 0 alone would read 1000.
    std::vector<std::int16_t> samples(4096);
    for (std::size_t f = 0; f < 2048; ++f) {
        samples[(f * 2) + 0] = 1000;
        samples[(f * 2) + 1] = 3000;
    }
    const ClipSource source(48000, 2, samples);

    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));

    // A listener at the origin, and a source dead ahead so the pan is exactly centre and the two
    // output channels carry the same gain.
    AudioCommand setListener;
    setListener.kind = AudioCommand::Kind::SetListener;
    setListener.listener.valid = true;
    mixer.applyCommand(setListener);

    VoiceParams params;
    params.spatialize = true;
    params.position = engine::Vec3{0.0F, 0.0F, -2.0F};
    params.minDistance = 10.0F;  // inside minDistance -> distance gain is EXACTLY 1
    params.maxDistance = 50.0F;
    mixer.applyCommand(startCommand(0, 1, 0, params));

    std::array<float, 8> warmUp{};
    mixer.render(warmUp, 2, 48000);
    std::array<float, 8> buffer{};
    mixer.render(buffer, 2, 48000);

    const float mean = 2000.0F * S16_SCALE;
    const float channelZeroOnly = 1000.0F * S16_SCALE;
    const float centreGain = 0.70710678F;  // constant power at x == 0

    CHECK(buffer[0] == doctest::Approx(mean * centreGain).epsilon(1e-5));
    CHECK(buffer[1] == doctest::Approx(mean * centreGain).epsilon(1e-5));
    // And NOT the "channel 0" answer, which is what makes this case about the mean specifically.
    CHECK(buffer[0] != doctest::Approx(channelZeroOnly * centreGain).epsilon(1e-5));
}

TEST_CASE("MX18: MAX_PITCH at the widest legal rate ratio stays inside u64") {
    // The D15 range argument, ASSERTED rather than trusted. The worst legal increment is
    // MAX_PITCH x (MAX rate / MIN rate) == 4 x (384000 / 8000) == 192 frames per output frame.
    constexpr double WIDEST_RATIO = static_cast<double>(engine::assets::MAX_COOKED_AUDIO_SAMPLE_RATE) /
                                    static_cast<double>(engine::assets::MIN_COOKED_AUDIO_SAMPLE_RATE);
    CHECK(WIDEST_RATIO == doctest::Approx(48.0).epsilon(1e-9));

    const double worstIncrement = WIDEST_RATIO * static_cast<double>(engine::audio::MAX_PITCH) * 4294967296.0;
    CHECK(worstIncrement < static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
    CHECK(static_cast<std::uint64_t>(worstIncrement) == 824633720832ULL);  // 192 * 2^32, as a literal

    // And the worst cursor: MAX_COOKED_AUDIO_FRAMES frames in 32.32.
    const auto worstCursor = static_cast<double>(engine::assets::MAX_COOKED_AUDIO_FRAMES) * 4294967296.0;
    CHECK(worstCursor < static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
    CHECK(worstCursor < 1.3e17);  // ~1.24e17 against u64's ~1.8e19

    // Driven through the real mixer, at the widest legal ratio, so the arithmetic above is not the
    // only thing asserted.
    const std::vector<std::int16_t> flat(4096, 8000);
    const ClipSource fast(engine::assets::MAX_COOKED_AUDIO_SAMPLE_RATE, 1, flat);
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, fast.get()));
    VoiceParams params = flatParams();
    params.pitch = engine::audio::MAX_PITCH;
    params.loop = true;
    mixer.applyCommand(startCommand(0, 1, 0, params));
    std::array<float, 16> buffer{};
    mixer.render(buffer, 1, engine::assets::MIN_COOKED_AUDIO_SAMPLE_RATE);
    for (const float sample : buffer) {
        CHECK(std::isfinite(sample));
    }
}

TEST_CASE("MX20: NO output element is EVER non-finite, across a matrix of poisoned inputs") {
    const ClipSource source(48000, 1, std::vector<std::int16_t>(4096, 20000));
    const std::array<float, 3> poisons = {std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity()};

    for (const float bad : poisons) {
        // Twelve poisoned inputs per bad value: the three position components, both distances, the
        // source volume, the pitch, the listener's three axes' x components, the listener position
        // and the listener volume.
        std::vector<VoiceParams> poisoned;
        {
            VoiceParams p;
            p.spatialize = true;
            for (int component = 0; component < 3; ++component) {
                VoiceParams q = p;
                const float x = component == 0 ? bad : 1.0F;
                const float y = component == 1 ? bad : 1.0F;
                const float z = component == 2 ? bad : 1.0F;
                q.position = engine::Vec3{x, y, z};
                poisoned.push_back(q);
            }
            VoiceParams minD = p;
            minD.minDistance = bad;
            poisoned.push_back(minD);
            VoiceParams maxD = p;
            maxD.maxDistance = bad;
            poisoned.push_back(maxD);
            VoiceParams vol = p;
            vol.volume = bad;
            poisoned.push_back(vol);
            VoiceParams pitch = p;
            pitch.pitch = bad;
            poisoned.push_back(pitch);
            VoiceParams flat = flatParams();
            flat.volume = bad;
            poisoned.push_back(flat);
            VoiceParams flatPitch = flatParams();
            flatPitch.pitch = bad;
            poisoned.push_back(flatPitch);
        }

        const std::array<engine::audio::ListenerPose, 4> listeners = [bad] {
            engine::audio::ListenerPose base;
            base.valid = true;
            engine::audio::ListenerPose position = base;
            position.position = engine::Vec3{bad, 0.0F, 0.0F};
            engine::audio::ListenerPose right = base;
            right.right = engine::Vec3{bad, 0.0F, 0.0F};
            engine::audio::ListenerPose volume = base;
            volume.volume = bad;
            return std::array<engine::audio::ListenerPose, 4>{base, position, right, volume};
        }();

        for (const VoiceParams& params : poisoned) {
            for (const engine::audio::ListenerPose& pose : listeners) {
                AudioMixer mixer;
                REQUIRE(mixer.publishClip(0, source.get()));
                AudioCommand setListener;
                setListener.kind = AudioCommand::Kind::SetListener;
                setListener.listener = pose;
                mixer.applyCommand(setListener);
                mixer.applyCommand(startCommand(0, 1, 0, params));

                std::array<float, 8> buffer{};
                mixer.render(buffer, 2, 48000);
                mixer.render(buffer, 2, 48000);
                for (const float sample : buffer) {
                    CHECK(std::isfinite(sample));
                    CHECK(sample <= 1.0F);
                    CHECK(sample >= -1.0F);
                }
            }
        }
    }

    // And a poisoned MASTER volume, which is the one scalar the loop above cannot reach.
    for (const float bad : poisons) {
        AudioMixer mixer;
        REQUIRE(mixer.publishClip(0, source.get()));
        AudioCommand master;
        master.kind = AudioCommand::Kind::SetMasterVolume;
        master.masterVolume = bad;
        mixer.applyCommand(master);
        mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

        std::array<float, 8> buffer{};
        mixer.render(buffer, 2, 48000);
        mixer.render(buffer, 2, 48000);
        for (const float sample : buffer) {
            CHECK(std::isfinite(sample));
        }
    }
}
