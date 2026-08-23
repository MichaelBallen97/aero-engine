// samples/phase-3-audio — task 3.7.2's deliverable: THE FIRST NOISE THIS ENGINE HAS EVER MADE.
//
// It is the first sample in the tree with NO WINDOW, NO rhi::Device, NO SHADER and no
// AERO_SHADER_TOOLS gate. It builds a real World with three entities — a listener, an orbiting source
// and a distant beacon — drives it through scene_audio::SceneAudio every frame, and either
// hands the mix to a real platform::AudioDevice or writes it to a file with --dump-pcm.
//
// THE TWO MODES DIFFER ONLY IN WHERE THE BUFFER GOES. Both call the same SceneAudio::update and the
// same AudioSystem::render, which is D18's payoff: the bytes a validation row measures come out of
// EXACTLY the code path a speaker hears, never a parallel offline renderer that could drift.
//
// THE TABLE IT PRINTS AT STARTUP IS COMPUTED FROM audio::distanceGain AND audio::panGains
// THEMSELVES, never from a number this comment predicts — the 3.6.3 rule: a validation pass compares
// a screen reading against a number THAT BUILD produced. Task 3.6.3's own plan was wrong in eight of
// eleven predicted rows, which is why this is a rule rather than a preference.

#include <aero/audio/audio.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/audio.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene_audio/scene_audio.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// File-scope aliases: they keep the printf-heavy blocks below comfortably under the column limit,
// which matters because Homebrew's and Ubuntu's clang-format-18 disagree on chains near it.
namespace audio = engine::audio;
namespace scene_audio = engine::scene_audio;

namespace {

constexpr std::uint32_t BLOCK_FRAMES = 512;
constexpr float ORBIT_RADIUS = 3.0F;
constexpr float BEACON_DISTANCE = 10.8F;  // 90 % of maxDistance: audible but faint
constexpr float SOURCE_MIN_DISTANCE = 1.0F;
constexpr float SOURCE_MAX_DISTANCE = 12.0F;

struct Options {
    float seconds = 8.0F;
    float period = 4.0F;   // one full orbit, in seconds
    float pitch = 1.0F;    // applied to the ORBITING source only
    std::string dumpPath;  // empty == device mode
    bool spatialize = true;
    bool loop = true;
};

[[nodiscard]] bool parseFloat(const char* text, float& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || end == nullptr || *end != '\0') {
        return false;
    }
    // FINITENESS IS PART OF PARSING HERE, not a later guard. strtod accepts "inf" and leaves *end at
    // '\0', so the caller's `seconds > 0.0F` test passes for infinity -- and the dump loop then does
    // static_cast<std::uint64_t>(inf * 48000.0F), an out-of-range float-to-integer conversion, which
    // is UNDEFINED BEHAVIOUR and exactly what UBSan traps. It is invisible in CI because CI builds
    // this sample and never runs it. ("nan" is already refused by `> 0.0F`; "inf" is not.)
    if (!std::isfinite(value)) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

[[nodiscard]] bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const bool hasValue = (i + 1) < argc;
        if (arg == "--seconds" && hasValue) {
            if (!parseFloat(argv[++i], options.seconds)) {
                std::printf("--seconds needs a FINITE number, got: %s\n", argv[i]);
                return false;
            }
        } else if (arg == "--period" && hasValue) {
            if (!parseFloat(argv[++i], options.period)) {
                std::printf("--period needs a FINITE number, got: %s\n", argv[i]);
                return false;
            }
        } else if (arg == "--pitch" && hasValue) {
            if (!parseFloat(argv[++i], options.pitch)) {
                std::printf("--pitch needs a FINITE number, got: %s\n", argv[i]);
                return false;
            }
        } else if (arg == "--dump-pcm" && hasValue) {
            options.dumpPath = argv[++i];
        } else if (arg == "--no-spatialize") {
            options.spatialize = false;
        } else if (arg == "--no-loop") {
            options.loop = false;
        } else {
            std::printf("unknown or incomplete argument: %s\n", argv[i]);
            std::printf(
                "usage: aero_sample_phase3_audio [--seconds N] [--period N] [--pitch F]\n"
                "                                [--dump-pcm <path>] [--no-spatialize] [--no-loop]\n");
            return false;
        }
    }
    if (options.seconds <= 0.0F || options.period <= 0.0F) {
        std::printf("--seconds and --period must both be positive\n");
        return false;
    }
    return true;
}

