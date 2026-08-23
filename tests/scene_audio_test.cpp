// tests/scene_audio_test.cpp -- task 3.7.2: engine::scene_audio, SA1-SA18. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// THE ONE TRANSLATION UNIT THAT SEES BOTH engine/scene AND engine/audio, which is what makes SA1 --
// the AERO_RANGE literal mirror -- expressible at all.
//
// TIER-0 BY CONSTRUCTION: buildAudioView is pure and takes no AudioSystem, and SceneAudio owns none,
// so every case here runs with no device, no thread and no GPU. All eighteen run in EVERY
// configuration this project builds.
//
// <ostream> is included preventively (the 0.4.1 trap). There is no #if of any kind in this file and
// there must never be one.

#include <aero/assets/audio_cook.hpp>
#include <aero/audio/audio.hpp>
#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene_audio/scene_audio.hpp>

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

using engine::AudioListener;
using engine::AudioSource;
using engine::ByteBuffer;
using engine::Entity;
using engine::Guid;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::audio::AudioClip;
using engine::audio::AudioSystem;
using engine::audio::ClipHandle;
using engine::audio::loadAudioClip;
using engine::audio::MAX_VOICES;
using engine::audio::VoiceHandle;
using engine::scene_audio::AudioView;
using engine::scene_audio::AudioViewScratch;
using engine::scene_audio::buildAudioView;
using engine::scene_audio::SceneAudio;

// ============================================================================================
// SA1 lives here rather than in a TEST_CASE, deliberately: it is a static_assert, so a change to
// engine::audio::MAX_PITCH is a COMPILE failure rather than a test failure (R6).
//
// IT COVERS ONE SIDE OF THE MIRROR, NOT BOTH, AND THAT WAS MEASURED RATHER THAN ASSUMED. Seeding
// MAX_PITCH to 8.0F fails to build, here. Seeding the AERO_RANGE LITERAL in audio_source.hpp to
// 8.0f builds clean and leaves this assertion GREEN -- because no C++ expression can read an
// AERO_RANGE argument, so this file cannot see it at all. That side is witnessed by
// tests/editor/inspector_test.cpp's AudioSource row, which reads rangeMax off the GENERATED meta.
// Together they close the loop; neither alone does. (3.7.1's A14 finding, one task later: a
// static_assert that reads as a literal-substitution tripwire can be a one-directional drift one.)
// ============================================================================================
static_assert(engine::audio::MAX_PITCH == 4.0F, "AudioSource::pitch's AERO_RANGE upper bound is 4.0f");
static_assert(engine::audio::MIN_PITCH == 0.0F, "AudioSource::pitch's AERO_RANGE lower bound is 0.0f");

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

