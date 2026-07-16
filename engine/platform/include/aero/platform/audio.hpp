#pragma once
// Aero Engine — silent audio-device stub (task 0.3.3; ADR-006). A move-only RAII handle to one
// miniaudio playback device that outputs SILENCE. The miniaudio device is hidden behind a pimpl (Impl
// is defined in src/audio_device.cpp), so no ma_ type reaches this header — the boundary rule, enforced
// by check-platform-boundary.sh and tests/platform_boundary_probe.cpp.
//
// This is a STUB: it proves the device path (open -> realtime thread -> silence -> close) on all 3 OSes.
// Clips, decoding, mixing, sources/listeners and spatialization are engine::audio, Phase 3.7 (3.7.1
// depends on this task) and Phase 6.4 — built ON TOP of this device primitive (ADR-006 layering).
//
// AudioDevice is INDEPENDENT of Context: miniaudio needs no SDL_Init, so a device can be opened with no
// window (that is exactly what the null-backend unit tests do).

#include <cstdint>
#include <memory>
#include <optional>

namespace engine::platform {

// What to open. Defaults request a conventional stereo/48 kHz float stream; miniaudio inserts a
// converter if the hardware's native format differs, so the callback (and sampleRate()/channels()) sees
// exactly these unless the device cannot satisfy them.
struct AudioDeviceConfig {
    std::uint32_t sampleRate = 48000;  // Hz; 0 is rejected (open() returns nullopt)
    std::uint32_t channels = 2;        // 0 is rejected
    bool headless = false;             // true -> miniaudio null backend: no hardware, for CI/tests
};

class AudioDevice {
public:
    // Opens AND starts a silent playback device. Returns nullopt on any failure; never throws
    // (docs/04: no exceptions across a public API boundary). On success the device is already running
    // and writing silence.
    [[nodiscard]] static std::optional<AudioDevice> open(const AudioDeviceConfig& config = {});

    // Move-only RAII; all special members are defined in src/audio_device.cpp (pimpl needs a complete
    // Impl to move/destroy). ~AudioDevice stops the realtime thread and releases the device+context.
    AudioDevice(AudioDevice&&) noexcept;
    AudioDevice& operator=(AudioDevice&&) noexcept;
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Start/stop the silent stream. Idempotent: start() on a running device and stop() on a stopped one
    // are no-ops. start() returns true if the device is running afterward.
    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    // Negotiated, callback-facing values (what the stream actually runs at). 0 only on a moved-from
    // instance.
    [[nodiscard]] std::uint32_t sampleRate() const;
    [[nodiscard]] std::uint32_t channels() const;

private:
    struct Impl;  // holds ma_context + ma_device; defined in src/audio_device.cpp
    explicit AudioDevice(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl;  // heap-pinned: ma_device is self-referential and read by the audio
                                 // thread, so it must not relocate on a move (D9).
};

}  // namespace engine::platform