// Loads one committed .aerowave through a real DirectoryBackend and hands it to the system.
[[nodiscard]] audio::ClipHandle loadClip(engine::VirtualFileSystem& vfs, audio::AudioSystem& system,
                                         std::string_view name) {
    audio::AudioClipLoadResult loaded = audio::loadAudioClip(vfs, name);
    if (loaded.status != audio::AudioClipLoadStatus::Ok) {
        AERO_LOG_ERROR("phase-3-audio: {} failed to load: {} ({})", name,
                       audio::audioClipLoadStatusLabel(loaded.status), loaded.message);
        return {};
    }
    const auto duration = static_cast<double>(loaded.clip.durationSeconds());
    const std::string label{name};
    std::printf("  %-18s %u Hz, %u ch, %u frames, %.3f s\n", label.c_str(), loaded.clip.sampleRate(),
                loaded.clip.channels(), loaded.clip.frameCount(), duration);
    return system.registerClip(std::move(loaded.clip));
}

// The expected-value table, computed from the real functions rather than predicted.
void printExpectedTable(const Options& options, std::uint32_t deviceRate, std::uint32_t deviceChannels) {
    std::printf("\nexpected values, COMPUTED AT STARTUP from audio::distanceGain and audio::panGains:\n");
    std::printf("  device                 %u Hz, %u ch, block %u frames\n", deviceRate, deviceChannels,
                static_cast<unsigned>(BLOCK_FRAMES));
    std::printf("  orbit radius           %.3f (minDistance %.3f, maxDistance %.3f)\n",
                static_cast<double>(ORBIT_RADIUS), static_cast<double>(SOURCE_MIN_DISTANCE),
                static_cast<double>(SOURCE_MAX_DISTANCE));
    const auto beaconGain =
        static_cast<double>(audio::distanceGain(BEACON_DISTANCE, SOURCE_MIN_DISTANCE, SOURCE_MAX_DISTANCE));
    const auto orbitGain =
        static_cast<double>(audio::distanceGain(ORBIT_RADIUS, SOURCE_MIN_DISTANCE, SOURCE_MAX_DISTANCE));
    std::printf("  beacon distance        %.3f  -> distanceGain %.6f\n", static_cast<double>(BEACON_DISTANCE),
                beaconGain);
    const auto orbitAt = static_cast<double>(ORBIT_RADIUS);
    std::printf("  orbit distance         %.3f  -> distanceGain %.6f\n", orbitAt, orbitGain);

    for (const float x : {-1.0F, 0.0F, 1.0F}) {
        const audio::ChannelGains gains = audio::panGains(x, deviceChannels);
        std::printf("  panGains(%+.1f)          L %.6f  R %.6f\n", static_cast<double>(x),
                    static_cast<double>(gains.gain[0]), static_cast<double>(gains.gain[1]));
    }
    std::printf("  pitch                  %.3f%s\n", static_cast<double>(options.pitch),
                options.pitch == 1.0F ? "" : "  (orbiting source only)");
    std::printf("  spatialize             %s\n", options.spatialize ? "on" : "OFF (--no-spatialize)");
    std::printf("  loop                   %s\n", options.loop ? "on" : "OFF (--no-loop)");
    if (!options.dumpPath.empty()) {
        const auto totalFrames = static_cast<std::uint64_t>(options.seconds * static_cast<float>(deviceRate));
        // The product is NAMED rather than cast inline, and the reason is a lane divergence rather
        // than taste: std::uint64_t is `unsigned long` on Linux and `unsigned long long` on macOS, so
        // casting the ARITHMETIC EXPRESSION straight into printf's %llu is a no-op here and a widening
        // cast there -- which bugprone-misplaced-widening-cast rejects on the Linux lane alone. Cast a
        // variable, never an expression, whenever the target type is spelled for a printf format.
        const std::uint64_t dumpBytes = totalFrames * deviceChannels * 4U;
        std::printf("  dump size              %llu bytes = %.1f s x %u Hz x %u ch x 4\n",
                    static_cast<unsigned long long>(dumpBytes), static_cast<double>(options.seconds), deviceRate,
                    deviceChannels);
    }
    std::printf("\n");
}