[[nodiscard]] AudioClip makeClip(Guid guid, std::uint32_t frames = 4096) {
    const std::vector<std::int16_t> samples(frames, 16384);
    engine::assets::AudioCookInput input;
    input.sourceGuid = guid;
    input.sampleRate = 48000;
    input.channels = 1;
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

// Advances one whole frame's worth of audio so a retirement can reach the ring and be serviced.
void pumpBlocks(AudioSystem& system, int blocks = 1) {
    std::array<float, 64> buffer{};
    for (int i = 0; i < blocks; ++i) {
        system.render(buffer, 1, 48000);
    }
}

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

TEST_CASE("SA1: the AERO_RANGE literal mirror is pinned at compile time") {
    // The static_asserts above are the real assertion; this case exists so the id appears in the
    // filter output and a reader looking for SA1 finds it.
    CHECK(engine::audio::MAX_PITCH == 4.0F);
    CHECK(engine::audio::MIN_PITCH == 0.0F);
}

TEST_CASE("SA2: an EMPTY world yields no sources, no listener, and volume STILL 1.0F") {
    World world;
    AudioViewScratch scratch;
    const AudioView view = buildAudioView(world, scratch);

    CHECK(view.sources.empty());
    CHECK(view.sourceCount == 0U);
    CHECK(view.listenerCount == 0U);
    CHECK_FALSE(view.listener.valid);
    // THE PROPERTY D23 RESTS ON: a world with no listener leaves the pose's volume at 1.0F, so the
    // mixer's uniform `* listener.volume` needs NO second branch for non-spatialized voices.
    CHECK(view.listener.volume == 1.0F);
}

TEST_CASE("SA3: one listener resolves from worldMatrix, and a PARENTED one resolves in WORLD space") {
    SUBCASE("unparented") {
        World world;
        const Entity e = world.create();
        world.add<Transform>(e, Transform{.position = Vec3{1.0F, 2.0F, 3.0F}});
        world.add<AudioListener>(e, AudioListener{.volume = 0.5F});

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.listener.valid);
        CHECK(view.listenerCount == 1U);
        CHECK(view.listener.position.x == 1.0F);
        CHECK(view.listener.position.y == 2.0F);
        CHECK(view.listener.position.z == 3.0F);
        CHECK(view.listener.volume == 0.5F);
        CHECK(view.listener.forward.z == doctest::Approx(-1.0F).epsilon(1e-6));  // -Z forward
        CHECK(view.listener.right.x == doctest::Approx(1.0F).epsilon(1e-6));
    }

    SUBCASE("PARENTED, with a translating AND rotating parent -- position and basis both") {
        World world;
        const Entity parent = world.create();
        // A 90-degree yaw about +Y: the child's local -Z forward becomes world -X.
        const float halfAngle = 0.25F * engine::PI;  // half of 90 degrees, in radians
        const Quat yaw{0.0F, std::sin(halfAngle), 0.0F, std::cos(halfAngle)};
        world.add<Transform>(parent, Transform{.position = Vec3{10.0F, 0.0F, 0.0F}, .rotation = yaw});

        const Entity child = world.create();
        world.add<Transform>(child, Transform{.position = Vec3{0.0F, 0.0F, -2.0F}});
        world.add<AudioListener>(child, AudioListener{});
        REQUIRE(world.setParent(child, parent));

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.listener.valid);
        // The child's local (0, 0, -2) rotated 90 degrees about +Y is (-2, 0, 0), then translated by
        // the parent's (10, 0, 0): world (8, 0, 0). LOCAL would be (0, 0, -2) -- a different answer.
        CHECK(view.listener.position.x == doctest::Approx(8.0F).epsilon(1e-5));
        CHECK(view.listener.position.y == doctest::Approx(0.0F).epsilon(1e-5));
        CHECK(view.listener.position.z == doctest::Approx(0.0F).epsilon(1e-5));
        // And the BASIS rotated with it: world forward is now -X, world right is -Z.
        CHECK(view.listener.forward.x == doctest::Approx(-1.0F).epsilon(1e-5));
        CHECK(view.listener.right.z == doctest::Approx(-1.0F).epsilon(1e-5));
    }
}

TEST_CASE("SA4: two listeners -- the LOWEST entity index wins, and they are at DIFFERENT positions") {
    // The two listeners are deliberately given different positions, so picking the wrong one is
    // VISIBLE rather than merely miscounted.
    World world;
    const Entity first = world.create();
    world.add<Transform>(first, Transform{.position = Vec3{1.0F, 0.0F, 0.0F}});
    world.add<AudioListener>(first, AudioListener{});

    const Entity second = world.create();
    world.add<Transform>(second, Transform{.position = Vec3{99.0F, 0.0F, 0.0F}});
    world.add<AudioListener>(second, AudioListener{});
    REQUIRE(first.index < second.index);

    AudioViewScratch scratch;
    const AudioView view = buildAudioView(world, scratch);
    CHECK(view.listenerCount == 2U);
    REQUIRE(view.listener.valid);
    CHECK(view.listener.position.x == 1.0F);  // the LOWEST index, not the last visited
}

