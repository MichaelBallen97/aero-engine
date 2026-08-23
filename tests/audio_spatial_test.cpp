// Aero Engine — the spatializer's tier-0 battery (task 3.7.2), SP1-SP14. Pure functions over pure
// data: no clip, no mixer, no system, no device, no GPU, no window. Every case here therefore runs in
// EVERY configuration this project builds, with AERO_REQUIRE_GPU set and unset.
//
// <ostream> is included preventively: a doctest CHECK over a std::string_view fails the Windows lane
// ALONE, inside the MS STL headers rather than at the CHECK. There is no #if of any kind in this file
// and there must never be one -- task 3.6.3 shipped four cases inside a file-level #if with
// everything green while the one arm that mattered never ran.

#include <aero/assets/cooked_audio.hpp>
#include <aero/audio/spatial.hpp>
#include <aero/core/math.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>

namespace {

using engine::Vec3;
using engine::audio::ChannelGains;
using engine::audio::computeSpatialGains;
using engine::audio::distanceGain;
using engine::audio::ListenerPose;
using engine::audio::panGains;
using engine::audio::SpatialParams;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// A listener at the origin with the identity basis and valid == true. Every case that wants a real
// listener starts here and perturbs one thing, so a perturbation is legible in the diff.
[[nodiscard]] ListenerPose originListener() {
    ListenerPose pose;
    pose.valid = true;
    return pose;
}

[[nodiscard]] bool allZero(const ChannelGains& gains) {
    for (const float value : gains.gain) {
        if (value != 0.0F) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool allFinite(const ChannelGains& gains) {
    for (const float value : gains.gain) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("SP1: ListenerPose's defaults, and the MAX_AUDIO_OUTPUT_CHANNELS coincidence") {
    const ListenerPose pose;
    CHECK_FALSE(pose.valid);  // a default pose means THERE IS NO LISTENER (D23)
    CHECK(pose.position.x == 0.0F);
    CHECK(pose.position.y == 0.0F);
    CHECK(pose.position.z == 0.0F);
    CHECK(pose.right.x == 1.0F);
    CHECK(pose.right.y == 0.0F);
    CHECK(pose.right.z == 0.0F);
    CHECK(pose.up.x == 0.0F);
    CHECK(pose.up.y == 1.0F);
    CHECK(pose.up.z == 0.0F);
    CHECK(pose.forward.x == 0.0F);
    CHECK(pose.forward.y == 0.0F);
    CHECK(pose.forward.z == -1.0F);  // -Z forward (ADR-005)
    CHECK(pose.volume == 1.0F);

    // spatial.hpp deliberately does NOT #include cooked_audio.hpp to derive this: the two numbers
    // happen to be equal and need not move together. The equality is pinned HERE instead, so drift is
    // visible without asserting a relationship that does not exist.
    CHECK(engine::audio::MAX_AUDIO_OUTPUT_CHANNELS == engine::assets::MAX_COOKED_AUDIO_CHANNELS);
    CHECK(engine::audio::SPATIAL_PAN_CHANNELS == 2U);
}

TEST_CASE("SP2: distanceGain is EXACTLY 1 at minDistance and EXACTLY 0 at maxDistance") {
    constexpr float MIN_D = 2.0F;
    constexpr float MAX_D = 10.0F;

    // NO EPSILON on either end, and neither is luck: at d == maxDistance the numerator is exactly 0,
    // and at d == minDistance the numerator and the denominator are the same expression.
    CHECK(distanceGain(MIN_D, MIN_D, MAX_D) == 1.0F);
    CHECK(distanceGain(MAX_D, MIN_D, MAX_D) == 0.0F);

    CHECK(distanceGain(0.0F, MIN_D, MAX_D) == 1.0F);     // below minDistance
    CHECK(distanceGain(1.5F, MIN_D, MAX_D) == 1.0F);     // still below
    CHECK(distanceGain(1000.0F, MIN_D, MAX_D) == 0.0F);  // well beyond maxDistance

    CHECK(distanceGain(6.0F, MIN_D, MAX_D) == doctest::Approx(0.5F).epsilon(1e-6));  // the midpoint

    // Monotone non-increasing across a 17-point sweep of the whole range.
    float previous = 2.0F;
    for (int step = 0; step <= 16; ++step) {
        const float d = MIN_D + (MAX_D - MIN_D) * (static_cast<float>(step) / 16.0F);
        const float g = distanceGain(d, MIN_D, MAX_D);
        CHECK(g <= previous);
        CHECK(g >= 0.0F);
        CHECK(g <= 1.0F);
        previous = g;
    }
}

TEST_CASE("SP3: maxDistance <= minDistance is 1 inside / 0 outside, with NO division performed") {
    // Three sub-arms. Each must be finite -- a division by (maxD - minD) here would be 0/0 or a
    // negative denominator, and the guard's whole job is that neither happens.
    const float inverted = distanceGain(1.0F, 10.0F, 5.0F);  // maxD < minD
    CHECK(std::isfinite(inverted));
    CHECK(inverted == 1.0F);
    CHECK(distanceGain(5.0F, 10.0F, 5.0F) == 0.0F);
    CHECK(distanceGain(7.0F, 10.0F, 5.0F) == 0.0F);

    const float equal = distanceGain(1.0F, 5.0F, 5.0F);  // maxD == minD
    CHECK(std::isfinite(equal));
    CHECK(equal == 1.0F);

    const float atBoundary = distanceGain(5.0F, 5.0F, 5.0F);  // d == maxD == minD
    CHECK(std::isfinite(atBoundary));
    CHECK(atBoundary == 0.0F);
}

TEST_CASE("SP4: a negative or zero minDistance stays finite and inside [0, 1]") {
    const float negative = distanceGain(3.0F, -5.0F, 10.0F);
    CHECK(std::isfinite(negative));
    CHECK(negative >= 0.0F);
    CHECK(negative <= 1.0F);

    const float bothZero = distanceGain(0.0F, 0.0F, 0.0F);
    CHECK(std::isfinite(bothZero));
    CHECK(bothZero >= 0.0F);
    CHECK(bothZero <= 1.0F);

    const float zeroMin = distanceGain(0.0F, 0.0F, 10.0F);
    CHECK(std::isfinite(zeroMin));
    CHECK(zeroMin == 1.0F);
}

TEST_CASE("SP5: panGains is hard left at x = -1 and hard right at x = +1") {
    // cos/sin at 0 and PI/2 are not exactly 1 and 0 in fp32, so the epsilon is NAMED in the assertion
    // rather than left to a default.
    const ChannelGains left = panGains(-1.0F, 2);
    CHECK(left.gain[0] == doctest::Approx(1.0F).epsilon(1e-6));
    CHECK(left.gain[1] == doctest::Approx(0.0F).epsilon(1e-6));

    const ChannelGains right = panGains(1.0F, 2);
    CHECK(right.gain[0] == doctest::Approx(0.0F).epsilon(1e-6));
    CHECK(right.gain[1] == doctest::Approx(1.0F).epsilon(1e-6));

    // Out-of-range x is clamped here, not at the call site.
    const ChannelGains clampedLeft = panGains(-4.0F, 2);
    CHECK(clampedLeft.gain[0] == doctest::Approx(left.gain[0]).epsilon(1e-6));
    CHECK(clampedLeft.gain[1] == doctest::Approx(left.gain[1]).epsilon(1e-6));
}

TEST_CASE("SP6: panGains at centre is sqrt(2)/2 on both channels") {
    // Pinned against the LITERAL with the epsilon as part of the assertion -- the TM20 rule. A
    // constant-power pan reads 0.70710678 at centre; a LINEAR pan would read 0.5, which 1e-6 sees and
    // a loose tolerance would not.
    const ChannelGains centre = panGains(0.0F, 2);
    CHECK(centre.gain[0] == doctest::Approx(0.70710678F).epsilon(1e-6));
    CHECK(centre.gain[1] == doctest::Approx(0.70710678F).epsilon(1e-6));
    CHECK(centre.gain[0] == doctest::Approx(centre.gain[1]).epsilon(1e-6));
}

TEST_CASE("SP7: the pan is CONSTANT POWER across a 33-point sweep") {
    for (int step = 0; step <= 32; ++step) {
        const float x = -1.0F + 2.0F * (static_cast<float>(step) / 32.0F);
        const ChannelGains gains = panGains(x, 2);
        const float power = gains.gain[0] * gains.gain[0] + gains.gain[1] * gains.gain[1];
        CHECK(power == doctest::Approx(1.0F).epsilon(1e-6));
    }
}

TEST_CASE("SP8: outputChannels 0, 1 and 8 each behave as documented") {
    CHECK(allZero(panGains(0.0F, 0)));

    const ChannelGains mono = panGains(-1.0F, 1);
    CHECK(mono.gain[0] == 1.0F);  // no pan is representable in one channel
    for (std::size_t ch = 1; ch < mono.gain.size(); ++ch) {
        CHECK(mono.gain[ch] == 0.0F);
    }

    // Through the composition: one output channel gets exactly g_dist * volume, with no pan applied.
    // The source is at distance 3 with minD 1 / maxD 5, so g_dist is exactly 0.5 and the expected
    // value is the literal 0.25 rather than a re-derivation through the function under test.
    const ListenerPose listener = originListener();
    SpatialParams onAxis;
    onAxis.position = Vec3{3.0F, 0.0F, 0.0F};
    onAxis.minDistance = 1.0F;
    onAxis.maxDistance = 5.0F;
    onAxis.volume = 0.5F;
    const ChannelGains one = computeSpatialGains(listener, onAxis, 1);
    CHECK(one.gain[0] == doctest::Approx(0.25F).epsilon(1e-6));
    for (std::size_t ch = 1; ch < one.gain.size(); ++ch) {
        CHECK(one.gain[ch] == 0.0F);
    }

    // Eight channels: 0 and 1 carry the pan, 2..7 are EXACTLY zero. The source is deliberately
    // OFF-AXIS (45 degrees right-front), because an on-axis one pans hard right and channel 0 then
    // reads cos(PI/2), which in fp32 is a small NEGATIVE number (-4.37e-8, measured) rather than 0.
    // That is -160 dBFS and inaudible, and it is left uncorrected: clamping it would deviate from the
    // normative formula for nothing. SP5 is where the near-zero end is pinned, with its epsilon named.
    SpatialParams offAxis = onAxis;
    offAxis.position = Vec3{3.0F, 0.0F, -3.0F};
    const ChannelGains eight = computeSpatialGains(listener, offAxis, 8);
    CHECK(eight.gain[0] > 0.0F);
    CHECK(eight.gain[1] > 0.0F);
    for (std::size_t ch = 2; ch < eight.gain.size(); ++ch) {
        CHECK(eight.gain[ch] == 0.0F);
    }
}

TEST_CASE("SP9: an INVALID listener yields all-zero gains from every source position") {
    const ListenerPose listener;  // valid == false by default
    CHECK_FALSE(listener.valid);

    const std::array<Vec3, 9> positions = {
        Vec3{0.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F},   Vec3{-1.0F, 0.0F, 0.0F},
        Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F},   Vec3{0.0F, 0.0F, -1.0F},
        Vec3{0.5F, 0.5F, 0.5F}, Vec3{-3.0F, 2.0F, -4.0F}, Vec3{100.0F, 0.0F, 0.0F},
    };
    for (const Vec3& position : positions) {
        SpatialParams source;
        source.position = position;
        CHECK(allZero(computeSpatialGains(listener, source, 2)));
    }
}

TEST_CASE("SP10: a non-finite value ANYWHERE in the input yields all-zero gains") {
    // THE case 3.5.2's `u` lesson exists for. The guard must be ONE predicate over the WHOLE input:
    // a chain of per-term guards lets a value through that is finite on its own and poisons a later
    // product (`inf * 0` and `NaN * 0` are both NaN). Every term below gets both a NaN arm and both
    // infinity arms.
    const std::array<float, 3> poisons = {NAN_F, INF_F, -INF_F};

    for (const float poison : poisons) {
        {  // source position, each component separately
            const ListenerPose listener = originListener();
            SpatialParams source;
            source.position = Vec3{poison, 0.0F, 0.0F};
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
            source.position = Vec3{0.0F, poison, 0.0F};
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
            source.position = Vec3{0.0F, 0.0F, poison};
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
        }
        {  // source minDistance / maxDistance / volume
            const ListenerPose listener = originListener();
            SpatialParams source;
            source.position = Vec3{1.0F, 0.0F, 0.0F};
            source.minDistance = poison;
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
            source.minDistance = 1.0F;
            source.maxDistance = poison;
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
            source.maxDistance = 50.0F;
            source.volume = poison;
            CHECK(allZero(computeSpatialGains(listener, source, 2)));
        }
        {  // listener position, each axis, and the listener's own volume
            SpatialParams source;
            source.position = Vec3{1.0F, 0.0F, 0.0F};

            ListenerPose poisoned = originListener();
            poisoned.position = Vec3{poison, 0.0F, 0.0F};
            CHECK(allZero(computeSpatialGains(poisoned, source, 2)));

            poisoned = originListener();
            poisoned.right = Vec3{poison, 0.0F, 0.0F};
            CHECK(allZero(computeSpatialGains(poisoned, source, 2)));

            poisoned = originListener();
            poisoned.up = Vec3{0.0F, poison, 0.0F};
            CHECK(allZero(computeSpatialGains(poisoned, source, 2)));

            poisoned = originListener();
            poisoned.forward = Vec3{0.0F, 0.0F, poison};
            CHECK(allZero(computeSpatialGains(poisoned, source, 2)));

            poisoned = originListener();
            poisoned.volume = poison;
            CHECK(allZero(computeSpatialGains(poisoned, source, 2)));
        }
    }

    // The arm a PER-TERM guard cannot catch and a whole-input predicate can: every individual term is
    // finite in isolation, but the difference overflows to infinity. A per-use `isfinite(position.x)`
    // is true for both operands here.
    {
        ListenerPose listener = originListener();
        listener.position = Vec3{-3.0e38F, 0.0F, 0.0F};
        SpatialParams source;
        source.position = Vec3{3.0e38F, 0.0F, 0.0F};
        const ChannelGains gains = computeSpatialGains(listener, source, 2);
        CHECK(allFinite(gains));
        CHECK(allZero(gains));  // an infinite distance is beyond every finite maxDistance
    }
}

TEST_CASE("SP11: distance EXACTLY zero pans to centre at full gain, with no NaN") {
    const ListenerPose listener = originListener();
    SpatialParams source;
    source.position = Vec3{0.0F, 0.0F, 0.0F};  // the listener is inside the source
    source.minDistance = 1.0F;
    source.maxDistance = 10.0F;
    source.volume = 1.0F;

    const ChannelGains gains = computeSpatialGains(listener, source, 2);
    CHECK(allFinite(gains));
    CHECK(gains.gain[0] == gains.gain[1]);  // exactly centre: normalizeOrZero short-circuits to x == 0
    CHECK(gains.gain[0] == doctest::Approx(0.70710678F).epsilon(1e-6));
}

TEST_CASE("SP12: a SCALED listener basis, normalised as buildAudioView does, is BIT-IDENTICAL") {
    // THE normalizeOrZero ARM, and the case's premise had to be corrected before it could hold:
    // computeSpatialGains does NOT normalise the listener's axes (it computes
    // clamp(dot(dir, listener.right), -1, 1) verbatim). The normalisation lives ONE LAYER UP, in
    // buildAudioView, which is what makes a SCALED LISTENER ENTITY behave identically to an unscaled
    // one. So this case applies the documented construction itself and asserts bit-identity of the
    // result -- and, so it is not vacuous, asserts that the UN-normalised pair genuinely DIFFERS.
    SpatialParams source;
    source.position = Vec3{2.0F, 1.0F, -3.0F};
    source.minDistance = 1.0F;
    source.maxDistance = 20.0F;

    ListenerPose rawScaled = originListener();
    rawScaled.right = Vec3{3.0F, 0.0F, 0.0F};
    rawScaled.up = Vec3{0.0F, 3.0F, 0.0F};
    rawScaled.forward = Vec3{0.0F, 0.0F, -3.0F};

    const ListenerPose unscaled = originListener();

    // ANTI-VACUITY FIRST: without the normalisation the scale changes the answer, so the assertion
    // below is about the normalisation rather than about nothing.
    const ChannelGains rawGains = computeSpatialGains(rawScaled, source, 2);
    const ChannelGains unscaledGains = computeSpatialGains(unscaled, source, 2);
    CHECK(rawGains.gain[0] != unscaledGains.gain[0]);

    // Now the construction buildAudioView performs. The factor 3 is exactly representable, so
    // normalizeOrZero({3,0,0}) is bit-exactly {1,0,0} and == is correct here -- a strictly stronger
    // statement than approxEquals. If this ever proves flaky on some lane, record it as a finding and
    // weaken it with the epsilon named, never silently.
    ListenerPose normalised = rawScaled;
    normalised.right = engine::normalizeOrZero(rawScaled.right);
    normalised.up = engine::normalizeOrZero(rawScaled.up);
    normalised.forward = engine::normalizeOrZero(rawScaled.forward);

    const ChannelGains normalisedGains = computeSpatialGains(normalised, source, 2);
    for (std::size_t ch = 0; ch < normalisedGains.gain.size(); ++ch) {
        CHECK(normalisedGains.gain[ch] == unscaledGains.gain[ch]);
    }
}

TEST_CASE("SP13: a ZERO-SCALE listener right axis collapses to centre and stays finite") {
    ListenerPose listener = originListener();
    listener.right = Vec3{0.0F, 0.0F, 0.0F};  // a degenerate column

    SpatialParams source;
    source.position = Vec3{4.0F, 0.0F, 0.0F};
    source.minDistance = 1.0F;
    source.maxDistance = 20.0F;

    const ChannelGains gains = computeSpatialGains(listener, source, 2);
    CHECK(allFinite(gains));
    CHECK(gains.gain[0] == gains.gain[1]);  // dot(dir, zero) == 0 -> centre, not a NaN
    CHECK(gains.gain[0] > 0.0F);
}

TEST_CASE("SP14: front and back at equal distance are IDENTICAL -- the documented D13 limit") {
    const ListenerPose listener = originListener();  // forward is -Z, right is +X

    SpatialParams front;
    front.position = Vec3{0.0F, 0.0F, -5.0F};
    front.minDistance = 1.0F;
    front.maxDistance = 20.0F;

    SpatialParams back = front;
    back.position = Vec3{0.0F, 0.0F, 5.0F};

    const ChannelGains a = computeSpatialGains(listener, front, 2);
    const ChannelGains b = computeSpatialGains(listener, back, 2);
    for (std::size_t ch = 0; ch < a.gain.size(); ++ch) {
        CHECK(a.gain[ch] == b.gain[ch]);
    }
    // And both are centre, because the azimuth against `right` is 0 for each.
    CHECK(a.gain[0] == a.gain[1]);
}
