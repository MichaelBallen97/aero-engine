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

TEST_CASE("MX3: the block gain is applied uniformly at this commit and the second block is flat") {
    // ONE SEQUENCE CASE, never two independent ones -- two independent cases both pass under the very
    // defect they exist to catch (the CD5 rule). The ramp arrives with the gain step; at this commit
    // the assertion is that both blocks carry the same, constant gain.
    const ClipSource source(48000, 1, std::vector<std::int16_t>(64, 16384));
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));

    VoiceParams params = flatParams();
    params.volume = 0.5F;
    mixer.applyCommand(startCommand(0, 1, 0, params));

    const float target = 16384.0F * S16_SCALE * 0.5F;

    std::array<float, 8> first{};
    mixer.render(first, 1, 48000);
    std::array<float, 8> second{};
    mixer.render(second, 1, 48000);

    CHECK(first[first.size() - 1] == target);  // the last sample of block 1 EQUALS the target
    for (const float sample : second) {
        CHECK(sample == target);  // block 2 is flat at the target
    }
}

TEST_CASE("MX4: s16 scaling maps -32768 to exactly -1 and 32767 to exactly 32767/32768") {
    const std::vector<std::int16_t> samples = {std::numeric_limits<std::int16_t>::min(),
                                               std::numeric_limits<std::int16_t>::max()};
    const ClipSource source(48000, 1, samples);
    AudioMixer mixer;
    REQUIRE(mixer.publishClip(0, source.get()));
    mixer.applyCommand(startCommand(0, 1, 0, flatParams()));

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

    std::array<float, 8> buffer{};
    mixer.render(buffer, 2, 48000);
    for (std::uint32_t f = 0; f < 4; ++f) {
        const float left = buffer[(f * 2) + 0];
        const float right = buffer[(f * 2) + 1];
        CHECK(left == static_cast<float>(f * 10) * S16_SCALE);
        CHECK(right == static_cast<float>((f * 10) + 1000) * S16_SCALE);
        CHECK(right != left);
    }
}

TEST_CASE("MX15: a channel-count mismatch downmixes to the MEAN and fans out") {
    // A 2-channel clip whose L is 1000 and R is 3000 at every frame: the mean is 2000.
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

    // 7 elements over 2 channels: 3 whole frames plus one trailing element.
    std::array<float, 7> buffer{};
    poison(buffer);
    mixer.render(buffer, 2, 48000);

    CHECK(buffer[0] == 0.0F);  // frame 0, channel 0 -- the ramp's first value is 0
    CHECK(buffer[1] == 1000.0F * S16_SCALE);
    CHECK(buffer[6] == 0.0F);  // THE TRAILING PARTIAL FRAME, zeroed and never written past
    CHECK(mixer.framesRendered() == 3U);
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