TEST_CASE("SA5: a SCALED AND ROTATED listener yields an ORTHONORMAL basis, and a DEGENERATE one is safe") {
    SUBCASE("scaled and rotated -- orthonormal") {
        World world;
        const Entity e = world.create();
        // A UNIT quaternion about a NORMALISED axis. Building one component-wise without normalising
        // yields a non-orthogonal basis whose columns normalizeOrZero cannot rescue -- the axes come
        // out unit-length and still not perpendicular, which is what this case would then measure.
        const Vec3 axis = engine::normalizeOrZero(Vec3{0.3F, 1.0F, 0.5F});
        const float halfAngle = 0.15F;
        const float s = std::sin(halfAngle);
        const Quat rotation{axis.x * s, axis.y * s, axis.z * s, std::cos(halfAngle)};
        const Vec3 scale{3.0F, 7.0F, 0.25F};
        world.add<Transform>(e, Transform{.position = Vec3{}, .rotation = rotation, .scale = scale});
        world.add<AudioListener>(e, AudioListener{});

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.listener.valid);

        CHECK(engine::length(view.listener.right) == doctest::Approx(1.0F).epsilon(1e-6));
        CHECK(engine::length(view.listener.up) == doctest::Approx(1.0F).epsilon(1e-6));
        CHECK(engine::length(view.listener.forward) == doctest::Approx(1.0F).epsilon(1e-6));
        CHECK(engine::dot(view.listener.right, view.listener.up) == doctest::Approx(0.0F).epsilon(1e-6));
        CHECK(engine::dot(view.listener.right, view.listener.forward) == doctest::Approx(0.0F).epsilon(1e-6));
        CHECK(engine::dot(view.listener.up, view.listener.forward) == doctest::Approx(0.0F).epsilon(1e-6));
    }

    SUBCASE("a ZERO-SCALE column yields a ZERO axis rather than a NaN") {
        // THE normalizeOrZero ARM, AND THE ONLY FIXTURE IN THE TREE THAT REACHES IT. Nothing else
        // gives a listener entity a degenerate column, so substituting normalize() for
        // normalizeOrZero() in buildAudioView left the whole suite GREEN in BOTH configurations --
        // measured, not assumed. normalize()'s contract is an assert-and-no-branch (vec3.hpp), so the
        // substitution ABORTS a Debug build here and produces a NaN basis in Release; either is
        // caught, and the abort is the stronger result.
        //
        // SP13 cannot witness this and never could: it constructs a ListenerPose with a zero `right`
        // DIRECTLY and never enters buildAudioView at all. It is a spatializer case; this is a bridge
        // one, and they are one layer apart.
        World world;
        const Entity e = world.create();
        const Vec3 flat{0.0F, 1.0F, 1.0F};  // the X column is degenerate
        world.add<Transform>(e, Transform{.position = Vec3{1.0F, 2.0F, 3.0F}, .scale = flat});
        world.add<AudioListener>(e, AudioListener{});

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.listener.valid);

        CHECK(std::isfinite(view.listener.right.x));
        CHECK(std::isfinite(view.listener.right.y));
        CHECK(std::isfinite(view.listener.right.z));
        CHECK(engine::lengthSquared(view.listener.right) == 0.0F);  // EXACTLY zero, never a NaN
        CHECK(engine::length(view.listener.up) == doctest::Approx(1.0F).epsilon(1e-6));
        CHECK(engine::length(view.listener.forward) == doctest::Approx(1.0F).epsilon(1e-6));
        CHECK(std::isfinite(view.listener.position.x));
    }
}

TEST_CASE("SA6: a source with playing == false is NOT EMITTED at all") {
    World world;
    const Entity playing = world.create();
    world.add<Transform>(playing, Transform{});
    world.add<AudioSource>(playing, AudioSource{.clip = guidOf(1), .playing = true});

    const Entity silent = world.create();
    world.add<Transform>(silent, Transform{});
    world.add<AudioSource>(silent, AudioSource{.clip = guidOf(2), .playing = false});

    AudioViewScratch scratch;
    const AudioView view = buildAudioView(world, scratch);
    REQUIRE(view.sourceCount == 1U);
    CHECK(view.sources[0].entity == playing);
    CHECK(view.sources[0].clip == guidOf(1));
}

TEST_CASE("SA7: a PARENTED source's position is WORLD space, not local") {
    // The parent carries a translation AND a rotation, so a seed that keeps the translation and drops
    // the rotation is caught too.
    World world;
    const Entity parent = world.create();
    const float halfAngle = 0.25F * engine::PI;
    const Quat yaw{0.0F, std::sin(halfAngle), 0.0F, std::cos(halfAngle)};
    world.add<Transform>(parent, Transform{.position = Vec3{5.0F, 1.0F, 0.0F}, .rotation = yaw});

    const Entity child = world.create();
    world.add<Transform>(child, Transform{.position = Vec3{0.0F, 0.0F, -4.0F}});
    world.add<AudioSource>(child, AudioSource{.clip = guidOf(1)});
    REQUIRE(world.setParent(child, parent));

    AudioViewScratch scratch;
    const AudioView view = buildAudioView(world, scratch);
    REQUIRE(view.sourceCount == 1U);
    const Vec3 position = view.sources[0].params.position;
    // local (0, 0, -4) yawed 90 degrees about +Y is (-4, 0, 0); plus the parent's (5, 1, 0) is (1, 1, 0).
    CHECK(position.x == doctest::Approx(1.0F).epsilon(1e-5));
    CHECK(position.y == doctest::Approx(1.0F).epsilon(1e-5));
    CHECK(position.z == doctest::Approx(0.0F).epsilon(1e-5));
    // The LOCAL answer would be (0, 0, -4); assert it is not that, so the case cannot pass vacuously.
    CHECK(position.z != doctest::Approx(-4.0F).epsilon(1e-5));
}

