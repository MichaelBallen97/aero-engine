// tests/render_animation_test.cpp -- task 3.5.2: the clip sampler (CL1-CL25). A TU of aero_tests,
// which supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// TIER 0 ONLY, and that is a claim rather than a gap: this task adds no draw path, no pipeline, no
// buffer and no bind, and the palette a clip produces is the same std::span<const Mat4> a sine
// produced -- which render_skinning_test.cpp's SN1-SN8 already cover on a real device. There is
// nothing here a GPU could witness that this file cannot.
//
// Every clip sampled below is built BY THIS FILE, byte by byte, from docs/09 section 13's own
// tables -- deliberately, so a cook bug can never mask a sampler bug, and so a channel can carry
// what the cook refuses to write: non-monotonic times (CL25), a non-unit stored quaternion (CL19),
// an all-zero one (CL18). Those are exactly the shapes a shipped, patched or crafted .aeroanim can
// present, and the sampler has to be total over all of them.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md). Enum comparisons
// are double-parenthesised, because engine::assets::toString is found by ADL from doctest's
// stringifier. Every case-local table pins a LITERAL row count, never TABLE.size() against itself.

#include <aero/assets/cooked_animation.hpp>
#include <aero/assets/cooked_skeleton.hpp>
#include <aero/core/math.hpp>
#include <aero/render/animation.hpp>
#include <aero/render/skinning.hpp>

#include "cooked_skeleton_golden.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

using engine::Guid;
using engine::Mat4;
using engine::Quat;
using engine::Vec3;
using engine::Vec4;
using engine::assets::COOKED_ANIMATION_CHANNEL_BYTES;
using engine::assets::COOKED_ANIMATION_FORMAT_VERSION;
using engine::assets::COOKED_ANIMATION_HEADER_BYTES;
using engine::assets::COOKED_ANIMATION_MAGIC;
using engine::assets::COOKED_SKELETON_INVALID_INDEX;
using engine::assets::CookedAnimation;
using engine::assets::CookedAnimationInterpolation;
using engine::assets::CookedAnimationParseResult;
using engine::assets::CookedAnimationPath;
using engine::assets::CookedAnimationStatus;
using engine::assets::cookedAnimationTimesPadding;
using engine::assets::CookedSkeleton;
using engine::assets::CookedSkeletonJoint;
using engine::assets::CookedSkeletonParseResult;
using engine::assets::CookedSkeletonStatus;
using engine::assets::parseCookedAnimation;
using engine::assets::parseCookedSkeleton;
using engine::render::AnimationBindStats;
using engine::render::bindAnimation;
using engine::render::bindPose;
using engine::render::computeJointPalette;
using engine::render::JointPose;
using engine::render::sampleAnimation;

namespace {

constexpr CookedAnimationPath TRANSLATION = CookedAnimationPath::Translation;
constexpr CookedAnimationPath ROTATION = CookedAnimationPath::Rotation;
constexpr CookedAnimationPath SCALE = CookedAnimationPath::Scale;
constexpr CookedAnimationInterpolation LINEAR = CookedAnimationInterpolation::Linear;
constexpr CookedAnimationInterpolation STEP = CookedAnimationInterpolation::Step;
constexpr CookedAnimationInterpolation CUBIC = CookedAnimationInterpolation::CubicSpline;

// This file's OWN copy of docs/09 section 13's offsets, spelled independently of the parser's and of
// cooked_animation_test.cpp's: a transposed offset must fail somewhere rather than agree with itself.
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_CHANNEL_COUNT = 36;
constexpr std::size_t H_KEY_COUNT = 40;
constexpr std::size_t H_VALUE_COUNT = 44;
constexpr std::size_t H_SOURCE_ANIMATION_INDEX = 48;
constexpr std::size_t H_DURATION_SECONDS = 52;
constexpr std::size_t H_TIMES_DATA_OFFSET = 56;
constexpr std::size_t H_VALUES_DATA_OFFSET = 64;
constexpr std::size_t H_TOTAL_BYTES = 72;

constexpr std::size_t C_TARGET_NODE_LOCAL_ID = 0;
constexpr std::size_t C_PATH = 4;
constexpr std::size_t C_INTERPOLATION = 6;
constexpr std::size_t C_KEY_COUNT = 8;
constexpr std::size_t C_FIRST_KEY = 12;
constexpr std::size_t C_FIRST_VALUE = 16;
constexpr std::size_t C_VALUE_COUNT = 20;

constexpr std::size_t TIME_BYTES = 4;
constexpr std::size_t VALUE_BYTES = 16;

// A Vec4 that reads like the glTF accessor it stands for. w defaults to 0, which is what a cooked
// Translation or Scale value carries.
[[nodiscard]] constexpr Vec4 v4(float x, float y, float z, float w = 0.0F) { return Vec4{x, y, z, w}; }

// One channel as this file authors it: times and values in full, with no constraint beyond the ones
// the PARSER imposes. The cook would refuse several of the shapes below; the format does not.
struct ChannelData {
    std::uint32_t node = 0;
    CookedAnimationPath path = TRANSLATION;
    CookedAnimationInterpolation interpolation = LINEAR;
    std::vector<float> times;
    std::vector<Vec4> values;
};

// A parsed clip and the buffer it spans. CookedAnimation RETAINS `bytes` as a span -- the whole
// promise of the format -- so the two have to travel together, and copying would leave the copy's
// span pointing into the original's buffer. Move-only, so that mistake cannot be made here: a move
// transfers the vector's heap block unchanged, which is exactly what keeps the span valid.
struct Clip {
    std::vector<std::byte> bytes;
    CookedAnimationParseResult parsed;

    Clip() = default;
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    Clip(Clip&&) = default;
    Clip& operator=(Clip&&) = default;
    ~Clip() = default;

