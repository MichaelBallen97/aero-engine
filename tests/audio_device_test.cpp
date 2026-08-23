// Aero Engine — AudioDevice lifecycle tests (task 0.3.3). Black-box: engine::platform::AudioDevice only,
// no ma_ type here. Always headless (miniaudio null backend) so CI needs no audio device. Real audible
// output is 3.7's concern; here we prove open/format/start/stop/running/move/RAII-close.

#include <aero/platform/audio.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <thread>
#include <utility>

namespace {
constexpr engine::platform::AudioDeviceConfig HEADLESS{.sampleRate = 48000, .channels = 2, .headless = true};
}

TEST_CASE("AudioDevice opens a running, silent null device") {
    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(HEADLESS);
    REQUIRE(device.has_value());  // AC-3: null backend opens with no hardware
    CHECK(device->isRunning());   // AC-2/D3: open() returns a STARTED device
    // AC-6: negotiated, callback-facing values are valid. Assert only the contract ("> 0"); the null
    // backend is expected to honor 48000/2, but don't over-specify a backend detail unverified offline.
    CHECK(device->sampleRate() > 0U);
    CHECK(device->channels() > 0U);
}

TEST_CASE("start/stop are idempotent") {
    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(HEADLESS);
    REQUIRE(device.has_value());

    device->stop();
    CHECK_FALSE(device->isRunning());
    device->stop();  // AC-5: double-stop is a safe no-op
    CHECK_FALSE(device->isRunning());

    CHECK(device->start());
    CHECK(device->isRunning());
    CHECK(device->start());  // AC-5: double-start is a safe no-op, still running
    CHECK(device->isRunning());
}

TEST_CASE("rejects an invalid config") {
    CHECK_FALSE(engine::platform::AudioDevice::open({.sampleRate = 0, .channels = 2, .headless = true})
                    .has_value());  // AC-2: 0 Hz -> nullopt, no throw
    CHECK_FALSE(
        engine::platform::AudioDevice::open({.sampleRate = 48000, .channels = 0, .headless = true}).has_value());
}

TEST_CASE("move leaves the source inert and the destination live") {
    std::optional<engine::platform::AudioDevice> source = engine::platform::AudioDevice::open(HEADLESS);
    REQUIRE(source.has_value());

    const engine::platform::AudioDevice moved = std::move(*source);
    CHECK(moved.isRunning());  // AC-7: destination owns the live device
    CHECK(moved.sampleRate() > 0U);
    CHECK(source->channels() == 0U);  // AC-7: moved-from is inert (no double-uninit at scope end)
}

// AC-7 (RAII close, no leak) is proven by every TEST_CASE above running under ASan in the Debug lane:
// each device is uninited at scope exit; a leaked ma_device or un-joined audio thread would trip ASan.

// ============================================================================================
// Task 3.7.2 — the AudioRenderFn seam. DV1-DV4, APPENDED; the four 0.3.3 cases above are left
// BYTE-IDENTICAL, which is AC-7's real content: `render == nullptr` reproduces the silent device.
//
// DV3 is the only non-tier-0 case this task ships, and the only genuinely threaded test in the tree.
// miniaudio's null backend runs a REAL device thread (ma_device_thread__null, created by
// ma_thread_create, with ma_device_read__null/ma_device_write__null wired), so this is an end-to-end
// proof rather than a mock. It polls with a generous bound and asserts NOTHING about timing.
// ============================================================================================