TEST_CASE("SA8: component values are CLAMPED and SANITISED on the way into the view") {
    constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

    SUBCASE("out-of-range volume and pitch are clamped") {
        World world;
        const Entity e = world.create();
        world.add<Transform>(e, Transform{});
        world.add<AudioSource>(e, AudioSource{.clip = guidOf(1), .volume = 7.0F, .pitch = 99.0F});

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.sourceCount == 1U);
        CHECK(view.sources[0].params.volume == 1.0F);
        CHECK(view.sources[0].params.pitch == engine::audio::MAX_PITCH);
    }

    SUBCASE("negative volume and pitch clamp at the lower bound") {
        World world;
        const Entity e = world.create();
        world.add<Transform>(e, Transform{});
        world.add<AudioSource>(e, AudioSource{.clip = guidOf(1), .volume = -3.0F, .pitch = -3.0F});

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.sourceCount == 1U);
        CHECK(view.sources[0].params.volume == 0.0F);
        CHECK(view.sources[0].params.pitch == engine::audio::MIN_PITCH);
    }

    SUBCASE("a NaN in EACH of the four floats is replaced, and the position too") {
        World world;
        const Entity e = world.create();
        world.add<Transform>(e, Transform{.position = Vec3{NAN_F, 1.0F, 2.0F}});
        AudioSource poisoned;
        poisoned.clip = guidOf(1);
        poisoned.volume = NAN_F;
        poisoned.pitch = NAN_F;
        poisoned.minDistance = NAN_F;
        poisoned.maxDistance = NAN_F;
        world.add<AudioSource>(e, poisoned);

        AudioViewScratch scratch;
        const AudioView view = buildAudioView(world, scratch);
        REQUIRE(view.sourceCount == 1U);
        const engine::audio::VoiceParams& params = view.sources[0].params;
        CHECK(std::isfinite(params.volume));
        CHECK(std::isfinite(params.pitch));
        CHECK(std::isfinite(params.minDistance));
        CHECK(std::isfinite(params.maxDistance));
        CHECK(std::isfinite(params.position.x));
        CHECK(std::isfinite(params.position.y));
        CHECK(std::isfinite(params.position.z));
        CHECK(params.position.y == 1.0F);  // per COMPONENT: the finite components survive
        CHECK(params.position.z == 2.0F);
    }
}

TEST_CASE("SA9: a SECOND buildAudioView reuses the scratch and allocates NOTHING after warm-up") {
    World world;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{});
        world.add<AudioSource>(e, AudioSource{.clip = guidOf(i)});
    }

    AudioViewScratch scratch;
    const AudioView first = buildAudioView(world, scratch);
    CHECK(first.sourceCount == 16U);
    const std::size_t capacityAfterWarmUp = scratch.sources.capacity();
    REQUIRE(capacityAfterWarmUp >= 16U);

    const AudioView second = buildAudioView(world, scratch);
    CHECK(second.sourceCount == 16U);
    CHECK(scratch.sources.capacity() == capacityAfterWarmUp);  // UNCHANGED: clear() kept the capacity
}

TEST_CASE("SA10: a new playing source with a REGISTERED clip starts EXACTLY ONE voice") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    const ClipHandle clip = system->registerClip(makeClip(guid));
    REQUIRE(clip.valid());

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid});

    SceneAudio bridge;
    bridge.update(world, *system);
    CHECK(bridge.bindingCount() == 1U);
    CHECK(bridge.lastUnresolvedClips() == 0U);

    pumpBlocks(*system);
    CHECK(system->stats().activeVoices == 1U);

    // A second update with an unchanged world starts NOTHING more.
    bridge.update(world, *system);
    pumpBlocks(*system);
    CHECK(bridge.bindingCount() == 1U);
    CHECK(system->stats().activeVoices == 1U);
}

