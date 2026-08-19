// engine/render/src/animation.cpp — task 3.5.2: the clip sampler behind animation.hpp. glTF 2.0's
// three interpolation modes, sampled to the letter of its section 3.6.2 and Appendix C.
//
// Nothing here allocates, logs, recurses or touches a GPU, and there is no profiling include either
// (a deliberate absence, not an oversight: this is a handful of float operations per channel per
// frame, and a Tracy zone would cost more than the work it measured -- skinning.cpp carries none for
// the same reason). Every entry point is total under a caller's bad sizes and under hostile file
// contents alike, because at Phase 5 these bytes come out of a .pak that may have been shipped,
// patched or crafted.

#include <aero/core/math.hpp>
#include <aero/render/animation.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::render {

namespace {

// The located segment: t[k] <= tc < t[k1] and u the fraction across it, or a CLAMPED end, which is
// spelled k1 == k (with u and td both zero) so every caller can test one thing.
struct Segment {
    std::uint32_t k = 0;
    std::uint32_t k1 = 0;
    float u = 0.0F;
    float td = 0.0F;
};

// n >= 1 by the parser (a channel with keyCount 0 is refused, docs/09 section 13.3).
// ALLOCATION-FREE, no recursion, and TOTAL for every float value there is.
[[nodiscard]] Segment locate(std::span<const std::byte> times, std::uint32_t n, float tc) noexcept {
    // ONE predicate covers three cases: tc below the range, tc exactly at t[0], and tc NaN --
    // !(tc > t0) is true for all three, which is what makes a non-finite time land on value(0)
    // instead of propagating a NaN through u into every interpolator. The two-comparison form
    // (tc <= t0) is true for only two of the three.
    if (n == 1 || !(tc > assets::animationKeyTime(times, 0))) {
        return Segment{0, 0, 0.0F, 0.0F};
    }
    if (!(tc < assets::animationKeyTime(times, n - 1))) {  // glTF section 3.6.2's trailing clamp
        return Segment{n - 1, n - 1, 0.0F, 0.0F};
    }
    // Invariant: t[lo] <= tc and t[hi] > tc, both established by the two clamps above and both
    // preserved by the two assignments below. ~log2(n) reads, no allocation, no recursion.
    std::uint32_t lo = 0;
    std::uint32_t hi = n - 1;
    while (hi - lo > 1) {
        const std::uint32_t mid = lo + ((hi - lo) / 2);
        if (assets::animationKeyTime(times, mid) <= tc) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const float t0 = assets::animationKeyTime(times, lo);
    const float t1 = assets::animationKeyTime(times, lo + 1);
    const float td = t1 - t0;
    // The two guards below are for the file the parser deliberately does NOT police: docs/09 section
    // 13.10 leaves a channel's times unchecked for monotonicity, so a hand-built or hostile clip can
    // present anything at all here. td > 0 is what keeps a division out of the hands of such a file
    // -- a UBSan report rather than a wrong picture -- and the clamp keeps u a FRACTION, so no mode
    // can be driven outside the two keys it was handed. Post-cook both are inert, and so is the
    // clamp for a hostile file too: the loop's exit invariant gives 0 <= tc - t[lo] < t[hi] - t[lo]
    // whatever the times do, so u is a ratio of a non-negative numerator to a strictly larger
    // denominator by construction. It stays because the invariant is a property of this function's
    // control flow, and the next person to touch that control flow should not have to re-derive it
    // before an interpolator can become an extrapolator.
    const float u = td > 0.0F ? std::clamp((tc - t0) / td, 0.0F, 1.0F) : 0.0F;
    return Segment{lo, lo + 1, u, td};
}

// The per-keyframe [inTangent, value, outTangent] layout, spelled ONCE. THE + 1 IS THE WHOLE TRAP: a
// clamp path or a one-key channel that reads values[3k] returns a TANGENT as a pose.
[[nodiscard]] Vec4 valueAt(std::span<const std::byte> values, bool cubic, std::uint32_t i) noexcept {
    return assets::animationKeyValue(values, cubic ? (3U * i) + 1U : i);
}

// glTF Appendix C's Hermite form with t_d-scaled tangents:
//   v = (2u^3 - 3u^2 + 1)*v_k + t_d*(u^3 - 2u^2 + u)*b_k
//     + (-2u^3 + 3u^2)*v_k1  + t_d*(u^3 - u^2)*a_k1
// with v_i = values[3i + 1], b_k = values[3k + 2] (key k's OUT-tangent) and a_k1 = values[3(k+1)]
// (key k+1's IN-tangent). The first in-tangent and the last out-tangent are unused, exactly as the
// specification says exporters SHOULD zero them. Dropping the t_d factors is the classic mistake and
// is invisible on any fixture whose segment happens to be one second long.
[[nodiscard]] Vec4 hermite(std::span<const std::byte> values, const Segment& s) noexcept {
    const Vec4 vk = valueAt(values, true, s.k);
    const Vec4 bk = assets::animationKeyValue(values, (3U * s.k) + 2U);
    const Vec4 ak1 = assets::animationKeyValue(values, 3U * s.k1);
    const Vec4 vk1 = valueAt(values, true, s.k1);

    const float u = s.u;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = (2.0F * u3) - (3.0F * u2) + 1.0F;
    const float h10 = s.td * (u3 - (2.0F * u2) + u);
    const float h01 = (-2.0F * u3) + (3.0F * u2);
    const float h11 = s.td * (u3 - u2);
    return (vk * h00) + (bk * h10) + (vk1 * h01) + (ak1 * h11);
}

// The ONE place a stored Vec4 becomes a quaternion and back. glTF's accessor order is (x, y, z, w)
// and so is engine::Quat's, so this is a relabelling rather than a conversion -- but it is spelled
// once anyway, because a second spelling is a second place for a component swap to live.
[[nodiscard]] Quat quatOf(Vec4 v) noexcept { return Quat{v.x, v.y, v.z, v.w}; }
[[nodiscard]] Vec4 vec4Of(Quat q) noexcept { return Vec4{q.x, q.y, q.z, q.w}; }

}  // namespace

AnimationBindStats bindAnimation(const assets::CookedAnimation& clip, const assets::CookedSkeleton& skeleton,
                                 std::span<std::uint32_t> out) {
    assert(out.size() == clip.channels.size() && "bindAnimation: one out entry per clip channel");

    AnimationBindStats stats;
    // The clip's OWN channel count, whatever the caller sized `out` to: a report about the artifact
    // must not quietly become a report about the caller's bookkeeping.
    stats.channelCount = static_cast<std::uint32_t>(clip.channels.size());
    stats.sourceGuidMatches = clip.sourceGuid == skeleton.sourceGuid;

    const std::size_t count = std::min(out.size(), clip.channels.size());
    for (std::size_t c = 0; c < count; ++c) {
        const std::uint32_t target = clip.channels[c].targetNodeLocalId;
        std::uint32_t found = assets::COOKED_SKELETON_INVALID_INDEX;
        // A LINEAR SCAN, and matching is on sourceNodeLocalId rather than on the record's position:
        // a clip must survive re-cooking its rig, and re-cooking renumbers records. FIRST MATCH
        // WINS, because a hostile file may repeat a localId and first-match is total.
        for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
            if (skeleton.joints[j].sourceNodeLocalId == target) {
                found = static_cast<std::uint32_t>(j);
                break;
            }
        }
        out[c] = found;
        if (found == assets::COOKED_SKELETON_INVALID_INDEX) {
            ++stats.unboundChannels;
        } else {
            ++stats.boundChannels;
        }
    }
    return stats;
}

