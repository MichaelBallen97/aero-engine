#pragma once
// Aero Engine — Phase 0 fps gate (task 0.5.3, sample-local). Turns the phase-0 "@60fps" gate from an
// eyeball judgment into an objective, repeatable verdict. Header-only; depends only on the standard
// library — no engine type, no third-party type (rule #3). It is fed REAL present-to-present wall-clock
// timestamps (engine::monotonicSeconds()) by the loop — NOT FrameClock's clamped delta, so a genuinely
// slow frame is measured at its true badness. It skips a warm-up window and ignores any interval that
// spans a not-presentable (minimized) gap, then reports average + worst-frame fps and a verdict.
//
// The pass rule is a LOWER bound (avg >= 58 fps), so it is high-refresh-honest: a 120/144 Hz panel that
// correctly runs faster than 60 under vsync still PASSES. Exotic sub-60 Hz displays are out of the
// gate's scope (documented in VALIDATION.md).
//
// Not shared with tests/ on purpose (0.5.2 D9, as cube_mesh.hpp states): sample-local headers stay out
// of the test tree. Correctness is verified by code review + the on-hardware macOS validation run.
//
// NOT thread-safe: one FpsGate belongs to the loop thread (like FrameClock).
#include <cstdint>

namespace cube {

// What the gate concludes at exit. PascalCase enum values (RHI convention). uint8_t base like every
// engine enum (docs/04; performance-enum-size) — four states fit in one byte.
enum class GateVerdict : std::uint8_t {
    Insufficient,  // fewer than MIN_SAMPLE_FRAMES measured frames — ran too briefly to judge
    Fail,          // avg < MARGINAL_FPS — well below the 60 fps class (e.g. a software rasterizer)
    Marginal,      // 60-adjacent but not clean: avg in [MARGINAL_FPS, PASS_FPS) or a bad worst-frame
    Pass,          // vsync-locked, >= PASS_FPS avg, no severe drops — the gate is met on this machine
};

[[nodiscard]] constexpr const char* toString(GateVerdict verdict) noexcept {
    switch (verdict) {
        case GateVerdict::Insufficient:
            return "INSUFFICIENT";
        case GateVerdict::Fail:
            return "FAIL";
        case GateVerdict::Marginal:
            return "MARGINAL";
        case GateVerdict::Pass:
            return "PASS";
    }
    return "?";  // unreachable; silences a non-void-return warning
}

struct GateSummary {
    double avgFps = 0.0;        // counted frames / summed measured intervals (warm-up skipped, gaps out)
    double worstFps = 0.0;      // 1 / the longest single measured interval — the stutter metric
    std::uint64_t samples = 0;  // number of intervals that counted toward the verdict
    GateVerdict verdict = GateVerdict::Insufficient;
};

// Accumulates present-to-present intervals and evaluates the Phase 0 60 fps gate.
class FpsGate {
public:
    // Pass rule (a LOWER bound — honest on high-refresh displays). Tunable named constants.
    static constexpr double PASS_FPS = 58.0;         // >= 58 avg reads as "60 fps class" (60 Hz - jitter)
    static constexpr double MARGINAL_FPS = 48.0;     // [48, 58) avg is 60-adjacent-but-not-clean
    static constexpr double WORST_FPS_FLOOR = 30.0;  // one frame slower than ~33 ms (a dropped vblank) = a real hitch
    static constexpr std::uint64_t WARMUP_FRAMES = 30;       // skip ~0.5 s of pipeline/shader warm-up
    static constexpr std::uint64_t MIN_SAMPLE_FRAMES = 120;  // need ~2 s of counted frames to judge

    // Feed one PRESENTED frame's monotonic wall-clock timestamp (seconds). Call once after each
    // successful endFrame(); never for a skipped (minimized) frame.
    void recordPresent(double nowSeconds) noexcept {
        ++presented;
        if (presented <= WARMUP_FRAMES || !haveLast) {
            lastPresent = nowSeconds;  // (re)establish the interval origin without counting it
            haveLast = true;
            return;
        }
        const double interval = nowSeconds - lastPresent;
        lastPresent = nowSeconds;
        if (interval <= 0.0) {  // clock non-advance (steady_clock shouldn't) — ignore
            return;
        }
        summedIntervals += interval;
        ++counted;
        if (interval > worstInterval) {
            worstInterval = interval;
        }
    }

    // Call whenever a frame was NOT presented (beginFrame() == nullopt / minimized) so the next present
    // does not measure an interval that spans the idle gap.
    void noteInterruption() noexcept { haveLast = false; }

    [[nodiscard]] GateSummary summary() const noexcept {
        GateSummary result;
        result.samples = counted;
        if (counted == 0 || summedIntervals <= 0.0) {
            return result;  // verdict defaults to Insufficient
        }
        result.avgFps = static_cast<double>(counted) / summedIntervals;
        result.worstFps = worstInterval > 0.0 ? 1.0 / worstInterval : 0.0;
        result.verdict = evaluate(result.avgFps, result.worstFps, counted);
        return result;
    }

    // The pure verdict rule, exposed so its edge cases are legible/reviewable in one place. NaN-safe:
    // `!(avgFps > 0.0)` is true for NaN and for <= 0.
    [[nodiscard]] static GateVerdict evaluate(double avgFps, double worstFps, std::uint64_t samples) noexcept {
        if (samples < MIN_SAMPLE_FRAMES || !(avgFps > 0.0)) {
            return GateVerdict::Insufficient;
        }
        if (avgFps < MARGINAL_FPS) {
            return GateVerdict::Fail;
        }
        if (avgFps < PASS_FPS || worstFps < WORST_FPS_FLOOR) {
            return GateVerdict::Marginal;
        }
        return GateVerdict::Pass;
    }

private:
    double lastPresent = 0.0;      // timestamp of the previous warm-up/counted present
    double summedIntervals = 0.0;  // denominator of avgFps
    double worstInterval = 0.0;    // longest counted interval => worstFps
    std::uint64_t presented = 0;   // total recordPresent() calls (warm-up gating)
    std::uint64_t counted = 0;     // intervals that counted toward the verdict
    bool haveLast = false;         // is lastPresent a valid origin for the next interval?
};

}  // namespace cube
