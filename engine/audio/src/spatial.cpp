// Aero Engine — the spatializer's implementation (task 3.7.2). Three functions, no state, no
// allocation, no logging. See spatial.hpp for why the math is deliberately this simple.

#include <aero/audio/spatial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::audio {
namespace {

// ONE predicate over the WHOLE input, returning ONE bool -- the D9 shape. Writing this as a chain of
// per-term guards at each use is seed A15, and 3.5.2's code-review round is why it is called out: a
// value can survive its own guard and still poison a later product (`inf * 0` and `NaN * 0` are both
// NaN). Every scalar and every vector component the caller supplies passes through here.
[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

[[nodiscard]] bool inputIsFinite(const ListenerPose& listener, const SpatialParams& source) noexcept {
    return allFinite(listener.position) && allFinite(listener.right) && allFinite(listener.up) &&
           allFinite(listener.forward) && std::isfinite(listener.volume) && allFinite(source.position) &&
           std::isfinite(source.minDistance) && std::isfinite(source.maxDistance) && std::isfinite(source.volume);
}

}  // namespace

float distanceGain(float distance, float minDistance, float maxDistance) noexcept {
    if (!std::isfinite(distance) || !std::isfinite(minDistance) || !std::isfinite(maxDistance)) {
        return 0.0F;
    }
    // NO DIVISION IS PERFORMED ON THIS ARM. A degenerate range is not an error: everything strictly
    // inside maxDistance is at full gain and everything at or beyond it is silent.
    if (maxDistance <= minDistance) {
        return distance < maxDistance ? 1.0F : 0.0F;
    }
    // Exact at both ends by construction: at distance == maxDistance the numerator is exactly 0, and
    // at distance == minDistance the numerator and the denominator are the same expression. SP2
    // asserts both with == and no epsilon.
    return std::clamp((maxDistance - distance) / (maxDistance - minDistance), 0.0F, 1.0F);
}

ChannelGains panGains(float x, std::uint32_t outputChannels) noexcept {
    ChannelGains gains{};
    if (outputChannels == 0) {
        return gains;
    }
    if (outputChannels == 1) {
        gains.gain[0] = 1.0F;  // no pan is representable in one channel
        return gains;
    }
    // theta sweeps 0 -> PI/2 as x sweeps -1 -> +1, so cos^2 + sin^2 == 1 for every x: constant power,
    // not constant amplitude. A linear pan would dip ~3 dB through the centre, which is audible as a
    // hole when a source sweeps past the listener.
    const float theta = (std::clamp(x, -1.0F, 1.0F) + 1.0F) * (PI / 4.0F);
    gains.gain[0] = std::cos(theta);
    gains.gain[1] = std::sin(theta);
    return gains;  // channels 2.. stay exactly 0
}

ChannelGains computeSpatialGains(const ListenerPose& listener, const SpatialParams& source,
                                 std::uint32_t outputChannels) noexcept {
    if (!listener.valid || !inputIsFinite(listener, source)) {
        return {};
    }

    const Vec3 toSource = source.position - listener.position;
    const float distance = length(toSource);
    const float gainDistance = distanceGain(distance, source.minDistance, source.maxDistance);
    if (gainDistance <= 0.0F) {
        return {};  // at or beyond maxDistance -> ALL ZERO, no pan computed
    }

    // normalizeOrZero, never normalize: at distance 0 (the listener inside the source) this yields the
    // zero vector, so x == 0 and the pan collapses to centre. normalize() asserts and does not branch
    // (vec3.hpp), so it would abort a Debug build here and produce a NaN in Release.
    const Vec3 direction = normalizeOrZero(toSource);
    const float x = std::clamp(dot(direction, listener.right), -1.0F, 1.0F);

    ChannelGains gains = panGains(x, outputChannels);
    // listener.volume is NOT applied here -- see spatial.hpp. This is the per-source scale only.
    const float scale = gainDistance * source.volume;
    for (float& value : gains.gain) {
        value *= scale;
    }
    return gains;
}

}  // namespace engine::audio