namespace {

// Recorded by the realtime callback. Every member is atomic because the audio thread writes them and
// the test thread reads them; relaxed throughout except the `ran` release/acquire pair, which is what
// publishes the other three.
struct CallbackProbe {
    std::atomic<std::uint32_t> channels{0};
    std::atomic<std::uint32_t> sampleRate{0};
    std::atomic<std::size_t> samples{0};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<bool> ran{false};
    std::atomic<bool> allWritten{true};
};

// The realtime callback: writes a known constant into every element and records what it was handed.
// NO allocation, NO lock, NO logging, NO exception — the AudioRenderFn contract.
void probeRender(void* user, std::span<float> output, std::uint32_t channels, std::uint32_t sampleRate) noexcept {
    auto* probe = static_cast<CallbackProbe*>(user);
    for (float& sample : output) {
        sample = 0.25F;
    }
    if (probe != nullptr) {
        probe->channels.store(channels, std::memory_order_relaxed);
        probe->sampleRate.store(sampleRate, std::memory_order_relaxed);
        probe->samples.store(output.size(), std::memory_order_relaxed);
        probe->calls.fetch_add(1, std::memory_order_relaxed);
        if (channels != 0 && output.size() % channels != 0) {
            probe->allWritten.store(false, std::memory_order_relaxed);
        }
        probe->ran.store(true, std::memory_order_release);
    }
}

// Polls for `ran` with a generous bound. Returns the number of milliseconds waited, or the bound if
// it never fired. NO assertion on timing lives in any caller — only on whether it ran at all.
int waitForCallback(const CallbackProbe& probe, int maxMilliseconds) {
    for (int waited = 0; waited < maxMilliseconds; waited += 5) {
        if (probe.ran.load(std::memory_order_acquire)) {
            return waited;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return maxMilliseconds;
}

}  // namespace

TEST_CASE("DV1: a null render callback still yields the 0.3.3 silent device") {
    // WHAT THIS CASE CAN AND CANNOT SEE, STATED SO IT IS NOT MISREAD AS MORE. It asserts that a
    // config with no callback still OPENS and RUNS -- the four 0.3.3 cases above are left
    // byte-identical, which is the other half of AC-7. It does NOT and CANNOT assert that the device
    // writes SILENCE on that path: the buffer belongs to miniaudio and no first-party code ever sees
    // it when `render` is null, by construction. Seeding the silence branch away (so the callback
    // returns leaving the buffer untouched) leaves this case and the whole suite GREEN -- measured,
    // not assumed. That property is AUDIBLE ONLY, and it is recorded as a declared seed rather than
    // certified here. The I96 rule: a case must not appear to cover an invariant it is blind to.
    const engine::platform::AudioDeviceConfig config = HEADLESS;
    CHECK(config.render == nullptr);
    CHECK(config.renderUser == nullptr);

    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(config);
    REQUIRE(device.has_value());
    CHECK(device->isRunning());
    CHECK(device->sampleRate() > 0U);
    CHECK(device->channels() > 0U);
}

TEST_CASE("DV2: a config WITH a callback opens and reports the same negotiated format") {
    CallbackProbe probe;
    engine::platform::AudioDeviceConfig config = HEADLESS;
    config.render = &probeRender;
    config.renderUser = &probe;

    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(config);
    REQUIRE(device.has_value());
    CHECK(device->isRunning());
    CHECK(device->sampleRate() > 0U);
    CHECK(device->channels() > 0U);
}

TEST_CASE("DV3: the realtime callback runs on the device thread and is handed the negotiated format") {
    CallbackProbe probe;
    engine::platform::AudioDeviceConfig config = HEADLESS;
    config.render = &probeRender;
    config.renderUser = &probe;

    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(config);
    REQUIRE(device.has_value());

    // A bounded poll with a generous ceiling. NOTHING here asserts on how long it took.
    waitForCallback(probe, 5000);
    REQUIRE(probe.ran.load(std::memory_order_acquire));

    CHECK(probe.channels.load(std::memory_order_relaxed) == device->channels());
    CHECK(probe.sampleRate.load(std::memory_order_relaxed) == device->sampleRate());
    CHECK(probe.samples.load(std::memory_order_relaxed) > 0U);
    CHECK(probe.calls.load(std::memory_order_relaxed) > 0U);
    // output.size() is an exact multiple of channels — the frame count is derived from it, so a
    // buffer that is not a whole number of frames would make that derivation lossy.
    CHECK(probe.allWritten.load(std::memory_order_relaxed));

    // Stop before the probe leaves scope: ma_device_stop joins the audio thread, so the callback
    // cannot be in flight against a destroyed probe. (~Impl would do the same, but the device is
    // declared AFTER the probe here precisely so ordinary reverse-order destruction is already right.)
    device->stop();
}

TEST_CASE("DV4: the device survives stop()/start() with a callback installed") {
    CallbackProbe probe;
    engine::platform::AudioDeviceConfig config = HEADLESS;
    config.render = &probeRender;
    config.renderUser = &probe;

    std::optional<engine::platform::AudioDevice> device = engine::platform::AudioDevice::open(config);
    REQUIRE(device.has_value());
    waitForCallback(probe, 5000);
    REQUIRE(probe.ran.load(std::memory_order_acquire));

    device->stop();
    CHECK_FALSE(device->isRunning());

    const std::uint64_t before = probe.calls.load(std::memory_order_relaxed);
    probe.ran.store(false, std::memory_order_release);
    CHECK(device->start());
    CHECK(device->isRunning());

    waitForCallback(probe, 5000);
    CHECK(probe.ran.load(std::memory_order_acquire));
    CHECK(probe.calls.load(std::memory_order_relaxed) > before);

    device->stop();
}
