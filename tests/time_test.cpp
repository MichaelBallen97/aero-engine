// Exercises engine::FrameClock and monotonicSeconds() (task 0.2.6): advance()-driven determinism,
// the spike clamp, negative/zero deltas, fps smoothing, and reset(). tick()/steady_clock is covered
// only by loose monotonic checks — wall-clock timing is not deterministic.
#include <aero/core/time.hpp>

#include <doctest/doctest.h>

TEST_CASE("FrameClock: a fresh clock reads zero and the default clamp") {
    const engine::FrameClock clock;
    CHECK(clock.frameCount() == 0);
    CHECK(clock.totalSeconds() == doctest::Approx(0.0));
    CHECK(clock.deltaSeconds() == doctest::Approx(0.0f));
    CHECK(clock.rawDeltaSeconds() == doctest::Approx(0.0f));
    CHECK(clock.fps() == doctest::Approx(0.0f));
    CHECK(clock.maxDeltaSeconds() == doctest::Approx(engine::FrameClock::DEFAULT_MAX_DELTA_SECONDS));
}

TEST_CASE("FrameClock: advance accumulates delta, total, and frame count") {
    engine::FrameClock clock;
    clock.advance(0.01f);
    clock.advance(0.02f);
    CHECK(clock.frameCount() == 2);
    CHECK(clock.deltaSeconds() == doctest::Approx(0.02f));
    CHECK(clock.totalSeconds() == doctest::Approx(0.03));
}

TEST_CASE("FrameClock: the spike clamp caps delta but rawDelta keeps the truth") {
    engine::FrameClock clock;  // default clamp 0.25s
    clock.advance(10.0f);
    CHECK(clock.deltaSeconds() == doctest::Approx(0.25f));
    CHECK(clock.rawDeltaSeconds() == doctest::Approx(10.0f));
    CHECK(clock.totalSeconds() == doctest::Approx(0.25));
}

TEST_CASE("FrameClock: setMaxDeltaSeconds(<=0) disables the clamp; a custom clamp is honored") {
    engine::FrameClock disabled;
    disabled.setMaxDeltaSeconds(0.0f);
    disabled.advance(10.0f);
    CHECK(disabled.deltaSeconds() == doctest::Approx(10.0f));
    CHECK(disabled.totalSeconds() == doctest::Approx(10.0));

    engine::FrameClock custom;
    custom.setMaxDeltaSeconds(0.5f);
    custom.advance(1.0f);
    CHECK(custom.deltaSeconds() == doctest::Approx(0.5f));
}

TEST_CASE("FrameClock: a negative delta is treated as zero but still counts as a frame") {
    engine::FrameClock clock;
    clock.advance(-1.0f);
    CHECK(clock.deltaSeconds() == doctest::Approx(0.0f));
    CHECK(clock.rawDeltaSeconds() == doctest::Approx(-1.0f));
    CHECK(clock.totalSeconds() == doctest::Approx(0.0));
    CHECK(clock.frameCount() == 1);
}

TEST_CASE("FrameClock: a zero delta does not divide by zero and leaves fps untouched") {
    engine::FrameClock clock;
    clock.advance(0.0f);
    CHECK(clock.fps() == doctest::Approx(0.0f));
    CHECK(clock.frameCount() == 1);
}

TEST_CASE("FrameClock: fps converges toward a steady frame rate") {
    engine::FrameClock clock;
    for (int i = 0; i < 200; ++i) {
        clock.advance(1.0f / 60.0f);
    }
    CHECK(clock.fps() == doctest::Approx(60.0f).epsilon(0.02));
}

TEST_CASE("FrameClock: reset re-baselines every accumulator") {
    engine::FrameClock clock;
    clock.advance(0.5f);
    clock.reset();
    CHECK(clock.frameCount() == 0);
    CHECK(clock.totalSeconds() == doctest::Approx(0.0));
    CHECK(clock.deltaSeconds() == doctest::Approx(0.0f));
    CHECK(clock.fps() == doctest::Approx(0.0f));
}

TEST_CASE("FrameClock: tick advances the frame count and never yields a negative delta") {
    engine::FrameClock clock;
    clock.tick();
    clock.tick();
    CHECK(clock.frameCount() == 2);
    CHECK(clock.deltaSeconds() >= 0.0f);
    CHECK(clock.rawDeltaSeconds() >= 0.0f);
}

TEST_CASE("monotonicSeconds: never runs backward") {
    const double first = engine::monotonicSeconds();
    const double second = engine::monotonicSeconds();
    CHECK(second >= first);
}
