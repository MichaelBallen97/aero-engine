// tests/audio_system_test.cpp -- task 3.7.2: engine::audio::AudioSystem, SY1-SY20. A TU of
// aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// TIER-0 BY CONSTRUCTION: AudioSystem owns no device and starts no thread, so every case here drives
// it directly and calls render() on the test thread (D18). Every case runs in EVERY configuration
// this project builds, with AERO_REQUIRE_GPU set and unset.
//
// THE PREFIX IS SY, NOT AS. `AS1`-`AS14` are already taken by
// tests/editor/animation_cook_source_test.cpp. Doctest names are per-binary so a duplicate would be
// legal, but a case id is only unambiguous WITH its binary named, and unused prefixes cost nothing.
//
// <ostream> is included preventively (the 0.4.1 trap). There is no #if of any kind in this file and
// there must never be one.

#include <aero/assets/audio_cook.hpp>
#include <aero/audio/audio.hpp>
#include <aero/core/log.hpp>
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
#include <type_traits>
#include <vector>

namespace {

using engine::ByteBuffer;
using engine::Guid;
using engine::audio::AudioClip;
using engine::audio::AudioSystem;
using engine::audio::ClipHandle;
using engine::audio::ListenerPose;
using engine::audio::loadAudioClip;
using engine::audio::MAX_CLIPS;
using engine::audio::MAX_VOICES;
using engine::audio::VoiceHandle;
using engine::audio::VoiceParams;

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

// Cooks and loads one clip through the REAL cook -> VFS -> loadAudioClip chain, in memory. Every clip
// in this file is built this way; no fixture is committed for any SY case.
[[nodiscard]] AudioClip makeClip(std::uint32_t rate, std::uint32_t channels, std::uint32_t frames, Guid guid,
                                 std::int16_t value = 20000) {
    const std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * channels, value);
    engine::assets::AudioCookInput input;
    input.sourceGuid = guid;
    input.sampleRate = rate;
    input.channels = channels;
    input.samples = std::span<const std::int16_t>{samples};
    engine::assets::AudioCookResult cooked = engine::assets::cookAudio(input);
    REQUIRE(cooked.status == engine::assets::AudioCookStatus::Ok);

    engine::VirtualFileSystem vfs;
    vfs.mount("res://", std::make_unique<MemoryBackend>(std::move(cooked.bytes)));
    engine::audio::AudioClipLoadResult loaded = loadAudioClip(vfs, "res://clip.aerowave");
    REQUIRE(loaded.status == engine::audio::AudioClipLoadStatus::Ok);
    return std::move(loaded.clip);
}

[[nodiscard]] Guid guidOf(std::uint64_t low) { return Guid{0x3720000000000000ULL, low}; }

[[nodiscard]] VoiceParams flatParams() {
    VoiceParams params;
    params.spatialize = false;
    return params;
}

// Captures log records through the public setLogCallback seam (the PP4 shape from task 3.6.3), so a
// test can assert a WARN fired EXACTLY once rather than that it fired at all.
class LogCapture {
public:
    LogCapture() {
        engine::setLogCallback([this](const engine::LogRecord& record) {
            if (record.level >= engine::LogLevel::Warn) {
                ++warnings;
            }
        });
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;
    ~LogCapture() { engine::setLogCallback(nullptr); }

    [[nodiscard]] int count() const noexcept { return warnings; }

private:
    int warnings = 0;
};

}  // namespace

TEST_CASE("SY1: AudioSystem is non-copyable AND NON-MOVABLE, and create() returns non-null") {
    // The second half is the load-bearing one: a platform::AudioDevice holds `this` as its
    // renderUser, so a move would leave a REALTIME THREAD calling into a moved-from object.
    static_assert(!std::is_copy_constructible_v<AudioSystem>);
    static_assert(!std::is_copy_assignable_v<AudioSystem>);
    static_assert(!std::is_move_constructible_v<AudioSystem>);
    static_assert(!std::is_move_assignable_v<AudioSystem>);

    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    CHECK(system->clipCount() == 0U);
    CHECK(system->stats().activeVoices == 0U);
}