void sampleAnimation(const assets::CookedAnimation& clip, std::span<const std::uint32_t> binding, float timeSeconds,
                     std::span<JointPose> pose) {
    assert(binding.size() == clip.channels.size() && "sampleAnimation: one binding entry per channel");
    const std::size_t count = std::min(binding.size(), clip.channels.size());  // release clamp

    for (std::size_t c = 0; c < count; ++c) {
        const std::uint32_t joint = binding[c];
        // Two distinct reasons to write nothing, both normal: the channel drives a node this rig does
        // not have, or a joint this pose span does not cover.
        if (joint == assets::COOKED_SKELETON_INVALID_INDEX || joint >= pose.size()) {
            continue;
        }

        const assets::CookedAnimationChannel& channel = clip.channels[c];
        const auto times = assets::channelTimeBytes(clip, static_cast<std::uint32_t>(c));
        const auto values = assets::channelValueBytes(clip, static_cast<std::uint32_t>(c));
        const bool cubic = channel.interpolation == assets::CookedAnimationInterpolation::CubicSpline;
        const bool rotation = channel.path == assets::CookedAnimationPath::Rotation;
        const Segment segment = locate(times, channel.keyCount, timeSeconds);

        Vec4 sampled{};
        if (segment.k1 == segment.k) {
            // The clamped end, and the one-key channel: glTF section 3.6.2 says the output holds at
            // the nearest key. For a cubic channel this is where the + 1 has no interpolation to
            // hide behind -- valueAt reads values[3k + 1], never the in-tangent at values[3k].
            sampled = valueAt(values, cubic, segment.k);
        } else if (channel.interpolation == assets::CookedAnimationInterpolation::Step) {
            sampled = valueAt(values, false, segment.k);
        } else if (cubic) {
            sampled = hermite(values, segment);
        } else if (rotation) {
            // LINEAR on a rotation is SLERP on the short path, and we CALL engine::slerp rather than
            // hand-rolling a second one: a second spelling of a rotation conversion is a second
            // place for a handedness mistake to live (skinning.cpp's poseMatrix rule).
            const Quat a = quatOf(valueAt(values, false, segment.k));
            const Quat b = quatOf(valueAt(values, false, segment.k1));
            sampled = vec4Of(engine::slerp(a, b, segment.u));
        } else {
            sampled = (valueAt(values, false, segment.k) * (1.0F - segment.u)) +
                      (valueAt(values, false, segment.k1) * segment.u);
        }

        // MEMBER-WISE, never a whole JointPose: a channel writes one of translation, rotation or
        // scale and leaves the other two exactly as the caller's bindPose left them. That contract
        // is the entire reason docs/09 section 12.3 stores bind LOCALS as TRS rather than baked
        // global matrices. No `default:` here, so a fourth path is a -Wswitch error rather than a
        // silent no-op.
        switch (channel.path) {
            case assets::CookedAnimationPath::Translation:
                pose[joint].translation = Vec3{sampled.x, sampled.y, sampled.z};
                break;
            case assets::CookedAnimationPath::Rotation:
                // In ALL THREE modes, not just cubic. The specification requires it for CUBICSPLINE and
                // warns that a cubic segment can produce an all-zero quaternion -- normalizeOrIdentity
                // turns exactly that into identity rather than NaN -- and the GLM slerp behind ours
                // returns an UNNORMALIZED componentwise mix on its near-parallel branch and preserves a
                // non-unit input's magnitude on the general one. One rule everywhere is cheaper to
                // state, cheaper to test and total under hostile data, for one sqrt per animated
                // rotation per frame.
                pose[joint].rotation = normalizeOrIdentity(quatOf(sampled));
                break;
            case assets::CookedAnimationPath::Scale:
                pose[joint].scale = Vec3{sampled.x, sampled.y, sampled.z};
                break;
        }
    }
}

}  // namespace engine::render