    [[nodiscard]] const CookedAnimation& animation() const { return parsed.animation; }
};

// A well-formed .aeroanim for `channels`, laid out exactly as section 13.1 requires: the running
// firstKey/firstValue sums, the one padding site, and both bulk regions written through putF32.
// REQUIREs its own output parses Ok, so no case downstream can be reading a buffer this helper got
// wrong.
[[nodiscard]] Clip makeClip(std::span<const ChannelData> channels, Guid guid = Guid{},
                            std::uint32_t animationIndex = 0) {
    const auto channelCount = static_cast<std::uint32_t>(channels.size());
    std::uint32_t keyCount = 0;
    std::uint32_t valueCount = 0;
    float duration = 0.0F;
    for (const ChannelData& channel : channels) {
        keyCount += static_cast<std::uint32_t>(channel.times.size());
        valueCount += static_cast<std::uint32_t>(channel.values.size());
        if (!channel.times.empty() && channel.times.back() > duration) {
            duration = channel.times.back();
        }
    }

    const std::size_t timesOffset =
        COOKED_ANIMATION_HEADER_BYTES + (COOKED_ANIMATION_CHANNEL_BYTES * std::size_t{channelCount});
    const std::size_t valuesOffset =
        timesOffset + (TIME_BYTES * std::size_t{keyCount}) + cookedAnimationTimesPadding(keyCount);
    const std::size_t total = valuesOffset + (VALUE_BYTES * std::size_t{valueCount});

    Clip clip;
    clip.bytes.assign(total, std::byte{0});
    const std::span<std::byte> raw{clip.bytes};
    for (std::size_t i = 0; i < COOKED_ANIMATION_MAGIC.size(); ++i) {
        clip.bytes[i] = static_cast<std::byte>(COOKED_ANIMATION_MAGIC[i]);
    }
    engine::assets::putU32(raw, H_FORMAT_VERSION, COOKED_ANIMATION_FORMAT_VERSION);
    engine::assets::putU32(raw, H_COOKER_VERSION, 1);
    engine::assets::putU64(raw, H_GUID_HI, guid.hi);
    engine::assets::putU64(raw, H_GUID_LO, guid.lo);
    engine::assets::putU32(raw, H_CHANNEL_COUNT, channelCount);
    engine::assets::putU32(raw, H_KEY_COUNT, keyCount);
    engine::assets::putU32(raw, H_VALUE_COUNT, valueCount);
    engine::assets::putU32(raw, H_SOURCE_ANIMATION_INDEX, animationIndex);
    engine::assets::putF32(raw, H_DURATION_SECONDS, duration);
    engine::assets::putU64(raw, H_TIMES_DATA_OFFSET, timesOffset);
    engine::assets::putU64(raw, H_VALUES_DATA_OFFSET, valuesOffset);
    engine::assets::putU64(raw, H_TOTAL_BYTES, total);

    std::uint32_t firstKey = 0;
    std::uint32_t firstValue = 0;
    for (std::uint32_t i = 0; i < channelCount; ++i) {
        const ChannelData& channel = channels[i];
        const std::size_t rec = COOKED_ANIMATION_HEADER_BYTES + (COOKED_ANIMATION_CHANNEL_BYTES * std::size_t{i});
        const auto keys = static_cast<std::uint32_t>(channel.times.size());
        const auto values = static_cast<std::uint32_t>(channel.values.size());
        engine::assets::putU32(raw, rec + C_TARGET_NODE_LOCAL_ID, channel.node);
        engine::assets::putU16(raw, rec + C_PATH, static_cast<std::uint16_t>(channel.path));
        engine::assets::putU16(raw, rec + C_INTERPOLATION, static_cast<std::uint16_t>(channel.interpolation));
        engine::assets::putU32(raw, rec + C_KEY_COUNT, keys);
        engine::assets::putU32(raw, rec + C_FIRST_KEY, firstKey);
        engine::assets::putU32(raw, rec + C_FIRST_VALUE, firstValue);
        engine::assets::putU32(raw, rec + C_VALUE_COUNT, values);

        for (std::uint32_t k = 0; k < keys; ++k) {
            engine::assets::putF32(raw, timesOffset + (TIME_BYTES * std::size_t{firstKey + k}), channel.times[k]);
        }
        for (std::uint32_t v = 0; v < values; ++v) {
            const std::size_t at = valuesOffset + (VALUE_BYTES * std::size_t{firstValue + v});
            engine::assets::putF32(raw, at + 0, channel.values[v].x);
            engine::assets::putF32(raw, at + 4, channel.values[v].y);
            engine::assets::putF32(raw, at + 8, channel.values[v].z);
            engine::assets::putF32(raw, at + 12, channel.values[v].w);
        }
        firstKey += keys;
        firstValue += values;
    }

    clip.parsed = parseCookedAnimation(std::span<const std::byte>(clip.bytes));
    REQUIRE((clip.parsed.status == CookedAnimationStatus::Ok));
    return clip;
}

// A flat skeleton: one record per localId, every record a root, palette slot == record index. The
// localIds are given in RECORD order, so a case can hand them in an order that disagrees with their
// numeric order -- which is the whole of CL6.
[[nodiscard]] CookedSkeleton makeSkeleton(std::span<const std::uint32_t> localIds) {
    CookedSkeleton skeleton;
    skeleton.formatVersion = 1;
    skeleton.cookerVersion = 1;
    skeleton.paletteJointCount = static_cast<std::uint32_t>(localIds.size());
    for (std::uint32_t i = 0; i < localIds.size(); ++i) {
        CookedSkeletonJoint joint;
        joint.parent = COOKED_SKELETON_INVALID_INDEX;
        joint.paletteSlot = i;
        joint.sourceNodeLocalId = localIds[i];
        skeleton.joints.push_back(joint);
    }
    return skeleton;
}

[[nodiscard]] std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }

[[nodiscard]] bool sameBits(Vec3 a, Vec3 b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z);
}

[[nodiscard]] bool sameBits(Quat a, Quat b) {
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z) && bits(a.w) == bits(b.w);
}

[[nodiscard]] bool allFinite(Quat q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

[[nodiscard]] bool allFinite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

[[nodiscard]] Quat aboutZ(float degrees) { return engine::fromAxisAngle(Vec3::unitZ(), engine::radians(degrees)); }

[[nodiscard]] Quat quatOf(Vec4 v) { return Quat{v.x, v.y, v.z, v.w}; }

// The three-mode fixture CL7, CL8 and CL23 all clamp against: one channel per interpolation mode, on
// three different joints, with the FIRST key at t = 1 so a sample at 0 is genuinely below the range.
// Its cubic channel is what makes the clamp arms read a value rather than a tangent: key 0's
// in-tangent is (5,5,5) and its VALUE is (2,4,8), and key 1's value is (7,7,7).
[[nodiscard]] Clip threeModeClip() {
    const std::array<ChannelData, 3> channels = {
        ChannelData{0, TRANSLATION, STEP, {1.0F, 2.0F}, {v4(1, 2, 3), v4(9, 9, 9)}},
        ChannelData{1, ROTATION, LINEAR, {1.0F, 2.0F}, {v4(0, 0, 0, 1), v4(0, 0, 1, 0)}},
        ChannelData{2,
                    SCALE,
                    CUBIC,
                    {1.0F, 2.0F},
                    {v4(5, 5, 5), v4(2, 4, 8), v4(0, 0, 0), v4(0, 0, 0), v4(7, 7, 7), v4(0, 0, 0)}},
    };
    return makeClip(channels);
}

// The skeleton threeModeClip() binds against: localIds 0, 1, 2 in record order.
[[nodiscard]] CookedSkeleton threeModeSkeleton() {
    const std::array<std::uint32_t, 3> ids = {0, 1, 2};
    return makeSkeleton(ids);
}

}  // namespace

// ---- bindAnimation ------------------------------------------------------------------------------

TEST_CASE("clip sampler: bindAnimation resolves every channel to its joint record index (CL1)") {
    const std::array<std::uint32_t, 3> ids = {0, 1, 2};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    REQUIRE(skeleton.joints.size() == 3);

    const std::array<ChannelData, 3> channels = {
        ChannelData{2, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
        ChannelData{0, ROTATION, LINEAR, {0.0F}, {v4(0, 0, 0, 1)}},
        ChannelData{1, SCALE, LINEAR, {0.0F}, {v4(1, 1, 1)}},
    };
    const Clip clip = makeClip(channels);
    REQUIRE(clip.animation().channels.size() == 3);

    std::array<std::uint32_t, 3> binding{};
    const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);

    CHECK(binding[0] == 2);
    CHECK(binding[1] == 0);
    CHECK(binding[2] == 1);
    CHECK(stats.channelCount == 3);
    CHECK(stats.boundChannels == 3);
    CHECK(stats.unboundChannels == 0);
}