TEST_CASE("SY2: registerClip -> findClip round-trips, and nil or absent is invalid") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);

    const Guid guid = guidOf(1);
    const ClipHandle handle = system->registerClip(makeClip(48000, 1, 64, guid));
    REQUIRE(handle.valid());
    CHECK(handle.generation == 1U);  // ALWAYS 1 in v0 -- there is no clip retirement (D7)
    CHECK(system->clipCount() == 1U);

    CHECK(system->findClip(guid) == handle);
    CHECK_FALSE(system->findClip(Guid{}).valid());       // nil is the ABSENCE of a key
    CHECK_FALSE(system->findClip(guidOf(999)).valid());  // an absent Guid
}

TEST_CASE("SY3: registering past MAX_CLIPS is refused, counted, and warns EXACTLY once") {
    const LogCapture capture;
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);

    for (std::uint32_t i = 0; i < MAX_CLIPS; ++i) {
        const ClipHandle handle = system->registerClip(makeClip(48000, 1, 8, guidOf(i)));
        REQUIRE(handle.valid());
    }
    CHECK(system->clipCount() == MAX_CLIPS);
    CHECK(capture.count() == 0);

    for (std::uint32_t i = 0; i < 20; ++i) {
        const ClipHandle refused = system->registerClip(makeClip(48000, 1, 8, guidOf(10000 + i)));
        CHECK_FALSE(refused.valid());
    }
    CHECK(system->clipCount() == MAX_CLIPS);
    CHECK(capture.count() == 1);  // LATCHED across twenty further attempts
}

TEST_CASE("SY4: play -> isPlaying true; stop + render + service -> false") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    const VoiceHandle voice = system->play(clip, flatParams());
    REQUIRE(voice.valid());
    CHECK(system->isPlaying(voice));

    system->stop(voice);
    std::array<float, 32> buffer{};
    system->render(buffer, 1, 48000);  // the Stop is drained here and the voice retires
    system->service();
    CHECK_FALSE(system->isPlaying(voice));
}

TEST_CASE("SY5: a STALE handle is inert on ALL FIVE entry points") {
    // Five arms, because one accessor proves nothing about the other four. Every arm is judged by
    // the EFFECT on the live voice that now occupies the same slot, never by the stale call's return.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle shortClip = system->registerClip(makeClip(48000, 1, 8, guidOf(1), 20000));
    const ClipHandle longClip = system->registerClip(makeClip(48000, 1, 4096, guidOf(2), 16384));
    REQUIRE(shortClip.valid());
    REQUIRE(longClip.valid());

    const VoiceHandle first = system->play(shortClip, flatParams());
    REQUIRE(first.valid());
    std::array<float, 64> buffer{};
    system->render(buffer, 1, 48000);  // the 8-frame clip runs out and retires
    system->service();
    CHECK_FALSE(system->isPlaying(first));  // ARM 5: isPlaying on a stale handle is false

    // Re-allocate the SAME slot; `first` is now stale by generation.
    VoiceParams live = flatParams();
    live.volume = 1.0F;
    const VoiceHandle second = system->play(longClip, live);
    REQUIRE(second.valid());
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);

    // ARMS 1-4: every mutator, aimed at the stale handle.
    system->stop(first);
    system->setVolume(first, 0.0F);
    system->setPitch(first, 0.0F);
    system->setPose(first, engine::Vec3{500.0F, 500.0F, 500.0F});

    std::array<float, 16> warmUp{};
    system->render(warmUp, 1, 48000);
    system->service();

    // The live voice is UNTOUCHED: still playing (so the stale stop did nothing), and still at full
    // volume on the frame it should be on (so the stale setVolume/setPitch/setPose did nothing).
    CHECK(system->isPlaying(second));
    CHECK(system->stats().activeVoices == 1U);
    std::array<float, 16> measured{};
    system->render(measured, 1, 48000);
    for (const float sample : measured) {
        CHECK(sample == 16384.0F / 32768.0F);
    }
}