TEST_CASE("SA10b: a SATURATED retrigger needs the slots service() freed on the PREVIOUS block") {
    // THE ARM THAT MAKES update()'s STEP ORDER OBSERVABLE, AND IT HAD TO BE ADDED. Every other case
    // here plays a handful of voices and calls system->service() from the test itself, so neither
    // dropping service() from update() nor moving it AFTER the reconcile changed anything -- the
    // whole suite stayed GREEN under both, measured rather than assumed.
    //
    // The order is only observable when a slot freed on the PREVIOUS block is needed THIS frame, at a
    // moment when the pool is otherwise fully committed. So: 64 looping sources, all stopped in one
    // frame, all retriggered in the next. THE TEST NEVER CALLS system->service() ITSELF -- doing so
    // would mask exactly the defect this exists to catch.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid, /*frames=*/8192)).valid());

    World world;
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});

    std::vector<Entity> sources;
    for (std::uint32_t i = 0; i < MAX_VOICES; ++i) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{});
        world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true});
        sources.push_back(e);
    }

    SceneAudio bridge;
    bridge.update(world, *system);
    pumpBlocks(*system);
    REQUIRE(bridge.bindingCount() == MAX_VOICES);
    REQUIRE(system->stats().activeVoices == MAX_VOICES);
    REQUIRE(system->stats().rejectedPlays == 0U);

    for (int cycle = 0; cycle < 4; ++cycle) {
        // Frame N: stop every one of them. The Stops reach the mixer on the block below and every
        // voice retires onto the retire ring.
        for (const Entity& e : sources) {
            world.get<AudioSource>(e)->playing = false;
        }
        bridge.update(world, *system);
        CHECK(bridge.bindingCount() == 0U);
        pumpBlocks(*system, 2);
        CHECK(system->stats().activeVoices == 0U);

        // Frame N+1: retrigger all 64. Their slots are on the retire ring and NOWHERE ELSE, so this
        // frame succeeds ONLY IF update() serviced BEFORE it reconciled.
        for (const Entity& e : sources) {
            world.get<AudioSource>(e)->playing = true;
        }
        bridge.update(world, *system);
        pumpBlocks(*system);
        CHECK(bridge.bindingCount() == MAX_VOICES);
        CHECK(system->stats().activeVoices == MAX_VOICES);
        CHECK(system->stats().rejectedPlays == 0U);
    }
}

TEST_CASE("SA11: a nil and an unregistered clip are each COUNTED and emit NO LOG LINE") {
    const LogCapture capture;
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);

    World world;
    const Entity nilClip = world.create();
    world.add<Transform>(nilClip, Transform{});
    world.add<AudioSource>(nilClip, AudioSource{});  // clip defaults NIL

    const Entity unregistered = world.create();
    world.add<Transform>(unregistered, Transform{});
    world.add<AudioSource>(unregistered, AudioSource{.clip = guidOf(42)});

    // A listener, so the no-listener latch does not fire and pollute the count.
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});

    SceneAudio bridge;
    for (int frame = 0; frame < 50; ++frame) {
        bridge.update(world, *system);
    }
    CHECK(bridge.lastUnresolvedClips() == 2U);  // per frame, not cumulative
    CHECK(bridge.bindingCount() == 0U);
    CHECK(capture.count() == 0);  // COUNTED, NEVER WARNED -- across fifty frames
}

TEST_CASE("SA12: playing true -> false STOPS the voice; false -> true starts a NEW one") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    const ClipHandle clip = system->registerClip(makeClip(guid));
    REQUIRE(clip.valid());

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    REQUIRE(bridge.bindingCount() == 1U);
    pumpBlocks(*system);
    CHECK(system->stats().activeVoices == 1U);

    world.get<AudioSource>(e)->playing = false;
    bridge.update(world, *system);
    CHECK(bridge.bindingCount() == 0U);  // the binding is DROPPED, which is what makes retrigger work
    pumpBlocks(*system, 2);
    system->service();
    CHECK(system->stats().activeVoices == 0U);

    world.get<AudioSource>(e)->playing = true;
    bridge.update(world, *system);
    CHECK(bridge.bindingCount() == 1U);
    pumpBlocks(*system);
    CHECK(system->stats().activeVoices == 1U);
}

