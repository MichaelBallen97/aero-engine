// Aero Engine — AudioDevice lifecycle tests (task 0.3.3). Black-box: engine::platform::AudioDevice only,
// no ma_ type here. Always headless (miniaudio null backend) so CI needs no audio device. Real audible
// output is 3.7's concern; here we prove open/format/start/stop/running/move/RAII-close.

#include <aero/platform/audio.hpp>

#include <doctest/doctest.h>

#include <optional>
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