TEST_CASE("SY6: 64 plays succeed, the 65th is invalid and counted, and it warns exactly once") {
    const LogCapture capture;
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    for (std::uint32_t i = 0; i < MAX_VOICES; ++i) {
        const VoiceHandle voice = system->play(clip, flatParams());
        REQUIRE(voice.valid());
    }
    CHECK(system->stats().rejectedPlays == 0U);
    CHECK(capture.count() == 0);

    const VoiceHandle refused = system->play(clip, flatParams());
    CHECK_FALSE(refused.valid());
    CHECK(system->stats().rejectedPlays == 1U);

    for (int i = 0; i < 10; ++i) {
        CHECK_FALSE(system->play(clip, flatParams()).valid());
    }
    CHECK(system->stats().rejectedPlays == 11U);
    CHECK(capture.count() == 1);  // LATCHED
}

TEST_CASE("SY7: a FAILED push RETURNS THE SLOT -- the leak activeVoices can never see") {
    // Fill the command ring so the next Start's push fails, then prove all 64 slots are still
    // available afterwards. If play() pushed before allocating, or forgot to return the slot on
    // failure, one voice would be lost per failed play with NO counter moving and NOTHING logging.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    // setMasterVolume is droppable, so it stops at the reserve; stopAll() is NOT, so it fills the
    // ring right to the top.
    for (int i = 0; i < 4096; ++i) {
        system->stopAll();
    }

    const VoiceHandle refused = system->play(clip, flatParams());
    CHECK_FALSE(refused.valid());
    CHECK(system->stats().rejectedPlays >= 1U);

    // Drain everything, then prove MAX_VOICES slots are still there to be handed out.
    std::array<float, 32> buffer{};
    system->render(buffer, 1, 48000);
    system->service();

    std::uint32_t granted = 0;
    for (std::uint32_t i = 0; i < MAX_VOICES; ++i) {
        if (system->play(clip, flatParams()).valid()) {
            ++granted;
        }
    }
    CHECK(granted == MAX_VOICES);  // not 63, which is what a leaked slot would read
}

TEST_CASE("SY8: droppable commands are discarded under the reserve WHILE Start and Stop still land") {
    // Asserted by DRAINING and COUNTING KINDS through their observable effects, not by counting a
    // total: a dropped Stop is a sound that never ends, and a dropped SetParams is benign.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    const VoiceHandle voice = system->play(clip, flatParams());
    REQUIRE(voice.valid());

    // Flood with DROPPABLE commands until the reserve starts refusing them.
    for (int i = 0; i < 4096; ++i) {
        system->setMasterVolume(0.5F);
    }
    const std::uint64_t dropped = system->stats().droppedCommands;
    CHECK(dropped > 0U);

    // The reserve is still free, so a NON-droppable Stop lands and the voice really stops.
    system->stop(voice);
    std::array<float, 32> buffer{};
    system->render(buffer, 1, 48000);
    system->service();
    CHECK_FALSE(system->isPlaying(voice));

    // And a Start lands too, on the same still-reserved room.
    for (int i = 0; i < 4096; ++i) {
        system->setMasterVolume(0.5F);
    }
    const VoiceHandle again = system->play(clip, flatParams());
    CHECK(again.valid());
}

TEST_CASE("SY9: droppedCommands counts EXACTLY the discards") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    CHECK(system->stats().droppedCommands == 0U);

    // The ring holds 1024 and the reserve is 64, so 1024 - 64 == 960 droppable commands fit before
    // the first discard. Pushing 1000 must therefore discard exactly 40.
    for (int i = 0; i < 1000; ++i) {
        system->setMasterVolume(1.0F);
    }
    CHECK(system->stats().droppedCommands == 40U);

    for (int i = 0; i < 7; ++i) {
        system->setMasterVolume(1.0F);
    }
    CHECK(system->stats().droppedCommands == 47U);
}

TEST_CASE("SY10: service() reclaims finished slots, and WITHOUT it play() eventually refuses") {
    // The contract stated as a test rather than left as folklore.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4, guidOf(1)));
    REQUIRE(clip.valid());

    std::array<float, 32> buffer{};

    SUBCASE("with service(), the same slot is reused indefinitely") {
        for (int cycle = 0; cycle < 200; ++cycle) {
            const VoiceHandle voice = system->play(clip, flatParams());
            REQUIRE(voice.valid());
            system->render(buffer, 1, 48000);  // the 4-frame clip finishes inside one block
            system->service();
        }
        CHECK(system->stats().rejectedPlays == 0U);
    }

    SUBCASE("WITHOUT service(), play() refuses once the pool is exhausted") {
        std::uint32_t granted = 0;
        for (int cycle = 0; cycle < 200; ++cycle) {
            if (system->play(clip, flatParams()).valid()) {
                ++granted;
            }
            system->render(buffer, 1, 48000);
            // service() DELIBERATELY NOT CALLED
        }
        CHECK(granted == MAX_VOICES);
        CHECK(system->stats().rejectedPlays == 200U - MAX_VOICES);
    }
}