TEST_CASE("SA13: changing `clip` mid-play stops the old voice and starts the new one") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid first = guidOf(1);
    const Guid second = guidOf(2);
    REQUIRE(system->registerClip(makeClip(first)).valid());
    REQUIRE(system->registerClip(makeClip(second)).valid());

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = first, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    REQUIRE(bridge.bindingCount() == 1U);
    pumpBlocks(*system);
    CHECK(system->stats().activeVoices == 1U);

    world.get<AudioSource>(e)->clip = second;
    bridge.update(world, *system);
    CHECK(bridge.bindingCount() == 1U);
    pumpBlocks(*system, 2);
    system->service();
    // A clip swap is a RESTART: exactly one voice, not two, and not zero.
    CHECK(system->stats().activeVoices == 1U);
}

TEST_CASE("SA14: an UNCHANGED frame pushes ZERO SetParams -- the coalescing, measured") {
    // The obvious form of this case is VACUOUS: asserting "no command was pushed" by reading a
    // counter that only counts DROPS proves nothing, because a pushed command increments no drop
    // counter. The assertion is over the DRAINED command stream instead -- specifically over the ring
    // occupancy, which the second update() must move by exactly ONE (the unconditional SetListener)
    // and never by two.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid)).valid());

    World world;
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    pumpBlocks(*system);  // drain the Start + the first SetListener

    // The ring is empty now. Flood it with droppable commands until the reserve refuses one, count how
    // many that took, then repeat after an unchanged update(): if the update pushed a SetParams as
    // well as the SetListener, the second flood fits ONE FEWER.
    auto floodCapacity = [&system]() {
        const std::uint64_t before = system->stats().droppedCommands;
        int pushed = 0;
        while (system->stats().droppedCommands == before) {
            system->setMasterVolume(1.0F);
            ++pushed;
        }
        return pushed;
    };

    const int baseline = floodCapacity();
    pumpBlocks(*system);  // drain

    bridge.update(world, *system);  // an UNCHANGED world
    const int afterUnchanged = floodCapacity();
    pumpBlocks(*system);

    // Exactly ONE command (the unconditional SetListener) went in, so exactly one fewer fits.
    CHECK(afterUnchanged == baseline - 1);
    CHECK(bridge.bindingCount() == 1U);
}

TEST_CASE("SA15: a MOVED source pushes EXACTLY ONE SetParams per frame") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid)).valid());

    World world;
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    pumpBlocks(*system);

    auto floodCapacity = [&system]() {
        const std::uint64_t before = system->stats().droppedCommands;
        int pushed = 0;
        while (system->stats().droppedCommands == before) {
            system->setMasterVolume(1.0F);
            ++pushed;
        }
        return pushed;
    };

    const int baseline = floodCapacity();
    pumpBlocks(*system);

    world.get<Transform>(e)->position = Vec3{1.0F, 0.0F, 0.0F};
    bridge.update(world, *system);
    const int afterMove = floodCapacity();
    pumpBlocks(*system);

    // TWO commands this time -- the SetListener and exactly one SetParams.
    CHECK(afterMove == baseline - 2);

    // A NON-POSITION FIELD MUST PUSH TOO, AND THESE ARMS HAD TO BE ADDED. Every earlier arm either
    // changed NOTHING or changed the POSITION, so narrowing the coalescing comparison to the position
    // alone -- which looks entirely correct -- left the whole suite GREEN, measured rather than
    // assumed. VoiceParams::operator== is defaulted precisely so the comparison is over the WHOLE
    // struct; these are the arms that hold it to that. A float field and a bool field, because a
    // field-by-field comparison is where a future appended field of either kind gets forgotten.
    world.get<AudioSource>(e)->volume = 0.25F;  // position UNTOUCHED
    bridge.update(world, *system);
    const int afterVolume = floodCapacity();
    pumpBlocks(*system);
    CHECK(afterVolume == baseline - 2);

    world.get<AudioSource>(e)->spatialize = false;  // position and volume UNTOUCHED
    bridge.update(world, *system);
    const int afterSpatialize = floodCapacity();
    pumpBlocks(*system);
    CHECK(afterSpatialize == baseline - 2);

    // And with nothing at all changed, back to one.
    bridge.update(world, *system);
    const int afterNothing = floodCapacity();
    pumpBlocks(*system);
    CHECK(afterNothing == baseline - 1);
}

