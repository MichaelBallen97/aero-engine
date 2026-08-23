// Aero Engine — AudioDevice: the miniaudio playback-device wrapper (task 0.3.3). The ONE .cpp that uses
// miniaudio's API (declarations only; the implementation is in miniaudio_impl.c). No ma_ type escapes
// into <aero/platform/audio.hpp> — the boundary rule (docs/04), guarded by check-platform-boundary.sh
// and tests/platform_boundary_probe.cpp.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/platform/audio.hpp>

#include <cstddef>
#include <memory>
#include <miniaudio.h>  // one of the ONLY two miniaudio includes in engine/ (the other: miniaudio_impl.c)
#include <optional>
#include <span>
#include <utility>

namespace engine::platform {

// Owns the miniaudio context + device. Heap-allocated (pimpl) so its address is stable across
// AudioDevice moves — ma_device is self-referential and read by the audio thread (D9). The destructor
// tears down in the right order, guarded by flags so a partially-constructed Impl (context inited but
// device not) never uninits a struct that was never inited.
//
// Task 3.7.2 adds the realtime seam: the two AudioRenderFn fields are copied out of the config in
// open() BEFORE ma_device_init, so they are written before the device thread exists and read only on
// that thread. No synchronisation is needed and none may be added.
struct AudioDevice::Impl {
    ma_context context{};
    ma_device device{};
    bool contextInited = false;
    bool deviceInited = false;
    AudioRenderFn render = nullptr;
    void* renderUser = nullptr;

    // The realtime data callback. Runs on miniaudio's audio thread: NO allocation, NO lock, NO
    // logging, NO exceptions. With no AudioRenderFn installed it writes metallic silence into the
    // whole output buffer every call, exactly as 0.3.3 did; with one installed it forwards the buffer
    // as an engine-typed span and the callee owns every element. (pInput is unused: playback-only.)
    //
    // It is a static member of Impl rather than an anonymous-namespace free function for one reason:
    // it reads AudioDevice::Impl, which is PRIVATE, so a non-member cannot name the type at all.
    // (pInput is unnamed in the definition: this is playback-only and it is never read.)
    static void dataCallback(ma_device* device, void* output, const void* pInput, ma_uint32 frameCount);

    ~Impl() {
        if (deviceInited) {
            ma_device_uninit(&device);  // stops the stream and joins the audio thread first
        }
        if (contextInited) {
            ma_context_uninit(&context);
        }
    }
};

// TASK 3.7.2 AMENDS 0.3.3's D10, per the 0.4.1 D18 amendment protocol.
//
// D10's original sentence was: "Format/channels are read from the ma_device it is handed, so there is
// no back-pointer to the AudioDevice — a move of the owning object can never dangle this callback."
// There IS a back-pointer now, and it points at the HEAP-PINNED Impl, never at the AudioDevice. A
// move of an AudioDevice transfers the unique_ptr; the Impl's ADDRESS IS UNCHANGED; the callback
// cannot dangle. ~Impl runs ma_device_uninit in its BODY, before any member is destroyed, so the
// callback cannot observe a half-destroyed Impl either.
//
// VERIFIED against the pinned miniaudio 0.11.25 header as read on this machine
// (build/<preset>/vcpkg_installed/<triplet>/include/miniaudio.h; MA_VERSION_{MAJOR,MINOR,REVISION}
// at :3748-3750):
//   * ma_device_uninit stops the stream and JOINS the audio thread before returning -- doc block at
//     :9009 ("This will explicitly stop the device"), definition at :44104, and the join itself at
//     :44131-44134: "Wake up the worker thread and wait for it to properly terminate" followed by
//     ma_event_signal(&pDevice->wakeupEvent) + ma_thread_wait(&pDevice->thread).
//   * the data callback receives the ma_device* (ma_device_data_proc at :6894), from which
//     pUserData (struct ma_device at :7781, the member at :7790) is reachable.
// D9's heap-pinning is what makes this sound, and D9 is one line above D10 in the original text --
// the two were always a pair.
void AudioDevice::Impl::dataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    AERO_PROFILE_ZONE_NAMED("audio.callback");
    // Name the audio thread once so it is legible in a Tracy capture (dev builds only; cheap one-shot).
    static thread_local bool named = false;
    if (!named) {
        AERO_PROFILE_SET_THREAD_NAME("Audio");
        named = true;
    }

    auto* impl = static_cast<Impl*>(device->pUserData);
    // The null-impl arm is not defensive theatre: it is what makes a pUserData that never got set a
    // SILENT failure rather than a crash on a realtime thread.
    if (impl == nullptr || impl->render == nullptr || output == nullptr) {
        ma_silence_pcm_frames(output, frameCount, device->playback.format, device->playback.channels);
        return;
    }

    // The float* cast is sound because open() sets deviceConfig.playback.format = ma_format_f32 two
    // lines above ma_device_init -- a REQUESTED value miniaudio converts TO, never a negotiated one,
    // so the callback's buffer is always f32.
    //
    // The sample count is computed in std::size_t: frameCount and channels are both ma_uint32 and
    // their product at 8 channels x a large period is comfortably inside 32 bits, but the widening is
    // free and the alternative is a class of bug nobody wants in this file.
    const ma_uint32 channels = device->playback.channels;
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(channels);
    impl->render(impl->renderUser, std::span<float>{static_cast<float*>(output), sampleCount}, channels,
                 device->sampleRate);
}

std::optional<AudioDevice> AudioDevice::open(const AudioDeviceConfig& config) {
    if (config.sampleRate == 0 || config.channels == 0) {
        AERO_LOG_ERROR("AudioDevice::open: invalid config ({} Hz, {} ch)", config.sampleRate, config.channels);
        return std::nullopt;
    }

    auto impl = std::make_unique<Impl>();
    // Copied out of the config BEFORE ma_device_init, i.e. before the device thread exists (3.7.2).
    impl->render = config.render;
    impl->renderUser = config.renderUser;

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
    deviceConfig.dataCallback = &Impl::dataCallback;
    // The back-pointer is the HEAP-PINNED Impl, never `this` — see the D10 amendment above (3.7.2).
    deviceConfig.pUserData = impl.get();

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

    AERO_LOG_INFO("audio device opened: {} Hz, {} ch, {} backend ({})", impl->device.sampleRate,
                  impl->device.playback.channels, config.headless ? "null" : "native",
                  impl->render != nullptr ? "rendering" : "silent");
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