TEST_CASE("SY11: the retire ring never overflows across 10 000 play/finish cycles") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4, guidOf(1)));
    REQUIRE(clip.valid());

    std::array<float, 32> buffer{};
    for (int cycle = 0; cycle < 10000; ++cycle) {
        const VoiceHandle voice = system->play(clip, flatParams());
        REQUIRE(voice.valid());  // a dropped retirement would exhaust the pool and fail here
        system->render(buffer, 1, 48000);
        system->service();
        CHECK_FALSE(system->isPlaying(voice));
    }
    CHECK(system->stats().rejectedPlays == 0U);
}

TEST_CASE("SY12: stopAll() stops every voice and is IDEMPOTENT") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    std::vector<VoiceHandle> voices;
    for (std::uint32_t i = 0; i < 20; ++i) {
        voices.push_back(system->play(clip, flatParams()));
        REQUIRE(voices.back().valid());
    }

    system->stopAll();
    std::array<float, 32> buffer{};
    system->render(buffer, 1, 48000);
    system->service();
    for (const VoiceHandle& voice : voices) {
        CHECK_FALSE(system->isPlaying(voice));
    }
    CHECK(system->stats().activeVoices == 0U);

    system->stopAll();  // idempotent: nothing to stop, nothing breaks
    system->render(buffer, 1, 48000);
    system->service();
    CHECK(system->stats().activeVoices == 0U);
}

TEST_CASE("SY13: master volume scales the output LINEARLY, and 0 is EXACT silence") {
    auto secondBlock = [](float master) {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1), 16384));
        REQUIRE(clip.valid());
        system->setMasterVolume(master);
        const VoiceHandle voice = system->play(clip, flatParams());
        REQUIRE(voice.valid());
        std::array<float, 16> warmUp{};
        system->render(warmUp, 1, 48000);
        std::array<float, 16> buffer{};
        system->render(buffer, 1, 48000);
        return buffer[0];
    };

    const float full = secondBlock(1.0F);
    CHECK(full == 16384.0F / 32768.0F);
    CHECK(secondBlock(0.5F) == full * 0.5F);  // EXACT: 0.5 is a power of two
    CHECK(secondBlock(0.0F) == 0.0F);         // EXACT silence
}

TEST_CASE("SY14: an INVALID listener silences spatialized voices and leaves 2D ones at full volume") {
    // D23, and the property that makes it true WITH NO SECOND BRANCH anywhere in the mixer: a world
    // with no listener leaves the pose default-constructed, whose volume is still 1.0F.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1), 16384));
    REQUIRE(clip.valid());

    system->setListener(ListenerPose{});  // valid == false: THERE IS NO LISTENER

    VoiceParams spatial;
    spatial.spatialize = true;
    spatial.position = engine::Vec3{0.0F, 0.0F, -1.0F};
    const VoiceHandle spatialVoice = system->play(clip, spatial);
    REQUIRE(spatialVoice.valid());

    std::array<float, 16> warmUp{};
    system->render(warmUp, 1, 48000);
    std::array<float, 16> buffer{};
    system->render(buffer, 1, 48000);
    for (const float sample : buffer) {
        CHECK(sample == 0.0F);  // spatialized: EXACTLY silent
    }

    const std::unique_ptr<AudioSystem> flat = AudioSystem::create();
    REQUIRE(flat != nullptr);
    const ClipHandle flatClip = flat->registerClip(makeClip(48000, 1, 4096, guidOf(2), 16384));
    REQUIRE(flatClip.valid());
    flat->setListener(ListenerPose{});
    const VoiceHandle flatVoice = flat->play(flatClip, flatParams());
    REQUIRE(flatVoice.valid());
    flat->render(warmUp, 1, 48000);
    std::array<float, 16> flatBuffer{};
    flat->render(flatBuffer, 1, 48000);
    for (const float sample : flatBuffer) {
        CHECK(sample == 16384.0F / 32768.0F);  // non-spatialized: FULL volume, unaffected
    }
}

