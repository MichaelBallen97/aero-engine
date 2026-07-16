// Aero Engine — AudioDevice: the miniaudio playback-device wrapper (task 0.3.3). The ONE .cpp that uses
// miniaudio's API (declarations only; the implementation is in miniaudio_impl.c). No ma_ type escapes
// into <aero/platform/audio.hpp> — the boundary rule (docs/04), guarded by check-platform-boundary.sh
// and tests/platform_boundary_probe.cpp.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/platform/audio.hpp>

#include <memory>
#include <miniaudio.h>  // one of the ONLY two miniaudio includes in engine/ (the other: miniaudio_impl.c)
#include <optional>
#include <utility>

namespace engine::platform {
namespace {

// The realtime data callback. Runs on miniaudio's audio thread: NO allocation, NO lock, NO logging, NO
// exceptions. It writes metallic silence into the whole output buffer every call. Format/channels are
// read from the ma_device it is handed, so there is no back-pointer to the AudioDevice — a move of the
// owning object can never dangle this callback (D10). (pInput is unused: this is playback-only.)
void silenceCallback(ma_device* device, void* output, const void* /*pInput*/, ma_uint32 frameCount) {
    AERO_PROFILE_ZONE_NAMED("audio.silence");
    // Name the audio thread once so it is legible in a Tracy capture (dev builds only; cheap one-shot).
    static thread_local bool named = false;
    if (!named) {
        AERO_PROFILE_SET_THREAD_NAME("Audio");
        named = true;
    }
    ma_silence_pcm_frames(output, frameCount, device->playback.format, device->playback.channels);
}

}  // namespace

// Owns the miniaudio context + device. Heap-allocated (pimpl) so its address is stable across
// AudioDevice moves — ma_device is self-referential and read by the audio thread (D9). The destructor
// tears down in the right order, guarded by flags so a partially-constructed Impl (context inited but
// device not) never uninits a struct that was never inited.
struct AudioDevice::Impl {
    ma_context context{};
    ma_device device{};
    bool contextInited = false;
    bool deviceInited = false;

    ~Impl() {
        if (deviceInited) {
            ma_device_uninit(&device);  // stops the stream and joins the audio thread first
        }
        if (contextInited) {
            ma_context_uninit(&context);
        }
    }
};

std::optional<AudioDevice> AudioDevice::open(const AudioDeviceConfig& config) {
    if (config.sampleRate == 0 || config.channels == 0) {
        AERO_LOG_ERROR("AudioDevice::open: invalid config ({} Hz, {} ch)", config.sampleRate, config.channels);
        return std::nullopt;
    }

    auto impl = std::make_unique<Impl>();

    const ma_context_config contextConfig = ma_context_config_init();
    ma_result result = MA_SUCCESS;
    if (config.headless) {
        const ma_backend backend = ma_backend_null;  // no hardware — the CI/test path (D7)
        result = ma_context_init(&backend, 1, &contextConfig, &impl->context);
    } else {
        result = ma_context_init(nullptr, 0, &contextConfig, &impl->context);  // default backends
    }
    if (result != MA_SUCCESS) {
        AERO_LOG_ERROR("AudioDevice::open: ma_context_init failed ({})", static_cast<int>(result));
        return std::nullopt;  // ~Impl uninits nothing (both flags false)
    }
    impl->contextInited = true;

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = config.channels;
    deviceConfig.sampleRate = config.sampleRate;
    deviceConfig.dataCallback = &silenceCallback;
    deviceConfig.pUserData = nullptr;  // callback reads format from the ma_device it is handed (D10)

    result = ma_device_init(&impl->context, &deviceConfig, &impl->device);
    if (result != MA_SUCCESS) {
        AERO_LOG_ERROR("AudioDevice::open: ma_device_init failed ({})", static_cast<int>(result));
        return std::nullopt;  // ~Impl uninits the context
    }
    impl->deviceInited = true;

    result = ma_device_start(&impl->device);
    if (result != MA_SUCCESS) {
        AERO_LOG_ERROR("AudioDevice::open: ma_device_start failed ({})", static_cast<int>(result));
        return std::nullopt;  // ~Impl uninits device + context
    }

    AERO_LOG_INFO("audio device opened: {} Hz, {} ch, {} backend (silent)", impl->device.sampleRate,
                  impl->device.playback.channels, config.headless ? "null" : "native");
    return AudioDevice{std::move(impl)};
}

// Special members: defined here where Impl is complete. Defaulted move/dtor do the right thing — the
// unique_ptr transfers ownership and ~Impl performs the miniaudio teardown; a moved-from AudioDevice
// holds a null impl and its ~Impl never runs (AC-7).
AudioDevice::AudioDevice(std::unique_ptr<Impl> impl) noexcept : impl(std::move(impl)) {}
AudioDevice::AudioDevice(AudioDevice&&) noexcept = default;
AudioDevice& AudioDevice::operator=(AudioDevice&&) noexcept = default;
AudioDevice::~AudioDevice() = default;

bool AudioDevice::start() {
    if (!impl) {
        return false;
    }
    if (ma_device_is_started(&impl->device)) {
        return true;
    }
    return ma_device_start(&impl->device) == MA_SUCCESS;
}

void AudioDevice::stop() {
    if (!impl) {
        return;
    }
    if (ma_device_is_started(&impl->device)) {
        ma_device_stop(&impl->device);
    }
}

bool AudioDevice::isRunning() const {
    return impl && ma_device_is_started(&impl->device);  // ma_bool32 -> bool via &&
}

std::uint32_t AudioDevice::sampleRate() const { return impl ? impl->device.sampleRate : 0U; }

std::uint32_t AudioDevice::channels() const { return impl ? impl->device.playback.channels : 0U; }

}  // namespace engine::platform