// Places the orbiting source on its circle at time t. Called once per frame in both modes; the DUMP
// mode advances t by a FIXED dt, which is what makes the dump byte-reproducible.
void placeOrbit(engine::World& world, engine::Entity orbit, float t, float period) {
    const float angle = engine::TWO_PI * (t / period);
    auto* transform = world.get<engine::Transform>(orbit);
    if (transform != nullptr) {
        const float x = ORBIT_RADIUS * std::sin(angle);
        const float z = -ORBIT_RADIUS * std::cos(angle);
        transform->position = engine::Vec3{x, 0.0F, z};
    }
}

[[nodiscard]] int runSample(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }

    std::printf("aero phase-3-audio — an orbiting source, a distant beacon, and a real mixer\n\n");
    std::printf("clips:\n");

    // ---- the system, FIRST. The device is declared after it, and that is the whole lifetime rule.
    const std::unique_ptr<audio::AudioSystem> system = audio::AudioSystem::create();
    if (system == nullptr) {
        AERO_LOG_ERROR("phase-3-audio: AudioSystem::create failed");
        return 1;
    }

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_PHASE3_AUDIO_DIR));
    const audio::ClipHandle orbitClip = loadClip(vfs, *system, "res://orbit.aerowave");
    const audio::ClipHandle beaconClip = loadClip(vfs, *system, "res://beacon.aerowave");
    if (!orbitClip.valid() || !beaconClip.valid()) {
        return 1;
    }

    // ---- the world -------------------------------------------------------------------------------
    engine::World world;

    const engine::Entity listener = world.create();
    world.setName(listener, "listener");
    world.add<engine::Transform>(listener, engine::Transform{});
    world.add<engine::AudioListener>(listener, engine::AudioListener{.volume = 1.0F});

    engine::AudioSource orbitSource;
    orbitSource.clip = engine::Guid{0x3720000000000000ULL, 0x0000000000000001ULL};
    orbitSource.pitch = options.pitch;
    orbitSource.minDistance = SOURCE_MIN_DISTANCE;
    orbitSource.maxDistance = SOURCE_MAX_DISTANCE;
    orbitSource.loop = options.loop;
    orbitSource.spatialize = options.spatialize;

    const engine::Entity orbit = world.create();
    world.setName(orbit, "orbit");
    world.add<engine::Transform>(orbit, engine::Transform{});
    world.add<engine::AudioSource>(orbit, orbitSource);

    engine::AudioSource beaconSource = orbitSource;
    beaconSource.clip = engine::Guid{0x3720000000000000ULL, 0x0000000000000002ULL};
    beaconSource.pitch = 1.0F;  // --pitch applies to the ORBITING source only

    const engine::Entity beacon = world.create();
    world.setName(beacon, "beacon");
    const engine::Vec3 beaconAt{0.0F, 0.0F, -BEACON_DISTANCE};
    world.add<engine::Transform>(beacon, engine::Transform{.position = beaconAt});
    world.add<engine::AudioSource>(beacon, beaconSource);

    scene_audio::SceneAudio sceneAudio;

    // ---- dump mode: NO DEVICE AND NO THREAD ------------------------------------------------------
    if (!options.dumpPath.empty()) {
        constexpr std::uint32_t DUMP_RATE = 48000;
        constexpr std::uint32_t DUMP_CHANNELS = 2;
        printExpectedTable(options, DUMP_RATE, DUMP_CHANNELS);

        std::FILE* out = std::fopen(options.dumpPath.c_str(), "wb");
        if (out == nullptr) {
            AERO_LOG_ERROR("phase-3-audio: cannot open {} for writing", options.dumpPath);
            return 1;
        }

        const auto totalFrames = static_cast<std::uint64_t>(options.seconds * static_cast<float>(DUMP_RATE));
        // FIXED dt, NEVER wall clock. That is what makes the dump byte-reproducible, and
        // reproducibility is a validation row rather than a hope.
        const float dt = static_cast<float>(BLOCK_FRAMES) / static_cast<float>(DUMP_RATE);
        std::vector<float> block(static_cast<std::size_t>(BLOCK_FRAMES) * DUMP_CHANNELS);

        std::uint64_t written = 0;
        float t = 0.0F;
        while (written < totalFrames) {
            placeOrbit(world, orbit, t, options.period);
            sceneAudio.update(world, *system);
            system->render(block, DUMP_CHANNELS, DUMP_RATE);

            const std::uint64_t remaining = totalFrames - written;
            const auto frames = static_cast<std::size_t>(remaining < BLOCK_FRAMES ? remaining : BLOCK_FRAMES);
            std::fwrite(block.data(), sizeof(float), frames * DUMP_CHANNELS, out);
            written += frames;
            t += dt;
        }
        std::fclose(out);

        const audio::AudioStats stats = system->stats();
        const auto frameTotal = static_cast<unsigned long long>(written);
        std::printf("wrote %llu frames to %s\n", frameTotal, options.dumpPath.c_str());
        std::printf(
            "stats: activeVoices %u, peakVoices %u, clips %u, dropped %llu, rejected %llu, "
            "callbacks %llu\n",
            stats.activeVoices, stats.peakVoices, stats.clipCount,
            static_cast<unsigned long long>(stats.droppedCommands),
            static_cast<unsigned long long>(stats.rejectedPlays),
            static_cast<unsigned long long>(stats.callbacksCompleted));
        std::printf("bindings %zu, unresolved clips %u, listeners %u\n", sceneAudio.bindingCount(),
                    sceneAudio.lastUnresolvedClips(), sceneAudio.lastListenerCount());
        return 0;
    }

    // ---- device mode -----------------------------------------------------------------------------
    // THE DEVICE IS DECLARED AFTER THE SYSTEM, AND THAT IS THE WHOLE LIFETIME RULE: ma_device_uninit
    // stops the stream and JOINS the audio thread before returning, so reverse-order destruction tears
    // the device down first, EVERY TIME, WITH NO FLAG AND NO HANDSHAKE.
    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(
        {.render = &audio::AudioSystem::renderCallback, .renderUser = system.get()});
    if (!device.has_value()) {
        AERO_LOG_ERROR("phase-3-audio: could not open an audio device");
        return 1;
    }

    printExpectedTable(options, device->sampleRate(), device->channels());
    std::printf("playing for %.1f s — one orbit every %.1f s\n", static_cast<double>(options.seconds),
                static_cast<double>(options.period));

    const auto start = std::chrono::steady_clock::now();
    float elapsed = 0.0F;
    while (elapsed < options.seconds) {
        placeOrbit(world, orbit, elapsed, options.period);
        sceneAudio.update(world, *system);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    }

    const audio::AudioStats stats = system->stats();
    const auto dropped = static_cast<unsigned long long>(stats.droppedCommands);
    const auto rejected = static_cast<unsigned long long>(stats.rejectedPlays);
    const auto callbacks = static_cast<unsigned long long>(stats.callbacksCompleted);
    std::printf(
        "stats: activeVoices %u, peakVoices %u, clips %u, dropped %llu, rejected %llu, "
        "callbacks %llu\n",
        stats.activeVoices, stats.peakVoices, stats.clipCount, dropped, rejected, callbacks);
    std::printf("bindings %zu, unresolved clips %u, listeners %u\n", sceneAudio.bindingCount(),
                sceneAudio.lastUnresolvedClips(), sceneAudio.lastListenerCount());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // The phase-3-tonemap / phase-3-culling shape: main itself cannot throw, so
    // bugprone-exception-escape has nothing to report and an unexpected exception becomes an exit
    // code rather than a terminate.
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-audio: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-audio: unexpected exception");
        return 1;
    }
}