TEST_CASE("SY15: non-finite arguments are REPLACED, not propagated -- three entry points, per axis") {
    const std::array<float, 3> poisons = {std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity()};

    for (const float bad : poisons) {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1), 16384));
        REQUIRE(clip.valid());

        VoiceParams params;
        params.spatialize = true;
        const VoiceHandle voice = system->play(clip, params);
        REQUIRE(voice.valid());

        system->setVolume(voice, bad);
        system->setPitch(voice, bad);
        system->setPose(voice, engine::Vec3{bad, 0.0F, 0.0F});
        system->setPose(voice, engine::Vec3{0.0F, bad, 0.0F});
        system->setPose(voice, engine::Vec3{0.0F, 0.0F, bad});
        system->setMasterVolume(bad);

        ListenerPose listener;
        listener.valid = true;
        listener.volume = bad;
        system->setListener(listener);

        std::array<float, 32> buffer{};
        system->render(buffer, 2, 48000);
        system->render(buffer, 2, 48000);
        for (const float sample : buffer) {
            CHECK(std::isfinite(sample));
        }

        // And play() sanitises the WHOLE VoiceParams on the way in, not merely the three setters.
        VoiceParams poisoned;
        poisoned.volume = bad;
        poisoned.pitch = bad;
        poisoned.minDistance = bad;
        poisoned.maxDistance = bad;
        poisoned.position = engine::Vec3{bad, bad, bad};
        const VoiceHandle second = system->play(clip, poisoned);
        REQUIRE(second.valid());
        system->render(buffer, 2, 48000);
        for (const float sample : buffer) {
            CHECK(std::isfinite(sample));
        }
    }
}

TEST_CASE("SY16: renderCallback is PROVEN to be an adapter -- byte-identical, and null-safe") {
    auto driveDirect = [](std::array<float, 64>& out) {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        const ClipHandle clip = system->registerClip(makeClip(48000, 2, 4096, guidOf(1), 12345));
        REQUIRE(clip.valid());
        const VoiceHandle voice = system->play(clip, flatParams());
        REQUIRE(voice.valid());
        system->render(out, 2, 48000);
    };
    auto driveThroughCallback = [](std::array<float, 64>& out) {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        const ClipHandle clip = system->registerClip(makeClip(48000, 2, 4096, guidOf(1), 12345));
        REQUIRE(clip.valid());
        const VoiceHandle voice = system->play(clip, flatParams());
        REQUIRE(voice.valid());
        AudioSystem::renderCallback(system.get(), out, 2, 48000);
    };

    std::array<float, 64> direct{};
    std::array<float, 64> viaCallback{};
    driveDirect(direct);
    driveThroughCallback(viaCallback);
    for (std::size_t i = 0; i < direct.size(); ++i) {
        CHECK(direct[i] == viaCallback[i]);  // BYTE-IDENTICAL
    }

    // A NULL user writes silence and does not crash -- what makes a pUserData that never got set a
    // SILENT failure on a realtime thread rather than a segfault.
    std::array<float, 8> poisoned{};
    for (float& sample : poisoned) {
        sample = -999.0F;
    }
    AudioSystem::renderCallback(nullptr, poisoned, 2, 48000);
    for (const float sample : poisoned) {
        CHECK(sample == 0.0F);
    }
}

TEST_CASE("SY17: peakVoices survives a RISE AND A FALL") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const ClipHandle clip = system->registerClip(makeClip(48000, 1, 4096, guidOf(1)));
    REQUIRE(clip.valid());

    std::vector<VoiceHandle> voices;
    for (std::uint32_t i = 0; i < 40; ++i) {
        voices.push_back(system->play(clip, flatParams()));
        REQUIRE(voices.back().valid());
    }
    CHECK(system->stats().peakVoices == 40U);

    for (std::size_t i = 5; i < voices.size(); ++i) {
        system->stop(voices[i]);
    }
    std::array<float, 32> buffer{};
    system->render(buffer, 1, 48000);
    system->service();
    CHECK(system->stats().activeVoices == 5U);
    CHECK(system->stats().peakVoices == 40U);  // the PEAK does not fall
}