TEST_CASE("clip sampler: a channel the skeleton has no node for binds to INVALID, and is counted (CL2)") {
    const std::array<std::uint32_t, 2> ids = {10, 20};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    REQUIRE(skeleton.joints.size() == 2);

    // The middle channel targets node 99, which this rig does not contain. That is a NORMAL glTF
    // shape -- a clip animating a camera or a mesh node alongside its joints -- not an error.
    const std::array<ChannelData, 3> channels = {
        ChannelData{10, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
        ChannelData{99, TRANSLATION, STEP, {0.0F}, {v4(2, 0, 0)}},
        ChannelData{20, SCALE, STEP, {0.0F}, {v4(3, 3, 3)}},
    };
    const Clip clip = makeClip(channels);

    std::array<std::uint32_t, 3> binding{};
    const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);

    CHECK(binding[0] == 0);
    CHECK(binding[1] == COOKED_SKELETON_INVALID_INDEX);
    CHECK(binding[2] == 1);
    // Three literals, none of them derived from another: a reporter that answered channelCount for
    // boundChannels would satisfy any two of these on their own.
    CHECK(stats.channelCount == 3);
    CHECK(stats.boundChannels == 2);
    CHECK(stats.unboundChannels == 1);
}

TEST_CASE("clip sampler: a clip that matches no joint at all binds entirely to INVALID (CL3)") {
    const std::array<std::uint32_t, 2> ids = {10, 20};
    const CookedSkeleton skeleton = makeSkeleton(ids);

    const std::array<ChannelData, 2> channels = {
        ChannelData{77, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
        ChannelData{88, ROTATION, STEP, {0.0F}, {v4(0, 0, 0, 1)}},
    };
    const Clip clip = makeClip(channels);

    std::array<std::uint32_t, 2> binding{};
    const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);

    CHECK(binding[0] == COOKED_SKELETON_INVALID_INDEX);
    CHECK(binding[1] == COOKED_SKELETON_INVALID_INDEX);
    CHECK(stats.channelCount == 2);
    CHECK(stats.boundChannels == 0);
    CHECK(stats.unboundChannels == 2);
    // And it is still not a refusal: bindAnimation reports, the caller decides.
}

TEST_CASE("clip sampler: sourceGuidMatches reports both answers on otherwise identical inputs (CL4)") {
    const Guid asset{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const Guid other{0x1111111111111111ULL, 0x2222222222222222ULL};

    const std::array<std::uint32_t, 1> ids = {10};
    CookedSkeleton skeleton = makeSkeleton(ids);

    const std::array<ChannelData, 1> channels = {
        ChannelData{10, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
    };
    const Clip clip = makeClip(channels, asset);
    const Clip nilClip = makeClip(channels);
    std::array<std::uint32_t, 1> binding{};

    skeleton.sourceGuid = asset;
    CHECK(bindAnimation(clip.animation(), skeleton, binding).sourceGuidMatches);

    skeleton.sourceGuid = other;
    CHECK_FALSE(bindAnimation(clip.animation(), skeleton, binding).sourceGuidMatches);

    // Nil == nil is a MATCH, not a special case: two artifacts cooked without a source GUID agree
    // about what they came from just as surely as two that carry one.
    skeleton.sourceGuid = Guid{};
    CHECK(bindAnimation(nilClip.animation(), skeleton, binding).sourceGuidMatches);
    CHECK_FALSE(bindAnimation(clip.animation(), skeleton, binding).sourceGuidMatches);

    // The binding itself is unaffected by the GUID either way -- a mismatch is a report, not a veto.
    CHECK(binding[0] == 0);
}

TEST_CASE("clip sampler: bindAnimation clamps an out span that is not the channel count (CL5)") {
    const std::array<std::uint32_t, 3> ids = {10, 20, 30};
    const CookedSkeleton skeleton = makeSkeleton(ids);

    const std::array<ChannelData, 3> channels = {
        ChannelData{10, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
        ChannelData{20, TRANSLATION, STEP, {0.0F}, {v4(2, 0, 0)}},
        ChannelData{30, TRANSLATION, STEP, {0.0F}, {v4(3, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    REQUIRE(clip.animation().channels.size() == 3);

    {
        // The contract itself: an exactly-sized span is filled exactly, in every configuration.
        std::array<std::uint32_t, 3> binding{};
        const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);
        CHECK(binding[0] == 0);
        CHECK(binding[1] == 1);
        CHECK(binding[2] == 2);
        CHECK(stats.boundChannels == 3);
    }

#if defined(NDEBUG)
    // The two MISMATCHED arms live behind NDEBUG by necessity rather than by preference. The
    // contract is bindPose's -- out.size() == channelCount, debug-ASSERTED and release-CLAMPED -- so
    // handing it a mismatched span in a Debug build aborts the binary at the assert instead of
    // reaching the clamp. The clamp is real code on the shipping lane, both macOS presets and all
    // three CI lanes build Release, and this is the only tier that can execute it.
    {
        constexpr std::uint32_t SENTINEL = 0xABCDEF01U;
        std::array<std::uint32_t, 3> storage = {SENTINEL, SENTINEL, SENTINEL};
        const std::span<std::uint32_t> shortSpan{storage.data(), 2};
        const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, shortSpan);
        CHECK(storage[0] == 0);
        CHECK(storage[1] == 1);
        CHECK(storage[2] == SENTINEL);  // untouched: the write stopped at the span, not at the clip
        CHECK(stats.channelCount == 3);
        CHECK(stats.boundChannels == 2);
        CHECK(stats.unboundChannels == 0);
    }
    {
        constexpr std::uint32_t SENTINEL = 0xABCDEF01U;
        std::array<std::uint32_t, 5> storage = {SENTINEL, SENTINEL, SENTINEL, SENTINEL, SENTINEL};
        const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, storage);
        CHECK(storage[0] == 0);
        CHECK(storage[1] == 1);
        CHECK(storage[2] == 2);
        CHECK(storage[3] == SENTINEL);  // a longer span is not padded with anything
        CHECK(storage[4] == SENTINEL);
        CHECK(stats.channelCount == 3);
        CHECK(stats.boundChannels == 3);
    }
#endif
}

TEST_CASE("clip sampler: binding matches on sourceNodeLocalId, never on record index (CL6)") {
    // Record order 30, 10, 20 -- deliberately NOT the numeric order, and no localId equals its own
    // record index. A bind that compared the channel's target against the record INDEX would answer
    // INVALID for all three here, while it answers correctly for a skeleton whose ids happen to run
    // 0, 1, 2 (which is why CL1 cannot see this).
    const std::array<std::uint32_t, 3> ids = {30, 10, 20};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    REQUIRE(skeleton.joints.size() == 3);
    REQUIRE(skeleton.joints[0].sourceNodeLocalId == 30);
    REQUIRE(skeleton.joints[1].sourceNodeLocalId == 10);
    REQUIRE(skeleton.joints[2].sourceNodeLocalId == 20);

    const std::array<ChannelData, 3> channels = {
        ChannelData{10, TRANSLATION, STEP, {0.0F}, {v4(1, 0, 0)}},
        ChannelData{20, TRANSLATION, STEP, {0.0F}, {v4(2, 0, 0)}},
        ChannelData{30, TRANSLATION, STEP, {0.0F}, {v4(3, 0, 0)}},
    };
    const Clip clip = makeClip(channels);

    std::array<std::uint32_t, 3> binding{};
    const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);

    CHECK(binding[0] == 1);
    CHECK(binding[1] == 2);
    CHECK(binding[2] == 0);
    CHECK(stats.boundChannels == 3);
    CHECK(stats.unboundChannels == 0);
}

// ---- sampleAnimation: the two clamps ------------------------------------------------------------

TEST_CASE("clip sampler: a time before the first key clamps to key 0, in all three modes (CL7)") {
    const Clip clip = threeModeClip();
    const CookedSkeleton skeleton = threeModeSkeleton();
    std::array<std::uint32_t, 3> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 3);

    constexpr std::array<float, 2> BELOW = {-3.0F, 0.0F};  // both strictly below the first key at t = 1
    REQUIRE(BELOW.size() == 2);
    for (const float when : BELOW) {
        CAPTURE(when);
        std::array<JointPose, 3> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, when, pose);

        CHECK(pose[0].translation == Vec3{1.0F, 2.0F, 3.0F});
        CHECK(engine::approxEquals(pose[1].rotation, Quat::identity()));
        // The cubic arm is the one that matters: key 0's IN-TANGENT is (5,5,5) and its VALUE is
        // (2,4,8). A clamp path reading values[3k] instead of values[3k + 1] answers the tangent.
        CHECK(pose[2].scale == Vec3{2.0F, 4.0F, 8.0F});
    }
}

TEST_CASE("clip sampler: a time after the last key clamps to key n-1, in all three modes (CL8)") {
    const Clip clip = threeModeClip();
    const CookedSkeleton skeleton = threeModeSkeleton();
    std::array<std::uint32_t, 3> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 3);

    constexpr std::array<float, 2> ABOVE = {2.0F, 100.0F};  // the last key itself, and far past it
    REQUIRE(ABOVE.size() == 2);
    for (const float when : ABOVE) {
        CAPTURE(when);
        std::array<JointPose, 3> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, when, pose);

        CHECK(pose[0].translation == Vec3{9.0F, 9.0F, 9.0F});
        CHECK(engine::approxEquals(pose[1].rotation, Quat{0.0F, 0.0F, 1.0F, 0.0F}));
        CHECK(pose[2].scale == Vec3{7.0F, 7.0F, 7.0F});
    }
}

