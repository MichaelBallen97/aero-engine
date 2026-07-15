#pragma once
// Aero Engine — frame & clock timing (task 0.2.6): the `time` half of core's Time & VFS deliverable.
// core sits at the bottom of the layer order and depends on NOTHING above it — in particular it may
// NOT use platform/SDL timers (SDL_GetTicks lives in engine/platform, a layer ABOVE core). The only
// legal clock here is the standard library's, so all timing is built on std::chrono::steady_clock,
// which is monotonic — it never runs backward, unlike system_clock, which an NTP correction or a
// user clock change can move. Header-only: there is no third-party backend to hide, so — unlike math
// (GLM) and log (spdlog) — `time` adds NO translation unit to aero_core and NO port to vcpkg.

#include <chrono>
#include <cstdint>

namespace engine {

// Per-frame timing driven by std::chrono::steady_clock.
//
// Drive it one of two ways per frame:
//   * tick()             measures the delta since the previous tick()/construction/reset from
//                        steady_clock. Use this in a simple loop with no delta of its own.
//   * advance(seconds)   advances by a delta you already measured (e.g. one the platform layer
//                        handed you). tick() is implemented as advance(measuredDelta); advancing by a
//                        known delta is also what makes every derived quantity deterministically
//                        testable (wall-clock timing is not).
//
// Reported quantities (all const, noexcept):
//   deltaSeconds()     last frame's delta AFTER the spike clamp — the dt the simulation should use.
//   rawDeltaSeconds()  last frame's delta BEFORE the clamp — for a HUD / spike diagnostics.
//   totalSeconds()     sum of clamped deltas since construction/reset (double — no precision cliff
//                      after hours of play).
//   frameCount()       number of tick()/advance() calls since construction/reset.
//   fps()              an exponential-moving-average frame rate (smoothed so one slow frame does not
//                      make the counter jump); 0 until the first non-zero delta.
//
// The spike clamp (maxDeltaSeconds(), default 0.25s ≈ a 4 fps floor) caps the dt the simulation sees
// so a stall — a breakpoint, a window drag, a slow asset load — cannot teleport the world or blow up
// a future fixed-step integrator. Set it <= 0 to disable.
//
// float delta / double total is the deliberate split (the one Unreal uses): a per-frame delta is
// tiny and feeds float gameplay/render math directly, while total app time wants double to stay
// exact over a long session.
//
// NOT thread-safe: a FrameClock belongs to the thread that runs the loop. Read it from other threads
// only behind external synchronization.
class FrameClock {
public:
    // Default spike clamp: dt is capped at 0.25s (≈ 4 fps) so a stalled frame cannot advance the
    // simulation by an arbitrarily large step. Overridable via setMaxDeltaSeconds().
    static constexpr float DEFAULT_MAX_DELTA_SECONDS = 0.25f;

    FrameClock() noexcept : lastTick(std::chrono::steady_clock::now()) {}

    // Measure the delta since the previous tick()/construction/reset, then advance by it.
    void tick() noexcept {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = now - lastTick;
        lastTick = now;
        advance(elapsed.count());
    }

    // Advance by a caller-supplied delta (seconds): apply the spike clamp, accumulate total time,
    // increment the frame count, and update the smoothed fps. A negative delta (which steady_clock
    // never yields, but a hand-fed value might) is treated as 0.
    void advance(float deltaSeconds) noexcept {
        rawDelta = deltaSeconds;

        float clamped = deltaSeconds;
        if (clamped < 0.0f) {
            clamped = 0.0f;
        }
        if (maxDelta > 0.0f && clamped > maxDelta) {
            clamped = maxDelta;
        }
        delta = clamped;
        total += static_cast<double>(clamped);
        ++frames;

        if (clamped > 0.0f) {
            const float instantFps = 1.0f / clamped;
            smoothedFps =
                (smoothedFps <= 0.0f) ? instantFps : smoothedFps + (FPS_SMOOTHING * (instantFps - smoothedFps));
        }
    }

    // Re-baseline: the next tick() measures from now, and every accumulator resets to 0. Call this
    // right before entering the main loop if construction happened long before it, so the first
    // frame's delta is measured from loop start (the clamp is the safety net regardless).
    void reset() noexcept {
        lastTick = std::chrono::steady_clock::now();
        rawDelta = 0.0f;
        delta = 0.0f;
        total = 0.0;
        frames = 0;
        smoothedFps = 0.0f;
    }

    [[nodiscard]] float deltaSeconds() const noexcept { return delta; }
    [[nodiscard]] float rawDeltaSeconds() const noexcept { return rawDelta; }
    [[nodiscard]] double totalSeconds() const noexcept { return total; }
    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frames; }
    [[nodiscard]] float fps() const noexcept { return smoothedFps; }

    [[nodiscard]] float maxDeltaSeconds() const noexcept { return maxDelta; }
    void setMaxDeltaSeconds(float seconds) noexcept { maxDelta = seconds; }

private:
    // fps() smoothing: the weight given to the newest sample in the exponential moving average
    // (10% new, 90% history) — low enough to steady the readout, high enough to track a real
    // frame-rate change within a fraction of a second.
    static constexpr float FPS_SMOOTHING = 0.1f;

    std::chrono::steady_clock::time_point lastTick;  // set by ctor/reset/tick; the tick() delta origin
    float rawDelta = 0.0f;
    float delta = 0.0f;
    double total = 0.0;
    std::uint64_t frames = 0;
    float smoothedFps = 0.0f;
    float maxDelta = DEFAULT_MAX_DELTA_SECONDS;
};

// A monotonic clock reading in seconds, for ad-hoc interval timing (take two readings, subtract).
// The origin is arbitrary (steady_clock's epoch) — only differences are meaningful. This is the same
// clock FrameClock::tick() samples; prefer FrameClock for per-frame timing.
[[nodiscard]] inline double monotonicSeconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace engine