TEST_CASE("SY18: two renders from an IDENTICAL command sequence are BYTE-IDENTICAL") {
    // The determinism the sample's --dump-pcm rests on (INV-10, validation row 5). It is assertable
    // only because the cursor is integer arithmetic and the increment conversion reaches NO libm
    // function -- contrast render::sampleAnimation, which docs/09 section 13.7 excludes from the
    // determinism contract by name.
    auto run = [](std::array<float, 512>& out) {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        const ClipHandle clip = system->registerClip(makeClip(44100, 2, 777, guidOf(1), 9001));
        REQUIRE(clip.valid());

        ListenerPose listener;
        listener.valid = true;
        listener.volume = 0.75F;
        system->setListener(listener);
        system->setMasterVolume(0.8F);

        VoiceParams params;
        params.spatialize = true;
        params.position = engine::Vec3{2.0F, 1.0F, -3.0F};
        params.pitch = 1.37F;
        params.loop = true;
        const VoiceHandle voice = system->play(clip, params);
        REQUIRE(voice.valid());

        std::array<float, 64> block{};
        for (std::size_t b = 0; b < 8; ++b) {
            system->render(block, 2, 48000);
            for (std::size_t i = 0; i < block.size(); ++i) {
                out[(b * block.size()) + i] = block[i];
            }
        }
    };

    std::array<float, 512> first{};
    std::array<float, 512> second{};
    run(first);
    run(second);
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i] == second[i]);
    }
    // Anti-vacuity: the run must actually have produced sound.
    bool anyNonZero = false;
    for (const float sample : first) {
        if (sample != 0.0F) {
            anyNonZero = true;
            break;
        }
    }
    CHECK(anyNonZero);
}

TEST_CASE("SY19: a Guid registered TWICE replaces the mapping and leaves the first PLAYABLE") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(7);

    const ClipHandle first = system->registerClip(makeClip(48000, 1, 4096, guid, 10000));
    REQUIRE(first.valid());
    const ClipHandle second = system->registerClip(makeClip(48000, 1, 4096, guid, 20000));
    REQUIRE(second.valid());
    CHECK(first != second);
    CHECK(system->clipCount() == 2U);         // BOTH are resident -- "replace" is not "evict"
    CHECK(system->findClip(guid) == second);  // the mapping takes the NEWER handle

    // The FIRST clip is still playable through its existing handle, and audibly different.
    const VoiceHandle voice = system->play(first, flatParams());
    REQUIRE(voice.valid());
    std::array<float, 16> warmUp{};
    system->render(warmUp, 1, 48000);
    std::array<float, 16> buffer{};
    system->render(buffer, 1, 48000);
    CHECK(buffer[0] == 10000.0F / 32768.0F);
}

TEST_CASE("SY20: destroying a system with voices playing and clips registered LEAKS NOTHING") {
    // HONEST ABOUT WHAT THIS PROVES ON THIS MACHINE: macOS has ASan but NOT LSan, so a green run here
    // proves the teardown is CLEAN, not that a leak is ABSENT. The Linux Debug lane is the only place
    // that turns this case into evidence -- task 3.7.1 learned that the hard way, when a 632-byte
    // leak in a decoder's own error path was invisible on two of three lanes.
    {
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);
        for (std::uint32_t i = 0; i < 32; ++i) {
            const ClipHandle clip = system->registerClip(makeClip(48000, 2, 512, guidOf(i)));
            REQUIRE(clip.valid());
        }
        const ClipHandle clip = system->findClip(guidOf(0));
        REQUIRE(clip.valid());
        for (std::uint32_t i = 0; i < MAX_VOICES; ++i) {
            const VoiceHandle voice = system->play(clip, flatParams());
            REQUIRE(voice.valid());
        }
        std::array<float, 64> buffer{};
        system->render(buffer, 2, 48000);
        CHECK(system->stats().activeVoices > 0U);
        // and the system is destroyed here, mid-playback, with 32 clips resident
    }
    CHECK(true);  // reaching this line under ASan is the assertion
}