TEST_CASE("clip sampler: an exact key timestamp returns that key's value bit-identically (CL9)") {
    // Selecting the LARGEST k with t[k] <= tc is glTF's exact-hit rule: an exact timestamp gives
    // u == 0 and every mode returns value(k) unmodified. A lower_bound-shaped search would put the
    // hit at u == 1 of the PREVIOUS segment -- the wrong key outright for Step, and for Linear a
    // (1-1)*a + 1*b that is only accidentally equal to b.
    const std::array<ChannelData, 4> channels = {
        ChannelData{0, TRANSLATION, STEP, {0.0F, 1.0F, 2.0F}, {v4(1, 2, 3), v4(4, 5, 6), v4(7, 8, 9)}},
        ChannelData{1, TRANSLATION, LINEAR, {0.0F, 1.0F, 2.0F}, {v4(1, 2, 3), v4(4, 5, 6), v4(7, 8, 9)}},
        ChannelData{2,
                    SCALE,
                    CUBIC,
                    {0.0F, 1.0F, 2.0F},
                    {v4(-1, -1, -1), v4(2, 3, 4), v4(5, 5, 5), v4(6, 6, 6), v4(8, 9, 10), v4(-2, -2, -2), v4(3, 3, 3),
                     v4(11, 12, 13), v4(0, 0, 0)}},
        ChannelData{3, ROTATION, LINEAR, {0.0F, 1.0F, 2.0F}, {v4(0, 0, 0, 1), v4(0, 0, 1, 0), v4(0, 0, 0, -1)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 4> ids = {0, 1, 2, 3};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 4> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 4);

    std::array<JointPose, 4> pose{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 1.0F, pose);

    CHECK(sameBits(pose[0].translation, Vec3{4.0F, 5.0F, 6.0F}));
    CHECK(sameBits(pose[1].translation, Vec3{4.0F, 5.0F, 6.0F}));
    CHECK(sameBits(pose[2].scale, Vec3{8.0F, 9.0F, 10.0F}));
    // Bit-identity is NOT claimed for a sampled rotation, and the reason is structural rather than
    // conservative: a Linear rotation goes through slerp (a division by sin(angle)) and then through
    // normalizeOrIdentity (a division by a square root), neither of which is the identity map on the
    // stored bits even at u == 0. The KEY it lands on is what this arm pins.
    CHECK(engine::approxEquals(pose[3].rotation, Quat{0.0F, 0.0F, 1.0F, 0.0F}, 1e-5F));
}

TEST_CASE("clip sampler: STEP holds a key across its whole segment and switches exactly at the next (CL10)") {
    const std::array<ChannelData, 1> channels = {
        ChannelData{0, TRANSLATION, STEP, {0.0F, 1.0F, 2.0F}, {v4(1, 0, 0), v4(2, 0, 0), v4(3, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    struct Row {
        float when;
        float expected;
    };
    // Seven rows, and the switch points are what they are for: 0.999 -> 1 and 1.0 -> 2 straddle the
    // key exactly, and 1.999 -> 2 with 2.0 -> 3 does it again at the last one.
    constexpr std::array<Row, 7> ROWS = {Row{0.0F, 1.0F},   Row{0.999F, 1.0F}, Row{1.0F, 2.0F}, Row{1.001F, 2.0F},
                                         Row{1.999F, 2.0F}, Row{2.0F, 3.0F},   Row{3.0F, 3.0F}};
    REQUIRE(ROWS.size() == 7);

    for (const Row& row : ROWS) {
        CAPTURE(row.when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);
        CHECK(pose[0].translation.x == row.expected);
        CHECK(pose[0].translation.y == 0.0F);
    }
}

// ---- sampleAnimation: LINEAR --------------------------------------------------------------------

TEST_CASE("clip sampler: LINEAR translation interpolates componentwise against hand-computed values (CL11)") {
    // times {0, 2}, so t = 0.5 / 1.0 / 1.5 are u = 0.25 / 0.5 / 0.75 and the segment is NOT unit
    // length -- a sampler that divided by nothing, or by the wrong pair, misses every row.
    const std::array<ChannelData, 1> channels = {
        ChannelData{0, TRANSLATION, LINEAR, {0.0F, 2.0F}, {v4(1, 2, 3), v4(5, 10, -5)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    struct Row {
        float when;
        Vec3 expected;
    };
    constexpr std::array<Row, 3> ROWS = {Row{0.5F, Vec3{2.0F, 4.0F, 1.0F}}, Row{1.0F, Vec3{3.0F, 6.0F, -1.0F}},
                                         Row{1.5F, Vec3{4.0F, 8.0F, -3.0F}}};
    REQUIRE(ROWS.size() == 3);

    for (const Row& row : ROWS) {
        CAPTURE(row.when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);
        CHECK(engine::approxEquals(pose[0].translation, row.expected));
    }
}

TEST_CASE("clip sampler: LINEAR scale interpolates componentwise against hand-computed values (CL12)") {
    const std::array<ChannelData, 1> channels = {
        ChannelData{0, SCALE, LINEAR, {0.0F, 2.0F}, {v4(1, 1, 1), v4(3, 5, 9)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    struct Row {
        float when;
        Vec3 expected;
    };
    constexpr std::array<Row, 3> ROWS = {Row{0.5F, Vec3{1.5F, 2.0F, 3.0F}}, Row{1.0F, Vec3{2.0F, 3.0F, 5.0F}},
                                         Row{1.5F, Vec3{2.5F, 4.0F, 7.0F}}};
    REQUIRE(ROWS.size() == 3);

    for (const Row& row : ROWS) {
        CAPTURE(row.when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);
        CHECK(engine::approxEquals(pose[0].scale, row.expected));
        // The translation the bind pose put there is untouched by a scale channel.
        CHECK(pose[0].translation == Vec3{0.0F, 0.0F, 0.0F});
    }
}

TEST_CASE("clip sampler: LINEAR on a rotation is SLERP, and at 179 degrees that is measurable (CL13)") {
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);

    {
        // 90 degrees about +Z, sampled at three fractions. slerp is constant angular velocity, so
        // the answer at u is exactly the rotation of 90*u degrees.
        const std::array<ChannelData, 1> channels = {
            ChannelData{0, ROTATION, LINEAR, {0.0F, 1.0F}, {v4(0, 0, 0, 1), v4(0, 0, 0.70710678F, 0.70710678F)}},
        };
        const Clip clip = makeClip(channels);
        std::array<std::uint32_t, 1> binding{};
        REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

        constexpr std::array<float, 3> FRACTIONS = {0.25F, 0.5F, 0.75F};
        REQUIRE(FRACTIONS.size() == 3);
        for (const float u : FRACTIONS) {
            CAPTURE(u);
            std::array<JointPose, 1> pose{};
            bindPose(skeleton, pose);
            sampleAnimation(clip.animation(), binding, u, pose);
            CHECK(engine::approxEquals(pose[0].rotation, aboutZ(90.0F * u), 1e-4F));
        }
    }

    {
        // 179 degrees, sampled at u = 0.25. The MIDPOINT is the one fraction where slerp and a
        // normalized lerp agree by symmetry, whatever the separation -- so a midpoint arm cannot
        // see the difference at any angle, and this arm deliberately is not one.
        const Quat wide = aboutZ(179.0F);
        const std::array<ChannelData, 1> channels = {
            ChannelData{0, ROTATION, LINEAR, {0.0F, 1.0F}, {v4(0, 0, 0, 1), v4(wide.x, wide.y, wide.z, wide.w)}},
        };
        const Clip clip = makeClip(channels);
        std::array<std::uint32_t, 1> binding{};
        REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, 0.25F, pose);

        CHECK(engine::approxEquals(pose[0].rotation, aboutZ(179.0F * 0.25F), 1e-4F));
        // And the discriminating half: the normalized lerp of the same pair, at the same u, is a
        // DIFFERENT rotation -- 0.065 apart componentwise, four hundred times the tolerance below.
        const Quat nlerp = engine::lerp(Quat::identity(), wide, 0.25F);
        CHECK_FALSE(engine::approxEquals(pose[0].rotation, nlerp, 1e-2F));
    }
}

TEST_CASE("clip sampler: an antipodal rotation pair takes the SHORT path (CL14)") {
    // The stored pair is (identity, -q90): dot is negative, so the two quaternions are more than a
    // quarter turn apart as vectors while the ROTATIONS they encode are 90 degrees apart. Without
    // the short-path negation the interpolation goes the long way round the sphere and produces a
    // -135 degree rotation about Z at the midpoint instead of +45.
    const Quat q90 = aboutZ(90.0F);
    const Vec4 stored = v4(-q90.x, -q90.y, -q90.z, -q90.w);
    REQUIRE(engine::dot(Quat::identity(), quatOf(stored)) < 0.0F);

    const std::array<ChannelData, 1> channels = {
        ChannelData{0, ROTATION, LINEAR, {0.0F, 1.0F}, {v4(0, 0, 0, 1), stored}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    std::array<JointPose, 1> pose{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 0.5F, pose);

    CHECK(engine::approxEquals(pose[0].rotation, aboutZ(45.0F), 1e-4F));
    // Stated the second way, because approxEquals forgives the double cover and this does not: the
    // result is nearer the NEGATED stored target than the stored one. The long way round flips this.
    const Quat negated = quatOf(v4(-stored.x, -stored.y, -stored.z, -stored.w));
    CHECK(engine::dot(pose[0].rotation, negated) > engine::dot(pose[0].rotation, quatOf(stored)));
    CHECK(engine::dot(pose[0].rotation, negated) > 0.9F);
}

// ---- sampleAnimation: CUBICSPLINE ---------------------------------------------------------------

TEST_CASE("clip sampler: CUBICSPLINE is Hermite with td-scaled tangents, hand-computed (CL15)") {
    // times {0, 2}, so t_d = 2 and a sampler that forgot to scale the tangents by the segment
    // duration is off by a factor of two on both tangent terms. Per key the storage is
    // [inTangent, value, outTangent]; key 0's in-tangent and key 1's out-tangent are unused, exactly
    // as the specification says exporters SHOULD zero them.
    //
    //   v0 = (1,2,3)  b0 = (4,0,-4)      a1 = (-2,8,2)  v1 = (3,6,9)
    //   v(u) = (2u^3-3u^2+1)v0 + 2(u^3-2u^2+u)b0 + (-2u^3+3u^2)v1 + 2(u^3-u^2)a1
    const std::array<ChannelData, 1> channels = {
        ChannelData{0,
                    SCALE,
                    CUBIC,
                    {0.0F, 2.0F},
                    {v4(0, 0, 0), v4(1, 2, 3), v4(4, 0, -4), v4(-2, 8, 2), v4(3, 6, 9), v4(0, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    struct Row {
        float when;
        Vec3 expected;
    };
    constexpr std::array<Row, 3> ROWS = {Row{0.5F, Vec3{2.625F, 1.875F, 2.625F}}, Row{1.0F, Vec3{3.5F, 2.0F, 4.5F}},
                                         Row{1.5F, Vec3{3.625F, 3.125F, 7.125F}}};
    REQUIRE(ROWS.size() == 3);

    for (const Row& row : ROWS) {
        CAPTURE(row.when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);
        CHECK(engine::approxEquals(pose[0].scale, row.expected, 1e-5F));
    }
}

TEST_CASE("clip sampler: a ONE-KEY cubic channel returns values[1], never values[0] (CL16)") {
    // The single-key channel is where the +1 has no interpolation to hide behind: locate answers the
    // clamped segment immediately, and the clamp arm reads a KEY VALUE. Reading values[3k] here
    // returns the in-tangent (9,9,9) as a scale.
    const std::array<ChannelData, 1> channels = {
        ChannelData{0, SCALE, CUBIC, {0.0F}, {v4(9, 9, 9), v4(1, 2, 3), v4(7, 7, 7)}},
    };
    const Clip clip = makeClip(channels);
    REQUIRE(clip.animation().channels.size() == 1);
    REQUIRE(clip.animation().channels[0].keyCount == 1);
    REQUIRE(clip.animation().channels[0].valueCount == 3);

    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    constexpr std::array<float, 3> WHENS = {-5.0F, 0.0F, 5.0F};
    REQUIRE(WHENS.size() == 3);
    for (const float when : WHENS) {
        CAPTURE(when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, when, pose);
        CHECK(pose[0].scale == Vec3{1.0F, 2.0F, 3.0F});
    }
}

TEST_CASE("clip sampler: the cubic CLAMP-HIGH path reads values[3(n-1) + 1] (CL17)") {
    // The fourth of the four places the [inTangent, value, outTangent] layout is read. The last
    // key's neighbours are deliberately loud: its in-tangent is (99,99,99) and its out-tangent
    // (-99,-99,-99), so reading either instead of the value is unmistakable.
    const std::array<ChannelData, 1> channels = {
        ChannelData{0,
                    SCALE,
                    CUBIC,
                    {0.0F, 1.0F},
                    {v4(0, 0, 0), v4(1, 1, 1), v4(0, 0, 0), v4(99, 99, 99), v4(5, 6, 7), v4(-99, -99, -99)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    std::array<JointPose, 1> pose{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 50.0F, pose);
    CHECK(pose[0].scale == Vec3{5.0F, 6.0F, 7.0F});
}

// ---- sampleAnimation: rotations are always normalized -------------------------------------------

TEST_CASE("clip sampler: an all-zero interpolated quaternion becomes IDENTITY, not NaN (CL18)") {
    // The specification warns that a cubic rotation segment can produce an all-zero quaternion, and
    // this fixture produces exactly one: with both tangents zero the Hermite midpoint of (0,0,0,1)
    // and (0,0,0,-1) is 0.5*q + 0.5*(-q) = (0,0,0,0). Normalizing that would divide by zero;
    // normalizeOrIdentity answers identity.
    const std::array<ChannelData, 2> channels = {
        ChannelData{0,
                    ROTATION,
                    CUBIC,
                    {0.0F, 2.0F},
                    {v4(0, 0, 0, 0), v4(0, 0, 0, 1), v4(0, 0, 0, 0), v4(0, 0, 0, 0), v4(0, 0, 0, -1), v4(0, 0, 0, 0)}},
        // And the same rule in a mode with no interpolation at all: a STORED zero quaternion.
        ChannelData{1, ROTATION, STEP, {0.0F, 2.0F}, {v4(0, 0, 0, 0), v4(0, 0, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 2> ids = {0, 1};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 2> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 2);

    std::array<JointPose, 2> pose{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 1.0F, pose);

    CHECK(allFinite(pose[0].rotation));
    CHECK(allFinite(pose[1].rotation));
    CHECK(pose[0].rotation == Quat::identity());
    CHECK(pose[1].rotation == Quat::identity());
}

TEST_CASE("clip sampler: a stored NON-UNIT quaternion comes back unit, in all three modes (CL19)") {
    const std::array<ChannelData, 3> channels = {
        // Step: (0,0,3,4) has length 5 and normalizes to (0,0,0.6,0.8).
        ChannelData{0, ROTATION, STEP, {0.0F, 1.0F}, {v4(0, 0, 3, 4), v4(0, 0, 0, 5)}},
        // Linear on a NEARLY PARALLEL non-unit pair: dot is 4, comfortably past 1 - epsilon, so the
        // slerp behind ours takes its near-parallel branch -- an UNNORMALIZED componentwise mix.
        // Trusting that branch to return a unit quaternion is exactly what this arm refuses.
        ChannelData{1, ROTATION, LINEAR, {0.0F, 1.0F}, {v4(0, 0, 0, 2), v4(0, 0, 0.001F, 2)}},
        // Cubic: the midpoint of (0,0,0,3) and (0,0,3,0) with zero tangents is (0,0,1.5,1.5).
        ChannelData{2,
                    ROTATION,
                    CUBIC,
                    {0.0F, 1.0F},
                    {v4(0, 0, 0, 0), v4(0, 0, 0, 3), v4(0, 0, 0, 0), v4(0, 0, 0, 0), v4(0, 0, 3, 0), v4(0, 0, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 3> ids = {0, 1, 2};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 3> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 3);

    std::array<JointPose, 3> pose{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 0.5F, pose);

    for (std::size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(allFinite(pose[i].rotation));
        CHECK(engine::approxEquals(engine::lengthSquared(pose[i].rotation), 1.0F, 1e-5F));
    }
    // The Step arm additionally pins the DIRECTION, so "unit" cannot be satisfied by any unit value.
    CHECK(engine::approxEquals(pose[0].rotation, Quat{0.0F, 0.0F, 0.6F, 0.8F}, 1e-5F));
}

// ---- sampleAnimation: what it must NOT touch ----------------------------------------------------

TEST_CASE("clip sampler: a rotation-only clip leaves translation and scale bit-identical (CL20)") {
    // The contract that makes the skeleton format's bind LOCALS worth storing as TRS: a clip drives
    // T, R and S member-wise, so a rotation channel writes a rotation and nothing else. A sampler
    // that assembled a whole JointPose and assigned it would silently reset the other two members
    // to their defaults -- (0,0,0) and (1,1,1) -- which is invisible on a rig whose bind pose is
    // identity and catastrophic on every rig that is not.
    CookedSkeleton skeleton;
    skeleton.formatVersion = 1;
    skeleton.cookerVersion = 1;
    skeleton.paletteJointCount = 1;
    CookedSkeletonJoint joint;
    joint.parent = COOKED_SKELETON_INVALID_INDEX;
    joint.paletteSlot = 0;
    joint.sourceNodeLocalId = 5;
    joint.translation = Vec3{1.0F, 2.0F, 3.0F};
    joint.rotation = aboutZ(30.0F);
    joint.scale = Vec3{2.0F, 3.0F, 4.0F};
    skeleton.joints.push_back(joint);

    const std::array<ChannelData, 1> channels = {
        ChannelData{5, ROTATION, LINEAR, {0.0F, 1.0F}, {v4(0, 0, 0, 1), v4(0, 0, 0.70710678F, 0.70710678F)}},
    };
    const Clip clip = makeClip(channels);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    std::array<JointPose, 1> pose{};
    bindPose(skeleton, pose);
    const Vec3 bindTranslation = pose[0].translation;
    const Vec3 bindScale = pose[0].scale;
    const Quat bindRotation = pose[0].rotation;
    REQUIRE(sameBits(bindTranslation, Vec3{1.0F, 2.0F, 3.0F}));
    REQUIRE(sameBits(bindScale, Vec3{2.0F, 3.0F, 4.0F}));

    sampleAnimation(clip.animation(), binding, 0.5F, pose);

    CHECK(sameBits(pose[0].translation, bindTranslation));
    CHECK(sameBits(pose[0].scale, bindScale));
    CHECK_FALSE(sameBits(pose[0].rotation, bindRotation));
    CHECK(engine::approxEquals(pose[0].rotation, aboutZ(45.0F), 1e-4F));
}

TEST_CASE("clip sampler: an UNBOUND channel writes nothing at all (CL21)") {
    const std::array<std::uint32_t, 1> ids = {5};
    const CookedSkeleton skeleton = makeSkeleton(ids);

    const std::array<ChannelData, 1> channels = {
        ChannelData{99, TRANSLATION, LINEAR, {0.0F, 1.0F}, {v4(10, 20, 30), v4(40, 50, 60)}},
    };
    const Clip clip = makeClip(channels);
    std::array<std::uint32_t, 1> binding{};
    const AnimationBindStats stats = bindAnimation(clip.animation(), skeleton, binding);
    REQUIRE(binding[0] == COOKED_SKELETON_INVALID_INDEX);
    REQUIRE(stats.unboundChannels == 1);

    std::array<JointPose, 1> pose{};
    bindPose(skeleton, pose);
    const JointPose before = pose[0];

    constexpr std::array<float, 4> WHENS = {-1.0F, 0.0F, 0.5F, 2.0F};
    REQUIRE(WHENS.size() == 4);
    for (const float when : WHENS) {
        CAPTURE(when);
        sampleAnimation(clip.animation(), binding, when, pose);
        CHECK(sameBits(pose[0].translation, before.translation));
        CHECK(sameBits(pose[0].rotation, before.rotation));
        CHECK(sameBits(pose[0].scale, before.scale));
    }
}

TEST_CASE("clip sampler: a joint index past the pose span is skipped rather than written (CL22)") {
    // Two ways the same read can go wrong, both reached by handing the sampler a binding this file
    // wrote rather than one bindAnimation produced: an index that is simply too large, and a pose
    // span narrower than the rig it came from. ASan on the Debug lane is the other half of both
    // assertions -- a write here lands outside a std::array.
    const std::array<ChannelData, 1> channels = {
        ChannelData{0, TRANSLATION, STEP, {0.0F}, {v4(7, 7, 7)}},
    };
    const Clip clip = makeClip(channels);

    {
        const std::array<std::uint32_t, 1> binding = {99};
        std::array<JointPose, 2> pose{};
        const JointPose before = pose[0];
        sampleAnimation(clip.animation(), binding, 0.0F, pose);
        CHECK(sameBits(pose[0].translation, before.translation));
        CHECK(sameBits(pose[1].translation, before.translation));
    }
    {
        // A pose span narrower than the skeleton: joint 2 exists in the rig and not in this span.
        const std::array<std::uint32_t, 1> binding = {2};
        std::array<JointPose, 3> storage{};
        const std::span<JointPose> narrow{storage.data(), 1};
        sampleAnimation(clip.animation(), binding, 0.0F, narrow);
        CHECK(sameBits(storage[0].translation, Vec3{0.0F, 0.0F, 0.0F}));
        CHECK(sameBits(storage[2].translation, Vec3{0.0F, 0.0F, 0.0F}));
    }
    {
        // And the same span, with the index inside it: the guard is a bound, not a blanket refusal.
        const std::array<std::uint32_t, 1> binding = {0};
        std::array<JointPose, 3> storage{};
        const std::span<JointPose> narrow{storage.data(), 1};
        sampleAnimation(clip.animation(), binding, 0.0F, narrow);
        CHECK(storage[0].translation == Vec3{7.0F, 7.0F, 7.0F});
    }
}

// ---- sampleAnimation: totality over hostile times ------------------------------------------------

TEST_CASE("clip sampler: a NON-FINITE time is total, in all three modes (CL23)") {
    // The low clamp is written !(tc > t[0]) rather than tc <= t[0], one predicate covering three
    // cases: below the range, exactly at t[0], and NaN. The two-comparison form lets a NaN through
    // to u = (NaN - t0)/td and turns every interpolator into a NaN.
    //
    // A CORRECTION TO THE CASE AS IT WAS SPECIFIED: +inf does NOT land on key 0, and should not.
    // It is greater than every key, so it takes the trailing clamp and lands on key n-1 -- which is
    // the right answer for a time infinitely far in the future, and the same answer any large finite
    // time gives. Only NaN and -inf land on key 0.
    const Clip clip = threeModeClip();
    const CookedSkeleton skeleton = threeModeSkeleton();
    std::array<std::uint32_t, 3> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 3);

    struct Row {
        float when;
        bool expectFirstKey;
    };
    constexpr std::array<Row, 3> ROWS = {Row{std::numeric_limits<float>::quiet_NaN(), true},
                                         Row{-std::numeric_limits<float>::infinity(), true},
                                         Row{std::numeric_limits<float>::infinity(), false}};
    REQUIRE(ROWS.size() == 3);

    for (const Row& row : ROWS) {
        CAPTURE(row.expectFirstKey);
        std::array<JointPose, 3> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);

        CHECK(allFinite(pose[0].translation));
        CHECK(allFinite(pose[1].rotation));
        CHECK(allFinite(pose[2].scale));
        if (row.expectFirstKey) {
            CHECK(pose[0].translation == Vec3{1.0F, 2.0F, 3.0F});
            CHECK(engine::approxEquals(pose[1].rotation, Quat::identity()));
            CHECK(pose[2].scale == Vec3{2.0F, 4.0F, 8.0F});
        } else {
            CHECK(pose[0].translation == Vec3{9.0F, 9.0F, 9.0F});
            CHECK(engine::approxEquals(pose[1].rotation, Quat{0.0F, 0.0F, 1.0F, 0.0F}));
            CHECK(pose[2].scale == Vec3{7.0F, 7.0F, 7.0F});
        }
    }
}

TEST_CASE("clip sampler: NON-MONOTONIC times stay inside their segment rather than extrapolate (CL25)") {
    // The parser deliberately does not check that a channel's times increase (docs/09 section
    // 13.10), so this shape is legal input and the sampler has to be total over it. times
    // {0, 3, 2, 10} breaks the order at index 2.
    //
    // WHAT THIS CASE ACTUALLY PINS, stated rather than implied: locate's own search maintains
    // t[lo] <= tc and t[hi] > tc on entry AND at exit -- lo only ever moves to a mid whose time is
    // <= tc, hi only to one whose time is > tc, and both clamps established the pair before the loop
    // -- so u is a ratio of a non-negative numerator to a strictly larger denominator whatever the
    // times do. Every sample below is therefore a convex combination of two STORED key values, and
    // the expected numbers are hand-computed from the pair the search lands on.
    const std::array<ChannelData, 1> channels = {
        ChannelData{0,
                    TRANSLATION,
                    LINEAR,
                    {0.0F, 3.0F, 2.0F, 10.0F},
                    {v4(0, 0, 0), v4(30, 0, 0), v4(20, 0, 0), v4(100, 0, 0)}},
    };
    const Clip clip = makeClip(channels);
    const std::array<std::uint32_t, 1> ids = {0};
    const CookedSkeleton skeleton = makeSkeleton(ids);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    struct Row {
        float when;
        float expected;
        float low;   // the smaller of the two stored values the search lands between
        float high;  // and the larger
    };
    // 1.5 lands between keys 0 and 1 (u = 0.5); 2.5 between 0 and 1 (u = 5/6); 5.0 and 9.0 between
    // keys 2 and 3 (u = 0.375 and 0.875), which is the pair whose times run 2 -> 10 across the break.
    constexpr std::array<Row, 4> ROWS = {Row{1.5F, 15.0F, 0.0F, 30.0F}, Row{2.5F, 25.0F, 0.0F, 30.0F},
                                         Row{5.0F, 50.0F, 20.0F, 100.0F}, Row{9.0F, 90.0F, 20.0F, 100.0F}};
    REQUIRE(ROWS.size() == 4);

    for (const Row& row : ROWS) {
        CAPTURE(row.when);
        std::array<JointPose, 1> pose{};
        bindPose(skeleton, pose);
        sampleAnimation(clip.animation(), binding, row.when, pose);
        CHECK(allFinite(pose[0].translation));
        CHECK(engine::approxEquals(pose[0].translation.x, row.expected, 1e-3F));
        CHECK(pose[0].translation.x >= row.low);
        CHECK(pose[0].translation.x <= row.high);
    }
}

// ---- the whole chain ----------------------------------------------------------------------------

TEST_CASE("clip sampler: bindPose -> sampleAnimation -> computeJointPalette, end to end (CL24)") {
    // Over the frozen CLOSURE .aeroskel golden, whose shape is what makes this worth running: three
    // records, the root HIERARCHY-ONLY (it occupies no palette slot but its transform reaches both
    // children), and palette slots that diverge from record order.
    const CookedSkeletonParseResult parsedSkeleton =
        parseCookedSkeleton(std::as_bytes(std::span<const std::uint8_t>(aero_test::COOKED_SKELETON_GOLDEN_CLOSURE)));
    REQUIRE((parsedSkeleton.status == CookedSkeletonStatus::Ok));
    const CookedSkeleton& skeleton = parsedSkeleton.skeleton;
    REQUIRE(skeleton.joints.size() == 3);
    REQUIRE(skeleton.paletteJointCount == 2);
    REQUIRE(skeleton.joints[0].sourceNodeLocalId == 10);
    REQUIRE(skeleton.joints[0].paletteSlot == COOKED_SKELETON_INVALID_INDEX);

    // One channel, on the hierarchy-only ROOT: nothing writes to a palette slot directly, so if the
    // ancestor's contribution did not reach its children the palette below could not move at all.
    // Key 0's value is the root's own bind translation (0,1,0), which is what makes the
    // before-the-first-key sample an EXACT identity rather than an approximate one.
    const std::array<ChannelData, 1> channels = {
        ChannelData{10, TRANSLATION, LINEAR, {1.0F, 2.0F}, {v4(0, 1, 0), v4(0, 5, 0)}},
    };
    const Clip clip = makeClip(channels);
    std::array<std::uint32_t, 1> binding{};
    REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

    std::array<JointPose, 3> pose{};
    std::array<Mat4, 2> bindPalette{};
    bindPose(skeleton, pose);
    computeJointPalette(skeleton, pose, bindPalette);

    std::array<Mat4, 2> palette{};
    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 0.0F, pose);
    computeJointPalette(skeleton, pose, palette);
    CHECK(palette[0] == bindPalette[0]);
    CHECK(palette[1] == bindPalette[1]);

    bindPose(skeleton, pose);
    sampleAnimation(clip.animation(), binding, 2.0F, pose);
    computeJointPalette(skeleton, pose, palette);
    CHECK(palette[0] != bindPalette[0]);
    CHECK(palette[1] != bindPalette[1]);
    // And the move is the one the clip asked for: +4 on Y, carried down from the root to both
    // children by the single forward pass.
    CHECK(engine::approxEquals(palette[0].columns[3].y - bindPalette[0].columns[3].y, 4.0F, 1e-5F));
    CHECK(engine::approxEquals(palette[1].columns[3].y - bindPalette[1].columns[3].y, 4.0F, 1e-5F));
}

TEST_CASE("clip sampler: a NON-FINITE or OVERFLOWING segment duration cannot poison a pose (CL26)") {
    // The two shapes locate's `td > 0` guard does NOT catch, and both reach hermite, which multiplies
    // its two tangent terms BY td:
    //
    //   * an OVERFLOWING segment. -3.0e38 and 3.0e38 are both finite, both legal binary32, and
    //     strictly increasing -- so the cook writes them bit for bit and the parser accepts them --
    //     but their difference is 6.0e38, which is past FLT_MAX and becomes +inf.
    //   * a NaN time, which docs/09 section 13.10 states the parser deliberately does not police.
    //
    // In BOTH cases u is already safe (`NaN > 0` is false, and finite/inf is 0), which is exactly why
    // this needs its own case: the bug hides behind a correct-looking u. inf * 0 and NaN * 0 are both
    // NaN, so a cubic channel returns a NaN pose -- and normalizeOrIdentity does not catch that
    // either, because `lenSq <= epsilon * epsilon` is FALSE for NaN, so it divides and propagates.
    // A NaN pose reaches computeJointPalette and the GPU.
    //
    // The contract this pins is animation.cpp's own: TOTAL for every float value there is.
    constexpr float QUIET_NAN = std::numeric_limits<float>::quiet_NaN();
    constexpr float POS_INF = std::numeric_limits<float>::infinity();

    struct Arm {
        const char* what;
        CookedAnimationInterpolation mode;
        std::array<float, 2> times;
    };
    constexpr std::size_t ARM_COUNT = 6;
    constexpr std::array<Arm, ARM_COUNT> ARMS = {
        Arm{"overflow/linear", LINEAR, {-3.0e38F, 3.0e38F}}, Arm{"overflow/step", STEP, {-3.0e38F, 3.0e38F}},
        Arm{"overflow/cubic", CUBIC, {-3.0e38F, 3.0e38F}},   Arm{"nan/linear", LINEAR, {0.0F, QUIET_NAN}},
        Arm{"nan/cubic", CUBIC, {0.0F, QUIET_NAN}},          Arm{"inf/cubic", CUBIC, {0.0F, POS_INF}},
    };
    REQUIRE(ARMS.size() == ARM_COUNT);  // literal row count, never TABLE.size() against itself

    for (const Arm& arm : ARMS) {
        const std::string_view what{arm.what};
        CAPTURE(what);
        const bool cubic = arm.mode == CUBIC;
        // A cubic channel stores [inTangent, value, outTangent] per key; the tangents are deliberately
        // LOUD here, so a surviving td multiplies something that cannot be mistaken for zero.
        std::vector<Vec4> values;
        if (cubic) {
            values = {v4(9, 9, 9), v4(1, 0, 0), v4(7, 7, 7), v4(8, 8, 8), v4(2, 0, 0), v4(6, 6, 6)};
        } else {
            values = {v4(1, 0, 0), v4(2, 0, 0)};
        }
        const std::array<ChannelData, 1> channels = {
            ChannelData{0, TRANSLATION, arm.mode, {arm.times[0], arm.times[1]}, values}};
        const Clip clip = makeClip(channels);
        const std::array<std::uint32_t, 1> ids = {0};
        const CookedSkeleton skeleton = makeSkeleton(ids);
        std::array<std::uint32_t, 1> binding{};
        REQUIRE(bindAnimation(clip.animation(), skeleton, binding).boundChannels == 1);

        // Sample INSIDE the segment as well as at and past its ends: the interior is where a surviving
        // td does its damage, because the clamped ends never reach hermite at all.
        constexpr std::array<float, 4> WHENS = {-1.0e38F, 0.0F, 1.0F, 1.0e38F};
        REQUIRE(WHENS.size() == 4);
        for (const float when : WHENS) {
            CAPTURE(when);
            std::array<JointPose, 1> pose{};
            bindPose(skeleton, pose);
            sampleAnimation(clip.animation(), binding, when, pose);
            CHECK(allFinite(pose[0].translation));
            // And the value is still one of the two stored keys or between them -- a hold, never a
            // number the file never carried.
            CHECK(pose[0].translation.x >= 1.0F);
            CHECK(pose[0].translation.x <= 2.0F);
        }
    }
}