TEST_CASE("SA16: a FINISHED non-looping voice is NOT restarted, across 100 further updates") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    constexpr std::uint32_t CLIP_FRAMES = 8;
    REQUIRE(system->registerClip(makeClip(guid, CLIP_FRAMES)).valid());  // a very short one-shot

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = false});

    SceneAudio bridge;
    bridge.update(world, *system);
    REQUIRE(bridge.bindingCount() == 1U);

    pumpBlocks(*system, 2);  // the 8-frame clip runs out
    system->service();
    CHECK(system->stats().activeVoices == 0U);

    // THE OBSERVATION WINDOW IS SHORTER THAN THE CLIP, AND IT HAD TO BE. The first form of this case
    // pumped a FULL 64-frame block after each update, so a restarted 8-frame voice finished again
    // before anything looked at it and `activeVoices == 0` held either way -- seeding the restart
    // left the whole suite GREEN, measured rather than assumed. A 4-frame block is half the clip, so
    // a restart is unmistakably still playing when the assertion runs.
    const std::uint64_t rejectedBefore = system->stats().rejectedPlays;
    std::array<float, 4> small{};
    static_assert(small.size() < CLIP_FRAMES, "the window must be shorter than the clip");
    for (int frame = 0; frame < 100; ++frame) {
        bridge.update(world, *system);
        system->render(small, 1, 48000);
        // D11: `playing` is authored state, and a one-shot that has run its course HAS. A restart
        // would leave a live voice sitting here at frame 4 of 8.
        CHECK(system->stats().activeVoices == 0U);
    }
    CHECK(system->stats().rejectedPlays == rejectedBefore);
    CHECK(bridge.bindingCount() == 1U);  // the binding survives, marked finished
}

TEST_CASE("SA17: destroying the entity stops its voice and erases the binding") {
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid)).valid());

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    REQUIRE(bridge.bindingCount() == 1U);
    pumpBlocks(*system);
    CHECK(system->stats().activeVoices == 1U);

    world.destroy(e);
    bridge.update(world, *system);
    CHECK(bridge.bindingCount() == 0U);
    pumpBlocks(*system, 2);
    system->service();
    CHECK(system->stats().activeVoices == 0U);
}

TEST_CASE("SA17b: a RECYCLED entity index stops the old voice rather than orphaning it") {
    // THE BLOCKING FINDING OF THIS TASK'S CODE-REVIEW ROUND, AS A TEST. Step 4 matched a binding by
    // FULL Entity equality (index AND generation) while step 5 swept presence by INDEX ALONE, so an
    // entity whose index was RECYCLED left its old binding looking "present": the sweep kept it, the
    // old voice was never stopped, and a looping one played forever with nothing able to name it --
    // every later lower_bound for that index returned the newer entry. Each churn added another
    // orphan until the 64-voice pool exhausted and rejectedPlays climbed with no cause visible.
    //
    // SA17 cannot see it: it destroys and updates in the SAME frame, so the index is never reused
    // ACROSS updates. This case is the one that reuses it.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid, /*frames=*/8192)).valid());

    World world;
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});

    const Entity first = world.create();
    world.add<Transform>(first, Transform{});
    world.add<AudioSource>(first, AudioSource{.clip = guid, .loop = true});

    SceneAudio bridge;
    bridge.update(world, *system);
    pumpBlocks(*system);
    REQUIRE(bridge.bindingCount() == 1U);
    REQUIRE(system->stats().activeVoices == 1U);

    // Destroy and recreate WITHOUT an update in between, so the index is recycled behind the bridge's
    // back. ASSERTED rather than assumed: if this World stopped recycling indices the case would pass
    // vacuously, and that must fail loudly instead.
    world.destroy(first);
    const Entity second = world.create();
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);
    world.add<Transform>(second, Transform{});
    world.add<AudioSource>(second, AudioSource{.clip = guid, .loop = true});

    bridge.update(world, *system);
    pumpBlocks(*system, 2);
    system->service();

    // ONE binding and ONE voice. The old one was stopped in the SAME update that replaced it.
    CHECK(bridge.bindingCount() == 1U);
    CHECK(system->stats().activeVoices == 1U);

    // And it stays that way under churn: without the fix each cycle strands one more looping voice
    // until the pool is gone.
    Entity current = second;
    for (int cycle = 0; cycle < 80; ++cycle) {
        world.destroy(current);
        current = world.create();
        world.add<Transform>(current, Transform{});
        world.add<AudioSource>(current, AudioSource{.clip = guid, .loop = true});
        bridge.update(world, *system);
        pumpBlocks(*system, 2);
        system->service();
        CHECK(bridge.bindingCount() == 1U);
        CHECK(system->stats().activeVoices == 1U);
    }
    CHECK(system->stats().rejectedPlays == 0U);
}

TEST_CASE("SA20: a SetParams dropped under back-pressure is re-pushed, never believed") {
    // FINDING 2 OF THIS TASK'S CODE-REVIEW ROUND, AS A TEST. D5 licenses dropping a SetParams with
    // "the voice keeps its previous parameters and THE NEXT FRAME CORRECTS IT" -- which holds only
    // while parameters keep changing. Edit one value ONCE, at a moment when the ring is under the
    // reserve, and the old code advanced lastPushed anyway: the bridge believed it landed and never
    // pushed again, so the voice kept the stale value FOR LIFE with only droppedCommands moving.
    //
    // Asserted END TO END through the rendered amplitude, not by inspecting a counter: the question
    // is whether the value reached the VOICE.
    const std::unique_ptr<AudioSystem> system = AudioSystem::create();
    REQUIRE(system != nullptr);
    const Guid guid = guidOf(1);
    REQUIRE(system->registerClip(makeClip(guid, /*frames=*/8192)).valid());

    World world;
    const Entity ear = world.create();
    world.add<Transform>(ear, Transform{});
    world.add<AudioListener>(ear, AudioListener{});
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<AudioSource>(e, AudioSource{.clip = guid, .loop = true, .spatialize = false});

    SceneAudio bridge;
    bridge.update(world, *system);

    constexpr float FULL = 16384.0F / 32768.0F;
    std::array<float, 16> block{};
    system->render(block, 1, 48000);
    system->render(block, 1, 48000);
    REQUIRE(block[0] == doctest::Approx(FULL).epsilon(1e-6));

    // Push the ring under its reserve so the next droppable command is refused.
    const std::uint64_t droppedBefore = system->stats().droppedCommands;
    for (int i = 0; i < 4096; ++i) {
        system->setMasterVolume(1.0F);
    }
    REQUIRE(system->stats().droppedCommands > droppedBefore);

    // ONE edit, and then the world is left alone forever after.
    world.get<AudioSource>(e)->volume = 0.25F;
    bridge.update(world, *system);  // this SetParams is DROPPED

    // Drain everything the flood left behind, then keep ticking with an UNCHANGED world.
    for (int i = 0; i < 8; ++i) {
        system->render(block, 1, 48000);
    }
    for (int frame = 0; frame < 4; ++frame) {
        bridge.update(world, *system);
        system->render(block, 1, 48000);
    }
    system->render(block, 1, 48000);

    // The edit reached the voice. Under the defect this reads FULL forever.
    CHECK(block[0] == doctest::Approx(FULL * 0.25F).epsilon(1e-6));
}

TEST_CASE("SA18: zero listeners and >1 listener each WARN EXACTLY ONCE across 100 updates") {
    // ASSERTED AS A SEQUENCE in one case each, because two independent cases both pass under the very
    // defect they exist to catch: a latch that never latches still fires once on the first frame.
    SUBCASE("zero listeners") {
        const LogCapture capture;
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);

        World world;
        SceneAudio bridge;
        for (int frame = 0; frame < 100; ++frame) {
            bridge.update(world, *system);
        }
        CHECK(bridge.lastListenerCount() == 0U);
        CHECK(capture.count() == 1);
    }

    SUBCASE("more than one listener") {
        const LogCapture capture;
        const std::unique_ptr<AudioSystem> system = AudioSystem::create();
        REQUIRE(system != nullptr);

        World world;
        for (int i = 0; i < 3; ++i) {
            const Entity e = world.create();
            world.add<Transform>(e, Transform{});
            world.add<AudioListener>(e, AudioListener{});
        }

        SceneAudio bridge;
        for (int frame = 0; frame < 100; ++frame) {
            bridge.update(world, *system);
        }
        CHECK(bridge.lastListenerCount() == 3U);
        CHECK(capture.count() == 1);
    }
}
